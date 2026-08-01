#include "audio/AudioDecoder.h"

#include <android/log.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
}

namespace aribplayer {
namespace {
constexpr char kTag[] = "aribplayer-audio-decode";

double FramePtsSeconds(const AVFrame& frame, AVRational time_base,
                       double fallback) {
    const int64_t timestamp = frame.best_effort_timestamp != AV_NOPTS_VALUE
                                  ? frame.best_effort_timestamp
                                  : frame.pts;
    return timestamp == AV_NOPTS_VALUE ? fallback : av_q2d(time_base) * timestamp;
}

}  // namespace

AudioDecoder::AudioDecoder(AVCodecContext* decoder, AVRational stream_time_base,
                           PacketQueue& packets, PcmRing& pcm_ring,
                           std::function<void()> on_dual_mono_detected)
    : decoder_(decoder),
      time_base_(stream_time_base),
      packets_(packets),
      pcm_ring_(pcm_ring),
      output_sample_rate_(pcm_ring.sample_rate()),
      on_dual_mono_detected_(std::move(on_dual_mono_detected)) {}

AudioDecoder::~AudioDecoder() {
    Stop();
    swr_free(&resampler_);
    av_channel_layout_uninit(&input_layout_);
    avcodec_free_context(&decoder_);
}

void AudioDecoder::Start() {
    if (decoder_ == nullptr || running_.exchange(true, std::memory_order_acq_rel)) {
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
    packets_.Reset();
    thread_ = std::thread(&AudioDecoder::DecodeLoop, this);
}

bool AudioDecoder::Stop(int64_t timeout_ms) {
    stop_requested_.store(true, std::memory_order_release);
    packets_.Abort();
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
                            "teardown: detaching audio decoder after %lldms timeout",
                            static_cast<long long>(timeout_ms));
        return false;
    }
    thread_.join();
    running_.store(false, std::memory_order_release);
    return true;
}

void AudioDecoder::FinishThread() {
    running_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(thread_mutex_);
        thread_finished_ = true;
    }
    thread_cv_.notify_all();
}

void AudioDecoder::Flush(int serial) {
    desired_serial_.store(serial, std::memory_order_release);
    requested_flush_serial_.store(serial, std::memory_order_release);
    packets_.Flush();
}

void AudioDecoder::FlushForSeek(int serial, int64_t target_us) {
    requested_discard_before_us_.store(target_us, std::memory_order_relaxed);
    requested_discard_serial_.store(serial, std::memory_order_release);
    Flush(serial);
}

void AudioDecoder::ApplyFlush(int serial) {
    avcodec_flush_buffers(decoder_);
    swr_free(&resampler_);
    av_channel_layout_uninit(&input_layout_);
    input_rate_ = 0;
    input_format_ = AV_SAMPLE_FMT_NONE;
    dual_mono_packet_seen_ = false;
    active_serial_ = serial;
    desired_serial_.store(serial, std::memory_order_release);
    next_pts_seconds_ = 0.0;
    if (requested_discard_serial_.load(std::memory_order_acquire) == serial) {
        discard_before_timestamp_ = av_rescale_q(
                requested_discard_before_us_.load(std::memory_order_relaxed),
                AV_TIME_BASE_Q, time_base_);
    } else {
        discard_before_timestamp_ = AV_NOPTS_VALUE;
    }
}

