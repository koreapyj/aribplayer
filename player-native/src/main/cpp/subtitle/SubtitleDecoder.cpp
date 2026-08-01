#include "subtitle/SubtitleDecoder.h"

#include <android/log.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixfmt.h>
}

namespace aribplayer {
namespace {
constexpr char kTag[] = "aribplayer-subtitle";
constexpr int64_t kMicrosecondsPerMillisecond = 1000;

std::string AvError(int error) {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(error, text, sizeof(text));
    return text;
}

int64_t SaturatingAdd(int64_t left, int64_t right) {
    if (right > 0 && left > std::numeric_limits<int64_t>::max() - right) {
        return std::numeric_limits<int64_t>::max();
    }
    return left + right;
}
}  // namespace

SubtitleDecoder::SubtitleDecoder(AVCodecContext* decoder, PacketQueue& packets,
                                 SubtitleQueue& events, SubtitleSource source,
                                 int canvas_width, int canvas_height)
        : decoder_(decoder),
          packets_(packets),
          events_(events),
          source_(source),
          canvas_width_(std::max(1, canvas_width)),
          canvas_height_(std::max(1, canvas_height)) {}

SubtitleDecoder::~SubtitleDecoder() {
    Stop();
    avcodec_free_context(&decoder_);
}

void SubtitleDecoder::Start() {
    if (decoder_ == nullptr || running_.exchange(true, std::memory_order_acq_rel)) return;
    {
        std::lock_guard<std::mutex> lock(thread_mutex_);
        if (thread_abandoned_) {
            running_.store(false, std::memory_order_release);
            return;
        }
        thread_finished_ = false;
    }
    stop_requested_.store(false, std::memory_order_release);
    requested_flush_serial_.store(-1, std::memory_order_release);
    last_error_.store(0, std::memory_order_release);
    eof_drained_.store(false, std::memory_order_release);
    active_serial_ = -1;
    packets_.Reset();
    events_.Reset();
    thread_ = std::thread(&SubtitleDecoder::DecodeLoop, this);
}

bool SubtitleDecoder::Stop(int64_t timeout_ms) {
    stop_requested_.store(true, std::memory_order_release);
    packets_.Abort();
    events_.Abort();
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
                            "teardown: detaching subtitle decoder after %lldms timeout",
                            static_cast<long long>(timeout_ms));
        return false;
    }
    thread_.join();
    running_.store(false, std::memory_order_release);
    return true;
}

void SubtitleDecoder::FinishThread() {
    running_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(thread_mutex_);
        thread_finished_ = true;
    }
    thread_cv_.notify_all();
}

void SubtitleDecoder::Flush(int serial) {
    requested_flush_serial_.store(serial, std::memory_order_release);
    packets_.Flush();
    events_.Flush();
}

void SubtitleDecoder::ApplyFlush(int serial) {
    if (decoder_ != nullptr) avcodec_flush_buffers(decoder_);
    active_serial_ = serial;
    eof_drained_.store(false, std::memory_order_release);
}

void SubtitleDecoder::DecodeLoop() {
    while (!stop_requested_.load(std::memory_order_acquire)) {
        const int pending_serial = requested_flush_serial_.exchange(-1, std::memory_order_acq_rel);
        if (pending_serial >= 0) ApplyFlush(pending_serial);

        PacketItem item;
        const QueueResult queue_result = packets_.Get(&item, true);
        if (queue_result == QueueResult::kFlushed) continue;
        if (queue_result != QueueResult::kOk) break;
        const int flush_after_wake =
                requested_flush_serial_.exchange(-1, std::memory_order_acq_rel);
        if (flush_after_wake >= 0) {
            ApplyFlush(flush_after_wake);
            if (item.serial != flush_after_wake) continue;
        }
        if (active_serial_ != item.serial) ApplyFlush(item.serial);
        if (item.end_of_stream) {
            eof_drained_.store(true, std::memory_order_release);
            continue;
        }
        if (item.packet == nullptr) continue;
        eof_drained_.store(false, std::memory_order_release);
        DecodePacket(*item.packet, item.serial);
    }
    FinishThread();
}

void SubtitleDecoder::DecodePacket(const AVPacket& packet, int serial) {
    AVSubtitle subtitle{};
    int got_subtitle = 0;
    const int result = avcodec_decode_subtitle2(decoder_, &subtitle, &got_subtitle, &packet);
    if (result < 0) {
        last_error_.store(result, std::memory_order_release);
        __android_log_print(ANDROID_LOG_WARN, kTag, "Caption packet decode failed: %s",
                            AvError(result).c_str());
        avsubtitle_free(&subtitle);
        return;
    }

    if (got_subtitle != 0) {
        SubtitleEvent event;
        if (BuildEvent(subtitle, packet, serial, &event) &&
            !stop_requested_.load(std::memory_order_acquire) &&
            requested_flush_serial_.load(std::memory_order_acquire) < 0 &&
            serial == active_serial_) {
            const QueueResult queued = events_.Push(std::move(event), true);
            if (queued != QueueResult::kOk && queued != QueueResult::kAborted) {
                __android_log_print(ANDROID_LOG_WARN, kTag,
                                    "Dropping decoded caption event: render queue full");
            }
        }
    }
    avsubtitle_free(&subtitle);
}

