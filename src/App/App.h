#pragma once
// Layer split (DirectComposition visuals under ONE topmost target, bottom to top; all drawn with D2D into surfaces):
//   1. content  - audio-only placeholder / idle full-screen black cover, redrawn on change
//      panel    - PlaylistPanel (windowed player view, docked beside the video); surface = the panel rect only
//      debug    - the Tab overlay
//   2. title    - TitleBar (slides/fades with the bar in full screen; static in windowed video modes)
//   3. bar      - PlayerBar (fade/slide animation in the full screen / mini player auto-hide modes, docked otherwise)
//   4. popup    - menus, flyouts, dialog smoke, notices/toasts and the tooltip (UiHost popups + tooltip), full client,
//                 allocated only while something is shown
// ONE pui::UiHost (root = ShellRoot) owns focus/hover/capture/popups. Each layer is painted by pointing the renderer at a
// subtree; a layer repaints only when its paint hash changed (LayerRenderer hash mode), after a UiHost invalidation
// (coalesced through a posted message) or the periodic tick (100 ms idle / 16 ms animating).
// Settings is a separate owned top-level window (shell/SettingsWindow): own HWND, DComp target, UiHost and renderer, drawn
// with the same UI device (gfx_); App::ShowSettings opens it or brings it to the front, RebindUi recreates its surfaces.
#include <windows.h>
#include <oleidl.h>

#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "AppContext.h"
#include "BarLogic.h"
#include "Gfx.h"
#include "LayerOrder.h"
#include "LayerRenderer.h"
#include "RebindLogic.h"
#include "WindowSnap.h"
#include "features/AcrylicLogic.h"
#include "features/DebugOverlay.h"
#include "features/DebugOverlayModel.h"
#include "pui/D2dRenderer.h"
#include "wvp/CapabilitySet.h"
#include "wvp/FrameStepLogic.h"
#include "wvp/PipelineSession.h"
#include "wvp/VideoGpu.h"
#include "pui/Uia.h"
#include "shell/ShellRoot.h"

namespace app {

class SettingsWindow;

/// Content layer of audio-only media in Now playing / full screen / mini: opaque black + music-note placeholder + title.
void PaintAudioContentLayer(pui::IRenderer& r, const pui::Theme& t, const pui::RectF& area, const std::wstring& title);

struct AppOptions {
    std::vector<std::wstring> inputs;
    bool headless = false;
    bool stats = false;               // --stats: BaseOptions stats=1 (ffplay status line through the log callback)
    bool plannerOff = false;          // --planner=off: no capability probe, the GPU pipeline planner stays off
    // Test / diagnostic switches, default off. Passed to libffplay's opens and to the capability probe child.
    bool d3d11Debug = false;          // --d3d11-debug: d3d11_debug=1 (debug layer, ID3D11InfoQueue messages in the log)
    std::wstring d3d11ForceCaps;      // --d3d11-force-caps=p010=0,decoder_srv=0,...: d3d11_force_caps
    std::wstring d3d11Route;          // --d3d11-route=vp|shader: d3d11_route
    bool noCuda = false;              // --no-cuda: the planner sees no CUDA device (capabilities masked after the probe)
    int testUiLossMs = -1;            // --test-ui-device-loss=<ms>: simulate one UI device loss that long after playback starts
    std::wstring resultPath;
};

/// libffplay open options for the UI adapter (d3d11_adapter_luid; "" = none, libffplay's default adapter) and the test switches.
std::vector<std::pair<std::wstring, std::wstring>> LibffplayDeviceOptions(const std::wstring& adapterLuid, const AppOptions& o);
/// The same as an ffp_probe_caps_opts string ('\n'-separated): d3d11_adapter_luid, d3d11_force_caps, d3d11_debug.
std::wstring ProbeOptions(const std::wstring& adapterLuid, const AppOptions& o);
/// The capabilities the planner sees under --no-cuda: CUDA devices not ok, no CUDA decoders, no CUDA import.
wvp::CapabilitySet WithoutCuda(wvp::CapabilitySet caps);
void LogCaps(const wvp::CapabilitySet& caps);
/// " --adapter-luid=... --d3d11-debug ..." switches of the --probe-gpu child for one adapter.
std::wstring ProbeSwitches(const std::wstring& adapterLuid, const AppOptions& o);
/// Probes the given adapters in parallel child processes; each answer is merged with BuildCapabilities.
/// Adapters whose probe failed or has no usable d3d11va device are left out.
std::vector<wvp::AdapterCandidate> ProbeAdapters(const std::vector<HwAdapter>& adapters, const AppOptions& o);

class App final : public AppContext {
    friend class AppDropTarget;
public:
    explicit App(const AppOptions& o);
    ~App() override;

