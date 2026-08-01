#include "video/FilterGraph.h"

#include <android/log.h>
#include <time.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <sstream>
#include <utility>

extern "C" {
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

namespace aribplayer {
namespace {
constexpr char kTag[] = "aribplayer-filter";

std::string AvError(int error) {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(error, text, sizeof(text));
    return text;
}

void FreeBundle(FilterGraph::GraphBundle* bundle) {
    if (bundle == nullptr) return;
    avfilter_graph_free(&bundle->graph);
    bundle->source = nullptr;
    bundle->sink = nullptr;
    bundle->sink_time_base = AVRational{0, 1};
}

int LinkFilters(AVFilterContext* source, AVFilterContext* destination) {
    if (source == nullptr || destination == nullptr) return AVERROR(EINVAL);
    return avfilter_link(source, 0, destination, 0);
}

unsigned OpenClModeBit(VideoMode mode) {
    switch (mode) {
        case VideoMode::kIvtc: return 1U << 0;
        case VideoMode::kDeinterlace: return 1U << 1;
        default: return 0;
    }
}

}  // namespace

FilterGraph::FilterGraph(AVRational time_base, AVRational frame_rate,
                         AVRational stream_sar, AVFieldOrder field_order,
                         FrameQueue& output, InfoCallback info_callback)
    : time_base_(time_base.num > 0 && time_base.den > 0 ? time_base : AVRational{1, 90000}),
      frame_rate_(frame_rate.num > 0 && frame_rate.den > 0 ? frame_rate : AVRational{30000, 1001}),
      stream_sar_(stream_sar.num > 0 && stream_sar.den > 0 ? stream_sar : AVRational{1, 1}),
      field_order_(field_order),
      output_(output),
      info_callback_(std::move(info_callback)) {}

FilterGraph::~FilterGraph() {
    ResetInternal();
    av_buffer_unref(&opencl_device_);
}

void FilterGraph::SetMode(VideoMode mode) {
    const int value = std::clamp(static_cast<int>(mode),
                                 static_cast<int>(VideoMode::kOff),
                                 static_cast<int>(VideoMode::kDeinterlace));
    requested_mode_.store(value, std::memory_order_release);
}

void FilterGraph::Prepare(const AVFrame& prototype) {
    const VideoMode requested = static_cast<VideoMode>(
            requested_mode_.load(std::memory_order_acquire));
    if (requested == VideoMode::kOff) return;

    // Warm both processing kernels whenever filtering is enabled. This keeps an
    // AUTO resolution or a later IVTC/deinterlace switch from compiling OpenCL
    // on the video decode thread after playback has started.
    EnsureOpenCl(prototype, VideoMode::kIvtc);
    EnsureOpenCl(prototype, VideoMode::kDeinterlace);
}

void FilterGraph::Reset() {
    ResetInternal();
    observed_mode_ = static_cast<VideoMode>(requested_mode_.load(std::memory_order_acquire));
    effective_mode_ = observed_mode_ == VideoMode::kAuto ? VideoMode::kOff : observed_mode_;
    effective_resolved_ = observed_mode_ != VideoMode::kAuto;
    backend_ = "none";
    info_reported_ = false;
    if (effective_resolved_ && effective_mode_ == VideoMode::kOff) ReportInfo();
}

void FilterGraph::ResetInternal() {
    FreeBundle(&active_);
    graph_width_ = 0;
    graph_height_ = 0;
    graph_format_ = -1;
    for (AVFrame* frame : auto_frames_) av_frame_free(&frame);
    auto_frames_.clear();
    auto_saw_repeat_ = false;
    auto_saw_interlaced_ = false;
}

bool FilterGraph::Process(AVFrame* frame, int serial) {
    if (frame == nullptr) return false;
    RecordInput();

    const VideoMode requested = static_cast<VideoMode>(
            requested_mode_.load(std::memory_order_acquire));
    if (requested != observed_mode_) {
        ResetInternal();
        observed_mode_ = requested;
        effective_mode_ = requested == VideoMode::kAuto ? VideoMode::kOff : requested;
        effective_resolved_ = requested != VideoMode::kAuto;
        backend_ = "none";
        info_reported_ = false;
        if (effective_resolved_ && effective_mode_ == VideoMode::kOff) ReportInfo();
    }

    if (observed_mode_ != VideoMode::kAuto) return ProcessResolved(frame, serial);

    AVFrame* copy = av_frame_clone(frame);
    if (copy == nullptr) return false;
    auto_saw_repeat_ = auto_saw_repeat_ || frame->repeat_pict > 0;
    auto_saw_interlaced_ = auto_saw_interlaced_ ||
                           ((frame->flags & AV_FRAME_FLAG_INTERLACED) != 0);
    auto_frames_.push_back(copy);
    ResolveAutoIfReady(static_cast<int>(auto_frames_.size()) >= kAutoProbeFrames);
    if (!effective_resolved_) return true;

    bool ok = true;
    std::vector<AVFrame*> pending;
    pending.swap(auto_frames_);
    for (AVFrame* pending_frame : pending) {
        if (ok) ok = ProcessResolved(pending_frame, serial);
        av_frame_free(&pending_frame);
    }
    return ok;
}

void FilterGraph::ResolveAutoIfReady(bool force) {
    if (effective_resolved_ || !force) return;
    if (auto_saw_repeat_) effective_mode_ = VideoMode::kIvtc;
    else if (auto_saw_interlaced_) effective_mode_ = VideoMode::kDeinterlace;
    else effective_mode_ = VideoMode::kOff;
    effective_resolved_ = true;
    if (effective_mode_ == VideoMode::kOff) {
        backend_ = "none";
        ReportInfo();
    }
}

bool FilterGraph::ProcessResolved(AVFrame* frame, int serial) {
    if (!effective_resolved_) return false;
    if (effective_mode_ == VideoMode::kOff) return PushBypass(*frame, serial);
    if (active_.graph != nullptr &&
        (frame->width != graph_width_ || frame->height != graph_height_ ||
         frame->format != graph_format_)) {
        FreeBundle(&active_);
        graph_width_ = 0;
        graph_height_ = 0;
        graph_format_ = -1;
        backend_ = "none";
        info_reported_ = false;
    }
    if (active_.graph == nullptr && !BuildForFrame(*frame)) {
        // A filter failure must not turn a playable stream into a black screen.
        effective_mode_ = VideoMode::kOff;
        backend_ = "none";
        ReportInfo();
        return PushBypass(*frame, serial);
    }

    const auto wall_start = std::chrono::steady_clock::now();
    const int64_t cpu_start = ThreadCpuNanoseconds();
    int result = av_buffersrc_add_frame_flags(active_.source, frame, AV_BUFFERSRC_FLAG_KEEP_REF);
    int produced = 0;
    double queue_wait_ms = 0.0;
    bool ok = result >= 0;
    if (ok) ok = DrainSink(serial, &produced, &queue_wait_ms);
    const int64_t cpu_end = ThreadCpuNanoseconds();
    const auto wall_end = std::chrono::steady_clock::now();
    const double wall_ms = std::max(0.0,
            std::chrono::duration<double, std::milli>(wall_end - wall_start).count() -
            queue_wait_ms);
    const double cpu_ms = cpu_start >= 0 && cpu_end >= cpu_start
            ? static_cast<double>(cpu_end - cpu_start) / 1000000.0 : 0.0;
    RecordFilterTiming(wall_ms, cpu_ms);
    if (result < 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "buffersrc failed: %s", AvError(result).c_str());
    }
    return ok;
}

bool FilterGraph::PushBypass(const AVFrame& frame, int serial) {
    const bool ok = output_.Push(frame, serial, FramePtsSeconds(frame, time_base_), true) ==
                    QueueResult::kOk;
    if (ok) RecordOutput(1);
    return ok;
}

bool FilterGraph::BuildForFrame(const AVFrame& frame) {
    std::string error;
    if (EnsureOpenCl(frame, effective_mode_) &&
        BuildBundle(frame, effective_mode_, true, &active_, &error)) {
        backend_ = "opencl";
        graph_width_ = frame.width;
        graph_height_ = frame.height;
        graph_format_ = frame.format;
        ReportInfo();
        return true;
    }

    FreeBundle(&active_);
    error.clear();
    if (BuildBundle(frame, effective_mode_, false, &active_, &error)) {
        backend_ = "software";
        graph_width_ = frame.width;
        graph_height_ = frame.height;
        graph_format_ = frame.format;
        ReportInfo();
        return true;
    }
    __android_log_print(ANDROID_LOG_ERROR, kTag, "Unable to build filter graph: %s", error.c_str());
    FreeBundle(&active_);
    return false;
}

bool FilterGraph::EnsureOpenCl(const AVFrame& frame, VideoMode mode) {
    const unsigned mode_bit = OpenClModeBit(mode);
    if (mode_bit == 0 || (failed_opencl_modes_ & mode_bit) != 0) return false;
    if ((prepared_opencl_modes_ & mode_bit) != 0) return true;
    if (opencl_state_ == OpenClState::kUnavailable) return false;

    if (opencl_state_ == OpenClState::kUnknown) {
        const int result = av_hwdevice_ctx_create(&opencl_device_, AV_HWDEVICE_TYPE_OPENCL,
                                                  nullptr, nullptr, 0);
        if (result < 0 || opencl_device_ == nullptr) {
            __android_log_print(ANDROID_LOG_INFO, kTag, "OpenCL unavailable: %s",
                                AvError(result).c_str());
            av_buffer_unref(&opencl_device_);
            opencl_state_ = OpenClState::kUnavailable;
            return false;
        }
        opencl_state_ = OpenClState::kAvailable;
    }

    GraphBundle trial;
    std::string error;
    if (!BuildBundle(frame, mode, true, &trial, &error)) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "OpenCL filter probe failed: %s",
                            error.c_str());
        FreeBundle(&trial);
        failed_opencl_modes_ |= mode_bit;
        return false;
    }
    FreeBundle(&trial);
    prepared_opencl_modes_ |= mode_bit;
    return true;
}

