#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>

#include "audio/PcmRing.h"

#include <aaudio/AAudio.h>

namespace aribplayer {

// Pulls s16 stereo PCM from PcmRing on the AAudio real-time callback thread.
// Its position API maps AAudio presentation timestamps back to media PTS.
class AudioSink final {
public:
    explicit AudioSink(PcmRing& pcm_ring);
    ~AudioSink();

    AudioSink(const AudioSink&) = delete;
    AudioSink& operator=(const AudioSink&) = delete;

    bool Open();
    bool Start();
    void Pause();
    bool Resume();
    void Stop();
    void Close();
    void ResetTimeline();

    bool is_open() const { return stream_ != nullptr; }
    bool is_running() const { return running_.load(std::memory_order_acquire); }
    int sample_rate() const { return sample_rate_; }

    // Returns a position derived only from AAudio's CLOCK_MONOTONIC
    // presentation timestamp. It remains unavailable until a callback has
    // consumed real PCM with a known starting PTS.
    bool TryPositionSeconds(double* position_seconds) const;
    // Used by PlaybackClock to bound the first-frame hold when the device never
    // supplies a usable presentation timestamp.
    bool AnchorWaitExpired() const;
    bool HasStableAnchor() const;

private:
    static aaudio_data_callback_result_t DataCallback(AAudioStream* stream,
                                                      void* user_data,
                                                      void* audio_data,
                                                      int32_t num_frames);
    static void ErrorCallback(AAudioStream* stream, void* user_data,
                              aaudio_result_t error);
    aaudio_data_callback_result_t OnData(void* audio_data, int32_t num_frames);
    uint64_t InvalidateTimelineFromCallback(uint64_t generation);

    PcmRing& pcm_ring_;
    AAudioStream* stream_ = nullptr;
    int sample_rate_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<bool> disconnected_{false};
    std::atomic<int64_t> submitted_frames_{0};
    std::atomic<int64_t> start_time_nanoseconds_{0};
    std::atomic<int64_t> last_debug_log_nanoseconds_{0};
    std::atomic<uint64_t> callback_count_{0};
    std::atomic<bool> underrun_since_pcm_{false};
    std::atomic<double> expected_pcm_pts_seconds_{0.0};
    std::atomic<uint64_t> expected_pcm_generation_{std::numeric_limits<uint64_t>::max()};
    std::atomic<int64_t> pending_anchor_frame_{0};
    std::atomic<double> pending_anchor_pts_seconds_{0.0};
    mutable std::atomic<uint64_t> pending_anchor_generation_{std::numeric_limits<uint64_t>::max()};
    mutable std::atomic<int64_t> anchor_frame_{0};
    mutable std::atomic<double> anchor_pts_seconds_{0.0};
    mutable std::atomic<uint64_t> anchor_generation_{std::numeric_limits<uint64_t>::max()};
    mutable std::atomic<bool> anchor_provisional_{false};
    std::atomic<uint64_t> timeline_generation_{0};
    mutable std::mutex timeline_mutex_;
};

}  // namespace aribplayer
