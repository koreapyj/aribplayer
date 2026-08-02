#include "video/FilterGraph.h"

#include <android/log.h>
#include <time.h>

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#ifndef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 120
#endif

extern "C" {
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_opencl.h>
#include <libavutil/pixdesc.h>
}

#pragma weak clGetDeviceInfo
#pragma weak clGetPlatformInfo

namespace aribplayer {
namespace {
constexpr char kTag[] = "aribplayer-filter";

std::string AvError(int error) {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(error, text, sizeof(text));
    return text;
}

int NormalizeVideoModeValue(int value) {
    if (value == 1) value = static_cast<int>(VideoMode::kDeinterlace);
    return std::clamp(value, static_cast<int>(VideoMode::kOff),
                      static_cast<int>(VideoMode::kDeinterlace));
}

const char* PixelFormatName(int format) {
    if (format < 0) return "none";
    const char* name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(format));
    return name != nullptr ? name : "unknown";
}

std::string PixelFormatList(const enum AVPixelFormat* formats) {
    if (formats == nullptr) return "unknown";
    std::ostringstream result;
    bool first = true;
    for (const enum AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
        if (!first) result << '|';
        result << PixelFormatName(*format);
        first = false;
    }
    return first ? "none" : result.str();
}

bool IsRendererSupportedFormat(int format) {
    return format == AV_PIX_FMT_NV12 || format == AV_PIX_FMT_YUV420P ||
           format == AV_PIX_FMT_YUVJ420P;
}

bool ContainsPixelFormat(const enum AVPixelFormat* formats, int format) {
    if (formats == nullptr) return false;
    for (const enum AVPixelFormat* candidate = formats;
         *candidate != AV_PIX_FMT_NONE; ++candidate) {
        if (*candidate == format) return true;
    }
    return false;
}

int SelectOpenClSoftwareFormat(int input_format, const AVHWFramesConstraints* constraints) {
    if (constraints == nullptr || constraints->valid_sw_formats == nullptr) return AV_PIX_FMT_NONE;
    if (IsRendererSupportedFormat(input_format) &&
        ContainsPixelFormat(constraints->valid_sw_formats, input_format)) {
        return input_format;
    }
    if (ContainsPixelFormat(constraints->valid_sw_formats, AV_PIX_FMT_YUV420P)) {
        return AV_PIX_FMT_YUV420P;
    }
    if (ContainsPixelFormat(constraints->valid_sw_formats, AV_PIX_FMT_NV12)) {
        return AV_PIX_FMT_NV12;
    }
    return AV_PIX_FMT_NONE;
}

std::string QueryOpenClDeviceString(cl_device_id device, cl_device_info parameter) {
    if (device == nullptr || clGetDeviceInfo == nullptr) return "unavailable";
    size_t size = 0;
    if (clGetDeviceInfo(device, parameter, 0, nullptr, &size) != CL_SUCCESS || size == 0) {
        return "unavailable";
    }
    std::vector<char> value(size, '\0');
    if (clGetDeviceInfo(device, parameter, value.size(), value.data(), nullptr) != CL_SUCCESS) {
        return "unavailable";
    }
    return std::string(value.data());
}

std::string QueryOpenClPlatformString(cl_platform_id platform, cl_platform_info parameter) {
    if (platform == nullptr || clGetPlatformInfo == nullptr) return "unavailable";
    size_t size = 0;
    if (clGetPlatformInfo(platform, parameter, 0, nullptr, &size) != CL_SUCCESS || size == 0) {
        return "unavailable";
    }
    std::vector<char> value(size, '\0');
    if (clGetPlatformInfo(platform, parameter, value.size(), value.data(), nullptr) != CL_SUCCESS) {
        return "unavailable";
    }
    return std::string(value.data());
}

void LogOpenClDeviceInfo(AVBufferRef* device) {
    const auto* device_context = device != nullptr
            ? reinterpret_cast<const AVHWDeviceContext*>(device->data) : nullptr;
    const auto* opencl_context = device_context != nullptr
            ? static_cast<const AVOpenCLDeviceContext*>(device_context->hwctx) : nullptr;
    const cl_device_id device_id = opencl_context != nullptr ? opencl_context->device_id : nullptr;
    cl_platform_id platform = nullptr;
    if (device_id != nullptr && clGetDeviceInfo != nullptr) {
        clGetDeviceInfo(device_id, CL_DEVICE_PLATFORM, sizeof(platform), &platform, nullptr);
    }
    __android_log_print(
            ANDROID_LOG_INFO, kTag,
            "OpenCL device info: device=%s vendor=%s version=%s platform=%s "
            "platform_vendor=%s platform_version=%s id=%p",
            QueryOpenClDeviceString(device_id, CL_DEVICE_NAME).c_str(),
            QueryOpenClDeviceString(device_id, CL_DEVICE_VENDOR).c_str(),
            QueryOpenClDeviceString(device_id, CL_DEVICE_VERSION).c_str(),
            QueryOpenClPlatformString(platform, CL_PLATFORM_NAME).c_str(),
            QueryOpenClPlatformString(platform, CL_PLATFORM_VENDOR).c_str(),
            QueryOpenClPlatformString(platform, CL_PLATFORM_VERSION).c_str(),
            static_cast<void*>(device_id));
}

int EffectiveColorRange(const OpenClGraphKey& key) {
    if (key.format == AV_PIX_FMT_YUVJ420P && key.color_range == AVCOL_RANGE_UNSPECIFIED) {
        return AVCOL_RANGE_JPEG;
    }
    return key.color_range;
}

bool IsFullRange(const OpenClGraphKey& key) {
    return EffectiveColorRange(key) == AVCOL_RANGE_JPEG ||
           key.format == AV_PIX_FMT_YUVJ420P;
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

constexpr int64_t kOpenClProbeDeadlineMs = 1500;

enum class OpenClProbeState { kUnknown, kProbing, kReady, kUnavailable };

struct OpenClProbeSpec {
    OpenClGraphKey key;
    AVFieldOrder field_order = AV_FIELD_UNKNOWN;
    int opencl_sw_format = AV_PIX_FMT_NONE;
};

struct OpenClProbeResult {
    OpenClGraphKey key;
    unsigned ready_modes = 0;
    unsigned failed_modes = 0;
    FilterGraph::GraphBundle ivtc;
    FilterGraph::GraphBundle deinterlace;
};

struct OpenClProbeControl {
    std::mutex mutex;
    std::condition_variable condition;
    OpenClProbeState state = OpenClProbeState::kProbing;
    bool worker_done = false;
    bool result_taken = false;
    unsigned ready_modes = 0;
    unsigned failed_modes = 0;
    std::chrono::steady_clock::time_point deadline{};
    OpenClGraphKey key;
    FilterGraph::GraphBundle ivtc;
    FilterGraph::GraphBundle deinterlace;
};

void MoveBundle(FilterGraph::GraphBundle* destination,
                FilterGraph::GraphBundle* source) {
    if (destination == nullptr || source == nullptr) return;
    *destination = *source;
    source->graph = nullptr;
    source->source = nullptr;
    source->sink = nullptr;
    source->sink_time_base = AVRational{0, 1};
}

bool BuildBundleForSpec(const OpenClProbeSpec& spec, VideoMode mode, bool opencl,
                        AVBufferRef* opencl_device, FilterGraph::GraphBundle* bundle,
                        std::string* error);
void RunOpenClProbe(const std::shared_ptr<OpenClProbeControl>& control,
                    const OpenClProbeSpec& spec);

class OpenClProbeCoordinator final {
public:
    static OpenClProbeCoordinator& Instance() {
        static OpenClProbeCoordinator* coordinator = new OpenClProbeCoordinator();
        return *coordinator;
    }

    void StartIfUnknown(const OpenClProbeSpec& spec) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (process_state_ == OpenClProbeState::kUnavailable || control_ != nullptr) return;
        StartProbeLocked(spec);
    }

    bool Acquire(const OpenClProbeSpec& spec, OpenClProbeResult* result) {
        if (result == nullptr) return false;
        StartIfUnknown(spec);

        std::shared_ptr<OpenClProbeControl> control;
        bool wait_for_worker = false;
        {
            std::lock_guard<std::mutex> coordinator_lock(mutex_);
            if (process_state_ == OpenClProbeState::kUnavailable) return false;
            if (control_ == nullptr) StartProbeLocked(spec);
            control = control_;
            if (control == nullptr) return false;

            OpenClProbeState worker_state;
            bool result_taken = false;
            OpenClGraphKey worker_key;
            {
                std::lock_guard<std::mutex> control_lock(control->mutex);
                worker_state = control->state;
                result_taken = control->result_taken;
                worker_key = control->key;
            }

            if (worker_state == OpenClProbeState::kUnavailable) {
                process_state_ = OpenClProbeState::kUnavailable;
                JoinWorkerIfCurrentLocked(control);
                return false;
            }
            if (worker_state == OpenClProbeState::kProbing) {
                // A different key must not make this open wait behind another
                // session's build. It will use software and a later open may try.
                if (!(worker_key == spec.key)) return false;
                wait_for_worker = true;
            } else if (worker_state == OpenClProbeState::kReady &&
                       (result_taken || !(worker_key == spec.key))) {
                // The compiler is known to be healthy, but the prepared graph
                // is single-use or keyed to another format. Join the completed
                // worker before replacing its control block so there is still
                // only one process-global worker at a time.
                process_state_ = OpenClProbeState::kReady;
                JoinWorkerIfCurrentLocked(control);
                StartProbeLocked(spec);
                control = control_;
                wait_for_worker = true;
            }
        }

        if (wait_for_worker) {
            std::unique_lock<std::mutex> control_lock(control->mutex);
            if (control->state == OpenClProbeState::kProbing) {
                const auto deadline = control->deadline;
                if (!control->condition.wait_until(control_lock, deadline, [&control] {
                        return control->worker_done ||
                               control->state != OpenClProbeState::kProbing;
                    })) {
                    control_lock.unlock();
                    LatchTimeout(control);
                    return false;
                }
            }
        }

        std::lock_guard<std::mutex> coordinator_lock(mutex_);
        JoinWorkerIfCurrentLocked(control);
        std::lock_guard<std::mutex> control_lock(control->mutex);
        if (control->state == OpenClProbeState::kUnavailable) {
            process_state_ = OpenClProbeState::kUnavailable;
            return false;
        }
        if (process_state_ == OpenClProbeState::kUnavailable ||
            control->state != OpenClProbeState::kReady || control->result_taken ||
            !(control->key == spec.key)) {
            return false;
        }
        result->key = control->key;
        result->ready_modes = control->ready_modes;
        result->failed_modes = control->failed_modes;
        MoveBundle(&result->ivtc, &control->ivtc);
        MoveBundle(&result->deinterlace, &control->deinterlace);
        control->result_taken = true;
        if (process_state_ == OpenClProbeState::kProbing) {
            process_state_ = OpenClProbeState::kReady;
        }
        return true;
    }

    void Snapshot(OpenClProbeState* state, unsigned* ready_modes,
                   unsigned* failed_modes) {
        std::lock_guard<std::mutex> coordinator_lock(mutex_);
        OpenClProbeState snapshot = process_state_;
        if (control_ != nullptr) {
            std::lock_guard<std::mutex> control_lock(control_->mutex);
            if (control_->state == OpenClProbeState::kUnavailable) {
                process_state_ = OpenClProbeState::kUnavailable;
                snapshot = process_state_;
            }
        }
        if (state) *state = snapshot;
        if (ready_modes) *ready_modes = snapshot == OpenClProbeState::kReady ? 3U : 0U;
        // Mode failures are per prepared key and are checked by FilterGraph;
        // kReady here reports only that the process-wide compiler is healthy.
        if (failed_modes) *failed_modes = 0;
    }

private:
    OpenClProbeCoordinator() = default;

    ~OpenClProbeCoordinator() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (worker_.joinable()) worker_.detach();
    }

