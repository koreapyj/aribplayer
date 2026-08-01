#include "video/VideoDecoder.h"

#include <android/log.h>

#include <cmath>
#include <utility>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
}

namespace aribplayer {
namespace {
constexpr char kTag[] = "aribplayer-decoder";

std::string AvError(int error) {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(error, text, sizeof(text));
    return text;
}

const AVCodec* FindSoftwareCodec(AVCodecID codec_id) {
    switch (codec_id) {
        case AV_CODEC_ID_H264:
            if (const AVCodec* codec = avcodec_find_decoder_by_name("h264")) return codec;
            break;
        case AV_CODEC_ID_MPEG2VIDEO:
            if (const AVCodec* codec = avcodec_find_decoder_by_name("mpeg2video")) return codec;
            break;
        default:
            break;
    }
    return avcodec_find_decoder(codec_id);
}

}  // namespace

VideoDecoder::VideoDecoder(AVCodecContext* decoder, bool decoder_is_hardware,
                           const AVCodecParameters* codec_parameters,
                           AVRational stream_time_base, AVRational frame_rate,
                           AVRational stream_sar, PacketQueue& packets, FrameQueue& frames,
                           DecoderInfoCallback decoder_info,
                           FilterInfoCallback filter_info,
                           FallbackSeekCallback fallback_seek,
                           VideoSizeCallback video_size)
    : decoder_(decoder),
      time_base_(stream_time_base),
      frame_rate_(frame_rate),
      stream_sar_(stream_sar),
      packets_(packets),
      frames_(frames),
      decoder_info_callback_(std::move(decoder_info)),
      fallback_seek_callback_(std::move(fallback_seek)),
      video_size_callback_(std::move(video_size)),
      decoder_name_(decoder != nullptr && decoder->codec != nullptr ? decoder->codec->name : "unknown"),
      decoder_is_hardware_(decoder_is_hardware) {
    if (decoder_ != nullptr) {
        default_skip_loop_filter_ = decoder_->skip_loop_filter;
        default_skip_frame_ = decoder_->skip_frame;
    }
    codec_parameters_ = avcodec_parameters_alloc();
    if (codec_parameters_ == nullptr || codec_parameters == nullptr ||
        avcodec_parameters_copy(codec_parameters_, codec_parameters) < 0) {
        avcodec_parameters_free(&codec_parameters_);
    }
    const AVFieldOrder field_order = codec_parameters != nullptr
            ? codec_parameters->field_order : AV_FIELD_UNKNOWN;
    filter_ = std::make_unique<FilterGraph>(
            time_base_, frame_rate_, stream_sar_, field_order, frames_, std::move(filter_info));
}

VideoDecoder::~VideoDecoder() {
    Stop();
    filter_.reset();
    avcodec_free_context(&decoder_);
    avcodec_parameters_free(&codec_parameters_);
}

void VideoDecoder::Prepare() {
    if (filter_ == nullptr || decoder_ == nullptr || codec_parameters_ == nullptr) return;

    AVFrame* prototype = av_frame_alloc();
    if (prototype == nullptr) return;
    prototype->width = decoder_->width > 0 ? decoder_->width : codec_parameters_->width;
    prototype->height = decoder_->height > 0 ? decoder_->height : codec_parameters_->height;
    const int decoder_format = static_cast<int>(decoder_->pix_fmt);
    prototype->format = IsRenderableFormat(decoder_format)
            ? decoder_format
            : (IsRenderableFormat(codec_parameters_->format)
                    ? codec_parameters_->format : AV_PIX_FMT_YUV420P);
    prototype->sample_aspect_ratio = decoder_->sample_aspect_ratio.num > 0 &&
                                     decoder_->sample_aspect_ratio.den > 0
            ? decoder_->sample_aspect_ratio : stream_sar_;
    prototype->colorspace = decoder_->colorspace != AVCOL_SPC_UNSPECIFIED
            ? decoder_->colorspace : codec_parameters_->color_space;
    prototype->color_range = decoder_->color_range != AVCOL_RANGE_UNSPECIFIED
            ? decoder_->color_range : codec_parameters_->color_range;
    switch (codec_parameters_->field_order) {
        case AV_FIELD_TT:
        case AV_FIELD_TB:
            prototype->flags |= AV_FRAME_FLAG_INTERLACED | AV_FRAME_FLAG_TOP_FIELD_FIRST;
            break;
        case AV_FIELD_BB:
        case AV_FIELD_BT:
            prototype->flags |= AV_FRAME_FLAG_INTERLACED;
            break;
        default:
            break;
    }

    if (prototype->width > 0 && prototype->height > 0) {
        filter_->SetMode(static_cast<VideoMode>(requested_mode_.load(std::memory_order_acquire)));
        filter_->Prepare(*prototype);
    }
    av_frame_free(&prototype);
}

void VideoDecoder::Start() {
    if (decoder_ == nullptr || codec_parameters_ == nullptr ||
        running_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(thread_mutex_);
        if (thread_abandoned_) {
            running_.store(false, std::memory_order_release);
            return;
        }
        thread_finished_ = false;
    }
    stop_requested_.store(false, std::memory_order_release);
    eof_drained_.store(false, std::memory_order_release);
    packets_.Reset();
    frames_.Reset();
    if (filter_) {
        filter_->SetMode(static_cast<VideoMode>(requested_mode_.load(std::memory_order_acquire)));
        filter_->Reset();
    }
    ReportDecoderInfo();
    thread_ = std::thread(&VideoDecoder::DecodeLoop, this);
}

bool VideoDecoder::Stop(int64_t timeout_ms) {
    stop_requested_.store(true, std::memory_order_release);
    packets_.Abort();
    frames_.Abort();
    if (!thread_.joinable()) {
        std::lock_guard<std::mutex> lock(thread_mutex_);
        return !thread_abandoned_;
    }

    std::unique_lock<std::mutex> lock(thread_mutex_);
    const bool finished = thread_cv_.wait_for(
            lock, std::chrono::milliseconds(std::max<int64_t>(0, timeout_ms)),
            [this] { return thread_finished_; });
    lock.unlock();
    if (!finished) {
        thread_.detach();
        std::lock_guard<std::mutex> abandoned_lock(thread_mutex_);
        thread_abandoned_ = true;
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "teardown: detaching video decoder after %lldms timeout",
                            static_cast<long long>(timeout_ms));
        return false;
    }
    thread_.join();
    running_.store(false, std::memory_order_release);
    return true;
}

