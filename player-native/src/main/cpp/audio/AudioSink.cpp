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
constexpr int64_t kStateChangeTimeoutNanoseconds = 500000000LL;
constexpr int64_t kDebugLogIntervalNanoseconds = 5000000000LL;
constexpr int64_t kAnchorWarningIntervalNanoseconds = 1000000000LL;
constexpr double kPtsDiscontinuitySeconds = 0.500;
constexpr double kMaxAnchorDeviationSeconds = 2.0;
constexpr double kConsistentAnchorToleranceSeconds = 0.250;
constexpr int kConsistentAnchorAcceptanceCount = 3;
constexpr double kMaxTrustedOutputLatencySeconds = 2.0;

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

bool AudioSink::OpenStream() {
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
        AAudioStream_close(stream_);
        stream_ = nullptr;
        sample_rate_ = 0;
        return false;
    }
    disconnected_.store(false, std::memory_order_release);
    return true;
}

bool AudioSink::Open() {
    if (stream_ != nullptr) {
        return true;
    }
    if (!OpenStream()) {
        return false;
    }

    submitted_frames_.store(0, std::memory_order_release);
    start_time_nanoseconds_.store(0, std::memory_order_release);
    last_debug_log_nanoseconds_.store(0, std::memory_order_release);
    last_latency_warning_nanoseconds_.store(0, std::memory_order_release);
    last_anchor_warning_nanoseconds_.store(0, std::memory_order_release);
    callback_count_.store(0, std::memory_order_release);
    pause_timestamp_frame_.store(-1, std::memory_order_release);
    pause_presented_app_frame_.store(-1, std::memory_order_release);
    pause_generation_.store(kInvalidGeneration, std::memory_order_release);
    stream_reopened_for_reset_.store(false, std::memory_order_release);
    device_queue_flushed_since_pause_.store(false, std::memory_order_release);
    resume_restart_origin_.store(0, std::memory_order_release);
    resume_reference_timestamp_frame_.store(-1, std::memory_order_release);
    device_frame_origin_submitted_.store(0, std::memory_order_release);
    device_epoch_mode_.store(static_cast<int>(DeviceEpochMode::kStreamLifetime),
                             std::memory_order_release);
    underrun_since_pcm_.store(false, std::memory_order_release);
    timeline_reset_in_progress_.store(false, std::memory_order_release);
    timeline_generation_.store(0, std::memory_order_release);
    ResetAnchorStateFromControl(0, std::numeric_limits<double>::quiet_NaN());
    running_.store(false, std::memory_order_release);
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
    if (stream_ == nullptr || !running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    int64_t frame_position = -1;
    int64_t timestamp_nanoseconds = -1;
    if (AAudioStream_getTimestamp(stream_, CLOCK_MONOTONIC, &frame_position,
                                  &timestamp_nanoseconds) != AAUDIO_OK ||
        frame_position < 0) {
        frame_position = -1;
    } else if (device_epoch_mode_.load(std::memory_order_acquire) ==
                       static_cast<int>(DeviceEpochMode::kResumePending) &&
               !ResolveDeviceEpoch(
                       frame_position,
                       submitted_frames_.load(std::memory_order_acquire))) {
        // A rapid pause can arrive before the previous resume epoch was
        // resolved. Never combine that timestamp with the old device origin;
        // mark the mapping unknown so Resume takes the safe reopen path.
        frame_position = -1;
    }
    pause_timestamp_frame_.store(frame_position, std::memory_order_release);
    const int64_t device_origin =
            device_frame_origin_submitted_.load(std::memory_order_acquire);
    pause_presented_app_frame_.store(
            frame_position >= 0 ? device_origin + frame_position : -1,
            std::memory_order_release);
    pause_generation_.store(timeline_generation_.load(std::memory_order_acquire),
                            std::memory_order_release);
    device_queue_flushed_since_pause_.store(false, std::memory_order_release);

    const aaudio_result_t result = AAudioStream_requestPause(stream_);
    if (result != AAUDIO_OK) {
        running_.store(true, std::memory_order_release);
        __android_log_print(ANDROID_LOG_WARN, kTag, "AAudio pause failed: %s",
                            AAudio_convertResultToText(result));
    }
}

bool AudioSink::WaitForState(aaudio_stream_state_t target_state,
                             int64_t timeout_nanoseconds) {
    if (stream_ == nullptr) {
        return false;
    }
    const int64_t deadline = MonotonicNanoseconds() + timeout_nanoseconds;
    aaudio_stream_state_t state = AAudioStream_getState(stream_);
    while (state != target_state) {
        const int64_t remaining = deadline - MonotonicNanoseconds();
        if (remaining <= 0) {
            return false;
        }
        aaudio_stream_state_t next_state = AAUDIO_STREAM_STATE_UNKNOWN;
        const aaudio_result_t result = AAudioStream_waitForStateChange(
                stream_, state, &next_state, remaining);
        if (result != AAUDIO_OK) {
            return false;
        }
        state = next_state;
    }
    return true;
}

bool AudioSink::FlushPausedStream() {
    if (stream_ == nullptr) {
        return true;
    }

    aaudio_stream_state_t state = AAudioStream_getState(stream_);
    if (state == AAUDIO_STREAM_STATE_OPEN || state == AAUDIO_STREAM_STATE_STOPPED ||
        state == AAUDIO_STREAM_STATE_FLUSHED) {
        return true;
    }
    if (state == AAUDIO_STREAM_STATE_STARTING || state == AAUDIO_STREAM_STATE_STARTED) {
        const aaudio_result_t pause_result = AAudioStream_requestPause(stream_);
        if (pause_result != AAUDIO_OK) {
            return false;
        }
        state = AAudioStream_getState(stream_);
    }
    if (state == AAUDIO_STREAM_STATE_PAUSING &&
        !WaitForState(AAUDIO_STREAM_STATE_PAUSED, kStateChangeTimeoutNanoseconds)) {
        return false;
    }
    state = AAudioStream_getState(stream_);
    if (state == AAUDIO_STREAM_STATE_FLUSHING) {
        return WaitForState(AAUDIO_STREAM_STATE_FLUSHED,
                            kStateChangeTimeoutNanoseconds);
    }
    if (state != AAUDIO_STREAM_STATE_PAUSED) {
        return false;
    }

    const aaudio_result_t flush_result = AAudioStream_requestFlush(stream_);
    if (flush_result != AAUDIO_OK) {
        return false;
    }
    return WaitForState(AAUDIO_STREAM_STATE_FLUSHED,
                        kStateChangeTimeoutNanoseconds);
}

bool AudioSink::ReopenStreamForReset() {
    if (stream_ != nullptr) {
        CloseStream(kStateChangeTimeoutNanoseconds);
    }
    sample_rate_ = 0;
    running_.store(false, std::memory_order_release);
    if (!OpenStream()) {
        return false;
    }

    submitted_frames_.store(0, std::memory_order_release);
    callback_count_.store(0, std::memory_order_release);
    start_time_nanoseconds_.store(0, std::memory_order_release);
    pause_timestamp_frame_.store(-1, std::memory_order_release);
    pause_presented_app_frame_.store(-1, std::memory_order_release);
    pause_generation_.store(kInvalidGeneration, std::memory_order_release);
    resume_restart_origin_.store(0, std::memory_order_release);
    resume_reference_timestamp_frame_.store(-1, std::memory_order_release);
    device_frame_origin_submitted_.store(0, std::memory_order_release);
    device_epoch_mode_.store(static_cast<int>(DeviceEpochMode::kStreamLifetime),
                             std::memory_order_release);
    device_queue_flushed_since_pause_.store(true, std::memory_order_release);
    stream_reopened_for_reset_.store(true, std::memory_order_release);
    return true;
}

void AudioSink::ResetAnchorStateFromControl(uint64_t generation,
                                            double expected_start_pts_seconds) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    expected_pcm_pts_seconds_.store(nan, std::memory_order_release);
    expected_pcm_generation_.store(kInvalidGeneration, std::memory_order_release);
    expected_start_pts_seconds_.store(expected_start_pts_seconds,
                                      std::memory_order_release);
    expected_start_time_nanoseconds_.store(
            std::isfinite(expected_start_pts_seconds) ? MonotonicNanoseconds() : 0,
            std::memory_order_release);
    expected_start_generation_.store(
            std::isfinite(expected_start_pts_seconds) ? generation : kInvalidGeneration,
            std::memory_order_release);

    candidate_app_frame_.store(0, std::memory_order_release);
    candidate_pts_seconds_.store(nan, std::memory_order_release);
    candidate_generation_.store(kInvalidGeneration, std::memory_order_release);

    pending_anchor_frame_.store(nan, std::memory_order_release);
    pending_anchor_pts_seconds_.store(nan, std::memory_order_release);
    pending_anchor_timestamp_frame_.store(-1, std::memory_order_release);
    pending_anchor_output_latency_ms_.store(-1.0, std::memory_order_release);
    pending_anchor_relative_epoch_.store(false, std::memory_order_release);
    pending_anchor_generation_.store(kInvalidGeneration, std::memory_order_release);

    anchor_frame_.store(nan, std::memory_order_release);
    anchor_pts_seconds_.store(nan, std::memory_order_release);
    anchor_generation_.store(kInvalidGeneration, std::memory_order_release);
    anchor_provisional_.store(false, std::memory_order_release);

    rejected_anchor_start_seconds_.store(nan, std::memory_order_release);
    rejected_anchor_count_.store(0, std::memory_order_release);
    rejected_anchor_generation_.store(kInvalidGeneration, std::memory_order_release);
    underrun_since_pcm_.store(false, std::memory_order_release);
}