    int RunGui();
    int RunHeadlessCheck();

    static constexpr UINT WM_NOTIFY_PLAYER = WM_APP + 1, WM_RUN_QUEUE = WM_APP + 2, WM_REDRAW = WM_APP + 3;

    HWND MainHwnd() const override { return hwnd_; }
    pui::UiHost& Ui() override { return host_; }
    wvp::PlaybackController* Controller() override { return ctl_.get(); }
    wvp::FfPlayer* Player() override { return player_.get(); }
    Services& Svc() override { return *svc_; }
    pui::IRenderer& Measurer() override { return d2d_; }
    void SetFullscreen(bool on) override;
    bool IsFullscreen() const override { return modes_.fullscreen; }
    void SetMiniPlayer(bool on) override;
    void ApplyWindowTopmost() override;
    bool IsMiniPlayer() const override { return modes_.mini; }
    ShellMode Mode() const override { return modes_.Mode(); }
    std::wstring CurrentPlaylistName() const override { return currentPlaylistName_; }
    std::vector<wvp::MediaItem> PlaylistItems(const std::wstring& name) override;
    bool SavePlaylistItems(const std::wstring& name, const std::vector<wvp::MediaItem>& items) override;
    void PlayPlaylist(const std::wstring& name, const std::vector<wvp::MediaItem>& items, int start) override;
    void PlayPlaylistAtPosition(const std::wstring& name, const std::vector<wvp::MediaItem>& items, int start, double position) override;
    void AppendPlaylistItems(const std::wstring& name, const std::vector<wvp::MediaItem>& items) override;
    void AppendToCurrentPlaylist(std::vector<wvp::MediaItem> items) override;
    /// Appends to `name` (the current playlist goes through the controller); startIfIdle = the Open rule (play the first
    /// item when nothing has played), false for every playlist-view drop / add.
    void AppendItems(const std::wstring& name, const std::vector<wvp::MediaItem>& items, bool startIfIdle);
    void ReplaceUnnamedPlaylistAtPosition(std::vector<wvp::MediaItem> items, double position) override;
    void SetResumeDecision(const std::wstring& decision) override;
    void PlaylistRenamed(const std::wstring& from, const std::wstring& to) override;
    void PlaylistDeleted(const std::wstring& name) override;
    void PlaylistsChanged() override;
    void ShowInfoBar(InfoSeverity severity, const std::wstring& title, const std::wstring& message) override;
    void ShowToast(const std::wstring& text) override;
    std::vector<wvp::VideoGpuPref> VideoGpus() const override;
    void VideoGpuSettingChanged() override;
    void ApplyTheme(pui::ThemeMode mode, uint32_t accent) override;
    /// F5 / main menu "Settings": opens the Settings window, or brings the open one to the front (single instance, modeless).
    void ShowSettings();
    void NotePlayerPressed();         // a mouse button went down on the player (never consumes the message): it goes in front of Settings
    void NoteTopmostReasserted();     // the player re-asserted its z-order / always-on-top state: Settings stays above unless the player is in front
    const pui::Theme& CurrentTheme() const override { return host_.GetTheme(); }
    pui::ThemeMode CurrentThemeMode() const override { return themeMode_; }
    float Scale() const override { return scale_; }
    void RequestRedraw() override;
    void Execute(CommandId command) override;
    void PostToUi(std::function<void()> fn) override;