bool SubtitleDecoder::BuildEvent(const AVSubtitle& subtitle, const AVPacket& packet,
                                 int serial, SubtitleEvent* event) const {
    if (event == nullptr) return false;
    event->serial = serial;
    event->source = source_;
    event->canvas_width = canvas_width_;
    event->canvas_height = canvas_height_;

    int64_t base_pts_us = subtitle.pts;
    if (base_pts_us == AV_NOPTS_VALUE) {
        base_pts_us = packet.pts == AV_NOPTS_VALUE
                ? 0
                : av_rescale_q(packet.pts, decoder_->pkt_timebase, AV_TIME_BASE_Q);
    }
    // AVSubtitle display offsets are milliseconds relative to subtitle.pts;
    // SubtitleEvent presentation timestamps are microseconds.
    event->start_pts_us = SaturatingAdd(
            base_pts_us, static_cast<int64_t>(subtitle.start_display_time) *
                         kMicrosecondsPerMillisecond);
    if (subtitle.end_display_time == UINT32_MAX) {
        event->end_pts_us = kSubtitleEndIndefinite;
    } else {
        event->end_pts_us = SaturatingAdd(
                base_pts_us, static_cast<int64_t>(subtitle.end_display_time) *
                             kMicrosecondsPerMillisecond);
        if (event->end_pts_us < event->start_pts_us) {
            event->end_pts_us = event->start_pts_us;
        }
    }

    if (subtitle.num_rects == 0) return true;  // Explicit clear event.

    for (unsigned index = 0; index < subtitle.num_rects; ++index) {
        const AVSubtitleRect* source = subtitle.rects == nullptr ? nullptr : subtitle.rects[index];
        if (source == nullptr || source->type != SUBTITLE_BITMAP ||
            source->data[0] == nullptr || source->data[1] == nullptr ||
            source->w <= 0 || source->h <= 0 || source->linesize[0] == 0) {
            continue;
        }

        const int palette_count = std::clamp(source->nb_colors, 0, AVPALETTE_COUNT);
        if (palette_count == 0) continue;
        const int64_t source_right = static_cast<int64_t>(source->x) + source->w;
        const int64_t source_bottom = static_cast<int64_t>(source->y) + source->h;
        const int clipped_left = std::clamp(source->x, 0, canvas_width_);
        const int clipped_top = std::clamp(source->y, 0, canvas_height_);
        const int clipped_right = static_cast<int>(std::clamp<int64_t>(
                source_right, 0, canvas_width_));
        const int clipped_bottom = static_cast<int>(std::clamp<int64_t>(
                source_bottom, 0, canvas_height_));
        const int width = clipped_right - clipped_left;
        const int height = clipped_bottom - clipped_top;
        if (width <= 0 || height <= 0) continue;

        const int source_x = clipped_left - source->x;
        const int source_y = clipped_top - source->y;
        const int stride = source->linesize[0];
        const int64_t absolute_stride = stride < 0
                ? -static_cast<int64_t>(stride) : static_cast<int64_t>(stride);
        if (absolute_stride < source->w || source_x < 0 || source_y < 0 ||
            source_x + width > source->w || source_y + height > source->h) {
            __android_log_print(ANDROID_LOG_WARN, kTag,
                                "Ignoring invalid caption bitmap stride or bounds");
            continue;
        }
        const std::size_t pixel_count = static_cast<std::size_t>(width) * height;
        if (pixel_count > std::numeric_limits<std::size_t>::max() / 4) continue;

        SubtitleRect rect;
        rect.x = clipped_left;
        rect.y = clipped_top;
        rect.width = width;
        rect.height = height;
        rect.rgba.resize(pixel_count * 4);

        for (int y = 0; y < height; ++y) {
            const uint8_t* row = source->data[0] +
                    static_cast<std::ptrdiff_t>(source_y + y) * stride + source_x;
            for (int x = 0; x < width; ++x) {
                const unsigned palette_index = row[x];
                uint32_t argb = 0;
                if (palette_index < static_cast<unsigned>(palette_count)) {
                    std::memcpy(&argb, source->data[1] + palette_index * sizeof(uint32_t),
                                sizeof(argb));
                }
                const std::size_t destination =
                        (static_cast<std::size_t>(y) * width + x) * 4;
                rect.rgba[destination] = static_cast<uint8_t>((argb >> 16) & 0xFF);
                rect.rgba[destination + 1] = static_cast<uint8_t>((argb >> 8) & 0xFF);
                rect.rgba[destination + 2] = static_cast<uint8_t>(argb & 0xFF);
                rect.rgba[destination + 3] = static_cast<uint8_t>((argb >> 24) & 0xFF);
            }
        }
        event->rects.emplace_back(std::move(rect));
    }

    // A true empty AVSubtitle is a clear. A malformed/non-bitmap non-empty one
    // is ignored rather than accidentally clearing a valid indefinite caption.
    return !event->rects.empty();
}

}  // namespace aribplayer