bool AudioSink::PrepareResumeEpoch() {
    if (stream_reopened_for_reset_.exchange(false, std::memory_order_acq_rel)) {
        device_frame_origin_submitted_.store(0, std::memory_order_release);
        device_epoch_mode_.store(static_cast<int>(DeviceEpochMode::kStreamLifetime),
                                 std::memory_order_release);
        device_queue_flushed_since_pause_.store(false, std::memory_order_release);
        return true;
    }

    const int64_t submitted = submitted_frames_.load(std::memory_order_acquire);
    const int64_t reference = pause_timestamp_frame_.load(std::memory_order_acquire);
    const bool queue_flushed =
            device_queue_flushed_since_pause_.load(std::memory_order_acquire);
    if (submitted == 0 && reference < 0) {
        device_frame_origin_submitted_.store(0, std::memory_order_release);
        device_epoch_mode_.store(static_cast<int>(DeviceEpochMode::kStreamLifetime),
                                 std::memory_order_release);
        device_queue_flushed_since_pause_.store(false, std::memory_order_release);
        return true;
    }

    const int64_t pause_presented_app =
            pause_presented_app_frame_.load(std::memory_order_acquire);
    if (!queue_flushed && pause_presented_app < 0) {
        __android_log_print(
                ANDROID_LOG_WARN, kTag,
                "AAudio resume has no pre-pause frame mapping; reopening stream to establish a fresh epoch");
        const bool reopened = ReopenStreamForReset();
        if (reopened) {
            stream_reopened_for_reset_.store(false, std::memory_order_release);
            device_queue_flushed_since_pause_.store(false,
                                                     std::memory_order_release);
        }
        return reopened;
    }

    // If AAudio was flushed, timestamp frame zero maps to the first callback
    // submitted after resume. For a plain user pause, queued audio is preserved,
    // so frame zero maps to the app frame that was presented at pause time.
    resume_restart_origin_.store(
            queue_flushed ? submitted : pause_presented_app,
            std::memory_order_release);
    resume_reference_timestamp_frame_.store(reference, std::memory_order_release);
    device_epoch_mode_.store(static_cast<int>(DeviceEpochMode::kResumePending),
                             std::memory_order_release);
    device_queue_flushed_since_pause_.store(false, std::memory_order_release);
    return true;
}