void VideoDecoder::FinishThread() {
    running_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(thread_mutex_);
        thread_finished_ = true;
    }
    thread_cv_.notify_all();
}

void VideoDecoder::Flush(int serial) {
    requested_flush_serial_.store(serial, std::memory_order_release);
    packets_.Flush();
    frames_.Flush();
}

void VideoDecoder::FlushForSeek(int serial, int64_t target_us) {
    requested_catchup_target_us_.store(target_us, std::memory_order_relaxed);
    requested_catchup_serial_.store(serial, std::memory_order_release);
    Flush(serial);
}

void VideoDecoder::SetVideoMode(VideoMode mode) {
    requested_mode_.store(static_cast<int>(mode), std::memory_order_release);
}

VideoDecoderStats VideoDecoder::GetStats() const {
    VideoDecoderStats stats;
    {
        std::lock_guard<std::mutex> lock(info_mutex_);
        stats.decoder = decoder_name_;
        stats.decoder_hw = decoder_is_hardware_;
    }
    if (filter_) stats.filter = filter_->GetStats();
    return stats;
}

void VideoDecoder::ApplyFlush(int serial) {
    SetCatchUpOptions(false);
    if (decoder_ != nullptr) avcodec_flush_buffers(decoder_);
    if (filter_) {
        filter_->SetMode(static_cast<VideoMode>(requested_mode_.load(std::memory_order_acquire)));
        filter_->Reset();
    }
    active_serial_ = serial;
    waiting_for_new_serial_ = false;
    eof_drained_.store(false, std::memory_order_release);

    const int catchup_serial = requested_catchup_serial_.load(std::memory_order_acquire);
    if (catchup_serial == serial) {
        catchup_target_us_ = requested_catchup_target_us_.load(std::memory_order_relaxed);
        catchup_target_timestamp_ = av_rescale_q(
                catchup_target_us_, AV_TIME_BASE_Q, time_base_);
        landed_keyframe_timestamp_ = AV_NOPTS_VALUE;
        catchup_frames_ = 0;
        catchup_started_ = std::chrono::steady_clock::now();
        catchup_active_ = catchup_target_us_ != AV_NOPTS_VALUE;
        SetCatchUpOptions(catchup_active_);
    } else {
        catchup_active_ = false;
        catchup_target_us_ = AV_NOPTS_VALUE;
        catchup_target_timestamp_ = AV_NOPTS_VALUE;
        landed_keyframe_timestamp_ = AV_NOPTS_VALUE;
        catchup_frames_ = 0;
    }
}

void VideoDecoder::SetCatchUpOptions(bool enabled) {
    if (decoder_ == nullptr) return;
    decoder_->skip_loop_filter = enabled ? AVDISCARD_ALL : default_skip_loop_filter_;
    decoder_->skip_frame = enabled ? AVDISCARD_NONREF : default_skip_frame_;
}