    void StartProbeLocked(const OpenClProbeSpec& spec) {
        control_ = std::make_shared<OpenClProbeControl>();
        control_->key = spec.key;
        control_->deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(kOpenClProbeDeadlineMs);
        if (process_state_ == OpenClProbeState::kUnknown) {
            process_state_ = OpenClProbeState::kProbing;
        }
        worker_ = std::thread(RunOpenClProbe, control_, spec);
    }

    void JoinWorkerIfCurrentLocked(const std::shared_ptr<OpenClProbeControl>& control) {
        if (control_ == control && worker_.joinable()) {
            worker_.join();
        }
    }

    void LatchTimeout(const std::shared_ptr<OpenClProbeControl>& control) {
        bool won = false;
        {
            std::lock_guard<std::mutex> coordinator_lock(mutex_);
            if (process_state_ == OpenClProbeState::kUnavailable) return;
            std::lock_guard<std::mutex> control_lock(control->mutex);
            if (control->state == OpenClProbeState::kProbing) {
                control->state = OpenClProbeState::kUnavailable;
                control->ready_modes = 0;
                control->failed_modes = 0;
                process_state_ = OpenClProbeState::kUnavailable;
                if (control_ == control && worker_.joinable()) worker_.detach();
                won = true;
            }
        }
        control->condition.notify_all();
        if (!won) return;
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "OpenCL probe timed out after 1500ms; disabled for process");
    }

    mutable std::mutex mutex_;
    OpenClProbeState process_state_ = OpenClProbeState::kUnknown;
    std::shared_ptr<OpenClProbeControl> control_;
    std::thread worker_;
};

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
    FreePreparedBundles();
}

