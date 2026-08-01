#pragma once

#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <vector>

extern "C" {
#include <libavcodec/packet.h>
#include <libavutil/frame.h>
}

namespace aribplayer {

enum class QueueResult {
    kOk,
    kAborted,
    kFlushed,
    kFull,
};

// Owns one AVPacket reference and annotates it with the seek generation that
// produced it. A decoder must discard buffered codec state when serial changes.
struct PacketItem {
    AVPacket* packet = nullptr;
    int serial = 0;
    bool end_of_stream = false;

    PacketItem() = default;
    ~PacketItem();
    PacketItem(PacketItem&& other) noexcept;
    PacketItem& operator=(PacketItem&& other) noexcept;
    PacketItem(const PacketItem&) = delete;
    PacketItem& operator=(const PacketItem&) = delete;
};

class PacketQueue {
public:
    explicit PacketQueue(std::size_t max_packets = 64,
                         std::size_t max_bytes = 4U * 1024U * 1024U);
    ~PacketQueue();

    // Creates an owned packet reference. Blocks for bounded backpressure when
    // requested; returns kFull immediately when block is false.
    QueueResult Put(const AVPacket& packet, int serial, bool block = true);
    QueueResult PutEof(int serial, bool block = true);
    QueueResult Get(PacketItem* item, bool block = true);

    void Flush();
    void Abort();
    void Reset();
    std::size_t Size() const;

private:
    QueueResult Enqueue(PacketItem&& item, std::size_t bytes, bool block);

    const std::size_t max_packets_;
    const std::size_t max_bytes_;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::deque<PacketItem> packets_;
    std::size_t bytes_ = 0;
    bool aborted_ = false;
    bool flush_pending_ = false;
};

// Owns one AVFrame reference and its presentation timestamp in seconds.
struct FrameItem {
    AVFrame* frame = nullptr;
    int serial = 0;
    double pts_seconds = 0.0;
    bool end_of_stream = false;

    FrameItem() = default;
    ~FrameItem();
    FrameItem(FrameItem&& other) noexcept;
    FrameItem& operator=(FrameItem&& other) noexcept;
    FrameItem(const FrameItem&) = delete;
    FrameItem& operator=(const FrameItem&) = delete;
};

class FrameQueue {
public:
    // Four frames provides a small renderer handoff window without allowing
    // video decoding to run arbitrarily ahead of playback.
    explicit FrameQueue(std::size_t capacity = 4);
    ~FrameQueue();

    QueueResult Push(const AVFrame& frame, int serial, double pts_seconds,
                     bool block = true);
    QueueResult PushEof(int serial, bool block = true);
    QueueResult Pop(FrameItem* item, bool block = true);

    void Flush();
    void Abort();
    void Reset();
    std::size_t Size() const;

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::deque<FrameItem> frames_;
    bool aborted_ = false;
};

constexpr int64_t kSubtitleEndIndefinite = std::numeric_limits<int64_t>::max();

struct SubtitleRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
};

enum class SubtitleSource {
    kCaption,
    kSuperimpose,
};

struct SubtitleEvent {
    int serial = 0;
    SubtitleSource source = SubtitleSource::kCaption;
    int64_t start_pts_us = 0;
    int64_t end_pts_us = kSubtitleEndIndefinite;
    int canvas_width = 0;
    int canvas_height = 0;
    std::vector<SubtitleRect> rects;
};

// Bounded, presentation-time-ordered handoff from the subtitle decoder to the
// renderer. Events own their RGBA buffers and are moved at both queue hops.
class SubtitleQueue {
public:
    explicit SubtitleQueue(std::size_t capacity = 64);
    ~SubtitleQueue();

    QueueResult Push(SubtitleEvent event, bool block = true);
    QueueResult Pop(SubtitleEvent* event, bool block = true);

    void Flush();
    void Abort();
    void Reset();
    std::size_t Size() const;

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::deque<SubtitleEvent> events_;
    bool aborted_ = false;
};

}  // namespace aribplayer