void AudioDecoder::DecodeLoop() {
    AVFrame* frame = av_frame_alloc();
    if (frame == nullptr) {
        last_error_.store(AVERROR(ENOMEM), std::memory_order_release);
        FinishThread();
        return;
    }

    while (!stop_requested_.load(std::memory_order_acquire)) {
        const int pending_serial = requested_flush_serial_.exchange(-1, std::memory_order_acq_rel);
        if (pending_serial >= 0) {
            ApplyFlush(pending_serial);
        }

        PacketItem item;
        const QueueResult queue_result = packets_.Get(&item, true);
        if (queue_result == QueueResult::kFlushed) {
            continue;
        }
        if (queue_result != QueueResult::kOk) {
            break;
        }
        if (active_serial_ != item.serial) {
            ApplyFlush(item.serial);
            int expected = item.serial;
            requested_flush_serial_.compare_exchange_strong(
                    expected, -1, std::memory_order_acq_rel);
        }

        if (decoder_->codec_id == AV_CODEC_ID_AAC && !item.end_of_stream &&
            item.packet != nullptr) {
            size_t side_data_size = 0;
            const uint8_t* dual_mono = av_packet_get_side_data(
                    item.packet, AV_PKT_DATA_JP_DUALMONO, &side_data_size);
            if (dual_mono != nullptr && side_data_size >= 1 && dual_mono[0] <= 2) {
                dual_mono_packet_seen_ = true;
            }
        }

        int result;
        do {
            result = avcodec_send_packet(decoder_, item.end_of_stream ? nullptr : item.packet);
            if (result == AVERROR(EAGAIN)) {
                if (!DrainDecoder(active_serial_)) {
                    break;
                }
            }
        } while (result == AVERROR(EAGAIN) && !stop_requested_.load(std::memory_order_acquire));

        if (result < 0 && result != AVERROR_EOF) {
            last_error_.store(result, std::memory_order_release);
            continue;
        }
        if (!DrainDecoder(active_serial_)) {
            break;
        }
        if (item.end_of_stream && !DrainResampler(active_serial_)) {
            break;
        }
        av_frame_unref(frame);
    }

    av_frame_free(&frame);
    FinishThread();
}

bool AudioDecoder::DrainDecoder(int serial) {
    AVFrame* frame = av_frame_alloc();
    if (frame == nullptr) {
        last_error_.store(AVERROR(ENOMEM), std::memory_order_release);
        return false;
    }

    bool keep_running = true;
    while (!stop_requested_.load(std::memory_order_acquire)) {
        const int result = avcodec_receive_frame(decoder_, frame);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
            break;
        }
        if (result < 0) {
            last_error_.store(result, std::memory_order_release);
            break;
        }
        const int64_t frame_timestamp = frame->best_effort_timestamp != AV_NOPTS_VALUE
                ? frame->best_effort_timestamp : frame->pts;
        if (discard_before_timestamp_ != AV_NOPTS_VALUE) {
            if (frame_timestamp == AV_NOPTS_VALUE ||
                frame_timestamp < discard_before_timestamp_) {
                av_frame_unref(frame);
                continue;
            }
            discard_before_timestamp_ = AV_NOPTS_VALUE;
            int expected_serial = serial;
            requested_discard_serial_.compare_exchange_strong(
                    expected_serial, -1, std::memory_order_acq_rel);
        }
        if (dual_mono_packet_seen_ && !dual_mono_reported_) {
            dual_mono_reported_ = true;
            if (on_dual_mono_detected_) on_dual_mono_detected_();
        }
        if (!ConvertFrame(*frame, serial)) {
            keep_running = false;
            break;
        }
        av_frame_unref(frame);
    }
    av_frame_free(&frame);
    (void)serial;
    return keep_running;
}

