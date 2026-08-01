#include "audio/PcmRing.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace aribplayer {

PcmRing::PcmRing(int sample_rate, std::size_t capacity_frames)
    : sample_rate_(std::max(sample_rate, 1)),
      capacity_frames_(capacity_frames == 0
                           ? static_cast<std::size_t>(std::max(sample_rate, 1))
                           : capacity_frames),
      samples_(capacity_frames_ * 2),
      timestamps_(capacity_frames_, std::numeric_limits<double>::quiet_NaN()) {}

std::size_t PcmRing::available_frames() const {
    const uint64_t generation = generation_.load(std::memory_order_acquire);
    if ((generation & 1U) != 0) return 0;
    const uint64_t write = write_index_.load(std::memory_order_acquire);
    const uint64_t read = read_index_.load(std::memory_order_acquire);
    if (generation_.load(std::memory_order_acquire) != generation) return 0;
    return static_cast<std::size_t>(write - read);
}

std::size_t PcmRing::free_frames() const {
    return capacity_frames_ - available_frames();
}

std::size_t PcmRing::Write(const int16_t* interleaved_stereo, std::size_t frames,
                           double first_pts_seconds) {
    if (interleaved_stereo == nullptr || frames == 0) {
        return 0;
    }

    const uint64_t generation = generation_.load(std::memory_order_acquire);
    if ((generation & 1U) != 0) return 0;
    const uint64_t write = write_index_.load(std::memory_order_relaxed);
    const uint64_t read = read_index_.load(std::memory_order_acquire);
    const std::size_t count = std::min(
        frames, capacity_frames_ - static_cast<std::size_t>(write - read));
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t slot = static_cast<std::size_t>((write + i) % capacity_frames_);
        samples_[slot * 2] = interleaved_stereo[i * 2];
        samples_[slot * 2 + 1] = interleaved_stereo[i * 2 + 1];
        timestamps_[slot] = std::isfinite(first_pts_seconds)
                                ? first_pts_seconds + static_cast<double>(i) / sample_rate_
                                : std::numeric_limits<double>::quiet_NaN();
    }
    if (generation_.load(std::memory_order_acquire) != generation) return 0;
    write_index_.store(write + count, std::memory_order_release);
    return count;
}

std::size_t PcmRing::Read(int16_t* interleaved_stereo, std::size_t requested_frames,
                          double* first_pts_seconds) {
    if (first_pts_seconds != nullptr) {
        *first_pts_seconds = std::numeric_limits<double>::quiet_NaN();
    }
    if (interleaved_stereo == nullptr || requested_frames == 0) {
        return 0;
    }

    const uint64_t generation = generation_.load(std::memory_order_acquire);
    if ((generation & 1U) != 0) return 0;
    const uint64_t read = read_index_.load(std::memory_order_relaxed);
    const uint64_t write = write_index_.load(std::memory_order_acquire);
    std::size_t count = std::min(
        requested_frames, static_cast<std::size_t>(write - read));
    // Keep a large PTS jump on a callback boundary so AudioSink can re-anchor
    // exactly at the first frame of the new timeline segment.
    for (std::size_t i = 1; i < count; ++i) {
        const std::size_t previous_slot =
                static_cast<std::size_t>((read + i - 1) % capacity_frames_);
        const std::size_t slot = static_cast<std::size_t>((read + i) % capacity_frames_);
        const double previous_pts = timestamps_[previous_slot];
        const double current_pts = timestamps_[slot];
        if (std::isfinite(previous_pts) && std::isfinite(current_pts) &&
            std::abs(current_pts - (previous_pts + 1.0 / sample_rate_)) > 0.500) {
            count = i;
            break;
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t slot = static_cast<std::size_t>((read + i) % capacity_frames_);
        if (i == 0 && first_pts_seconds != nullptr) {
            *first_pts_seconds = timestamps_[slot];
        }
        interleaved_stereo[i * 2] = samples_[slot * 2];
        interleaved_stereo[i * 2 + 1] = samples_[slot * 2 + 1];
    }
    if (generation_.load(std::memory_order_acquire) != generation) {
        if (first_pts_seconds != nullptr) {
            *first_pts_seconds = std::numeric_limits<double>::quiet_NaN();
        }
        return 0;
    }
    read_index_.store(read + count, std::memory_order_release);
    return count;
}

void PcmRing::Clear() {
    generation_.fetch_add(1, std::memory_order_acq_rel); // odd: reset in progress
    const uint64_t write = write_index_.load(std::memory_order_acquire);
    read_index_.store(write, std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_release); // even: resume SPSC access
}

}  // namespace aribplayer