void FilterGraph::SetMode(VideoMode mode) {
    const int value = NormalizeVideoModeValue(static_cast<int>(mode));
    requested_mode_.store(value, std::memory_order_release);
}

void FilterGraph::Prepare(const AVFrame& prototype) {
    const VideoMode requested = static_cast<VideoMode>(
            requested_mode_.load(std::memory_order_acquire));
    if (requested == VideoMode::kOff) return;

    OpenClProbeSpec spec;
    spec.key = MakeGraphKey(prototype);
    spec.field_order = field_order_;
    OpenClProbeResult result;
    if (!OpenClProbeCoordinator::Instance().Acquire(spec, &result)) return;

    FreePreparedBundles();
    failed_opencl_modes_ = result.failed_modes;
    prepared_opencl_modes_ = result.ready_modes;
    if ((prepared_opencl_modes_ & OpenClModeBit(VideoMode::kIvtc)) != 0) {
        MoveBundle(&prepared_ivtc_, &result.ivtc);
        prepared_ivtc_key_ = result.key;
    }
    if ((prepared_opencl_modes_ & OpenClModeBit(VideoMode::kDeinterlace)) != 0) {
        MoveBundle(&prepared_deinterlace_, &result.deinterlace);
        prepared_deinterlace_key_ = result.key;
    }
}

