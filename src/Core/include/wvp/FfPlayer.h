#pragma once
// IMediaPlayer over libffplay.dll.
//
// Threading model
//  * ffp_create blocks until SDL has created its window from our HWND, so it MUST NOT run on the UI thread
//    while that thread is not pumping messages. Use CreateAsync (worker thread + completion on the UI thread)
//    or CreateSync (pumps messages while waiting).
//  * libffplay raises events on its own threads. FfPlayer parses them, queues them and posts
//    (notifyHwnd, notifyMsg, wParam = kNotifyEvents, lParam = player id). The UI window procedure answers
//    that message by calling FfPlayer::HandleNotify(wParam, lParam) (or player.DispatchPendingEvents()),
//    which raises the IMediaPlayer Event<> members on the UI thread. Stale ids are ignored.
//  * All control methods and the destructor are meant to be called from the UI thread. The destructor blocks
//    until the native player thread has stopped and is safe when the notify window is already gone.
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "CapabilitySet.h"
#include "DebugInfo.h"
#include "IMediaPlayer.h"

struct FFPlayer;

namespace wvp {

class FfPlayer final : public IMediaPlayer {
public:
    static constexpr WPARAM kNotifyEvents = 1;        // lParam = player id: call HandleNotify -> DispatchPendingEvents
    static constexpr WPARAM kNotifyCreateDone = 2;    // lParam = create id: call HandleNotify -> completion callback

    using CreateDone = std::function<void(std::unique_ptr<FfPlayer> player, int err)>;

    // `done` is invoked on the UI thread from HandleNotify(kNotifyCreateDone) (so notifyHwnd/notifyMsg are REQUIRED).
    // On failure player is null and err is negative. If notifyHwnd is destroyed before completion the player is
    // destroyed quietly.
    // videoHwnd: host child window for video; NULL makes libffplay create its own top-level window.
    static void CreateAsync(HWND videoHwnd, HWND notifyHwnd, UINT notifyMsg, CreateDone done);

    // Same as CreateAsync but blocks the caller while pumping its message queue (for tests / simple hosts).
    // Returns null on failure or timeout (*err receives the code; -22 = API mismatch, -1 = ffp_create failed, -110 = timeout).
    static std::unique_ptr<FfPlayer> CreateSync(HWND videoHwnd, HWND notifyHwnd, UINT notifyMsg,
                                                int* err = nullptr, DWORD timeoutMs = 20000);

    // Call from the notify message handler: WM_APP-style `case notifyMsg: FfPlayer::HandleNotify(wp, lp);`
    static void HandleNotify(WPARAM wParam, LPARAM lParam);

    // UI thread only.
    void DispatchPendingEvents();

    static std::string Version();
    static void SetLogLevel(int avLogLevel);   // AV_LOG_* (default 24 = warning)
    // Process-wide log sink (ffp_set_log_callback): one formatted UTF-8 line per call, from any thread, level = AV_LOG_*.
    // nullptr restores the default stderr logger.
    using LogCallback = std::function<void(int level, const std::string& line)>;
    static void SetLogCallback(LogCallback cb);

    // Capability probe (ffp_probe_caps), run once per process and cached. ProbeCapsJson returns "" when the probe returns
    // no answer. ProbeCaps merges it with BuildCapabilities and returns nullopt for an empty or invalid answer. The first
    // call blocks (device creation, decoder and interop tests): call it OFF the UI thread. Thread-safe.
    static std::string ProbeCapsJson();
    static std::optional<CapabilitySet> ProbeCaps();
    // ffp_probe_caps_opts options ('\n'-separated key=value: d3d11_adapter_luid, d3d11_force_caps, d3d11_debug) for the
    // capability probe of this process. Call before the first probe.
    static void SetProbeOptions(const std::string& optionsKv);

    // Builds the '\n'-separated "key=value" UTF-8 string for ffp_open. Returns false for invalid keys/values.
    static bool BuildOptionsString(const OpenOptions& options, std::string& out);

    ~FfPlayer() override;
    FfPlayer(const FfPlayer&) = delete;
    FfPlayer& operator=(const FfPlayer&) = delete;

    PlayerState State() const override { return state_.load(); }
    int Volume() const override { return volume_.load(); }
    void SetVolume(int v) override;
    bool IsMuted() const override { return muted_.load(); }
    void SetMuted(bool m) override;
    double Position() override;
    double Duration() override;
    int Open(const std::wstring& url, const OpenOptions& options = {}) override;
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
    int SetAudioPreference(int pref) override;
    int AudioPreference() const override { return audioPref_.load(); }
    int SetSubtitleVisible(SubtitleLayer layer, bool on) override;
    // Reopens only the ARIB caption decoder; video/audio are untouched.
    int RefreshCaptionStyle(bool ignoreBackground, bool forceOutline);
    int SetVideoFilter(const std::wstring& vf) override;
    int SetAudioFilter(const std::wstring& af) override;
    int SendFilterCommand(const std::wstring& target, const std::wstring& command, const std::wstring& argument) override;
    std::optional<MediaInfo> GetMediaInfo() override;
    std::optional<BgraFrame> Snapshot() override;
    std::optional<BgraFrame> Snapshot(bool withSubtitles) override;
    std::optional<std::string> GetDebugInfoJson() override;
    // ffp_set_presenter_params; returns FFP_EINVAL if the active presenter cannot apply the parameters.
    int SetPresenterParams(const PresenterParams& p) override;
    // ffp_set_backdrop_regions; returns FFP_EINVAL for invalid regions or when the active presenter is not D3D11.
    int SetBackdropRegions(const BackdropRegion* regions, int count) override;

    // Id used in notify messages (unique per process).
    std::uint64_t Id() const { return id_; }

private:
    FfPlayer(HWND notifyHwnd, UINT notifyMsg);
    static void CreateOnWorker(HWND videoHwnd, HWND notifyHwnd, UINT notifyMsg,
                               std::function<void(std::unique_ptr<FfPlayer>, int)> workerDone);
    static void OnNativeEvent(void* user, int ev, const char* json);
    void HandleNative(int ev, const char* json);
    void Enqueue(std::function<void()> fn);
    void Post(WPARAM wp, LPARAM lp);

    ::FFPlayer* p_ = nullptr;
    const std::uint64_t id_;
    HWND notifyHwnd_;
    UINT notifyMsg_;
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