void VideoDecoder::FinishCatchUp(int64_t reached_timestamp) {
    if (!catchup_active_) return;
    catchup_active_ = false;
    SetCatchUpOptions(false);
    int expected_serial = active_serial_;
    requested_catchup_serial_.compare_exchange_strong(
            expected_serial, -1, std::memory_order_acq_rel);

    const auto elapsed = std::chrono::steady_clock::now() - catchup_started_;
    const int64_t decode_ms = std::max<int64_t>(0, std::chrono::duration_cast<
            std::chrono::milliseconds>(elapsed).count());
    const int64_t landed_timestamp = landed_keyframe_timestamp_ != AV_NOPTS_VALUE
            ? landed_keyframe_timestamp_ : reached_timestamp;
    const int64_t landed_ms = landed_timestamp == AV_NOPTS_VALUE ? -1
            : av_rescale_q(landed_timestamp, time_base_, AVRational{1, 1000});
    __android_log_print(
            ANDROID_LOG_INFO, kTag,
            "seek: target=%lldms landed_keyframe=%lldms catchup_frames=%d decode_ms=%lld",
            static_cast<long long>(catchup_target_us_ / 1000),
            static_cast<long long>(landed_ms), catchup_frames_,
            static_cast<long long>(decode_ms));
}

void VideoDecoder::DecodeLoop() {
    while (!stop_requested_.load(std::memory_order_acquire)) {
        const int pending_serial = requested_flush_serial_.exchange(-1, std::memory_order_acq_rel);
        if (pending_serial >= 0) ApplyFlush(pending_serial);

        PacketItem item;
        const QueueResult queue_result = packets_.Get(&item, true);
        if (queue_result == QueueResult::kFlushed) continue;
        if (queue_result != QueueResult::kOk) break;
        if (active_serial_ != item.serial) {
            ApplyFlush(item.serial);
            int expected = item.serial;
            requested_flush_serial_.compare_exchange_strong(
                    expected, -1, std::memory_order_acq_rel);
        }
        if (waiting_for_new_serial_ && item.serial == active_serial_) continue;
        if (!SendPacket(item.end_of_stream ? nullptr : item.packet, active_serial_)) break;
    }
    FinishThread();
}

bool VideoDecoder::SendPacket(const AVPacket* packet, int serial) {
    if (packet != nullptr) eof_drained_.store(false, std::memory_order_release);
    while (!stop_requested_.load(std::memory_order_acquire)) {
        const int result = avcodec_send_packet(decoder_, packet);
        if (result == AVERROR(EAGAIN)) {
            if (!DrainDecoder(serial, false)) return false;
            continue;
        }
        if (result < 0 && result != AVERROR_EOF) {
            last_error_.store(result, std::memory_order_release);
            if (decoder_is_hardware_) return FallBackToSoftware(0.0);
            return true;  // Bad software packet: keep decoding the stream.
        }
        const bool drained = DrainDecoder(serial, packet == nullptr);
        if (drained && packet == nullptr) eof_drained_.store(true, std::memory_order_release);
        return drained;
    }
    return false;
}