    void ToggleDebugOverlay(const char* src = "command");
    bool DebugOverlayVisible() const { return debugVisible_; }
    void RefreshDebugOverlay();
    /// Per-adapter probe results (AppVideoGpu.cpp): kept as the video GPU candidates, then the video GPU is selected.
    void OnAdaptersProbed(std::vector<wvp::AdapterCandidate> candidates, const std::string& why);
    /// Probes every hardware adapter again in child processes (adapter set changed, device lost); coalesced while one runs.
    void StartAdapterProbe(const std::string& why);
    /// Selects the video GPU for the current request (wvp::SelectVideoGpu) and applies it: capabilities, preferences, the
    /// d3d11_adapter_luid open option. onlyIfCheaper: keep the current adapter unless the new choice's total cost is strictly
    /// lower (drag end). Returns true when the adapter changed. Does not reopen.
    bool SelectVideoGpuNow(const std::string& trigger, bool onlyIfCheaper, wvp::FilterChange* change = nullptr);
    /// The video device was lost (presenter device_lost event, or the UI device loss that took the adapter with it):
    /// re-select and reopen the current item at its position and play state.
    void OnVideoDeviceLost(const std::string& why);
    void OnWindowDragEnd();
    /// Enumerates the hardware adapters, groups them into physical GPUs by PCI address (physGpus_, logged) and returns the
    /// canonical adapter of each GPU: the probe and the video GPU candidates use those only.
    std::vector<HwAdapter> PhysicalAdapters(const std::string& why);
    std::string DisplayGpuLuid() const;   // canonical LUID of the GPU driving the window's monitor
    void OnDisplayChange();

    /// Timestamps (QPC us) of the latest UI rebind: detectedUs = first failing call / removal event, startUs = rebuild began,
    /// deviceReadyUs = new D3D/D2D/DComp devices exist, hookUs = OnUiRebound called, uiVisibleUs = first successful Commit of the
    /// redrawn UI. The orchestrator adds its own detectedUs -> first video frame measurement on top.
    struct RebindTiming {
        double detectedUs = 0, startUs = 0, deviceReadyUs = 0, hookUs = 0, uiVisibleUs = 0;
        std::string reason, where;
        bool fallback = false;
        double DetectToUiMs() const { return (uiVisibleUs - detectedUs) / 1000.0; }
        double DeviceToUiMs() const { return (uiVisibleUs - deviceReadyUs) / 1000.0; }
    };
    const RebindTiming& LastRebindTiming() const { return rebindTiming_; }
    /// Completed UI rebuilds (successful ones) since start; the incident is over when !RebindBusy().
    int RebindCount() const { return rebuilds_; }
    bool RebindBusy() const { return rebind_.Busy(); }
    bool RebindFallbackActive() const { return rebind_.FallbackActive(); }
    int RebindIncidents() const { return rebind_.Incidents(); }
    /// Test hook: the next Gfx draw/commit fails with DXGI_ERROR_DEVICE_REMOVED, exactly as a lost device would; the normal
    /// detection path then runs the rebind. Forces a render so it happens at once.
    void SimulateUiDeviceLoss(RebindReason reason = RebindReason::Test);
    /// Test hook: leaves the WARP fallback and rebinds to the hardware adapter again.
    void ResetRebindFallback();
    /// Reports a loss from any other source (e.g. a future "adapter_change" trigger); starts the rebind on the UI thread.
    void ReportGpuLoss(RebindReason reason, const char* where, double detectedUs = 0);

private:
    /// Called on the UI thread once the UI device is rebuilt and playAdapter_ re-picked, BEFORE the UI is redrawn, so the
    /// playback restore can start in parallel. reason = RebindReasonName(); adapterChanged = the playback adapter's LUID differs.
    /// Orchestrator fills this in (AppPlayback.cpp); it must not block.
    void OnUiRebound(const std::string& reason, bool adapterChanged);
    void ArmGfx();
    void OnGfxFailure(const char* where, HRESULT hr);
    void RunRebind();
    bool RebindUi(RebindReason reason, bool forceWarp);
    RebindController rebind_;
    RebindTiming rebindTiming_;
    int rebuilds_ = 0;
    unsigned gfxGen_ = 0;
    bool rebindPosted_ = false, rebuilding_ = false, rebuildFailed_ = false;
    RebindReason injectedReason_ = RebindReason::Test;
    uint64_t testLossArmedMs_ = 0;
    bool testLossFired_ = false;