bool FilterGraph::BuildBundle(const AVFrame& frame, VideoMode mode, bool opencl,
                              GraphBundle* bundle, std::string* error) {
    if (bundle == nullptr || (mode != VideoMode::kIvtc && mode != VideoMode::kDeinterlace)) {
        if (error) *error = "invalid graph request";
        return false;
    }
    FreeBundle(bundle);
    bundle->graph = avfilter_graph_alloc();
    if (bundle->graph == nullptr) {
        if (error) *error = "unable to allocate filter graph";
        return false;
    }

    const AVFilter* buffer_filter = avfilter_get_by_name("buffer");
    const AVFilter* sink_filter = avfilter_get_by_name("buffersink");
    if (buffer_filter == nullptr || sink_filter == nullptr) {
        if (error) *error = "buffer filters unavailable";
        FreeBundle(bundle);
        return false;
    }

    const AVRational sar = frame.sample_aspect_ratio.num > 0 && frame.sample_aspect_ratio.den > 0
            ? frame.sample_aspect_ratio : stream_sar_;
    char source_args[512]{};
    std::snprintf(source_args, sizeof(source_args),
                  "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d:frame_rate=%d/%d:colorspace=%d:range=%d",
                  frame.width, frame.height, frame.format, time_base_.num, time_base_.den,
                  sar.num, sar.den, frame_rate_.num, frame_rate_.den,
                  frame.colorspace, frame.color_range);
    int result = avfilter_graph_create_filter(&bundle->source, buffer_filter, "in",
                                               source_args, nullptr, bundle->graph);
    if (result < 0) {
        if (error) *error = "buffer: " + AvError(result);
        FreeBundle(bundle);
        return false;
    }

    AVFilterContext* previous = bundle->source;
    auto add_filter = [&](const char* filter_name, const char* instance_name,
                          const char* args, bool attach_device) -> bool {
        const AVFilter* filter = avfilter_get_by_name(filter_name);
        if (filter == nullptr) {
            if (error) *error = std::string("missing filter ") + filter_name;
            return false;
        }
        AVFilterContext* context = nullptr;
        if (attach_device) {
            context = avfilter_graph_alloc_filter(bundle->graph, filter, instance_name);
            if (context == nullptr) {
                if (error) *error = std::string("allocate ") + filter_name;
                return false;
            }
            context->hw_device_ctx = av_buffer_ref(opencl_device_);
            if (context->hw_device_ctx == nullptr) {
                if (error) *error = std::string("device ref for ") + filter_name;
                return false;
            }
            result = avfilter_init_str(context, args);
        } else {
            result = avfilter_graph_create_filter(&context, filter, instance_name,
                                                   args, nullptr, bundle->graph);
        }
        if (result < 0) {
            if (error) *error = std::string(filter_name) + ": " + AvError(result);
            return false;
        }
        result = LinkFilters(previous, context);
        if (result < 0) {
            if (error) *error = std::string("link ") + filter_name + ": " + AvError(result);
            return false;
        }
        previous = context;
        return true;
    };

    const bool tff = FrameIsTopFieldFirst(frame, field_order_);
    const char* parity = tff ? "tff" : "bff";
    if (opencl) {
        if (opencl_device_ == nullptr ||
            !add_filter("hwupload", "hwupload", nullptr, true)) {
            FreeBundle(bundle);
            return false;
        }
        std::string processing_args;
        const char* processing_filter = nullptr;
        if (mode == VideoMode::kIvtc) {
            processing_filter = "ivtc_opencl";
            processing_args = std::string("tff=") + (tff ? "1" : "0");
        } else {
            processing_filter = "bwdif_opencl";
            processing_args = std::string("mode=send_field:parity=") + parity + ":deint=all";
        }
        if (!add_filter(processing_filter, "process", processing_args.c_str(), true) ||
            !add_filter("hwdownload", "hwdownload", nullptr, false) ||
            !add_filter("format", "format", "pix_fmts=nv12|yuv420p", false)) {
            FreeBundle(bundle);
            return false;
        }
    } else if (mode == VideoMode::kIvtc) {
        const std::string field_args = std::string("order=") + parity + ":combmatch=full";
        if (!add_filter("fieldmatch", "fieldmatch", field_args.c_str(), false) ||
            !add_filter("decimate", "decimate", nullptr, false)) {
            FreeBundle(bundle);
            return false;
        }
    } else {
        const std::string bwdif_args = std::string("mode=send_field:parity=") + parity + ":deint=all";
        if (!add_filter("bwdif", "bwdif", bwdif_args.c_str(), false)) {
            FreeBundle(bundle);
            return false;
        }
    }

    result = avfilter_graph_create_filter(&bundle->sink, sink_filter, "out",
                                           nullptr, nullptr, bundle->graph);
    if (result >= 0) result = LinkFilters(previous, bundle->sink);
    if (result >= 0) result = avfilter_graph_config(bundle->graph, nullptr);
    if (result < 0) {
        if (error) *error = "graph config: " + AvError(result);
        FreeBundle(bundle);
        return false;
    }
    bundle->sink_time_base = av_buffersink_get_time_base(bundle->sink);
    if (bundle->sink_time_base.num <= 0 || bundle->sink_time_base.den <= 0) {
        if (error) *error = "invalid output time base";
        FreeBundle(bundle);
        return false;
    }
    return true;
}