bool AudioSink::Resume() {
    if (!Open() || disconnected_.load(std::memory_order_acquire)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(timeline_mutex_);
    aaudio_stream_state_t state = AAudioStream_getState(stream_);
    if (state == AAUDIO_STREAM_STATE_PAUSING &&
        !WaitForState(AAUDIO_STREAM_STATE_PAUSED, kStateChangeTimeoutNanoseconds)) {
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "AAudio did not reach PAUSED before resume");
        return false;
    }

    const uint64_t generation = timeline_generation_.load(std::memory_order_acquire);
    if (pause_generation_.load(std::memory_order_acquire) == generation) {
        // A plain user pause/resume may start a new Samsung timestamp epoch even
        // though no pipeline flush occurred. Invalidate the old stable mapping,
        // but preserve the PCM ring and device queue.
        timeline_reset_in_progress_.store(true, std::memory_order_release);
        const uint64_t next_generation =
                timeline_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
        ResetAnchorStateFromControl(next_generation,
                                    std::numeric_limits<double>::quiet_NaN());
        timeline_reset_in_progress_.store(false, std::memory_order_release);
    }

    if (!PrepareResumeEpoch()) {
        return false;
    }
    pause_generation_.store(kInvalidGeneration, std::memory_order_release);

    const aaudio_result_t result = AAudioStream_requestStart(stream_);
    const bool started = result == AAUDIO_OK;
    if (started) {
        start_time_nanoseconds_.store(MonotonicNanoseconds(), std::memory_order_release);
    }
    running_.store(started, std::memory_order_release);
    return started;
}

void AudioSink::Stop() {
    if (stream_ != nullptr && running_.exchange(false, std::memory_order_acq_rel)) {
        AAudioStream_requestStop(stream_);
    }
}

void AudioSink::CloseStream(int64_t timeout_nanoseconds) {
    if (stream_ == nullptr) return;

    running_.store(false, std::memory_order_release);
    const int64_t deadline = MonotonicNanoseconds() + timeout_nanoseconds;
    const aaudio_stream_state_t state = AAudioStream_getState(stream_);
    if (state == AAUDIO_STREAM_STATE_STARTING || state == AAUDIO_STREAM_STATE_STARTED ||
        state == AAUDIO_STREAM_STATE_PAUSING || state == AAUDIO_STREAM_STATE_PAUSED ||
        state == AAUDIO_STREAM_STATE_FLUSHING || state == AAUDIO_STREAM_STATE_FLUSHED ||
        state == AAUDIO_STREAM_STATE_STOPPING) {
        AAudioStream_requestStop(stream_);
        const int64_t remaining = deadline - MonotonicNanoseconds();
        if (remaining > 0 && !WaitForState(AAUDIO_STREAM_STATE_STOPPED, remaining)) {
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                                "teardown: AAudio did not reach STOPPED within %lldms; closing anyway",
                                static_cast<long long>(timeout_nanoseconds / 1000000LL));
        }
    }

    AAudioStream* stream = stream_;
    stream_ = nullptr;
    const aaudio_result_t result = AAudioStream_close(stream);
    if (result != AAUDIO_OK) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "teardown: abandoning AAudio stream after close failure: %s",
                            AAudio_convertResultToText(result));
    }
}