    bool Create(bool hidden);
    void Destroy();
    static LRESULT CALLBACK StaticProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK VideoProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK VideoDropProc(HWND, UINT, WPARAM, LPARAM);
    /// Keep SDL's host child from taking the legacy WM_DROPFILES path; OLE has a target registered on this child.
    /// Idempotent; re-run from Tick in case SDL changes the child style or subclasses it later.
    void EnsureVideoDropHook();
    void UnhookVideoDrop();
    HRESULT RegisterDropTargets();
    void RevokeDropTargets();
    DropRoute RouteDropAtClient(POINT client) const;
    LRESULT Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT NcHitTest(LPARAM);
    // Playlist panel splitter: the gap between the video column and the docked panel (ShellLayout::splitterPx), the only resize handle.
    bool SplitterAt(POINT clientPx) const;
    bool BeginSplitterDrag(LPARAM lp, bool doubleClick);
    void DragSplitter(int xPx);
    bool EndSplitterDrag();
    void SetPanelWidth(int widthDip);   // applies the width (relayout) and stores what the layout actually allowed
    LPARAM NcClientLParam(LPARAM screenLp) const;   // screen lParam of a WM_NC* message -> client lParam
    bool splitDrag_ = false;                        // dragging the panel splitter (mouse captured)
    int splitGrabPx_ = 0;                           // press x minus the splitter left edge
    bool ncTracked_ = false;                        // TME_NONCLIENT leave tracking armed (maximize button hover)
    void OnClose();
    int S(double dip) const { return (int)std::lround(dip * scale_); }
    uint64_t Now() const { return GetTickCount64(); }
    void SetTimerMs(UINT ms);
    void ApplyWindowFullscreen(bool on);
    /// The auto-hide animation finished in the hidden state and `w` is the bar / mini bar / sliding title: UIA reports it
    /// IsOffscreen and it is not hit-testable by UIA.
    bool IsAutoHiddenChrome(pui::Widget* w) const;
    bool chromeOffscreen_ = false;

    pui::PointerEvent MakePointer(LPARAM lp, pui::PointerButton b, int clicks = 1) const;
    void OnVideoInput(const wvp::InputEventArgs& e);
    void VideoPressDrag(HWND video, LPARAM lp);   // left press on the video child: window drag past the drag threshold
    void HandleGlobalKey(int vk, unsigned mods, bool repeat = false);
    /// A key mapped to a command. D / F go to the frame-step controller (key held-ness, no queue); the rest to Execute.
    void RunKeyCommand(int vk, CommandId command, bool repeat);
    static constexpr int kKeyStepFrame = 0, kKeyStepKeyframe = 1, kKeyStepSkip = 100;   // actions of the shared held-key step controller; kKeyStepSkip + n = a timed skip of n seconds
    wvp::FrameStepController::Hooks MakeFrameStepHooks();
    bool BarHittable(uint64_t now) const;
    /// Whether a pointer press / move at `dip` reaches the shell: full screen gates the bar and the title strip separately
    /// (each only while shown); a popup (incl. the edge playlist panel) always does.
    bool ChromeHittableAt(uint64_t now, pui::PointF dip) const;
    bool BarInteractionActive() const;
    bool TitleInteractionActive() const;
    bool PanelInteractionActive() const;
    size_t NonPanelPopups() const;   // user popups other than the edge playlist panel
    unsigned CurrentMods() const;
    int modsOverride_ = -1;   // >= 0: synthetic input (--headless-check) ignores the physical Shift/Ctrl/Alt state

