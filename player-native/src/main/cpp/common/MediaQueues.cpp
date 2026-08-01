#include "common/MediaQueues.h"

#include <algorithm>
#include <utility>

namespace aribplayer {

PacketItem::~PacketItem() {
    av_packet_free(&packet);
}

PacketItem::PacketItem(PacketItem&& other) noexcept
    : packet(other.packet), serial(other.serial), end_of_stream(other.end_of_stream) {
    other.packet = nullptr;
}

PacketItem& PacketItem::operator=(PacketItem&& other) noexcept {
    if (this != &other) {
        av_packet_free(&packet);
        packet = other.packet;
        serial = other.serial;
        end_of_stream = other.end_of_stream;
        other.packet = nullptr;
    }
    return *this;
}

PacketQueue::PacketQueue(std::size_t max_packets, std::size_t max_bytes)
    : max_packets_(max_packets), max_bytes_(max_bytes) {}

PacketQueue::~PacketQueue() {
    Abort();
    Flush();
}

QueueResult PacketQueue::Put(const AVPacket& packet, int serial, bool block) {
    AVPacket* copy = av_packet_alloc();
    if (copy == nullptr || av_packet_ref(copy, &packet) < 0) {
        av_packet_free(&copy);
        return QueueResult::kFull;
    }

    PacketItem item;
    item.packet = copy;
    item.serial = serial;
    return Enqueue(std::move(item), static_cast<std::size_t>(packet.size), block);
}

QueueResult PacketQueue::PutEof(int serial, bool block) {
    PacketItem item;
    item.serial = serial;
    item.end_of_stream = true;
    return Enqueue(std::move(item), 0, block);
}

QueueResult PacketQueue::Enqueue(PacketItem&& item, std::size_t bytes, bool block) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto has_room = [this, bytes] {
        return packets_.size() < max_packets_ && bytes_ + bytes <= max_bytes_;
    };
    if (block) {
        not_full_.wait(lock, [this, &has_room] { return aborted_ || has_room(); });
    }
    if (aborted_) {
        return QueueResult::kAborted;
    }
    if (!has_room()) {
        return QueueResult::kFull;
    }

    bytes_ += bytes;
    packets_.emplace_back(std::move(item));
    lock.unlock();
    not_empty_.notify_one();
    return QueueResult::kOk;
}

QueueResult PacketQueue::Get(PacketItem* item, bool block) {
    if (item == nullptr) {
        return QueueResult::kAborted;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    if (block) {
        not_empty_.wait(lock, [this] {
            return aborted_ || flush_pending_ || !packets_.empty();
        });
    }
    if (aborted_) {
        return QueueResult::kAborted;
    }
    if (flush_pending_) {
        flush_pending_ = false;
        return QueueResult::kFlushed;
    }
    if (packets_.empty()) {
        return QueueResult::kFull;
    }

    bytes_ -= static_cast<std::size_t>(packets_.front().packet == nullptr
                                           ? 0
                                           : packets_.front().packet->size);
    *item = std::move(packets_.front());
    packets_.pop_front();
    lock.unlock();
    not_full_.notify_one();
    return QueueResult::kOk;
}

void PacketQueue::Flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    packets_.clear();
    bytes_ = 0;
    flush_pending_ = true;
    not_empty_.notify_all();
    not_full_.notify_all();
}

void PacketQueue::Abort() {
    std::lock_guard<std::mutex> lock(mutex_);
    aborted_ = true;
    not_empty_.notify_all();
    not_full_.notify_all();
}

void PacketQueue::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    packets_.clear();
    bytes_ = 0;
    aborted_ = false;
    flush_pending_ = false;
    not_full_.notify_all();
}

std::size_t PacketQueue::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return packets_.size();
}

FrameItem::~FrameItem() {
    av_frame_free(&frame);
}

FrameItem::FrameItem(FrameItem&& other) noexcept
    : frame(other.frame), serial(other.serial), pts_seconds(other.pts_seconds),
      end_of_stream(other.end_of_stream) {
    other.frame = nullptr;
}

FrameItem& FrameItem::operator=(FrameItem&& other) noexcept {
    if (this != &other) {
        av_frame_free(&frame);
        frame = other.frame;
        serial = other.serial;
        pts_seconds = other.pts_seconds;
        end_of_stream = other.end_of_stream;
        other.frame = nullptr;
    }
    return *this;
}

FrameQueue::FrameQueue(std::size_t capacity) : capacity_(capacity) {}

