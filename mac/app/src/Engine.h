#pragma once
// The playback engine side of app::App on macOS: the Mac counterpart of App::Create's platform + startup probe,
// AppVideoGpu.cpp (ProbeAdapters, SelectVideoGpuNow, MakePresenter / UsePresenterOn, device loss, presenter errors,
// display changes), AppPipeline.cpp (WirePipeline, OnPipelinePrepared, PipelinePoll, PipelineFallback) and
// AppPlayback.cpp's InitPlayer (FfPlayer::Create with the presenter on the display adapter). Same decisions, same Core
// calls (wvp::SelectVideoGpu, FilterManager, PipelineSession, FfPlayer); only the platform (wvp::mac::MacVideoPlatform)
// is Mac-side; Log() writes stderr lines "[area] text" and the app log sink (features/Logging: log::Event).
//
// Threads: everything on the main thread except the adapter probe, which runs on its own thread and posts its result.
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "CaptionFont.h"
#include "wvp/FfPlayer.h"
#include "wvp/FilterSettings.h"
#include "wvp/PipelineSession.h"
#include "wvp/PlaybackController.h"
#include "wvp/VideoGpu.h"

namespace app { struct Services; }
namespace wvp::mac { class MacVideoPlatform; class MetalPresenter; }

namespace mainview {

void Log(const std::string& area, const std::string& line);

class Engine {
public:
    using Post = std::function<void(std::function<void()>)>;

    /// videoView: the NSView* (CAMetalLayer-backed) of the video area. post: queued run on the main thread, never inline.
    Engine(app::Services& svc, void* videoView, Post post);
    ~Engine();   // joins the probe thread; the player must be released first (ReleasePlayer)
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    /// App::Create: adapter list + VideoPlatform::Probe of each on a thread; OnAdaptersProbed runs on the main thread.
    void StartProbe(const std::string& why);
    /// App::InitPlayer: the presenter on the display adapter (SelectVideoGpuNow moves it once the probe is done) and
    /// FfPlayer::Create. audioDriver: "" = SDL's default (CoreAudio). False with *error.
    bool CreatePlayer(const std::string& audioDriver, std::string* error);
    /// App::WirePipeline (+ the open hooks of WirePlayer): plan provider, beforeOpen / onOpening / onFilterChange, the
    /// player's Prepared / Error / FilterError / PresenterError. The controller must live until ReleasePlayer.
    void Wire(wvp::PlaybackController& ctl);
    /// Destroys the FfPlayer (blocks until libffplay stopped; then its presenter). After the controller was disposed.
    void ReleasePlayer();

    wvp::FfPlayer* Player() const { return player_.get(); }
    /// The presenter in use (FfPlayer::CurrentPresenter as the Mac type), nullptr when none.
    wvp::mac::MetalPresenter* Presenter() const;
    /// Main thread, after the video view's drawableSize / contentsScale changed: ffp_metal_presenter_view_changed.
    void ViewChanged();
    /// The NSWindow moved to another screen (App::OnDisplayChange).
    void DisplayChanged();
    /// Periodic (main thread): App::PipelinePoll (warm-up fallbacks during the first 10 s of an open).
    void Poll(std::uint64_t nowMs);

    /// FiltersPanel's apply path: FilterManager::Apply + the live change to the controller.
    void ApplyFilterSettings(const wvp::FilterSettings& next);
    /// Settings > Playback > Video GPU changed (App::VideoGpuSettingChanged).
    void VideoGpuSettingChanged();

    bool Probed() const { return capsProbed_; }
    const std::vector<wvp::AdapterCandidate>& Candidates() const { return adapters_; }
    const wvp::VideoGpuChoice& Choice() const { return videoChoice_; }
    const std::string& LastOpenUrl() const { return lastOpenUrl_; }
    const wvp::OpenOptions& LastOpenOptions() const { return lastOpenOpts_; }
    const AribFont& CaptionFont() const { return font_; }
    /// FFPPresenter.stats_json of the presenter in use ("{}" when none). Its counters are cumulative over the presenter's
    /// lifetime (every open); PresenterStatsAtPrepared is the snapshot taken at the last PREPARED, for per-media deltas.
    std::string PresenterStats() const;
    const std::string& PresenterStatsAtPrepared() const { return statsAtPrepared_; }
    std::uint64_t StartupProbeMs() const { return startupProbeMs_; }

    static std::string FormatOptions(const wvp::OpenOptions& o);   // "k=v k=v": features/LoggingFormat's FormatOptionsLine
    /// The last open's options with the live vf / af of the latest filter change (LoggingFormat EffectiveOptions).
    wvp::OpenOptions EffectiveOpenOptions() const;
    /// The debug overlay's "adapter:" row (App::BuildDebugContext); *warn as adapterWarn.
    std::string AdapterNote(bool* warn) const;

    /// Notice for the user (App::ReportPlanProblem / ShowPlaybackError): title, message.
    std::function<void(const std::wstring&, const std::wstring&)> onProblem;
    /// The adapter probe finished (after the pending open ran).
    std::function<void()> onProbed;
    /// FilterManager content changed (including beforeOpen); the AppContext owner refreshes its Filters panel.
    std::function<void()> onPipelineContentChanged;

private:
    std::vector<wvp::AdapterEntry> PhysicalAdapters();
    void LogAdapters(const std::string& why) const;
    void OnAdaptersProbed(std::vector<wvp::AdapterCandidate> candidates, const std::string& why);
    bool SelectVideoGpuNow(const std::string& trigger, bool onlyIfCheaper, wvp::FilterChange* change = nullptr, bool* presenterSwitched = nullptr);
    wvp::PresenterHandle MakePresenter(const std::string& luid);
    bool UsePresenterOn(const std::string& luid);
    std::string DisplayGpuLuid() const;
    void ReportPlanProblem();
    void ApplyCaptionOpenOptions();
    void UpdateCaptionFont();
    void ApplyPipelineContent();
    void PipelineFallback(const wvp::PipelineSession::Fallback& fb);
    void OnVideoDeviceLost(const std::string& why);
    void OnPresenterError(const wvp::PresenterErrorInfo& e);
    bool Loaded() const;
    void ReopenHere();

    app::Services& svc_;
    void* view_;
    Post post_;
    std::unique_ptr<wvp::mac::MacVideoPlatform> platform_;
    std::unique_ptr<wvp::FfPlayer> player_;
    wvp::PlaybackController* ctl_ = nullptr;
    wvp::PipelineSession pipe_;
    AribFont font_;

    std::thread probeThread_;
    bool probeBusy_ = false;
    std::string probePending_;
    bool capsProbed_ = false;
    std::uint64_t startMs_ = 0, startupProbeMs_ = 0;
    std::vector<wvp::AdapterEntry> probedAdapters_;
    std::vector<std::string> probedLuids_;
    std::vector<wvp::PhysicalGpu> physGpus_;
    std::vector<wvp::AdapterCandidate> adapters_;
    std::string displayLuid_, playLuid_;
    wvp::VideoGpuChoice videoChoice_;
    bool haveVideoChoice_ = false;
    std::string reportedPlanProblem_;

    static std::string OptionOf(const wvp::OpenOptions& o, const std::string& key);
    std::string lastOpenUrl_;
    wvp::OpenOptions lastOpenOpts_;
    std::string liveVf_, liveAf_;
    std::string statsAtPrepared_ = "{}";
    std::uint64_t pipeOpenMs_ = 0, pipePollNextMs_ = 0, presenterReopenMs_ = 0, lastRestoreMs_ = 0;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

std::uint64_t NowMs();

}