    void Relayout();
    void ApplyMode();
    void UpdateAcrylicState(bool forceSystemCheck = false);
    void Flush();
    void RenderLayers(bool force);
    bool RenderLayer(Surf& s, uint64_t& lastHash, int x, int y, int w, int h, const std::function<void(LayerRenderer&)>& paint, bool force);
    void StartBarAnim(const AutoHide::Action& a);
    void StartTitleAnim(const AutoHide::Action& a);   // full screen: the title strip has its own edge-driven AutoHide
    /// Full screen: shows the overlay whose edge zone holds the cursor (title top, bar bottom, playlist panel right).
    void UpdateEdgeReveal(uint64_t now);
    void SnapBar();
    void AutoHideTick(uint64_t now);
    void PointerMoved(uint64_t now);
    void SetCursorHidden(bool hide);
    void CheckPointerLeft(uint64_t now);
    void Tick();
    void SyncModel();
    PlaylistPanel* SelectionPanel() const;   // the visible panel with a selected row (nothing loaded: Play starts it)
    bool CanPlaySelection() const override;
    void OnPanelSelectionChanged() override;
    /// Rebuilds the bar's info chips (H/W, codecs, channels) from the debug info + media info; empty when nothing is loaded.
    void RefreshChips();
    void ChipsTick(uint64_t now);
    /// Stops playback and closes the media; the queue stays.
    void CloseMedia();
    void TogglePlaylistPanel();
    void FocusPlaylistSearch();
    /// Narrow window: the playlist panel as a right-side light-dismiss popup over the video (second PlaylistPanel instance).
    void OpenPanelOverlay();   // full screen: the same panel, full height at the right edge (opened by the right edge zone)
    /// Runs f on the docked panel and on the overlay instance when open (queue / playlist change notifications).
    void ForPanels(const std::function<void(PlaylistPanel&)>& f);
    PlaylistPanel* panelPopup_ = nullptr;
    /// Ctrl+T: Disable -> Enable -> While playing -> While fullscreen; persisted, applied at once, toast with the mode name.
    void CycleAlwaysOnTop();
    void ToggleTimeMode();
    /// 1-5 keys: window sized so the Now playing video rect is the video's display size x scale (<= 0 = largest that fits
    /// the work area); restores mini / full screen / maximized first. No video: does nothing.
    void SizeWindowToVideo(double scale, std::optional<PxSize> knownVideo = std::nullopt);
    SnapTracker snap_;
    void SnapWindowMessage(LPARAM lp, bool move, int sizingEdge);   // WM_MOVING / WM_SIZING edge snapping
    std::wstring sizedForUrl_;   // url the window was last sized for on open (once per opened item)
    bool MediaActive() const { return Loaded() || pending_.has_value(); }
    void CloseNotice(pui::Widget* w);
    ShellLayout ComputeLayout() const;

    void InitPlayer();
    void UpdateKeepAwake();   // system / display sleep is blocked only while playing (SetThreadExecutionState)
    void WirePlayer();
    void RestoreCurrentPlaylist();
    bool PersistCurrentPlaylist();
    void SetCurrentPlaylistName(const std::wstring& name);
    void SavePlaybackHistory(bool force, bool ended = false, double positionOverride = -1);
    void SavePlaybackSnapshot(const std::wstring& playlist, const std::wstring& url, int index, double position, double duration,
                              bool seekable, bool force, bool ended = false);
    void ApplyPendingQueue();
    void CheckReady();
    void Capture(bool withSubtitles = false);
    void TogglePause();
    void SetVolume(int v);
    void AdjustVolume(int d);
    bool Loaded() const { return ctl_ && ctl_->Current() != nullptr; }
    void UpdateTitleText();
    dbg::OverlayContext BuildDebugContext();

