#include "audio/AudioSink.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <time.h>

#include <android/log.h>

namespace aribplayer {
namespace {
constexpr char kTag[] = "aribplayer-audio";
constexpr uint64_t kInvalidGeneration = std::numeric_limits<uint64_t>::max();
constexpr int64_t kNanosecondsPerSecond = 1000000000LL;
constexpr int64_t kProvisionalTimestampWindowNanoseconds = 200000000LL;
constexpr int64_t kAnchorTimeoutNanoseconds = 2000000000LL;
constexpr int64_t kDebugLogIntervalNanoseconds = 5000000000LL;
constexpr double kPtsDiscontinuitySeconds = 0.500;

int64_t MonotonicNanoseconds() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<int64_t>(now.tv_sec) * kNanosecondsPerSecond + now.tv_nsec;
}
}  // namespace

AudioSink::AudioSink(PcmRing& pcm_ring) : pcm_ring_(pcm_ring) {}

AudioSink::~AudioSink() {
    Close();
}

bool AudioSink::Open() {
    if (stream_ != nullptr) {
        return true;
    }

    AAudioStreamBuilder* builder = nullptr;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK || builder == nullptr) {
        return false;
    }
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setSampleRate(builder, pcm_ring_.sample_rate());
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setDataCallback(builder, &AudioSink::DataCallback, this);
    AAudioStreamBuilder_setErrorCallback(builder, &AudioSink::ErrorCallback, this);

    const aaudio_result_t result = AAudioStreamBuilder_openStream(builder, &stream_);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK || stream_ == nullptr) {
        stream_ = nullptr;
        return false;
    }

    sample_rate_ = AAudioStream_getSampleRate(stream_);
    if (sample_rate_ != pcm_ring_.sample_rate() ||
        AAudioStream_getChannelCount(stream_) != 2 ||
        AAudioStream_getFormat(stream_) != AAUDIO_FORMAT_PCM_I16) {
        Close();
        return false;
    }
    submitted_frames_.store(0, std::memory_order_release);
    start_time_nanoseconds_.store(0, std::memory_order_release);
    last_debug_log_nanoseconds_.store(0, std::memory_order_release);
    callback_count_.store(0, std::memory_order_release);
    underrun_since_pcm_.store(false, std::memory_order_release);
    expected_pcm_generation_.store(kInvalidGeneration, std::memory_order_release);
    pending_anchor_generation_.store(kInvalidGeneration, std::memory_order_release);
    anchor_generation_.store(kInvalidGeneration, std::memory_order_release);
    anchor_provisional_.store(false, std::memory_order_release);
    disconnected_.store(false, std::memory_order_release);
    return true;
}

bool AudioSink::Start() {
    if (!Open() || disconnected_.load(std::memory_order_acquire)) {
        return false;
    }
    const aaudio_result_t result = AAudioStream_requestStart(stream_);
    const bool started = result == AAUDIO_OK;
    if (started) {
        start_time_nanoseconds_.store(MonotonicNanoseconds(), std::memory_order_release);
    }
    running_.store(started, std::memory_order_release);
    return started;
}

void AudioSink::Pause() {
    if (stream_ != nullptr && running_.exchange(false, std::memory_order_acq_rel)) {
        AAudioStream_requestPause(stream_);
    }
}

bool AudioSink::Resume() {
    return Start();
}

void AudioSink::Stop() {
    if (stream_ != nullptr && running_.exchange(false, std::memory_order_acq_rel)) {
        AAudioStream_requestStop(stream_);
    }
}

void AudioSink::Close() {
    Stop();
    if (stream_ != nullptr) {
        AAudioStream_close(stream_);
        stream_ = nullptr;
    }
    sample_rate_ = 0;
}

