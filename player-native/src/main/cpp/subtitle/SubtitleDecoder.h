#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "common/MediaQueues.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace aribplayer {

// Decodes ARIB STD-B24 packets into owned, canvas-relative RGBA subtitle events.
class SubtitleDecoder final {
public:
    SubtitleDecoder(AVCodecContext* decoder, PacketQueue& packets,
                    SubtitleQueue& events, SubtitleSource source,
                    int canvas_width, int canvas_height);
    ~SubtitleDecoder();

    SubtitleDecoder(const SubtitleDecoder&) = delete;
    SubtitleDecoder& operator=(const SubtitleDecoder&) = delete;

    void Start();
    // Signals all queue waits before waiting for the decode thread. Returns false
    // after detaching a non-responsive thread; the owner must abandon this decoder.
    bool Stop(int64_t timeout_ms = 500);
    void Flush(int serial);

    bool running() const { return running_.load(std::memory_order_acquire); }
    bool eof_drained() const { return eof_drained_.load(std::memory_order_acquire); }
    int last_error() const { return last_error_.load(std::memory_order_acquire); }

private:
    void DecodeLoop();
    void FinishThread();
    void ApplyFlush(int serial);
    void DecodePacket(const AVPacket& packet, int serial);
    bool BuildEvent(const AVSubtitle& subtitle, const AVPacket& packet,
                    int serial, SubtitleEvent* event) const;

    AVCodecContext* decoder_ = nullptr;
    PacketQueue& packets_;
    SubtitleQueue& events_;
    const SubtitleSource source_;
    const int canvas_width_;
    const int canvas_height_;

    std::thread thread_;
    std::mutex thread_mutex_;
    std::condition_variable thread_cv_;
    bool thread_finished_ = true;
    bool thread_abandoned_ = false;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> eof_drained_{false};
    std::atomic<int> requested_flush_serial_{-1};
    std::atomic<int> last_error_{0};
    int active_serial_ = -1;
};

}  // namespace aribplayer