void FilterGraph::Reset() {
    ResetInternal();
    observed_mode_ = static_cast<VideoMode>(
            requested_mode_.load(std::memory_order_acquire));
    effective_mode_ = observed_mode_;
    backend_ = "none";
    info_reported_ = false;
    if (effective_mode_ == VideoMode::kOff) ReportInfo();
}

void FilterGraph::ResetInternal() {
    FreeBundle(&active_);
    graph_width_ = 0;
    graph_height_ = 0;
    graph_format_ = -1;
    active_key_ = OpenClGraphKey{};
    failed_graph_key_ = OpenClGraphKey{};
    failed_graph_key_valid_ = false;
}

bool FilterGraph::Process(AVFrame* frame, int serial) {
    if (frame == nullptr) return false;
    RecordInput();

    const VideoMode requested = static_cast<VideoMode>(
            requested_mode_.load(std::memory_order_acquire));
    if (requested != observed_mode_) {
        ResetInternal();
        observed_mode_ = requested;
        effective_mode_ = requested;
        backend_ = "none";
        info_reported_ = false;
        if (effective_mode_ == VideoMode::kOff) ReportInfo();
    }

    return ProcessResolved(frame, serial);
}

bool FilterGraph::ProcessResolved(AVFrame* frame, int serial) {
    if (effective_mode_ == VideoMode::kOff) return PushBypass(*frame, serial);
    const OpenClGraphKey key = MakeGraphKey(*frame);
    if (active_.graph != nullptr && !(active_key_ == key)) {
        FreeBundle(&active_);
        graph_width_ = 0;
        graph_height_ = 0;
        graph_format_ = -1;
        active_key_ = OpenClGraphKey{};
        failed_graph_key_ = OpenClGraphKey{};
        failed_graph_key_valid_ = false;
        backend_ = "none";
        info_reported_ = false;
    }
    if (failed_graph_key_valid_ && !(failed_graph_key_ == key)) {
        failed_graph_key_valid_ = false;
    }
    if (failed_graph_key_valid_ && failed_graph_key_ == key) {
        return PushBypass(*frame, serial);
    }
    if (active_.graph == nullptr && !BuildForFrame(*frame)) {
        // A filter failure must not turn a playable stream into a black screen.
        // Keep the requested mode and bypass only this graph key until it changes.
        failed_graph_key_ = key;
        failed_graph_key_valid_ = true;
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
        TakePreparedBundle(frame, effective_mode_, &active_)) {
        backend_ = "opencl";
        graph_width_ = frame.width;
        graph_height_ = frame.height;
        graph_format_ = frame.format;
        active_key_ = MakeGraphKey(frame);
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
        active_key_ = MakeGraphKey(frame);
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

    OpenClProbeState state = OpenClProbeState::kUnknown;
    unsigned ready_modes = 0;
    unsigned failed_modes = 0;
    OpenClProbeCoordinator::Instance().Snapshot(&state, &ready_modes, &failed_modes);
    if (state != OpenClProbeState::kReady || (ready_modes & mode_bit) == 0 ||
        (failed_modes & mode_bit) != 0) {
        return false;
    }
    return (prepared_opencl_modes_ & mode_bit) != 0 &&
           ((mode == VideoMode::kIvtc ? prepared_ivtc_key_ : prepared_deinterlace_key_) ==
            MakeGraphKey(frame));
}

bool FilterGraph::TakePreparedBundle(const AVFrame& frame, VideoMode mode,
                                     GraphBundle* bundle) {
    const unsigned mode_bit = OpenClModeBit(mode);
    if (bundle == nullptr || (prepared_opencl_modes_ & mode_bit) == 0) return false;
    const OpenClGraphKey key = MakeGraphKey(frame);
    if (mode == VideoMode::kIvtc) {
        if (!(prepared_ivtc_key_ == key)) return false;
        MoveBundle(bundle, &prepared_ivtc_);
        prepared_ivtc_key_ = OpenClGraphKey{};
    } else if (mode == VideoMode::kDeinterlace) {
        if (!(prepared_deinterlace_key_ == key)) return false;
        MoveBundle(bundle, &prepared_deinterlace_);
        prepared_deinterlace_key_ = OpenClGraphKey{};
    } else {
        return false;
    }
    prepared_opencl_modes_ &= ~mode_bit;
    return true;
}

OpenClGraphKey FilterGraph::MakeGraphKey(const AVFrame& frame) const {
    OpenClGraphKey key;
    key.width = frame.width;
    key.height = frame.height;
    key.format = frame.format;
    key.colorspace = frame.colorspace;
    key.color_range = frame.color_range;
    if (key.format == AV_PIX_FMT_YUVJ420P && key.color_range == AVCOL_RANGE_UNSPECIFIED) {
        key.color_range = AVCOL_RANGE_JPEG;
    }
    key.sample_aspect_ratio = frame.sample_aspect_ratio.num > 0 &&
                              frame.sample_aspect_ratio.den > 0
            ? frame.sample_aspect_ratio : stream_sar_;
    key.time_base = time_base_;
    key.frame_rate = frame_rate_;
    key.top_field_first = FrameIsTopFieldFirst(frame, field_order_);
    key.valid = key.width > 0 && key.height > 0 && key.format >= 0 &&
                key.sample_aspect_ratio.num > 0 && key.sample_aspect_ratio.den > 0 &&
                key.time_base.num > 0 && key.time_base.den > 0 &&
                key.frame_rate.num > 0 && key.frame_rate.den > 0;
    return key;
}

void FilterGraph::FreePreparedBundles() {
    FreeBundle(&prepared_ivtc_);
    FreeBundle(&prepared_deinterlace_);
    prepared_opencl_modes_ = 0;
    prepared_ivtc_key_ = OpenClGraphKey{};
    prepared_deinterlace_key_ = OpenClGraphKey{};
}

namespace {

bool BuildBundleForSpec(const OpenClProbeSpec& spec, VideoMode mode, bool opencl,
                        AVBufferRef* opencl_device, FilterGraph::GraphBundle* bundle,
                        std::string* error) {
    const OpenClGraphKey& key = spec.key;
    if (bundle == nullptr || !key.valid ||
        (mode != VideoMode::kIvtc && mode != VideoMode::kDeinterlace)) {
        if (error) *error = "invalid graph request";
        return false;
    }
    if (opencl && !IsRendererSupportedFormat(spec.opencl_sw_format)) {
        if (error) *error = "OpenCL format selection unavailable";
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

    char source_args[512]{};
    std::snprintf(source_args, sizeof(source_args),
                  "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d:frame_rate=%d/%d:colorspace=%d:range=%d",
                  key.width, key.height, key.format, key.time_base.num, key.time_base.den,
                  key.sample_aspect_ratio.num, key.sample_aspect_ratio.den,
                  key.frame_rate.num, key.frame_rate.den, key.colorspace,
                  EffectiveColorRange(key));
    int result = avfilter_graph_create_filter(&bundle->source, buffer_filter, "in",
                                               source_args, nullptr, bundle->graph);
    if (result < 0) {
        if (error) *error = "buffer: " + AvError(result);
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "%s buffer stage failed: %s (%d)",
                            opencl ? "OpenCL" : "software", AvError(result).c_str(), result);
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
            if (opencl_device == nullptr) {
                if (error) *error = std::string("missing OpenCL device for ") + filter_name;
                return false;
            }
            context = avfilter_graph_alloc_filter(bundle->graph, filter, instance_name);
            if (context == nullptr) {
                if (error) *error = std::string("allocate ") + filter_name;
                return false;
            }
            context->hw_device_ctx = av_buffer_ref(opencl_device);
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
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                                "%s %s init stage failed: %s (%d)",
                                opencl ? "OpenCL" : "software", filter_name,
                                AvError(result).c_str(), result);
            return false;
        }
        result = LinkFilters(previous, context);
        if (result < 0) {
            if (error) *error = std::string("link ") + filter_name + ": " + AvError(result);
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                                "%s link %s stage failed: %s (%d)",
                                opencl ? "OpenCL" : "software", filter_name,
                                AvError(result).c_str(), result);
            return false;
        }
        previous = context;
        return true;
    };

    const char* parity = key.top_field_first ? "tff" : "bff";
    if (opencl) {
        const int selected_format = spec.opencl_sw_format;
        const bool full_range = IsFullRange(key);
        if (selected_format != key.format) {
            const std::string upload_format_args =
                    std::string("pix_fmts=") + PixelFormatName(selected_format);
            if (!add_filter("format", "format_upload", upload_format_args.c_str(), false) ||
                (full_range && selected_format != AV_PIX_FMT_YUVJ420P &&
                 !add_filter("setparams", "range_upload", "range=full", false))) {
                FreeBundle(bundle);
                return false;
            }
        }
        if (!add_filter("hwupload", "hwupload", nullptr, true)) {
            FreeBundle(bundle);
            return false;
        }
        std::string processing_args;
        const char* processing_filter = nullptr;
        if (mode == VideoMode::kIvtc) {
            processing_filter = "ivtc_opencl";
            processing_args = std::string("tff=") + (key.top_field_first ? "1" : "0");
        } else {
            processing_filter = "bwdif_opencl";
            processing_args = std::string("mode=send_field:parity=") + parity + ":deint=all";
        }
        const std::string download_format_args =
                std::string("pix_fmts=") + PixelFormatName(selected_format);
        if (!add_filter(processing_filter, "process", processing_args.c_str(), true) ||
            !add_filter("hwdownload", "hwdownload", nullptr, false) ||
            !add_filter("format", "format_download", download_format_args.c_str(), false) ||
            (full_range && selected_format != AV_PIX_FMT_YUVJ420P &&
             !add_filter("setparams", "range_download", "range=full", false))) {
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
    if (result < 0) {
        if (error) *error = "buffersink: " + AvError(result);
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "%s buffersink init stage failed: %s (%d)",
                            opencl ? "OpenCL" : "software", AvError(result).c_str(), result);
        FreeBundle(bundle);
        return false;
    }
    result = LinkFilters(previous, bundle->sink);
    if (result < 0) {
        if (error) *error = "link buffersink: " + AvError(result);
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "%s link buffersink stage failed: %s (%d)",
                            opencl ? "OpenCL" : "software", AvError(result).c_str(), result);
        FreeBundle(bundle);
        return false;
    }
    result = avfilter_graph_config(bundle->graph, nullptr);
    if (result < 0) {
        if (error) *error = "graph config: " + AvError(result);
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "%s graph-config stage failed: %s (%d)",
                            opencl ? "OpenCL" : "software", AvError(result).c_str(), result);
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

}  // namespace