bool FilterGraph::DrainSink(int serial, int* produced, double* queue_wait_ms) {
    AVFrame* filtered = av_frame_alloc();
    if (filtered == nullptr) return false;
    bool ok = true;
    int count = 0;
    while (true) {
        const int result = av_buffersink_get_frame(active_.sink, filtered);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
        if (result < 0) {
            __android_log_print(ANDROID_LOG_ERROR, kTag, "buffersink failed: %s", AvError(result).c_str());
            ok = false;
            break;
        }
        if (!IsSupportedSoftwareFormat(filtered->format)) {
            __android_log_print(ANDROID_LOG_ERROR, kTag, "Unsupported filtered format: %s",
                                av_get_pix_fmt_name(static_cast<AVPixelFormat>(filtered->format)));
            ok = false;
            av_frame_unref(filtered);
            break;
        }
        const int64_t output_timestamp = filtered->pts != AV_NOPTS_VALUE
                ? filtered->pts : filtered->best_effort_timestamp;
        const double pts = output_timestamp == AV_NOPTS_VALUE ? 0.0
                : output_timestamp * av_q2d(active_.sink_time_base);
        const auto queue_start = std::chrono::steady_clock::now();
        const QueueResult queue_result = output_.Push(*filtered, serial, pts, true);
        const auto queue_end = std::chrono::steady_clock::now();
        if (queue_wait_ms) {
            *queue_wait_ms += std::chrono::duration<double, std::milli>(
                    queue_end - queue_start).count();
        }
        if (queue_result != QueueResult::kOk) {
            ok = false;
            av_frame_unref(filtered);
            break;
        }
        ++count;
        av_frame_unref(filtered);
    }
    av_frame_free(&filtered);
    if (count > 0) RecordOutput(count);
    if (produced) *produced += count;
    return ok;
}

