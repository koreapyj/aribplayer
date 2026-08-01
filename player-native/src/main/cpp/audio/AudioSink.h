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
    // Waits for at most timeout_nanoseconds before force-closing from the
    // current AAudio state.
    void Close(int64_t timeout_nanoseconds = 2000000000LL);
    // Invalidates every clock-anchor input, clears queued PCM, and flushes the
    // paused AAudio device queue. expected_start_pts_seconds is used to reject
    // a stale first anchor after a seek or pipeline flush.
    void ResetTimeline(double expected_start_pts_seconds);

    bool is_open() const { return stream_ != nullptr; }
    bool is_running() const { return running_.load(std::memory_order_acquire); }
    int sample_rate() const { return sample_rate_; }

    // Returns a position derived only from AAudio's CLOCK_MONOTONIC
    // presentation timestamp. It remains unavailable until real PCM with a
    // known PTS has been mapped into the current device timestamp epoch.
    bool TryPositionSeconds(double* position_seconds) const;
    // Used by PlaybackClock to bound the first-frame hold when the device never
    // supplies a usable presentation timestamp.
    bool AnchorWaitExpired() const;
    bool HasStableAnchor() const;

private:
    enum class DeviceEpochMode : int {
        kStreamLifetime = 0,
        kResumePending = 1,
        kResumeRelative = 2,
    };

    static aaudio_data_callback_result_t DataCallback(AAudioStream* stream,
                                                      void* user_data,
                                                      void* audio_data,
                                                      int32_t num_frames);
    static void ErrorCallback(AAudioStream* stream, void* user_data,
                              aaudio_result_t error);

    bool OpenStream();
    bool ReopenStreamForReset();
    void CloseStream(int64_t timeout_nanoseconds);
    bool WaitForState(aaudio_stream_state_t target_state,
                      int64_t timeout_nanoseconds);
    bool FlushPausedStream();
    void ResetAnchorStateFromControl(uint64_t generation,
                                     double expected_start_pts_seconds);
    bool PrepareResumeEpoch();

    aaudio_data_callback_result_t OnData(void* audio_data, int32_t num_frames);
    bool InvalidateTimelineFromCallback(uint64_t generation,
                                        uint64_t* next_generation);
    bool ResolveDeviceEpoch(int64_t timestamp_frame, int64_t current_app_frame);
    bool CaptureDeviceAnchor(int64_t candidate_app_frame,
                             int64_t current_app_frame,
                             double* device_frame,
                             double* presented_frames,
                             int64_t* timestamp_frame,
                             double* output_latency_ms,
                             bool* relative_epoch);
    void TryPublishPendingAnchor(uint64_t generation,
                                 int64_t current_app_frame);
    bool AcceptAnchorForExpectedStart(uint64_t generation,
                                      double implied_start_position,
                                      double candidate_pts_seconds);
    void LogUntrustedLatency(double latency_seconds, int64_t written_frames,
                             double presented_frames);
    void LogAnchorRejection(uint64_t generation, double implied_start_position,
                            double candidate_pts_seconds,
                            double expected_start_pts_seconds,
                            double deviation_seconds, int rejection_count);

    PcmRing& pcm_ring_;
    AAudioStream* stream_ = nullptr;
    int sample_rate_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<bool> disconnected_{false};

    // Stream-lifetime callback frame count. It is used only through differences
    // or after conversion by device_frame_origin_submitted_; it is never mixed
    // directly with an unresolved post-resume timestamp epoch.
    std::atomic<int64_t> submitted_frames_{0};
    std::atomic<int64_t> start_time_nanoseconds_{0};
    std::atomic<int64_t> last_debug_log_nanoseconds_{0};
    std::atomic<int64_t> last_latency_warning_nanoseconds_{0};
    std::atomic<int64_t> last_anchor_warning_nanoseconds_{0};
    std::atomic<uint64_t> callback_count_{0};

    std::atomic<int64_t> pause_timestamp_frame_{-1};
    std::atomic<int64_t> pause_presented_app_frame_{-1};
    std::atomic<uint64_t> pause_generation_{std::numeric_limits<uint64_t>::max()};
    std::atomic<bool> stream_reopened_for_reset_{false};
    std::atomic<bool> device_queue_flushed_since_pause_{false};
    std::atomic<int64_t> resume_restart_origin_{0};
    std::atomic<int64_t> resume_reference_timestamp_frame_{-1};
    std::atomic<int64_t> device_frame_origin_submitted_{0};
    std::atomic<int> device_epoch_mode_{
            static_cast<int>(DeviceEpochMode::kStreamLifetime)};

    std::atomic<bool> underrun_since_pcm_{false};
    std::atomic<bool> timeline_reset_in_progress_{false};
    std::atomic<uint64_t> timeline_generation_{0};

    std::atomic<double> expected_pcm_pts_seconds_{
            std::numeric_limits<double>::quiet_NaN()};
    std::atomic<uint64_t> expected_pcm_generation_{
            std::numeric_limits<uint64_t>::max()};
    std::atomic<double> expected_start_pts_seconds_{
            std::numeric_limits<double>::quiet_NaN()};
    std::atomic<int64_t> expected_start_time_nanoseconds_{0};
    std::atomic<uint64_t> expected_start_generation_{
            std::numeric_limits<uint64_t>::max()};

    // The first real PCM record survives callbacks where AAudio has not yet
    // produced a timestamp. candidate_app_frame_ is in submitted_frames_' frame
    // domain and is converted only after the device epoch is resolved.
    std::atomic<int64_t> candidate_app_frame_{0};
    std::atomic<double> candidate_pts_seconds_{
            std::numeric_limits<double>::quiet_NaN()};
    std::atomic<uint64_t> candidate_generation_{
            std::numeric_limits<uint64_t>::max()};

    std::atomic<double> pending_anchor_frame_{
            std::numeric_limits<double>::quiet_NaN()};
    std::atomic<double> pending_anchor_pts_seconds_{
            std::numeric_limits<double>::quiet_NaN()};
    std::atomic<int64_t> pending_anchor_timestamp_frame_{-1};
    std::atomic<double> pending_anchor_output_latency_ms_{-1.0};
    std::atomic<bool> pending_anchor_relative_epoch_{false};
    mutable std::atomic<uint64_t> pending_anchor_generation_{
            std::numeric_limits<uint64_t>::max()};

    mutable std::atomic<double> anchor_frame_{
            std::numeric_limits<double>::quiet_NaN()};
    mutable std::atomic<double> anchor_pts_seconds_{
            std::numeric_limits<double>::quiet_NaN()};
    mutable std::atomic<uint64_t> anchor_generation_{
            std::numeric_limits<uint64_t>::max()};
    mutable std::atomic<bool> anchor_provisional_{false};

    std::atomic<double> rejected_anchor_start_seconds_{
            std::numeric_limits<double>::quiet_NaN()};
    std::atomic<int> rejected_anchor_count_{0};
    std::atomic<uint64_t> rejected_anchor_generation_{
            std::numeric_limits<uint64_t>::max()};

    mutable std::mutex timeline_mutex_;
};

}  // namespace aribplayer