void AudioSink::Close(int64_t timeout_nanoseconds) {
    CloseStream(std::max<int64_t>(0, timeout_nanoseconds));
    sample_rate_ = 0;
}

void AudioSink::ResetTimeline(double expected_start_pts_seconds) {
    std::lock_guard<std::mutex> lock(timeline_mutex_);

    // Pause is asynchronous. Gate callback publication, advance the timeline,
    // invalidate every tagged input, and clear the ring before touching AAudio's
    // queued device data. No callback takes timeline_mutex_.
    timeline_reset_in_progress_.store(true, std::memory_order_release);
    const uint64_t generation =
            timeline_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    ResetAnchorStateFromControl(generation, expected_start_pts_seconds);
    pcm_ring_.Clear();
    running_.store(false, std::memory_order_release);

    if (!FlushPausedStream()) {
        __android_log_print(
                ANDROID_LOG_WARN, kTag,
                "AAudio pause/flush did not complete; reopening stream to discard the device queue");
        if (!ReopenStreamForReset()) {
            disconnected_.store(true, std::memory_order_release);
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                                "AAudio reopen failed after flush failure");
        }
    } else {
        stream_reopened_for_reset_.store(false, std::memory_order_release);
        device_queue_flushed_since_pause_.store(true,
                                                 std::memory_order_release);
    }
    timeline_reset_in_progress_.store(false, std::memory_order_release);
}