bool FilterGraph::EndOfStream(int serial) {
    if (observed_mode_ == VideoMode::kAuto && !effective_resolved_) {
        ResolveAutoIfReady(true);
        std::vector<AVFrame*> pending;
        pending.swap(auto_frames_);
        for (AVFrame* frame : pending) {
            const bool ok = ProcessResolved(frame, serial);
            av_frame_free(&frame);
            if (!ok) return false;
        }
    }
    if (active_.graph == nullptr) return true;
    int result = av_buffersrc_add_frame_flags(active_.source, nullptr, 0);
    if (result < 0 && result != AVERROR_EOF) return false;
    int produced = 0;
    return DrainSink(serial, &produced);
}

void FilterGraph::ReportInfo() {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        changed = !info_reported_ ||
                  reported_effective_mode_ != static_cast<int>(effective_mode_) ||
                  reported_backend_ != backend_;
        reported_effective_mode_ = static_cast<int>(effective_mode_);
        reported_backend_ = backend_;
    }
    info_reported_ = true;
    if (changed && info_callback_) info_callback_(effective_mode_, backend_);
}

void FilterGraph::RecordInput() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    RollStatsWindowLocked(std::chrono::steady_clock::now());
    ++input_frames_;
}

void FilterGraph::RecordOutput(int count) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    RollStatsWindowLocked(std::chrono::steady_clock::now());
    output_frames_ += static_cast<uint64_t>(std::max(0, count));
}