    void WirePipeline();
    void OnPipelinePrepared();                            // content-aware replanning once per open generation
    void PipelinePoll(uint64_t now);                      // 1 Hz debug-info check for the first 10 s of an open
    void PipelineFallback(const wvp::PipelineSession::Fallback& fb);
    void LogOpenPlan();
    wvp::PipelineSession pipe_;
    bool capsProbed_ = false;
    PlaybackAdapter playAdapter_;     // the video GPU (SelectVideoGpuNow); luid "" = software decoding + SDL
    std::vector<wvp::AdapterCandidate> adapters_;   // probed video GPU candidates (d3d11va usable)
    std::vector<std::string> probedLuids_;          // every hardware adapter of the last probe (adapter-set change detection)
    std::string displayLuid_;                       // GPU driving the window's monitor at the last selection (canonical LUID)
    std::string displayRawLuid_;                    // the DXGI adapter of that monitor (an alias when it differs)
    std::vector<wvp::PhysicalGpu> physGpus_;
    wvp::VideoGpuChoice videoChoice_;
    bool haveVideoChoice_ = false, manualMissingNoticed_ = false;
    bool adapterProbeBusy_ = false;
    std::string adapterProbePending_;               // a re-probe requested while one ran
    std::thread adapterProbeThread_;
    uint64_t lastRestoreMs_ = 0;                    // last device-loss reopen (Now()), coalesces repeated losses
    bool videoSoftwareFallback_ = false;            // the rebind controller fell back: video on software decoding + SDL
    uint64_t pipeOpenMs_ = 0, pipePollNextMs_ = 0;
    bool RenderDebugLayer(bool force);
    bool UserPopupOpen() const;
    bool TextInputOwnsKey(int vk, unsigned mods) const;
    dbg::TabAction DecideTabNow(unsigned mods);   // DecideTab with the live inputs; records them in lastTabDecision_
    bool AllowsFocus(const pui::Widget* w) const;   // host_.focusFilter: the player view keeps keyboard focus off every widget but the playlist search box
    void NotePointerTarget(pui::PointF dip);        // a press in the client: activates / deactivates the playlist panel, releases the search box focus
    bool PanelKey(int vk, unsigned mods);           // offers a key to the active docked playlist panel
    bool SpaceIsPlayPause() const;
    std::string lastTabDecision_;
    void LogSeek(const char* how, double seconds, bool relative);

    AppOptions opt_;
    bool hidden_ = false, destroyed_ = false;
    std::wstring dataDir_;
    HWND hwnd_ = nullptr, video_ = nullptr;
    IDropTarget* dropTarget_ = nullptr;            // owns one ref; OLE holds refs for the main and SDL child HWNDs
    bool dropTargetMainRegistered_ = false, dropTargetVideoRegistered_ = false;
    float scale_ = 1.f;
    int cw_ = 0, ch_ = 0;
    bool wasMax_ = false, minimized_ = false;
    /// True while a key press is being dispatched (WM_KEYDOWN / video-child key): commands it runs leave the OSD alone.
    bool keyInput_ = false;
    wvp::FrameStepController frameStep_{MakeFrameStepHooks()};
    int videoKeyVk_ = 0;                            // last key pressed in the video child, until released (auto-repeat detection)
    bool topmostWanted_ = false;   // last ApplyWindowTopmost decision (mini player: true); WM_WINDOWPOSCHANGING keeps it
    bool pointerInside_ = false;
    struct KeyInputScope {
        bool& flag;
        bool prev;
        explicit KeyInputScope(bool& f) : flag(f), prev(f) { flag = true; }
        ~KeyInputScope() { flag = prev; }
    };
    RECT restoreRect_{};
    UINT timerMs_ = 0;

    std::unique_ptr<Services> svc_;
    pui::UiHost host_;
    ShellRoot* shell_ = nullptr;
    ModeState modes_;
    pui::ThemeMode themeMode_ = pui::ThemeMode::System;
    uint32_t accentOverride_ = 0;
    std::unique_ptr<pui::UiaBridge> uia_;

