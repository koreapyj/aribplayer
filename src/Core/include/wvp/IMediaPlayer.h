#pragma once
// Pure interface (plus public multicast events) so playback logic can be tested with a fake player.
// Control methods return 0 on success or a negative AVERROR.
// Events are raised on the UI thread (FfPlayer runs them through its UiPost).
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Event.h"
#include "PlayerTypes.h"
#include "SubtitleStyle.h"

namespace wvp {

// Ordered key=value open options (ffplay option names without '-', AVFormat/AVCodec options, ...).
using OpenOptions = std::vector<std::pair<std::string, std::string>>;

enum class SubtitleLayer : int { Caption = 0, Superimpose = 1 };

class IMediaPlayer {
public:
    virtual ~IMediaPlayer() = default;

    Event<const MediaPreparedInfo&> Prepared;
    Event<double, double> PositionChanged;        // position, duration (seconds)
    Event<PlayerState> StateChanged;
    Event<bool> BufferingChanged;
    Event<> Ended;
    Event<const PlayerErrorInfo&> Error;
    Event<const FilterErrorInfo&> FilterError;
    Event<> MediaInfoChanged;                     // GetMediaInfo now differs (the video field order was decided from decoded frames)
    Event<const PresenterErrorInfo&> PresenterError;   // the presenter stopped for this open; the host reopens
    Event<int, bool> FrameStepDone;               // StepFrame finished (after its PositionChanged): direction, a frame was presented
    Event<int, bool, double, double> KeyframeSeekDone;   // SeekKeyframe finished (after its PositionChanged): direction, a keyframe was presented,
                                                  // presented output position and its source PTS (media-relative seconds, -1 when none)

    virtual PlayerState State() const = 0;
    virtual int Volume() const = 0;               // 0..100, persists across Open
    virtual void SetVolume(int v) = 0;
    virtual bool IsMuted() const = 0;             // persists across Open
    virtual void SetMuted(bool m) = 0;
    virtual double Position() = 0;                // seconds, NaN if unknown
    virtual double Duration() = 0;                // seconds, <= 0 if unknown/live

    virtual int Open(const std::string& url, const OpenOptions& options = {}) = 0;
    virtual int Stop() = 0;
    virtual int Play() = 0;
    virtual int Pause() = 0;
    virtual int TogglePause() = 0;
    virtual int Seek(double seconds, bool relative = false) = 0;
    // Asynchronous output-frame step (+1/-1); completion is reported by FrameStepDone. One step at a time: returns
    // FFP_EBUSY (-16) while one is in flight (the request is dropped). Unsupported backends return -1.
    virtual int StepFrame(int direction) { (void)direction; return -1; }
    // Asynchronous seek to the previous (-1) / next (+1) keyframe of the video relative to the displayed frame, for
    // playing and paused alike; completion is KeyframeSeekDone (moved = false at the first / last keyframe). Shares the
    // one-at-a-time slot with StepFrame: FFP_EBUSY (-16) while either is in flight. Unsupported backends return -1.
    virtual int SeekKeyframe(int direction) { (void)direction; return -1; }
    // Asynchronous relative skip of `seconds` (signed) to the video key packet closest to the displayed frame + seconds,
    // honouring the direction (a backward skip lands before the displayed frame, a forward one after it); same slot and
    // KeyframeSeekDone completion as SeekKeyframe (moved = false when no key packet qualifies; also for media without a
    // seekable video stream, where a plain relative Seek is queued instead). Unsupported backends return -1.
    virtual int Skip(double seconds) { (void)seconds; return -1; }
    virtual int CycleStream(StreamType type) = 0;
    /// Selects a stream by MediaInfo index; subtitle index -1 turns the caption layer's track off. Unsupported backends return -22.
    virtual int SelectStream(StreamType type, int index) { (void)type; (void)index; return -22; }
    /// Preloads a subtitle file (after PREPARED); returns the first new stream index or a negative error. Does not select.
    virtual int AddSubtitleFile(const std::string& utf8Path) { (void)utf8Path; return -22; }
    /// Milliseconds, positive = subtitles later; reset to 0 by Open().
    virtual int SetSubtitleDelay(std::int64_t ms) { (void)ms; return -22; }
    virtual std::int64_t SubtitleDelay() const { return 0; }
    virtual int SetSubtitleStyle(const SubtitleRenderStyle& style) { (void)style; return -22; }
    /// ARIB TS (MediaInfo::AribAudio.Present): 0 = audio 1, 1 = audio 2 wherever it has samples and audio 1 otherwise
    /// (gapless, never announced). Every Open() starts on audio 1. Unsupported backends return -22.
    virtual int SetAudioPreference(int pref) { (void)pref; return -22; }
    virtual int AudioPreference() const { return 0; }
    /// Hides/shows a decoded subtitle layer without stopping its decoder. Unsupported backends return -22.
    virtual int SetSubtitleVisible(SubtitleLayer layer, bool on) { (void)layer; (void)on; return -22; }
    virtual int SetVideoFilter(const std::string& vf) = 0;   // empty = none
    virtual int SetAudioFilter(const std::string& af) = 0;
    virtual int SendFilterCommand(const std::string& target, const std::string& command, const std::string& argument) = 0;
    virtual std::optional<MediaInfo> GetMediaInfo() = 0;
    // nullopt when no frame was displayed yet (or on error).
    virtual std::optional<BgraFrame> Snapshot() = 0;
    /// Captures with optional subtitle composition. The default implementation captures video only.
    virtual std::optional<BgraFrame> Snapshot(bool withSubtitles) { (void)withSubtitles; return Snapshot(); }
    // UTF-8 JSON snapshot of live decoder/filter/sync state (libffplay ffp_get_debug_info, see wvp/DebugInfo.h),
    // nullopt when unsupported or nothing is open. Default: unsupported.
    virtual std::optional<std::string> GetDebugInfoJson() { return std::nullopt; }
};

}