bool VideoDecoder::DrainDecoder(int serial, bool end_of_stream) {
    AVFrame* frame = av_frame_alloc();
    if (frame == nullptr) {
        last_error_.store(AVERROR(ENOMEM), std::memory_order_release);
        return false;
    }

    bool keep_running = true;
    while (!stop_requested_.load(std::memory_order_acquire)) {
        const int result = avcodec_receive_frame(decoder_, frame);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
        if (result < 0) {
            last_error_.store(result, std::memory_order_release);
            if (decoder_is_hardware_ && !received_hardware_frame_) {
                av_frame_unref(frame);
                keep_running = FallBackToSoftware(0.0);
            }
            break;
        }

        if (frame->pts == AV_NOPTS_VALUE && frame->best_effort_timestamp != AV_NOPTS_VALUE) {
            frame->pts = frame->best_effort_timestamp;
        }
        const int64_t frame_timestamp = frame->best_effort_timestamp != AV_NOPTS_VALUE
                ? frame->best_effort_timestamp : frame->pts;
        const double pts = FramePtsSeconds(*frame, time_base_);
        if (catchup_active_) {
            if (landed_keyframe_timestamp_ == AV_NOPTS_VALUE &&
                frame_timestamp != AV_NOPTS_VALUE) {
                landed_keyframe_timestamp_ = frame_timestamp;
            }
            if (frame_timestamp == AV_NOPTS_VALUE ||
                frame_timestamp < catchup_target_timestamp_) {
                ++catchup_frames_;
                av_frame_unref(frame);
                continue;
            }
            FinishCatchUp(frame_timestamp);
        }
        if (!IsRenderableFormat(frame->format)) {
            last_error_.store(AVERROR(ENOSYS), std::memory_order_release);
            if (decoder_is_hardware_) {
                av_frame_unref(frame);
                keep_running = FallBackToSoftware(pts);
                break;
            }
            __android_log_print(ANDROID_LOG_ERROR, kTag, "Unsupported software output format: %s",
                                av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format)));
            av_frame_unref(frame);
            continue;
        }
        if (decoder_is_hardware_) received_hardware_frame_ = true;

        AVRational sar = frame->sample_aspect_ratio.num > 0 && frame->sample_aspect_ratio.den > 0
                ? frame->sample_aspect_ratio : stream_sar_;
        if (sar.num <= 0 || sar.den <= 0) sar = AVRational{1, 1};
        if (frame->width != last_width_ || frame->height != last_height_ ||
            av_cmp_q(sar, last_sar_) != 0) {
            last_width_ = frame->width;
            last_height_ = frame->height;
            last_sar_ = sar;
            if (video_size_callback_) video_size_callback_(frame->width, frame->height, sar);
        }

        if (filter_ == nullptr || !filter_->Process(frame, serial)) {
            keep_running = false;
            av_frame_unref(frame);
            break;
        }
        av_frame_unref(frame);
    }
    if (keep_running && end_of_stream && catchup_active_) {
        FinishCatchUp(AV_NOPTS_VALUE);
    }
    if (keep_running && end_of_stream && filter_) {
        keep_running = filter_->EndOfStream(serial);
        if (keep_running) {
            keep_running = frames_.PushEof(serial, true) == QueueResult::kOk;
        }
    }
    av_frame_free(&frame);
    return keep_running;
}

bool VideoDecoder::FallBackToSoftware(double position_seconds) {
    if (!decoder_is_hardware_ || codec_parameters_ == nullptr) return false;
    std::string error;
    AVCodecContext* software = OpenSoftwareDecoder(&error);
    if (software == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "Software fallback failed: %s", error.c_str());
        return false;
    }

    avcodec_free_context(&decoder_);
    decoder_ = software;
    default_skip_loop_filter_ = decoder_->skip_loop_filter;
    default_skip_frame_ = decoder_->skip_frame;
    {
        std::lock_guard<std::mutex> lock(info_mutex_);
        decoder_is_hardware_ = false;
        decoder_name_ = decoder_->codec != nullptr ? decoder_->codec->name : "software";
    }
    received_hardware_frame_ = false;
    if (filter_) filter_->Reset();
    ReportDecoderInfo();

    const double safe_position = std::isfinite(position_seconds) && position_seconds > 0.0
            ? position_seconds : 0.0;
    waiting_for_new_serial_ = fallback_seek_callback_ && fallback_seek_callback_(safe_position);
    return true;
}

AVCodecContext* VideoDecoder::OpenSoftwareDecoder(std::string* error) const {
    if (codec_parameters_ == nullptr) return nullptr;
    const AVCodec* codec = FindSoftwareCodec(codec_parameters_->codec_id);
    if (codec == nullptr) {
        if (error) *error = "software decoder not found";
        return nullptr;
    }
    AVCodecContext* context = avcodec_alloc_context3(codec);
    if (context == nullptr) {
        if (error) *error = "decoder allocation failed";
        return nullptr;
    }
    int result = avcodec_parameters_to_context(context, codec_parameters_);
    if (result >= 0) {
        context->pkt_timebase = time_base_;
        result = avcodec_open2(context, codec, nullptr);
    }
    if (result < 0) {
        if (error) *error = std::string(codec->name) + ": " + AvError(result);
        avcodec_free_context(&context);
        return nullptr;
    }
    return context;
}

void VideoDecoder::ReportDecoderInfo() {
    std::string name;
    bool hardware = false;
    {
        std::lock_guard<std::mutex> lock(info_mutex_);
        name = decoder_name_;
        hardware = decoder_is_hardware_;
    }
    if (decoder_info_callback_) decoder_info_callback_(name, hardware);
}

bool VideoDecoder::IsRenderableFormat(int format) {
    return format == AV_PIX_FMT_NV12 || format == AV_PIX_FMT_YUV420P ||
           format == AV_PIX_FMT_YUVJ420P;
}

double VideoDecoder::FramePtsSeconds(const AVFrame& frame, AVRational time_base) {
    const int64_t timestamp = frame.best_effort_timestamp != AV_NOPTS_VALUE
            ? frame.best_effort_timestamp : frame.pts;
    return timestamp == AV_NOPTS_VALUE ? 0.0 : timestamp * av_q2d(time_base);
}

}  // namespace aribplayer
