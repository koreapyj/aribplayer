#pragma once
// Pure interface (plus public multicast events) so playback logic can be tested with a fake player.
// Control methods return 0 on success or a negative AVERROR.
// Events are raised on the thread that calls FfPlayer::DispatchPendingEvents() (the UI thread).
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Event.h"
#include "PlayerTypes.h"

namespace wvp {

// Ordered key=value open options (ffplay option names without '-', AVFormat/AVCodec options, ...).
using OpenOptions = std::vector<std::pair<std::wstring, std::wstring>>;

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
    Event<const InputEventArgs&> Input;
    Event<const FilterErrorInfo&> FilterError;
    Event<const PresenterErrorInfo&> PresenterError;   // the d3d11 presenter failed; libffplay fell back to SDL
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

    virtual int Open(const std::wstring& url, const OpenOptions& options = {}) = 0;
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
    /// ARIB TS (MediaInfo::AribAudio.Present): 0 = audio 1, 1 = audio 2 wherever it has samples and audio 1 otherwise
    /// (gapless, never announced). Every Open() starts on audio 1. Unsupported backends return -22.
    virtual int SetAudioPreference(int pref) { (void)pref; return -22; }
    virtual int AudioPreference() const { return 0; }
    /// Hides/shows a decoded subtitle layer without stopping its decoder. Unsupported backends return -22.
    virtual int SetSubtitleVisible(SubtitleLayer layer, bool on) { (void)layer; (void)on; return -22; }
    virtual int SetVideoFilter(const std::wstring& vf) = 0;   // empty = none
    virtual int SetAudioFilter(const std::wstring& af) = 0;
    virtual int SendFilterCommand(const std::wstring& target, const std::wstring& command, const std::wstring& argument) = 0;
    virtual std::optional<MediaInfo> GetMediaInfo() = 0;
    // Live presenter parameters (rotation/flip/eq the d3d11 presenter applies itself). 0 or a negative error: FFP_EINVAL-like
    // (-22) when the presenter cannot apply them (state unchanged). Default: unsupported.
    virtual int SetPresenterParams(const PresenterParams&) { return -22; }
    // Acrylic backdrop regions; count 0 clears. -22 when unsupported. Default: unsupported.
    virtual int SetBackdropRegions(const BackdropRegion*, int) { return -22; }
    // nullopt when no frame was displayed yet (or on error).
    virtual std::optional<BgraFrame> Snapshot() = 0;
    /// Captures with optional subtitle composition. The default implementation captures video only.
    virtual std::optional<BgraFrame> Snapshot(bool withSubtitles) { (void)withSubtitles; return Snapshot(); }
    // UTF-8 JSON snapshot of live decoder/filter/sync state (libffplay ffp_get_debug_info, see wvp/DebugInfo.h),
    // nullopt when unsupported or nothing is open. Default: unsupported.
    virtual std::optional<std::string> GetDebugInfoJson() { return std::nullopt; }
};

}