FrameQueue::~FrameQueue() {
    Abort();
    Flush();
}

QueueResult FrameQueue::Push(const AVFrame& frame, int serial, double pts_seconds,
                             bool block) {
    AVFrame* copy = av_frame_alloc();
    if (copy == nullptr || av_frame_ref(copy, &frame) < 0) {
        av_frame_free(&copy);
        return QueueResult::kFull;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    if (block) {
        not_full_.wait(lock, [this] { return aborted_ || frames_.size() < capacity_; });
    }
    if (aborted_) {
        av_frame_free(&copy);
        return QueueResult::kAborted;
    }
    if (frames_.size() >= capacity_) {
        av_frame_free(&copy);
        return QueueResult::kFull;
    }

    FrameItem item;
    item.frame = copy;
    item.serial = serial;
    item.pts_seconds = pts_seconds;
    frames_.emplace_back(std::move(item));
    lock.unlock();
    not_empty_.notify_one();
    return QueueResult::kOk;
}

QueueResult FrameQueue::PushEof(int serial, bool block) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (block) {
        not_full_.wait(lock, [this] { return aborted_ || frames_.size() < capacity_; });
    }
    if (aborted_) return QueueResult::kAborted;
    if (frames_.size() >= capacity_) return QueueResult::kFull;

    FrameItem item;
    item.serial = serial;
    item.end_of_stream = true;
    frames_.emplace_back(std::move(item));
    lock.unlock();
    not_empty_.notify_one();
    return QueueResult::kOk;
}

QueueResult FrameQueue::Pop(FrameItem* item, bool block) {
    if (item == nullptr) {
        return QueueResult::kAborted;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    if (block) {
        not_empty_.wait(lock, [this] { return aborted_ || !frames_.empty(); });
    }
    if (aborted_) {
        return QueueResult::kAborted;
    }
    if (frames_.empty()) {
        return QueueResult::kFull;
    }

    *item = std::move(frames_.front());
    frames_.pop_front();
    lock.unlock();
    not_full_.notify_one();
    return QueueResult::kOk;
}

void FrameQueue::Flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    frames_.clear();
    not_full_.notify_all();
}

void FrameQueue::Abort() {
    std::lock_guard<std::mutex> lock(mutex_);
    aborted_ = true;
    not_empty_.notify_all();
    not_full_.notify_all();
}

void FrameQueue::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    frames_.clear();
    aborted_ = false;
    not_full_.notify_all();
}

std::size_t FrameQueue::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_.size();
}

SubtitleQueue::SubtitleQueue(std::size_t capacity) : capacity_(capacity) {}

SubtitleQueue::~SubtitleQueue() {
    Abort();
    Flush();
}

QueueResult SubtitleQueue::Push(SubtitleEvent event, bool block) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (block) {
        not_full_.wait(lock, [this] { return aborted_ || events_.size() < capacity_; });
    }
    if (aborted_) return QueueResult::kAborted;
    if (events_.size() >= capacity_) return QueueResult::kFull;

    const auto position = std::upper_bound(
            events_.begin(), events_.end(), event,
            [](const SubtitleEvent& candidate, const SubtitleEvent& queued) {
                if (candidate.serial != queued.serial) return candidate.serial < queued.serial;
                return candidate.start_pts_us < queued.start_pts_us;
            });
    events_.insert(position, std::move(event));
    lock.unlock();
    not_empty_.notify_one();
    return QueueResult::kOk;
}

QueueResult SubtitleQueue::Pop(SubtitleEvent* event, bool block) {
    if (event == nullptr) return QueueResult::kAborted;
    std::unique_lock<std::mutex> lock(mutex_);
    if (block) {
        not_empty_.wait(lock, [this] { return aborted_ || !events_.empty(); });
    }
    if (aborted_) return QueueResult::kAborted;
    if (events_.empty()) return QueueResult::kFull;

    *event = std::move(events_.front());
    events_.pop_front();
    lock.unlock();
    not_full_.notify_one();
    return QueueResult::kOk;
}

void SubtitleQueue::Flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.clear();
    not_full_.notify_all();
}

void SubtitleQueue::Abort() {
    std::lock_guard<std::mutex> lock(mutex_);
    aborted_ = true;
    not_empty_.notify_all();
    not_full_.notify_all();
}

void SubtitleQueue::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.clear();
    aborted_ = false;
    not_full_.notify_all();
}

std::size_t SubtitleQueue::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_.size();
}

}  // namespace aribplayer