    Gfx gfx_;
    Surf sContent_, sPanel_, sDebug_, sTitle_, sBar_, sPopup_;   // DComp layers, created in LayerOrder.h order
    std::unique_ptr<SettingsWindow> settingsWin_;                // created on the first ShowSettings; its HWND lives while open
    bool skipActivationPress_ = false;   // the next press is the click that brought the app back from another application
    bool themeLight_ = false;
    pui::Theme SettingsTheme() const;                            // the app theme for the Settings window (no in-video Acrylic)
    Surf& LayerSurf(Layer l);
    uint64_t hContent_ = 0, hPanel_ = 0, hTitle_ = 0, hBar_ = 0, hPopup_ = 0;
    bool gfxOk_ = false, redrawPosted_ = false, layersDirtyAll_ = true;
    Microsoft::WRL::ComPtr<IDWriteFactory> dw_;
    std::unique_ptr<pui::D2dRenderer> d2d_p_;
    pui::D2dRenderer& d2d_;
    std::unique_ptr<LayerRenderer> lr_;
    dbg::DebugOverlayPainter dbgPainter_;
    bool debugVisible_ = false, debugDirty_ = false;
    uint64_t debugNextMs_ = 0, hDebug_ = 0;
    int debugCols_ = 0;
    std::vector<dbg::OverlayLine> debugLines_, debugRows_;   // full text / wrapped+clipped rows last drawn
    std::wstring lastOpenUrl_, lastOpenCmd_;
    wvp::OpenOptions lastOpenOpts_;
    wvp::OpenOptions effOpts_;               // lastOpenOpts_ + the live vf/af of the latest filter change (overlay / log)
    std::thread probeThread_;
    ShellLayout layout_;
    bool acrylicMediaHasVideo_ = false, acrylicD3D11_ = false, acrylicPresenterFailed_ = false;
    bool themeHighContrast_ = false;   // the tokens of the applied theme carry the high-contrast overrides
    bool acrylicTransparencyEffects_ = true, acrylicBatterySaver_ = false, acrylicRemoteSession_ = false, acrylicHighContrast_ = false;
    bool acrylicApiReady_ = false, acrylicApiUnsupported_ = false, acrylicNeedsApply_ = true;
    bool popupWasVisible_ = false, popupAcrylicWasActive_ = false;
    uint64_t acrylicSystemCheckNextMs_ = 0, acrylicPresenterCheckNextMs_ = 0;
    acrylic::FallbackReason acrylicFallbackReason_ = acrylic::FallbackReason::NoVideo;
    std::vector<wvp::BackdropRegion> acrylicAppliedRegions_;
    HRESULT lastDrawHr_ = S_OK, lastCommitHr_ = S_OK;

    AutoHide autoHide_;
    AutoHide titleHide_;                // full screen only (edge mode); the bar uses autoHide_
    CursorAutoHide cursorIdle_;
    POINT lastCur_{-1, -1};
    void ResizeWindowForPlaylistPanel(int videoWidthPx, bool panelWanted);
    bool cursorHidden_ = false, mouseTracked_ = false;
    std::unique_ptr<wvp::FfPlayer> player_;
    std::unique_ptr<wvp::PlaybackController> ctl_;
    struct PendingPlay { std::vector<wvp::MediaItem> items; int start; bool replace; double startAt = -1; };
    std::optional<PendingPlay> pending_;
    std::wstring currentPlaylistName_;   // empty = persistent unnamed playlist
    std::vector<wvp::MediaItem> currentPlaylistItems_;
    std::wstring historyCurrentUrl_, historyCurrentPlaylistName_;
    int historyCurrentIndex_ = -1;
    uint64_t historyWriteAt_ = 0, playbackProgressUiAt_ = 0;
    std::string resumeDecision_;
    // QPC us at the steps of the last StateChanged handler (headless pause/resume latency notes)
    struct { double enter = 0, topmost = 0, synced = 0, saved = 0; } stateTiming_;
    bool suppressPlaylistPersistence_ = false;
    bool playerFailed_ = false, playerCreating_ = false;

