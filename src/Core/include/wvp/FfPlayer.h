#pragma once
// IMediaPlayer over libffplay.
//
// Threading model
//  * Create may run on any thread, the UI thread included: ffp_create needs nothing from the caller's thread
//    (the presenter is made beforehand by VideoPlatform::CreatePresenter). The
//    first player of the process also initialises SDL audio inside ffp_create (tens of ms on some drivers); a host that
//    wants its first frame of UI earlier calls Create on a worker and hands the player to the UI thread itself.
//  * libffplay raises events on its own threads. FfPlayer parses them, queues them and, once per batch, hands
//    `post` a closure that dispatches the batch. The closure looks the player up by id, so a closure that runs after
//    the player was destroyed does nothing. `post` must run the closure later on the UI thread, never inline: it is
//    called on libffplay's threads, where IMediaPlayer events must not be raised.
//  * All control methods and the destructor are meant to be called from the UI thread. The destructor blocks until
//    libffplay has stopped (ffp_destroy), then destroys the presenter; queued closures become no-ops.
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "wvp/CapabilitySet.h"
#include "wvp/DebugInfo.h"
#include "wvp/IMediaPlayer.h"
#include "wvp/VideoPlatform.h"

struct FFPlayer;
struct AVBufferRef;

namespace wvp {

// Runs fn later on the UI thread (App: App::PostToUi). Called from any thread; never runs fn inline.
using UiPost = std::function<void(std::function<void()>)>;

class FfPlayer final : public IMediaPlayer {
public:
    // presenter may be null (no video output: pictures are timed and dropped; tests, audio-only hosts). err: 0, or
    // -1 when ffp_create failed. Creation options: audio_driver (process-wide, see ffplay.h).
    static std::unique_ptr<FfPlayer> Create(PresenterHandle presenter, UiPost post, int* err = nullptr,
                                            const std::string& audioDriver = {});

    static std::string Version();
    static void SetLogLevel(int avLogLevel);   // AV_LOG_* (default 24 = warning)
    // Process-wide log sink (ffp_set_log_callback): one formatted UTF-8 line per call, from any thread, level = AV_LOG_*.
    // nullptr restores the default stderr logger.
    using LogCallback = std::function<void(int level, const std::string& line)>;
    static void SetLogCallback(LogCallback cb);

    // libffplay's portable probe (ffp_probe_caps) on the given devices: devices, build decoders, interop, features.
    // Blocking, any thread, not cached. Used by VideoPlatform::Probe implementations; nullopt for an empty or invalid
    // answer.
    static std::optional<CapabilitySet> ProbeEngine(AVBufferRef* const* devices, int count);

    // Builds the '\n'-separated "key=value" UTF-8 string for ffp_open. Returns false for invalid keys/values.
    static bool BuildOptionsString(const OpenOptions& options, std::string& out);

    ~FfPlayer() override;
    FfPlayer(const FfPlayer&) = delete;
    FfPlayer& operator=(const FfPlayer&) = delete;

    // Another presenter (other adapter, adapter removed): stops the current media (ffp_set_output), then destroys the
    // previous presenter. The next Open uses the new one. A lost device does not need this: reopening is enough.
    int SetPresenter(PresenterHandle presenter);
    // The presenter in use (nullptr when none), e.g. for the host's platform-specific calls on its concrete type.
    Presenter* CurrentPresenter() const { return presenter_.get(); }

    PlayerState State() const override { return state_.load(); }
    int Volume() const override { return volume_.load(); }
    void SetVolume(int v) override;
    bool IsMuted() const override { return muted_.load(); }
    void SetMuted(bool m) override;
    double Position() override;
    double Duration() override;
    int Open(const std::string& url, const OpenOptions& options = {}) override;
    int Stop() override;
    int Play() override;
    int Pause() override;
    int TogglePause() override;
    int Seek(double seconds, bool relative = false) override;
    // Asynchronous output-frame step; completion is FrameStepDone. FFP_EBUSY while a step is in flight.
    int StepFrame(int direction) override;
    // Asynchronous previous / next keyframe seek; completion is KeyframeSeekDone. FFP_EBUSY while a step or keyframe seek is in flight.
    int SeekKeyframe(int direction) override;
    // Asynchronous timed skip to the closest key packet; completion is KeyframeSeekDone. Same slot as SeekKeyframe.
    int Skip(double seconds) override;
    int CycleStream(StreamType type) override;
    int SelectStream(StreamType type, int index) override;
    int AddSubtitleFile(const std::string& utf8Path) override;
    int SetSubtitleDelay(std::int64_t ms) override;
    std::int64_t SubtitleDelay() const override;
    int SetSubtitleStyle(const SubtitleRenderStyle& style) override;
    int SetAudioPreference(int pref) override;
    int AudioPreference() const override { return audioPref_.load(); }
    int SetSubtitleVisible(SubtitleLayer layer, bool on) override;
    // Reopens only the ARIB caption decoder; video/audio are untouched.
    int RefreshCaptionStyle(bool ignoreBackground, bool forceOutline);
    int SetVideoFilter(const std::string& vf) override;
    int SetAudioFilter(const std::string& af) override;
    int SendFilterCommand(const std::string& target, const std::string& command, const std::string& argument) override;
    std::optional<MediaInfo> GetMediaInfo() override;
    std::optional<BgraFrame> Snapshot() override;
    std::optional<BgraFrame> Snapshot(bool withSubtitles) override;
    std::optional<std::string> GetDebugInfoJson() override;
    // Id the posted closures look the player up by (unique per process).
    std::uint64_t Id() const { return id_; }

private:
    FfPlayer(PresenterHandle presenter, UiPost post);
    static void OnNativeEvent(void* user, int ev, const char* json);
    void HandleNative(int ev, const char* json);
    void Enqueue(std::function<void()> fn);   // queues fn; posts one dispatch closure per batch
    void DispatchPendingEvents();             // UI thread, from the posted closure

    ::FFPlayer* p_ = nullptr;
    PresenterHandle presenter_;               // destroyed after ffp_destroy / ffp_set_output released it
    const std::uint64_t id_;
    UiPost post_;
    std::atomic<bool> closing_{false};
    std::atomic<bool> notifyPending_{false};
    std::atomic<PlayerState> state_{PlayerState::Idle};
    std::atomic<int> volume_{100};
    std::atomic<bool> muted_{false};
    std::atomic<int> audioPref_{0};
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::mutex qmu_;
    std::deque<std::function<void()>> queue_;
};

}