void FilterGraph::RecordFilterTiming(double wall_ms, double cpu_ms) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    ++timed_frames_;
    total_filter_ms_ += wall_ms;
    total_thread_cpu_ms_ += cpu_ms;
}

void FilterGraph::RollStatsWindowLocked(std::chrono::steady_clock::time_point now) const {
    const double seconds = std::chrono::duration<double>(now - stats_started_).count();
    if (seconds < 2.0) return;
    rolling_in_fps_ = static_cast<double>(input_frames_) / seconds;
    rolling_out_fps_ = static_cast<double>(output_frames_) / seconds;
    input_frames_ = 0;
    output_frames_ = 0;
    stats_started_ = now;
}

VideoFilterStats FilterGraph::GetStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    const auto now = std::chrono::steady_clock::now();
    RollStatsWindowLocked(now);
    const double seconds = std::max(0.001,
            std::chrono::duration<double>(now - stats_started_).count());
    VideoFilterStats result;
    result.requested_mode = requested_mode_.load(std::memory_order_acquire);
    result.effective_mode = reported_effective_mode_;
    result.backend = reported_backend_;
    result.in_fps = input_frames_ > 0 ? static_cast<double>(input_frames_) / seconds : rolling_in_fps_;
    result.out_fps = output_frames_ > 0 ? static_cast<double>(output_frames_) / seconds : rolling_out_fps_;
    result.avg_filter_ms = timed_frames_ > 0 ? total_filter_ms_ / timed_frames_ : 0.0;
    result.filter_thread_cpu_ms = total_thread_cpu_ms_;
    return result;
}

bool FilterGraph::IsSupportedSoftwareFormat(int format) {
    return format == AV_PIX_FMT_NV12 || format == AV_PIX_FMT_YUV420P ||
           format == AV_PIX_FMT_YUVJ420P;
}

bool FilterGraph::FrameIsTopFieldFirst(const AVFrame& frame, AVFieldOrder field_order) {
    if ((frame.flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) != 0) return true;
    if ((frame.flags & AV_FRAME_FLAG_INTERLACED) != 0) return false;
    switch (field_order) {
        case AV_FIELD_BB:
        case AV_FIELD_BT:
            return false;
        case AV_FIELD_TT:
        case AV_FIELD_TB:
            return true;
        default:
            return true;
    }
}

double FilterGraph::FramePtsSeconds(const AVFrame& frame, AVRational time_base) {
    const int64_t timestamp = frame.best_effort_timestamp != AV_NOPTS_VALUE
            ? frame.best_effort_timestamp : frame.pts;
    return timestamp == AV_NOPTS_VALUE ? 0.0 : timestamp * av_q2d(time_base);
}

int64_t FilterGraph::ThreadCpuNanoseconds() {
    timespec value{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0) return -1;
    return static_cast<int64_t>(value.tv_sec) * 1000000000LL + value.tv_nsec;
}

}  // namespace aribplayer
