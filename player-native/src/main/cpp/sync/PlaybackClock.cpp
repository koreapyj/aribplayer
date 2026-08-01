#include "sync/PlaybackClock.h"

#include "audio/AudioSink.h"

#include <cmath>

#include <android/log.h>

namespace aribplayer {
namespace {
constexpr char kAudioTag[] = "aribplayer-audio";
}

void PlaybackClock::SetAudioSink(const AudioSink* audio_sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = SteadyClock::now();
    anchor_position_seconds_ = PositionLocked(now);
    anchor_time_ = now;
    audio_sink_ = audio_sink;
    audio_position_established_ = false;
    fallback_active_ = false;
    video_fallback_available_ = false;
}

void PlaybackClock::Reset(double position_seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    anchor_position_seconds_ = position_seconds;
    anchor_time_ = SteadyClock::now();
    audio_position_established_ = false;
    fallback_active_ = false;
    video_fallback_available_ = false;
}

void PlaybackClock::SetVideoFallbackAnchor(double position_seconds) {
    if (!std::isfinite(position_seconds)) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!video_fallback_available_) {
        video_fallback_position_seconds_ = position_seconds;
        video_fallback_available_ = true;
    }
}

void PlaybackClock::Pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!paused_) {
        const auto now = SteadyClock::now();
        anchor_position_seconds_ = PositionLocked(now);
        anchor_time_ = now;
        paused_ = true;
    }
}

void PlaybackClock::Resume() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (paused_) {
        anchor_time_ = SteadyClock::now();
        paused_ = false;
    }
}

bool PlaybackClock::paused() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return paused_;
}

bool PlaybackClock::TryPositionSeconds(double* position_seconds) const {
    if (position_seconds == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return TryPositionLocked(SteadyClock::now(), position_seconds);
}

double PlaybackClock::PositionSeconds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return PositionLocked(SteadyClock::now());
}

bool PlaybackClock::TryPositionLocked(SteadyClock::time_point now,
                                      double* position_seconds) const {
    if (position_seconds == nullptr) {
        return false;
    }

    if (audio_sink_ != nullptr) {
        double audio_position = 0.0;
        if (!paused_ && audio_sink_->is_running() &&
            audio_sink_->TryPositionSeconds(&audio_position)) {
            const bool stable = audio_sink_->HasStableAnchor();
            // Once video has fallen back, do not jump to a provisional frame-zero
            // mapping. Wait for the refined device timestamp.
            if (!fallback_active_ || stable) {
                anchor_position_seconds_ = audio_position;
                anchor_time_ = now;
                if (stable) {
                    audio_position_established_ = true;
                    fallback_active_ = false;
                }
                *position_seconds = audio_position;
                return true;
            }
        }
        if (!audio_position_established_) {
            if (!paused_ && video_fallback_available_ &&
                audio_sink_->AnchorWaitExpired()) {
                anchor_position_seconds_ = video_fallback_position_seconds_;
                anchor_time_ = now;
                audio_position_established_ = true;
                fallback_active_ = true;
                *position_seconds = anchor_position_seconds_;
                __android_log_print(
                        ANDROID_LOG_WARN, kAudioTag,
                        "Clock anchored: pts_ms=%.3f frames_presented=-1 timestamp_frame=-1 output_latency_ms=-1.000 source=fallback",
                        anchor_position_seconds_ * 1000.0);
                return true;
            }
            return false;
        }
    }

    if (paused_) {
        *position_seconds = anchor_position_seconds_;
        return true;
    }

    const std::chrono::duration<double> elapsed = now - anchor_time_;
    *position_seconds = anchor_position_seconds_ + elapsed.count();
    return true;
}

double PlaybackClock::PositionLocked(SteadyClock::time_point now) const {
    double position = anchor_position_seconds_;
    return TryPositionLocked(now, &position) ? position : anchor_position_seconds_;
}

}  // namespace aribplayer
