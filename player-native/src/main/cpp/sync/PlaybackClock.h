#pragma once

#include <chrono>
#include <mutex>

namespace aribplayer {

class AudioSink;

// Audio-master media clock. When AAudio has no timestamp (including
// video-only playback), it continues from a monotonic pause/resume anchor.
class PlaybackClock final {
public:
    PlaybackClock() = default;

    void SetAudioSink(const AudioSink* audio_sink);
    void Reset(double position_seconds = 0.0);
    void SetVideoFallbackAnchor(double position_seconds);
    void Pause();
    void Resume();
    bool paused() const;
    // Returns false while an attached audio sink has not established its first
    // presentation-timestamp anchor. Video uses this to hold its first frame.
    bool TryPositionSeconds(double* position_seconds) const;
    double PositionSeconds() const;

private:
    using SteadyClock = std::chrono::steady_clock;
    bool TryPositionLocked(SteadyClock::time_point now, double* position_seconds) const;
    double PositionLocked(SteadyClock::time_point now) const;

    mutable std::mutex mutex_;
    const AudioSink* audio_sink_ = nullptr;  // non-owning
    mutable double anchor_position_seconds_ = 0.0;
    mutable SteadyClock::time_point anchor_time_ = SteadyClock::now();
    mutable bool audio_position_established_ = false;
    mutable bool fallback_active_ = false;
    bool video_fallback_available_ = false;
    double video_fallback_position_seconds_ = 0.0;
    bool paused_ = true;
};

}  // namespace aribplayer