    BarModel model_;
    wvp::PlayerState state_ = wvp::PlayerState::Idle;
    bool readyWritten_ = false;
    int positionEvents_ = 0;
    int dropEvents_ = 0;
    double mediaAspect_ = 16.0 / 9.0;   // display aspect of the current media (mini player aspect lock)
    std::vector<badge::Chip> chips_;                 // BarModel::chips source (RefreshChips)
    wvp::DebugInfo debugCache_;                      // last debug info parsed by the 250 ms acrylic poll (shared with RefreshChips)
    uint64_t debugCacheMs_ = 0;
    uint64_t chipsNextMs_ = 0, chipsPreparedMs_ = 0;
    int chipsStage_ = 0;                             // 0: first refresh due 0.5 s after Prepared, 1: second at 1.5 s, 2: every 2 s while playing
    struct Notice { pui::Widget* w; uint64_t until; };
    std::vector<Notice> notices_;

public:
    std::vector<std::pair<std::string, HRESULT>> steps_;
    std::vector<std::pair<std::string, std::string>> notes_;
    void Step(const char* name, HRESULT hr);
    void Note(const char* k, const std::string& v) { notes_.emplace_back(k, v); }
    bool PumpUntil(const std::function<bool()>& cond, DWORD timeoutMs);
    /// Headless: the recorded DComp stack equals LayerOrder.h (popup topmost).
    bool LayerStackMatches();
    /// Headless: overlay surface state / text / draw counter / z-index of a layer.
    bool DebugOverlayDrawn() const { return sDebug_.surface != nullptr; }
    std::string DebugText() const { return dbg::PlainText(debugLines_); }
    const std::vector<dbg::OverlayLine>& DebugRows() const { return debugRows_; }
    int DebugDraws() const { return dbgPainter_.DrawCount(); }
    int DebugCols() const { return debugCols_; }
    int LayerIndex(Layer l) { const auto& z = gfx_.ZOrder(); for (size_t i = 0; i < z.size(); i++) if (z[i] == LayerSurf(l).visual.Get()) return (int)i; return -1; }
    const std::string& LastTabDecision() const { return lastTabDecision_; }
    const std::wstring& LastOpenCmd() const { return lastOpenCmd_; }
    /// Headless: opens the video context menu in the current mode, renders, checks it is painted into the popup layer
    /// and that the popup layer is above the bar and title layers; closes it again.
    HRESULT CheckPopupOverBar();
    /// Test hooks (headless-check): drive everything through the public paths on a hidden window.
    ShellRoot* Shell() const { return shell_; }
    SettingsWindow* Settings() const { return settingsWin_.get(); }
    const ShellLayout& Layout() const { return layout_; }
    HRESULT LastDrawHr() const { return lastDrawHr_; }
    bool GfxOk() const { return gfxOk_; }
    void ForceRender() { layersDirtyAll_ = true; Flush(); }
    RECT VideoRect() const { RECT r{}; if (video_) { GetWindowRect(video_, &r); } return r; }
    bool VideoVisible() const { return video_ && (GetWindowLongPtrW(video_, GWL_STYLE) & WS_VISIBLE) != 0; }
    int ClientW() const { return cw_; }
    int ClientH() const { return ch_; }
    /// Number of accepted file-drop batches processed through the OLE target.
    int DropEvents() const { return dropEvents_; }
    HWND VideoHwnd() const { return video_; }
    bool VideoDropHooked() const { return video_ && GetWindowLongPtrW(video_, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(&App::VideoDropProc); }
    bool DropTargetsReady() const { return dropTarget_ && dropTargetMainRegistered_ && dropTargetVideoRegistered_; }
    /// Test/headless entry point shared with IDropTarget::Drop; `client` is in main-window device pixels.
    bool HandleDropAtClient(POINT client, const std::vector<std::wstring>& files);
    bool Prepared() const { return model_.prepared; }
    double Duration() const { return model_.dur; }
    bool Warp() const { return gfx_.warp; }
    const BarModel& Model() const { return model_; }
    static App* Instance();
};

}