bool FilterGraph::BuildBundle(const AVFrame& frame, VideoMode mode, bool opencl,
                              GraphBundle* bundle, std::string* error) {
    if (opencl) {
        if (error) *error = "OpenCL graph was not prepared before decode";
        return false;
    }
    OpenClProbeSpec spec;
    spec.key = MakeGraphKey(frame);
    spec.field_order = field_order_;
    return BuildBundleForSpec(spec, mode, false, nullptr, bundle, error);
}

namespace {

void RunOpenClProbe(const std::shared_ptr<OpenClProbeControl>& control,
                    const OpenClProbeSpec& spec) {
    AVBufferRef* device = nullptr;
    const auto device_start = std::chrono::steady_clock::now();
    const int device_result = av_hwdevice_ctx_create(
            &device, AV_HWDEVICE_TYPE_OPENCL, nullptr, nullptr, 0);
    const double device_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - device_start).count();
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "OpenCL device-create completed in %.1f ms (result=%d)",
                        device_ms, device_result);

    if (device_result < 0 || device == nullptr) {
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "OpenCL unavailable: result=%d error=%s",
                            device_result, AvError(device_result).c_str());
        {
            std::lock_guard<std::mutex> lock(control->mutex);
            if (control->state == OpenClProbeState::kProbing) {
                control->state = OpenClProbeState::kUnavailable;
                control->ready_modes = 0;
                control->failed_modes = 0;
            }
            control->worker_done = true;
        }
        control->condition.notify_all();
        av_buffer_unref(&device);
        return;
    }

    LogOpenClDeviceInfo(device);
    __android_log_print(
            ANDROID_LOG_INFO, kTag,
            "OpenCL probe format: %dx%d %s(%d)",
            spec.key.width, spec.key.height, PixelFormatName(spec.key.format), spec.key.format);
    AVHWFramesConstraints* constraints = av_hwdevice_get_hwframe_constraints(device, nullptr);
    __android_log_print(
            ANDROID_LOG_INFO, kTag,
            "OpenCL valid_sw_formats: %s min=%dx%d max=%dx%d",
            constraints != nullptr ? PixelFormatList(constraints->valid_sw_formats).c_str() : "unavailable",
            constraints != nullptr ? constraints->min_width : 0,
            constraints != nullptr ? constraints->min_height : 0,
            constraints != nullptr ? constraints->max_width : 0,
            constraints != nullptr ? constraints->max_height : 0);
    const int selected_format = SelectOpenClSoftwareFormat(
            spec.key.format, constraints);
    av_hwframe_constraints_free(&constraints);
    if (selected_format == AV_PIX_FMT_NONE) {
        __android_log_print(
                ANDROID_LOG_ERROR, kTag,
                "OpenCL format selection failed: probe=%s(%d) no compatible software format",
                PixelFormatName(spec.key.format), spec.key.format);
        {
            std::lock_guard<std::mutex> lock(control->mutex);
            if (control->state == OpenClProbeState::kProbing) {
                control->key = spec.key;
                control->ready_modes = 0;
                control->failed_modes = OpenClModeBit(VideoMode::kIvtc) |
                                        OpenClModeBit(VideoMode::kDeinterlace);
                control->state = OpenClProbeState::kReady;
            }
            control->worker_done = true;
        }
        control->condition.notify_all();
        av_buffer_unref(&device);
        return;
    }
    __android_log_print(
            ANDROID_LOG_INFO, kTag,
            "OpenCL selected software format: %s(%d)",
            PixelFormatName(selected_format), selected_format);

    OpenClProbeSpec build_spec = spec;
    build_spec.opencl_sw_format = selected_format;
    FilterGraph::GraphBundle ivtc;
    FilterGraph::GraphBundle deinterlace;
    unsigned ready_modes = 0;
    unsigned failed_modes = 0;
    for (const VideoMode mode : {VideoMode::kIvtc, VideoMode::kDeinterlace}) {
        FilterGraph::GraphBundle bundle;
        std::string error;
        const auto build_start = std::chrono::steady_clock::now();
        const bool ok = BuildBundleForSpec(build_spec, mode, true, device, &bundle, &error);
        const double build_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - build_start).count();
        const char* mode_name = mode == VideoMode::kIvtc ? "ivtc" : "bwdif";
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "OpenCL %s-build completed in %.1f ms%s",
                            mode_name, build_ms, ok ? "" : " (failed)");
        if (!ok) {
            __android_log_print(ANDROID_LOG_INFO, kTag,
                                "OpenCL %s filter probe failed: %s",
                                mode_name, error.c_str());
            FreeBundle(&bundle);
            failed_modes |= OpenClModeBit(mode);
        } else {
            ready_modes |= OpenClModeBit(mode);
            if (mode == VideoMode::kIvtc) MoveBundle(&ivtc, &bundle);
            else MoveBundle(&deinterlace, &bundle);
        }
    }

    {
        std::lock_guard<std::mutex> lock(control->mutex);
        if (control->state != OpenClProbeState::kProbing) {
            FreeBundle(&ivtc);
            FreeBundle(&deinterlace);
        } else {
            control->key = spec.key;
            control->ready_modes = ready_modes;
            control->failed_modes = failed_modes;
            MoveBundle(&control->ivtc, &ivtc);
            MoveBundle(&control->deinterlace, &deinterlace);
            control->state = OpenClProbeState::kReady;
        }
        control->worker_done = true;
    }
    control->condition.notify_all();
    av_buffer_unref(&device);
}

}  // namespace