void AudioSink::ResetTimeline() {
    std::lock_guard<std::mutex> lock(timeline_mutex_);
    timeline_generation_.fetch_add(1, std::memory_order_acq_rel);
    pending_anchor_generation_.store(kInvalidGeneration, std::memory_order_release);
    anchor_generation_.store(kInvalidGeneration, std::memory_order_release);
    anchor_provisional_.store(false, std::memory_order_release);
    expected_pcm_generation_.store(kInvalidGeneration, std::memory_order_release);
    underrun_since_pcm_.store(false, std::memory_order_release);
}

bool AudioSink::TryPositionSeconds(double* position_seconds) const {
    if (position_seconds == nullptr || stream_ == nullptr || sample_rate_ <= 0 ||
        !running_.load(std::memory_order_acquire)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(timeline_mutex_);
    const uint64_t generation = timeline_generation_.load(std::memory_order_acquire);
    int64_t frame_position = -1;
    int64_t timestamp_nanoseconds = -1;
    if (AAudioStream_getTimestamp(stream_, CLOCK_MONOTONIC, &frame_position,
                                  &timestamp_nanoseconds) != AAUDIO_OK) {
        return false;
    }

    const int64_t now_nanoseconds = MonotonicNanoseconds();
    if (frame_position < 0 || timestamp_nanoseconds <= 0 ||
        timestamp_nanoseconds > now_nanoseconds + 50000000LL) {
        return false;
    }
    const int64_t start_nanoseconds =
            start_time_nanoseconds_.load(std::memory_order_acquire);
    const int64_t start_age_nanoseconds = start_nanoseconds > 0
            ? std::max<int64_t>(0, now_nanoseconds - start_nanoseconds)
            : kAnchorTimeoutNanoseconds;

    bool log_anchor = false;
    const char* anchor_source = "timestamp";
    const bool pending_for_generation =
            pending_anchor_generation_.load(std::memory_order_acquire) == generation;
    if (anchor_generation_.load(std::memory_order_acquire) != generation) {
        if (!pending_for_generation) {
            return false;
        }
        // Legacy AAudio can briefly return an otherwise valid timestamp at frame
        // zero. Treat that mapping as provisional and require a later advancing
        // timestamp to make it stable.
        if (frame_position == 0 &&
            start_age_nanoseconds > kProvisionalTimestampWindowNanoseconds) {
            return false;
        }
        anchor_frame_.store(pending_anchor_frame_.load(std::memory_order_acquire),
                            std::memory_order_release);
        anchor_pts_seconds_.store(
                pending_anchor_pts_seconds_.load(std::memory_order_acquire),
                std::memory_order_release);
        anchor_generation_.store(generation, std::memory_order_release);
        anchor_provisional_.store(frame_position == 0, std::memory_order_release);
        log_anchor = true;
    } else if (anchor_provisional_.load(std::memory_order_acquire)) {
        if (frame_position == 0) {
            if (start_age_nanoseconds > kProvisionalTimestampWindowNanoseconds) {
                return false;
            }
        } else {
            if (!pending_for_generation) {
                return false;
            }
            anchor_frame_.store(pending_anchor_frame_.load(std::memory_order_acquire),
                                std::memory_order_release);
            anchor_pts_seconds_.store(
                    pending_anchor_pts_seconds_.load(std::memory_order_acquire),
                    std::memory_order_release);
            anchor_provisional_.store(false, std::memory_order_release);
            anchor_source = "refined";
            log_anchor = true;
        }
    }

    const double timestamp_age_seconds = std::max(
            0.0, static_cast<double>(now_nanoseconds - timestamp_nanoseconds) /
                         kNanosecondsPerSecond);
    double presented_frames = static_cast<double>(frame_position) +
                              timestamp_age_seconds * sample_rate_;
    const int64_t written_frames = AAudioStream_getFramesWritten(stream_);
    // A Legacy stream may leave framesWritten at zero after timestamps start
    // advancing. Only use it as a cap when it is in the same frame domain.
    if (written_frames >= frame_position) {
        presented_frames = std::min(presented_frames, static_cast<double>(written_frames));
    }

    const double anchor_pts = anchor_pts_seconds_.load(std::memory_order_acquire);
    const int64_t anchor_frame = anchor_frame_.load(std::memory_order_acquire);
    const double position = anchor_pts +
                            (presented_frames - static_cast<double>(anchor_frame)) /
                                    static_cast<double>(sample_rate_);
    if (timeline_generation_.load(std::memory_order_acquire) != generation) {
        return false;
    }
    *position_seconds = position;

    if (log_anchor) {
        if (!anchor_provisional_.load(std::memory_order_acquire)) {
            uint64_t expected = generation;
            pending_anchor_generation_.compare_exchange_strong(
                    expected, kInvalidGeneration, std::memory_order_acq_rel);
        }
        const double latency_ms = written_frames >= frame_position
                ? std::max(0.0, static_cast<double>(written_frames) - presented_frames) *
                          1000.0 / sample_rate_
                : -1.0;
        __android_log_print(
                ANDROID_LOG_INFO, kTag,
                "Clock anchored: pts_ms=%.3f frames_presented=%lld timestamp_frame=%lld output_latency_ms=%.3f source=%s",
                position * 1000.0,
                static_cast<long long>(std::llround(presented_frames)),
                static_cast<long long>(frame_position), latency_ms, anchor_source);
    }
    return true;
}

bool AudioSink::AnchorWaitExpired() const {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    const int64_t start_nanoseconds =
            start_time_nanoseconds_.load(std::memory_order_acquire);
    return start_nanoseconds > 0 &&
           MonotonicNanoseconds() - start_nanoseconds >= kAnchorTimeoutNanoseconds;
}

bool AudioSink::HasStableAnchor() const {
    const uint64_t generation = timeline_generation_.load(std::memory_order_acquire);
    return anchor_generation_.load(std::memory_order_acquire) == generation &&
           !anchor_provisional_.load(std::memory_order_acquire);
}

aaudio_data_callback_result_t AudioSink::DataCallback(AAudioStream*, void* user_data,
                                                      void* audio_data, int32_t num_frames) {
    return static_cast<AudioSink*>(user_data)->OnData(audio_data, num_frames);
}

void AudioSink::ErrorCallback(AAudioStream*, void* user_data, aaudio_result_t error) {
    auto* sink = static_cast<AudioSink*>(user_data);
    sink->running_.store(false, std::memory_order_release);
    sink->disconnected_.store(true, std::memory_order_release);
    __android_log_print(ANDROID_LOG_ERROR, kTag, "AAudio disconnected: %s",
                        AAudio_convertResultToText(error));
}

uint64_t AudioSink::InvalidateTimelineFromCallback(uint64_t generation) {
    uint64_t expected = generation;
    if (timeline_generation_.compare_exchange_strong(
                expected, generation + 1, std::memory_order_acq_rel)) {
        pending_anchor_generation_.store(kInvalidGeneration, std::memory_order_release);
        anchor_generation_.store(kInvalidGeneration, std::memory_order_release);
        anchor_provisional_.store(false, std::memory_order_release);
        expected_pcm_generation_.store(kInvalidGeneration, std::memory_order_release);
        return generation + 1;
    }
    return expected;
}

aaudio_data_callback_result_t AudioSink::OnData(void* audio_data, int32_t num_frames) {
    auto* output = static_cast<int16_t*>(audio_data);
    uint64_t generation = timeline_generation_.load(std::memory_order_acquire);
    double first_pts = std::numeric_limits<double>::quiet_NaN();
    const std::size_t requested = static_cast<std::size_t>(num_frames);
    const std::size_t copied = pcm_ring_.Read(output, requested, &first_pts);
    if (copied < requested) {
        std::memset(output + copied * 2, 0,
                    (requested - copied) * 2 * sizeof(int16_t));
    }

    const int64_t first_frame = submitted_frames_.fetch_add(num_frames,
                                                             std::memory_order_acq_rel);
    const uint64_t callbacks = callback_count_.fetch_add(1, std::memory_order_acq_rel) + 1;
    const bool had_underrun = underrun_since_pcm_.load(std::memory_order_acquire);
    const bool real_pcm = copied > 0 && std::isfinite(first_pts);

    if (real_pcm && timeline_generation_.load(std::memory_order_acquire) == generation) {
        const bool had_anchor =
                anchor_generation_.load(std::memory_order_acquire) == generation ||
                pending_anchor_generation_.load(std::memory_order_acquire) == generation;
        const bool have_expected =
                expected_pcm_generation_.load(std::memory_order_acquire) == generation;
        const double expected_pts =
                expected_pcm_pts_seconds_.load(std::memory_order_acquire);
        const bool pts_discontinuity = have_expected &&
                std::abs(first_pts - expected_pts) > kPtsDiscontinuitySeconds;
        const bool underrun_recovery = had_underrun && had_anchor;
        if (pts_discontinuity || underrun_recovery) {
            generation = InvalidateTimelineFromCallback(generation);
            if (pts_discontinuity) {
                __android_log_print(
                        ANDROID_LOG_INFO, kTag,
                        "audio PTS discontinuity: expected=%.3f got=%.3f, re-anchoring",
                        expected_pts, first_pts);
            } else {
                __android_log_print(
                        ANDROID_LOG_INFO, kTag,
                        "audio PCM resumed after underrun: pts=%.3f, re-anchoring", first_pts);
            }
        }

        if (timeline_generation_.load(std::memory_order_acquire) == generation) {
            expected_pcm_pts_seconds_.store(
                    first_pts + static_cast<double>(copied) / sample_rate_,
                    std::memory_order_release);
            expected_pcm_generation_.store(generation, std::memory_order_release);
            if (anchor_generation_.load(std::memory_order_acquire) != generation &&
                pending_anchor_generation_.load(std::memory_order_acquire) != generation) {
                pending_anchor_frame_.store(first_frame, std::memory_order_release);
                pending_anchor_pts_seconds_.store(first_pts, std::memory_order_release);
                pending_anchor_generation_.store(generation, std::memory_order_release);
                if (timeline_generation_.load(std::memory_order_acquire) != generation) {
                    uint64_t expected = generation;
                    pending_anchor_generation_.compare_exchange_strong(
                            expected, kInvalidGeneration, std::memory_order_acq_rel);
                }
            }
        }
    }
    underrun_since_pcm_.store(copied < requested, std::memory_order_release);

    const int64_t now_nanoseconds = MonotonicNanoseconds();
    int64_t last_log = last_debug_log_nanoseconds_.load(std::memory_order_relaxed);
    if (now_nanoseconds - last_log >= kDebugLogIntervalNanoseconds &&
        last_debug_log_nanoseconds_.compare_exchange_strong(
                last_log, now_nanoseconds, std::memory_order_acq_rel)) {
        const uint64_t current_generation =
                timeline_generation_.load(std::memory_order_acquire);
        const char* anchor_state = "waiting";
        if (anchor_generation_.load(std::memory_order_acquire) == current_generation) {
            anchor_state = anchor_provisional_.load(std::memory_order_acquire)
                    ? "provisional" : "anchored";
        } else if (pending_anchor_generation_.load(std::memory_order_acquire) ==
                   current_generation) {
            anchor_state = "pending";
        }
        __android_log_print(
                ANDROID_LOG_DEBUG, kTag,
                "callback: state=%s ring_frames=%zu/%zu copied=%zu requested=%zu callbacks=%llu submitted=%lld generation=%llu anchor=%s running=%s underrun=%s",
                copied > 0 ? "pcm" : "silence",
                pcm_ring_.available_frames(), pcm_ring_.capacity_frames(), copied, requested,
                static_cast<unsigned long long>(callbacks),
                static_cast<long long>(first_frame + num_frames),
                static_cast<unsigned long long>(current_generation), anchor_state,
                running_.load(std::memory_order_acquire) ? "true" : "false",
                copied < requested ? "true" : "false");
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

}  // namespace aribplayer