bool AudioDecoder::ConfigureResampler(const AVFrame& frame, int serial) {
    AVChannelLayout frame_layout{};
    if (frame.ch_layout.nb_channels > 0) {
        if (av_channel_layout_copy(&frame_layout, &frame.ch_layout) < 0) {
            return false;
        }
    } else {
        av_channel_layout_default(&frame_layout,
                                  std::max(decoder_->ch_layout.nb_channels, 1));
    }

    const AVSampleFormat frame_format = static_cast<AVSampleFormat>(frame.format);
    const bool unchanged = resampler_ != nullptr && input_rate_ == frame.sample_rate &&
                           input_format_ == frame_format &&
                           av_channel_layout_compare(&input_layout_, &frame_layout) == 0;
    if (unchanged) {
        av_channel_layout_uninit(&frame_layout);
        return true;
    }

    SwrContext* replacement = nullptr;
    AVChannelLayout stereo{};
    av_channel_layout_default(&stereo, 2);
    const int configure_result = swr_alloc_set_opts2(
        &replacement, &stereo, AV_SAMPLE_FMT_S16, output_sample_rate_, &frame_layout,
        frame_format, frame.sample_rate, 0, nullptr);
    av_channel_layout_uninit(&stereo);
    if (configure_result < 0 || replacement == nullptr || swr_init(replacement) < 0) {
        swr_free(&replacement);
        av_channel_layout_uninit(&frame_layout);
        return false;
    }

    if (resampler_ != nullptr && !DrainResampler(serial)) {
        swr_free(&replacement);
        av_channel_layout_uninit(&frame_layout);
        return false;
    }
    swr_free(&resampler_);
    av_channel_layout_uninit(&input_layout_);
    resampler_ = replacement;
    input_layout_ = frame_layout;
    input_rate_ = frame.sample_rate;
    input_format_ = frame_format;
    return true;
}

bool AudioDecoder::DrainResampler(int serial) {
    if (resampler_ == nullptr) {
        return true;
    }
    while (!stop_requested_.load(std::memory_order_acquire) &&
           desired_serial_.load(std::memory_order_acquire) == serial) {
        const int capacity = swr_get_out_samples(resampler_, 0);
        if (capacity <= 0) {
            break;
        }
        std::vector<int16_t> output(static_cast<std::size_t>(capacity) * 2);
        uint8_t* output_data[] = {reinterpret_cast<uint8_t*>(output.data())};
        const int converted = swr_convert(resampler_, output_data, capacity, nullptr, 0);
        if (converted < 0) {
            last_error_.store(converted, std::memory_order_release);
            return false;
        }
        if (converted == 0) {
            break;
        }
        if (!WritePcm(output.data(), converted, next_pts_seconds_, serial)) {
            return false;
        }
        next_pts_seconds_ += static_cast<double>(converted) / output_sample_rate_;
    }
    return !stop_requested_.load(std::memory_order_acquire);
}

bool AudioDecoder::ConvertFrame(const AVFrame& frame, int serial) {
    if (frame.nb_samples <= 0 || frame.sample_rate <= 0 ||
        !ConfigureResampler(frame, serial)) {
        last_error_.store(AVERROR(EINVAL), std::memory_order_release);
        return false;
    }

    const int capacity = swr_get_out_samples(resampler_, frame.nb_samples);
    if (capacity <= 0) {
        return true;
    }
    std::vector<int16_t> output(static_cast<std::size_t>(capacity) * 2);
    uint8_t* output_data[] = {reinterpret_cast<uint8_t*>(output.data())};
    const int converted = swr_convert(resampler_, output_data, capacity,
                                      const_cast<const uint8_t**>(frame.extended_data),
                                      frame.nb_samples);
    if (converted < 0) {
        last_error_.store(converted, std::memory_order_release);
        return false;
    }

    const double pts = FramePtsSeconds(frame, time_base_, next_pts_seconds_);
    next_pts_seconds_ = pts + static_cast<double>(converted) / output_sample_rate_;
    return WritePcm(output.data(), converted, pts, serial);
}

bool AudioDecoder::WritePcm(const int16_t* samples, int frames, double pts_seconds,
                            int serial) {
    int written = 0;
    while (written < frames && !stop_requested_.load(std::memory_order_acquire) &&
           desired_serial_.load(std::memory_order_acquire) == serial) {
        const std::size_t count = pcm_ring_.Write(
            samples + static_cast<std::size_t>(written) * 2,
            static_cast<std::size_t>(frames - written),
            pts_seconds + static_cast<double>(written) / output_sample_rate_);
        written += static_cast<int>(count);
        if (count == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    return !stop_requested_.load(std::memory_order_acquire);
}

}  // namespace aribplayer