void FilterGraph::StartOpenClProbe(const AVFrame& prototype, AVRational time_base,
                                   AVRational frame_rate, AVRational stream_sar,
                                   AVFieldOrder field_order) {
    OpenClGraphKey key;
    key.width = prototype.width;
    key.height = prototype.height;
    key.format = prototype.format;
    key.colorspace = prototype.colorspace;
    key.color_range = prototype.color_range;
    if (key.format == AV_PIX_FMT_YUVJ420P && key.color_range == AVCOL_RANGE_UNSPECIFIED) {
        key.color_range = AVCOL_RANGE_JPEG;
    }
    key.sample_aspect_ratio = prototype.sample_aspect_ratio.num > 0 &&
                              prototype.sample_aspect_ratio.den > 0
            ? prototype.sample_aspect_ratio : stream_sar;
    key.time_base = time_base;
    key.frame_rate = frame_rate;
    if ((prototype.flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) != 0) {
        key.top_field_first = true;
    } else if ((prototype.flags & AV_FRAME_FLAG_INTERLACED) != 0) {
        key.top_field_first = false;
    } else {
        switch (field_order) {
            case AV_FIELD_BB:
            case AV_FIELD_BT:
                key.top_field_first = false;
                break;
            default:
                key.top_field_first = true;
                break;
        }
    }
    key.valid = key.width > 0 && key.height > 0 && key.format >= 0 &&
                key.sample_aspect_ratio.num > 0 && key.sample_aspect_ratio.den > 0 &&
                key.time_base.num > 0 && key.time_base.den > 0 &&
                key.frame_rate.num > 0 && key.frame_rate.den > 0;
    if (!key.valid) return;
    OpenClProbeSpec spec;
    spec.key = key;
    spec.field_order = field_order;
    OpenClProbeCoordinator::Instance().StartIfUnknown(spec);
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