bool AudioSink::TryPositionSeconds(double* position_seconds) const {
    if (position_seconds == nullptr || stream_ == nullptr || sample_rate_ <= 0 ||
        !running_.load(std::memory_order_acquire) ||
        timeline_reset_in_progress_.load(std::memory_order_acquire) ||
        device_epoch_mode_.load(std::memory_order_acquire) ==
                static_cast<int>(DeviceEpochMode::kResumePending)) {
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
    const bool relative_epoch =
            pending_anchor_relative_epoch_.load(std::memory_order_acquire);
    const char* anchor_source = relative_epoch ? "timestamp-relative" : "timestamp";
    const bool pending_for_generation =
            pending_anchor_generation_.load(std::memory_order_acquire) == generation;
    if (anchor_generation_.load(std::memory_order_acquire) != generation) {
        if (!pending_for_generation) {
            return false;
        }
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
            anchor_source = relative_epoch ? "refined-relative" : "refined";
            log_anchor = true;
        }
    }

    const double timestamp_age_seconds = std::max(
            0.0, static_cast<double>(now_nanoseconds - timestamp_nanoseconds) /
                         kNanosecondsPerSecond);
    double presented_frames = static_cast<double>(frame_position) +
                              timestamp_age_seconds * sample_rate_;

    const auto epoch_mode = static_cast<DeviceEpochMode>(
            device_epoch_mode_.load(std::memory_order_acquire));
    int64_t write_head = -1;
    if (epoch_mode == DeviceEpochMode::kStreamLifetime) {
        write_head = AAudioStream_getFramesWritten(stream_);
    } else if (epoch_mode == DeviceEpochMode::kResumeRelative) {
        write_head = submitted_frames_.load(std::memory_order_acquire) -
                     device_frame_origin_submitted_.load(std::memory_order_acquire);
    }
    // Direction is essential: stale or zero write-head values must never clamp
    // an advancing timestamp backward. Clamp only when the raw timestamp has
    // not passed the write head and age projection overshoots it by a sane amount.
    if (write_head >= frame_position &&
        presented_frames > static_cast<double>(write_head)) {
        const double overshoot_frames =
                presented_frames - static_cast<double>(write_head);
        if (overshoot_frames <= kMaxTrustedOutputLatencySeconds * sample_rate_) {
            presented_frames = static_cast<double>(write_head);
        }
    }

    const double anchor_pts = anchor_pts_seconds_.load(std::memory_order_acquire);
    const double anchor_frame = anchor_frame_.load(std::memory_order_acquire);
    if (!std::isfinite(anchor_pts) || !std::isfinite(anchor_frame)) {
        return false;
    }
    const double position = anchor_pts +
                            (presented_frames - anchor_frame) /
                                    static_cast<double>(sample_rate_);
    if (timeline_generation_.load(std::memory_order_acquire) != generation ||
        timeline_reset_in_progress_.load(std::memory_order_acquire)) {
        return false;
    }
    *position_seconds = position;

    if (log_anchor) {
        if (!anchor_provisional_.load(std::memory_order_acquire)) {
            uint64_t expected = generation;
            pending_anchor_generation_.compare_exchange_strong(
                    expected, kInvalidGeneration, std::memory_order_acq_rel);
        }
        __android_log_print(
                ANDROID_LOG_INFO, kTag,
                "Clock anchored: pts_ms=%.3f frames_presented=%lld timestamp_frame=%lld output_latency_ms=%.3f source=%s",
                position * 1000.0,
                static_cast<long long>(std::llround(presented_frames)),
                static_cast<long long>(pending_anchor_timestamp_frame_.load(
                        std::memory_order_acquire)),
                pending_anchor_output_latency_ms_.load(std::memory_order_acquire),
                anchor_source);
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

bool AudioSink::InvalidateTimelineFromCallback(uint64_t generation,
                                               uint64_t* next_generation) {
    if (next_generation == nullptr ||
        timeline_reset_in_progress_.load(std::memory_order_acquire)) {
        return false;
    }
    uint64_t expected = generation;
    if (!timeline_generation_.compare_exchange_strong(
                expected, generation + 1, std::memory_order_acq_rel)) {
        // Never adopt a generation created by a concurrent control-thread reset.
        // This callback's PCM belongs to the generation it started with.
        return false;
    }
    *next_generation = generation + 1;
    return true;
}

bool AudioSink::ResolveDeviceEpoch(int64_t timestamp_frame,
                                   int64_t current_app_frame) {
    const auto mode = static_cast<DeviceEpochMode>(
            device_epoch_mode_.load(std::memory_order_acquire));
    if (mode != DeviceEpochMode::kResumePending) {
        return true;
    }
    if (sample_rate_ <= 0) {
        return false;
    }

    const int64_t continuing_origin =
            device_frame_origin_submitted_.load(std::memory_order_acquire);
    const int64_t restart_origin =
            resume_restart_origin_.load(std::memory_order_acquire);
    const int64_t continuing_write_head = current_app_frame - continuing_origin;
    const int64_t restarted_write_head = current_app_frame - restart_origin;
    const int64_t stream_lifetime_write_head = current_app_frame;
    const int64_t max_queue_frames = static_cast<int64_t>(
            kMaxTrustedOutputLatencySeconds * sample_rate_);
    const int64_t continuing_queue = continuing_write_head - timestamp_frame;
    const int64_t restarted_queue = restarted_write_head - timestamp_frame;
    const int64_t stream_lifetime_queue =
            stream_lifetime_write_head - timestamp_frame;
    const bool continuing_plausible = continuing_write_head >= 0 &&
            continuing_queue >= 0 && continuing_queue <= max_queue_frames;
    const bool restarted_plausible = restarted_write_head >= 0 &&
            restarted_queue >= 0 && restarted_queue <= max_queue_frames;
    const bool stream_lifetime_plausible = stream_lifetime_write_head >= 0 &&
            stream_lifetime_queue >= 0 &&
            stream_lifetime_queue <= max_queue_frames;
    const int64_t reference =
            resume_reference_timestamp_frame_.load(std::memory_order_acquire);

    constexpr uint64_t kUnresolvedEpochLogIntervalCallbacks = 128;
    const auto log_unresolved_epoch = [&]() {
        const uint64_t callbacks = callback_count_.load(std::memory_order_relaxed);
        if (callbacks == 0 || callbacks % kUnresolvedEpochLogIntervalCallbacks == 0) {
            __android_log_print(
                    ANDROID_LOG_WARN, kTag,
                    "AAudio resume epoch unresolved: timestamp=%lld continuing_queue=%lld restarted_queue=%lld stream_lifetime_queue=%lld",
                    static_cast<long long>(timestamp_frame),
                    static_cast<long long>(continuing_queue),
                    static_cast<long long>(restarted_queue),
                    static_cast<long long>(stream_lifetime_queue));
        }
    };

    bool restarted = false;
    int64_t resolved_origin = continuing_origin;
    if (continuing_origin == 0 || restart_origin == 0) {
        // Keep the original two-candidate behavior when origin zero is already
        // represented by either existing candidate.
        if (reference >= 0 && timestamp_frame < reference) {
            restarted = true;
        } else if (restarted_plausible && !continuing_plausible) {
            restarted = true;
        } else if (continuing_plausible && !restarted_plausible) {
            restarted = false;
        } else if (continuing_plausible && restarted_plausible) {
            // Both epochs can look numerically sane after a very short pre-pause
            // run. Choose the continuing mapping: if the HAL actually restarted,
            // this produces a bounded lag instead of putting the media clock ahead
            // of queued audio. The start-hint convergence guard can recover later.
            restarted = false;
            __android_log_print(
                    ANDROID_LOG_WARN, kTag,
                    "AAudio resume epoch is ambiguous: timestamp=%lld continuing_queue=%lld restarted_queue=%lld; choosing conservative continuing epoch",
                    static_cast<long long>(timestamp_frame),
                    static_cast<long long>(continuing_queue),
                    static_cast<long long>(restarted_queue));
        } else {
            log_unresolved_epoch();
            return false;
        }
        resolved_origin = restarted ? restart_origin : continuing_origin;
    } else {
        const int plausible_candidates =
                static_cast<int>(continuing_plausible) +
                static_cast<int>(restarted_plausible) +
                static_cast<int>(stream_lifetime_plausible);
        if (reference >= 0 && timestamp_frame < reference) {
            // Preserve the strong Samsung restart signal over numeric
            // plausibility, as in the original two-candidate resolver.
            restarted = true;
            resolved_origin = restart_origin;
        } else if (plausible_candidates == 1) {
            if (continuing_plausible) {
                resolved_origin = continuing_origin;
            } else if (restarted_plausible) {
                restarted = true;
                resolved_origin = restart_origin;
            } else {
                resolved_origin = 0;
            }
        } else if (plausible_candidates > 1) {
            // Origin zero is valid only as the sole plausible mapping. When
            // mappings are ambiguous, prefer the existing continuing epoch;
            // otherwise prefer the explicit restart candidate.
            if (continuing_plausible) {
                if (restarted_plausible) {
                    __android_log_print(
                            ANDROID_LOG_WARN, kTag,
                            "AAudio resume epoch is ambiguous: timestamp=%lld continuing_queue=%lld restarted_queue=%lld; choosing conservative continuing epoch",
                            static_cast<long long>(timestamp_frame),
                            static_cast<long long>(continuing_queue),
                            static_cast<long long>(restarted_queue));
                }
                resolved_origin = continuing_origin;
            } else {
                // With more than one plausible candidate, this path can only
                // be reached with the restart and stream-lifetime candidates.
                restarted = true;
                resolved_origin = restart_origin;
            }
        } else {
            log_unresolved_epoch();
            return false;
        }
    }

    device_frame_origin_submitted_.store(resolved_origin, std::memory_order_release);
    device_epoch_mode_.store(
            static_cast<int>(resolved_origin == 0
                    ? DeviceEpochMode::kStreamLifetime
                    : DeviceEpochMode::kResumeRelative),
            std::memory_order_release);
    if (restarted) {
        __android_log_print(
                ANDROID_LOG_INFO, kTag,
                "AAudio timestamp epoch restarted: before_pause=%lld after_resume=%lld app_origin=%lld queue_frames=%lld",
                static_cast<long long>(reference),
                static_cast<long long>(timestamp_frame),
                static_cast<long long>(resolved_origin),
                static_cast<long long>(restarted_queue));
    }
    return true;
}

void AudioSink::LogUntrustedLatency(double latency_seconds, int64_t written_frames,
                                    double presented_frames) {
    const int64_t now_nanoseconds = MonotonicNanoseconds();
    int64_t last_warning =
            last_latency_warning_nanoseconds_.load(std::memory_order_relaxed);
    if (now_nanoseconds - last_warning >= kDebugLogIntervalNanoseconds &&
        last_latency_warning_nanoseconds_.compare_exchange_strong(
                last_warning, now_nanoseconds, std::memory_order_acq_rel)) {
        __android_log_print(
                ANDROID_LOG_WARN, kTag,
                "AAudio output latency is not trustworthy: latency_ms=%.3f written=%lld presented=%.3f; waiting for a same-epoch projection",
                latency_seconds * 1000.0, static_cast<long long>(written_frames),
                presented_frames);
    }
}

bool AudioSink::CaptureDeviceAnchor(int64_t candidate_app_frame,
                                    int64_t current_app_frame,
                                    double* device_frame,
                                    double* presented_frames,
                                    int64_t* timestamp_frame,
                                    double* output_latency_ms,
                                    bool* relative_epoch) {
    if (device_frame == nullptr || presented_frames == nullptr ||
        timestamp_frame == nullptr || output_latency_ms == nullptr ||
        relative_epoch == nullptr || stream_ == nullptr || sample_rate_ <= 0) {
        return false;
    }

    int64_t frame_position = -1;
    int64_t timestamp_nanoseconds = -1;
    if (AAudioStream_getTimestamp(stream_, CLOCK_MONOTONIC, &frame_position,
                                  &timestamp_nanoseconds) != AAUDIO_OK) {
        return false;
    }
    const int64_t now_nanoseconds = MonotonicNanoseconds();
    if (frame_position < 0 || timestamp_nanoseconds <= 0 ||
        timestamp_nanoseconds > now_nanoseconds + 50000000LL ||
        !ResolveDeviceEpoch(frame_position, current_app_frame)) {
        return false;
    }

    const auto mode = static_cast<DeviceEpochMode>(
            device_epoch_mode_.load(std::memory_order_acquire));
    const int64_t origin =
            device_frame_origin_submitted_.load(std::memory_order_acquire);
    const int64_t candidate_device_frame = candidate_app_frame - origin;
    const int64_t current_write_head = current_app_frame - origin;
    if (candidate_device_frame < 0 || current_write_head < 0 ||
        candidate_device_frame > current_write_head) {
        return false;
    }

    const double timestamp_age_seconds = std::max(
            0.0, static_cast<double>(now_nanoseconds - timestamp_nanoseconds) /
                         kNanosecondsPerSecond);
    const double presented = static_cast<double>(frame_position) +
                             timestamp_age_seconds * sample_rate_;
    const double latency_frames =
            static_cast<double>(candidate_device_frame) - presented;
    const double latency_seconds = latency_frames / sample_rate_;
    if (latency_seconds > kMaxTrustedOutputLatencySeconds) {
        LogUntrustedLatency(latency_seconds, AAudioStream_getFramesWritten(stream_),
                            presented);
        return false;
    }

    *device_frame = static_cast<double>(candidate_device_frame);
    *presented_frames = presented;
    *timestamp_frame = frame_position;
    *output_latency_ms = std::max(0.0, latency_seconds) * 1000.0;
    *relative_epoch = mode == DeviceEpochMode::kResumeRelative;
    return true;
}

void AudioSink::LogAnchorRejection(uint64_t generation,
                                   double implied_start_position,
                                   double candidate_pts_seconds,
                                   double expected_start_pts_seconds,
                                   double deviation_seconds,
                                   int rejection_count) {
    const int64_t now_nanoseconds = MonotonicNanoseconds();
    int64_t last_warning =
            last_anchor_warning_nanoseconds_.load(std::memory_order_relaxed);
    if (now_nanoseconds - last_warning >= kAnchorWarningIntervalNanoseconds &&
        last_anchor_warning_nanoseconds_.compare_exchange_strong(
                last_warning, now_nanoseconds, std::memory_order_acq_rel)) {
        __android_log_print(
                ANDROID_LOG_WARN, kTag,
                "Rejecting audio clock anchor: implied_start=%.3f pcm_pts=%.3f expected_start=%.3f deviation_ms=%.3f generation=%llu consistent=%d/%d",
                implied_start_position, candidate_pts_seconds,
                expected_start_pts_seconds, deviation_seconds * 1000.0,
                static_cast<unsigned long long>(generation), rejection_count,
                kConsistentAnchorAcceptanceCount);
    }
}

bool AudioSink::AcceptAnchorForExpectedStart(uint64_t generation,
                                             double implied_start_position,
                                             double candidate_pts_seconds) {
    if (expected_start_generation_.load(std::memory_order_acquire) != generation) {
        return true;
    }
    const double expected_start =
            expected_start_pts_seconds_.load(std::memory_order_acquire);
    if (!std::isfinite(expected_start)) {
        return true;
    }
    const double deviation = std::abs(implied_start_position - expected_start);
    if (deviation <= kMaxAnchorDeviationSeconds) {
        rejected_anchor_count_.store(0, std::memory_order_release);
        rejected_anchor_generation_.store(kInvalidGeneration,
                                           std::memory_order_release);
        return true;
    }

    const uint64_t rejected_generation =
            rejected_anchor_generation_.load(std::memory_order_acquire);
    const double previous =
            rejected_anchor_start_seconds_.load(std::memory_order_acquire);
    int count = 1;
    if (rejected_generation == generation && std::isfinite(previous) &&
        std::abs(previous - implied_start_position) <=
                kConsistentAnchorToleranceSeconds) {
        count = rejected_anchor_count_.load(std::memory_order_acquire) + 1;
    }
    rejected_anchor_start_seconds_.store(implied_start_position,
                                         std::memory_order_release);
    rejected_anchor_count_.store(count, std::memory_order_release);
    rejected_anchor_generation_.store(generation, std::memory_order_release);

    if (count < kConsistentAnchorAcceptanceCount) {
        LogAnchorRejection(generation, implied_start_position,
                           candidate_pts_seconds, expected_start, deviation, count);
        return false;
    }

    __android_log_print(
            ANDROID_LOG_WARN, kTag,
            "Accepting consistent audio anchor despite stale start hint: implied_start=%.3f expected_start=%.3f deviation_ms=%.3f generation=%llu samples=%d",
            implied_start_position, expected_start, deviation * 1000.0,
            static_cast<unsigned long long>(generation), count);
    return true;
}

void AudioSink::TryPublishPendingAnchor(uint64_t generation,
                                        int64_t current_app_frame) {
    if (timeline_reset_in_progress_.load(std::memory_order_acquire) ||
        timeline_generation_.load(std::memory_order_acquire) != generation ||
        anchor_generation_.load(std::memory_order_acquire) == generation ||
        pending_anchor_generation_.load(std::memory_order_acquire) == generation ||
        candidate_generation_.load(std::memory_order_acquire) != generation) {
        return;
    }

    const int64_t candidate_app_frame =
            candidate_app_frame_.load(std::memory_order_acquire);
    const double candidate_pts =
            candidate_pts_seconds_.load(std::memory_order_acquire);
    if (!std::isfinite(candidate_pts)) {
        return;
    }

    double device_frame = std::numeric_limits<double>::quiet_NaN();
    double presented_frames = std::numeric_limits<double>::quiet_NaN();
    int64_t timestamp_frame = -1;
    double output_latency_ms = -1.0;
    bool relative_epoch = false;
    if (!CaptureDeviceAnchor(candidate_app_frame, current_app_frame,
                             &device_frame, &presented_frames, &timestamp_frame,
                             &output_latency_ms, &relative_epoch)) {
        return;
    }

    double elapsed_since_hint = 0.0;
    const int64_t hint_time =
            expected_start_time_nanoseconds_.load(std::memory_order_acquire);
    if (hint_time > 0) {
        elapsed_since_hint = std::max(
                0.0, static_cast<double>(MonotonicNanoseconds() - hint_time) /
                             kNanosecondsPerSecond);
    }
    const double implied_current_position = candidate_pts +
            (presented_frames - device_frame) / sample_rate_;
    const double implied_start_position =
            implied_current_position - elapsed_since_hint;
    if (!AcceptAnchorForExpectedStart(generation, implied_start_position,
                                      candidate_pts)) {
        return;
    }

    if (timeline_reset_in_progress_.load(std::memory_order_acquire) ||
        timeline_generation_.load(std::memory_order_acquire) != generation) {
        return;
    }

    pending_anchor_frame_.store(device_frame, std::memory_order_release);
    pending_anchor_pts_seconds_.store(candidate_pts, std::memory_order_release);
    pending_anchor_timestamp_frame_.store(timestamp_frame,
                                          std::memory_order_release);
    pending_anchor_output_latency_ms_.store(output_latency_ms,
                                            std::memory_order_release);
    pending_anchor_relative_epoch_.store(relative_epoch,
                                         std::memory_order_release);
    pending_anchor_generation_.store(generation, std::memory_order_release);

    if (timeline_reset_in_progress_.load(std::memory_order_acquire) ||
        timeline_generation_.load(std::memory_order_acquire) != generation) {
        uint64_t expected = generation;
        pending_anchor_generation_.compare_exchange_strong(
                expected, kInvalidGeneration, std::memory_order_acq_rel);
    }
}

aaudio_data_callback_result_t AudioSink::OnData(void* audio_data, int32_t num_frames) {
    auto* output = static_cast<int16_t*>(audio_data);
    uint64_t generation = timeline_generation_.load(std::memory_order_acquire);
    double first_pts = std::numeric_limits<double>::quiet_NaN();
    const std::size_t requested = static_cast<std::size_t>(num_frames);
    const bool reset_in_progress =
            timeline_reset_in_progress_.load(std::memory_order_acquire);
    const std::size_t copied = reset_in_progress
            ? 0 : pcm_ring_.Read(output, requested, &first_pts);
    if (copied < requested) {
        std::memset(output + copied * 2, 0,
                    (requested - copied) * 2 * sizeof(int16_t));
    }

    const int64_t first_frame = submitted_frames_.fetch_add(
            num_frames, std::memory_order_acq_rel);
    const uint64_t callbacks = callback_count_.fetch_add(
            1, std::memory_order_acq_rel) + 1;
    const bool had_underrun = underrun_since_pcm_.load(std::memory_order_acquire);
    const bool real_pcm = copied > 0 && std::isfinite(first_pts);
    bool may_publish = !reset_in_progress;

    if (real_pcm && may_publish &&
        timeline_generation_.load(std::memory_order_acquire) == generation) {
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
            uint64_t next_generation = kInvalidGeneration;
            if (!InvalidateTimelineFromCallback(generation, &next_generation)) {
                may_publish = false;
            } else {
                generation = next_generation;
                if (pts_discontinuity) {
                    __android_log_print(
                            ANDROID_LOG_INFO, kTag,
                            "audio PTS discontinuity: expected=%.3f got=%.3f, re-anchoring",
                            expected_pts, first_pts);
                } else {
                    // The resumed PCM is authoritative; do not seed the new
                    // generation from a stale pre-underrun expected PTS.
                    __android_log_print(
                            ANDROID_LOG_INFO, kTag,
                            "audio PCM resumed after underrun: pts=%.3f, re-anchoring",
                            first_pts);
                }
            }
        }

        if (may_publish &&
            !timeline_reset_in_progress_.load(std::memory_order_acquire) &&
            timeline_generation_.load(std::memory_order_acquire) == generation) {
            expected_pcm_pts_seconds_.store(
                    first_pts + static_cast<double>(copied) / sample_rate_,
                    std::memory_order_release);
            expected_pcm_generation_.store(generation, std::memory_order_release);

            if (anchor_generation_.load(std::memory_order_acquire) != generation &&
                pending_anchor_generation_.load(std::memory_order_acquire) != generation &&
                candidate_generation_.load(std::memory_order_acquire) != generation) {
                candidate_app_frame_.store(first_frame, std::memory_order_release);
                candidate_pts_seconds_.store(first_pts, std::memory_order_release);
                candidate_generation_.store(generation, std::memory_order_release);
            }
        }
    }

    if (may_publish &&
        !timeline_reset_in_progress_.load(std::memory_order_acquire) &&
        timeline_generation_.load(std::memory_order_acquire) == generation) {
        // This also runs on silence callbacks, allowing a retained short PCM
        // burst to anchor once AAudio begins returning timestamps.
        TryPublishPendingAnchor(generation, first_frame);
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
        } else if (candidate_generation_.load(std::memory_order_acquire) ==
                   current_generation) {
            anchor_state = "candidate";
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
