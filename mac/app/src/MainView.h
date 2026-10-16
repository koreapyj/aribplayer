#pragma once
// The macOS app::App: implements AppContext for the src/App shell, pages and features (shell/ShellRoot,
// TitleBar, PlayerBar, PlaylistPanel, pages/SettingsPage, features/Menus, FiltersPanel, PlaybackExtras, ...), owns the
// pui::UiHost + ShellRoot tree, a wvp::PlaybackController over the engine (wvp::FfPlayer over libffplay,
// created by Engine with the Mac VideoPlatform's Metal presenter on the video view), the Services (settings /
// playlists / history in ~/Library/Application Support/aribplayer, or a test directory), the playlist + history logic
// of AppPlayback.cpp (current / unnamed playlist persistence, resume positions), the shell layout (ComputeShellLayout /
// ComputeMiniShellLayout, the same math as the Windows app) and the command routing (Commands.h key table -> Execute,
// the held-key frame stepper). Native things (window, full screen, mini player, resizing, always-on-top, the Settings
// window, the clipboard) are hooks the AppKit host (main.mm) fills in.
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "AppContext.h"
#include "BarLogic.h"
#include "ShellLogic.h"
#include "features/DebugOverlayModel.h"
#include "shell/ShellRoot.h"
#include "pui/Widget.h"
#include "wvp/FilterSettings.h"
#include "wvp/FrameStepLogic.h"

namespace puimac { class CGRenderer; }

namespace mainview {

class Engine;
class DebugOverlay;

class MainView final : public app::AppContext {
public:
    struct Hooks {
        std::function<HWND()> window;                            // the NSWindow* of the player (AppContext::MainHwnd)
        std::function<void(const app::RectPx&)> videoRect;      // place the Metal view (device px, top-left origin; Empty = hide)
        std::function<void()> toggleFullScreen;
        std::function<void(int videoWidthPx, bool panelWanted)> resizeForPanel;   // window grows / shrinks by the panel
        /// Size the window so the video shows at `scale` x its display size (-1 = as large as the screen allows);
        /// true when the window was sized.
        std::function<bool(const wvp::MediaPreparedInfo&, double scale)> sizeWindowToVideo;
        std::function<void(bool topmost)> applyTopmost;
        std::function<void(pui::ThemeMode, std::uint32_t accent)> applyTheme;
        std::function<void(bool on)> miniPlayer;                // after the mini player window shape changed (MiniPlayerMac.mm)
        std::function<void()> openSettings;
        std::function<void(const wvp::BgraFrame&)> copyFrame;   // Capture frame -> clipboard
        std::function<void()> quit;
        std::function<void()> layoutChanged;                    // the shell layout was recomputed (cursor rects: the splitter)
        /// The pointer in client DIP while it is over the player window with nothing of this app (a modal dialog, Settings)
        /// or another app in front of it there; nullopt otherwise (App::AutoHideTick's WindowFromPoint + IsWindowEnabled).
        std::function<std::optional<pui::PointF>()> cursorAt;
    };

    struct Options {
        std::string audioDriver;   // FfPlayer::Create's audio_driver ("" = SDL's default: CoreAudio)
    };
    /// videoView: the NSView* of the video area (CAMetalLayer-backed, VideoView). dataDir: UTF-8 path of the data
    /// directory (settings.json, Playlists/, unnamed.m3u8); "" = platform::AppDataDir() (~/Library/Application
    /// Support/aribplayer). Starts the adapter probe and creates the player; throws std::runtime_error when libffplay
    /// cannot start. Call Start() once the hooks are set.
    MainView(puimac::CGRenderer* measurer, void* videoView, const std::string& dataDir, const Options& options);
    ~MainView() override;

    /// App::Create after the window exists: LoadSettingsAtStartup (theme, window placement, filters),
    /// RestoreCurrentPlaylist, InitPlaybackExtras, OnPlayerReady.
    void Start();
    /// WM_CLOSE: SaveSettingsAtShutdown + the resume position of the current item.
    void Shutdown();

    pui::UiHost host;
    Hooks hooks;
    app::ShellRoot* shell = nullptr;
    const app::ShellLayout& Layout() const { return layout_; }
    const app::ModeState& Modes() const { return modes_; }
    wvp::PlaybackController& Ctl() { return *ctl_; }
    Engine& Eng() { return *engine_; }
    const std::string& DataDir() const { return dataDir_; }
    /// FiltersPanel's deinterlace choice (Off / On / IVTC), applied live (replans; reopens when the plan says so).
    void SetDeinterlace(wvp::DeinterlaceMode mode);
    /// The Mac theme for the current appearance, the accent of the settings (0 = system) and a backing scale.
    pui::Theme MakeTheme(float scale) const;

    /// Window client size in device pixels and the backing scale: recomputes the shell layout.
    void Resize(int wPx, int hPx, float scale);
    void SetFullscreenState(bool on);   // the NSWindow entered / left full screen
    /// Periodic (10 Hz): position / duration of the player into the bar model.
    void Tick();
    /// Full screen / mini player reveal: the pointer moved to a client DIP (or left the window: nullopt). Full screen:
    /// the title strip (top kStripDip), the bar (bottom kDockDip) and the playlist panel (right kEdgeRightDip) each show
    /// while the pointer is in its edge zone or over its rect (App::UpdateEdgeReveal).
    void PointerAt(std::optional<pui::PointF> p);
    /// A mouse button went down on the player: pointer activity (App's WM_*BUTTONDOWN -> autoHide_.Activity).
    void PointerPressed();
    /// Key routing like App: open popup / focused search box -> active playlist panel -> global key table (held-key
    /// stepping for frame / keyframe / timed seeks as App::RunKeyCommand).
    bool Key(int vk, unsigned mods, bool repeat = false);
    void KeyUp(int vk);
    void VideoDoubleClick() { Execute(app::CommandId::PlayPause); }
    /// App's WM_MOUSEWHEEL when no widget took the wheel (video, title strip, bar background): 1 % volume per notch
    /// (AdjustVolume). Fractions of a notch (trackpads) add up; the mini player's bar is revealed.
    void WheelVolume(float notches);
    void ShowMainMenu(pui::PointF at);
    void ShowMainMenu(pui::Widget* anchor);
    /// App::NotePointerTarget: a press inside a playlist panel (docked or overlay) makes it the key target, a press on the
    /// video / bar / title deactivates them; a press in another popup (a menu opened from the panel) leaves them alone. A
    /// press anywhere but the search box ends text entry.
    void PressAt(pui::Widget* hit);
    bool IsTitleStripPoint(pui::PointF p) const;   // empty strip area (window drag)
    bool IsVideoPoint(pui::PointF p) const;
    /// Popups excluding the app's own notice; PlaybackExtras errors still count, as in App::UserPopupOpen.
    bool UserPopupOpen() const;
    /// App::RouteDropAtClient + HandleDropAtClient: true when the drop at `dip` was accepted (video / bar / title: the
    /// unnamed playlist plays them; the docked panel: appended to its playlist).
    bool AcceptsDropAt(pui::PointF dip) const;
    bool DropAt(pui::PointF dip, const std::vector<std::wstring>& paths);
    const std::wstring& Title() const { return model_.title; }
    bool PanelOpen() const { return svc_.settings.playlistPanel; }
    /// The overlay playlist panel (App::panelPopup_): the full-screen right-edge panel or, in a window too narrow to dock
    /// it, the toggle's light-dismiss popup over the video; nullptr while closed.
    app::PlaylistPanel* PanelPopup() const { return panelPopup_; }
    std::uint32_t Accent() const { return accent_; }

    // --- AppContext ---
    HWND MainHwnd() const override { return hooks.window ? hooks.window() : nullptr; }
    pui::UiHost& Ui() override { return host; }
    wvp::PlaybackController* Controller() override { return ctl_.get(); }
    wvp::FfPlayer* Player() override { return player_; }
    app::Services& Svc() override { return svc_; }
    pui::IRenderer& Measurer() override;
    void ApplyWindowTopmost() override;
    void SetFullscreen(bool on) override;
    bool IsFullscreen() const override { return modes_.fullscreen; }
    void SetMiniPlayer(bool on) override;
    bool IsMiniPlayer() const override { return modes_.mini; }
    app::ShellMode Mode() const override { return modes_.Mode(); }
    std::string CurrentPlaylistName() const override { return currentPlaylist_; }
    std::vector<wvp::MediaItem> PlaylistItems(const std::string& name) override;
    bool SavePlaylistItems(const std::string& name, const std::vector<wvp::MediaItem>& items) override;
    void PlayPlaylist(const std::string& name, const std::vector<wvp::MediaItem>& items, int start) override;
    void PlayPlaylistAtPosition(const std::string& name, const std::vector<wvp::MediaItem>& items, int start, double position) override;
    void ReplaceUnnamedPlaylistAtPosition(std::vector<wvp::MediaItem> items, double position) override;
    void SetResumeDecision(const std::string& decision) override { resumeDecision_ = decision; }
    void AppendPlaylistItems(const std::string& name, const std::vector<wvp::MediaItem>& items) override;
    void AppendToCurrentPlaylist(std::vector<wvp::MediaItem> items) override;
    void PlaylistRenamed(const std::string& from, const std::string& to) override;
    void PlaylistDeleted(const std::string& name) override;
    void PlaylistsChanged() override;
    void ShowInfoBar(app::InfoSeverity severity, const std::wstring& title, const std::wstring& message) override;
    void ShowToast(const std::wstring& text) override;
    const std::wstring& LastToast() const { return lastToast_; }   // self-test
    pui::RectF ToastBounds() const;                                 // self-test
    void LayoutToast();
    std::vector<wvp::VideoGpuPref> VideoGpus() const override;
    void VideoGpuSettingChanged() override;
    void ApplyTheme(pui::ThemeMode mode, uint32_t accent) override;
    const pui::Theme& CurrentTheme() const override { return host.GetTheme(); }
    pui::ThemeMode CurrentThemeMode() const override { return themeMode_; }
    float Scale() const override { return scale_; }
    void RequestRedraw() override { host.Invalidate(); }
    void Execute(app::CommandId command) override;
    bool CanPlaySelection() const override;
    /// A selectable caption/subtitle track is selected and showCaptions is on (Mac-only lit bar button).
    bool SubtitlesShown();
    /// The bar's subtitle button is lit (last SyncModel).
    bool SubsButtonLit() const { return subsShown_; }
    void OnPanelSelectionChanged() override { SyncModel(); }
    void PostToUi(std::function<void()> fn) override;

    const std::string& ResumeDecision() const { return resumeDecision_; }

    // --- the Tab debug overlay (App::ToggleDebugOverlay / RefreshDebugOverlay / BuildDebugContext / RenderDebugLayer) ---
    void ToggleDebugOverlay(const char* src = "command");
    bool DebugOverlayVisible() const { return debugVisible_; }
    /// The overlay is shown and has rows (App::DebugOverlayDrawn).
    bool DebugOverlayDrawn() const;
    std::string DebugText() const;                        // the full text (App::DebugText)
    app::dbg::OverlayContext DebugContext() { return BuildDebugContext(); }   // read-only snapshot for self-tests
    const std::vector<app::dbg::OverlayLine>& DebugRows() const;   // wrapped + clipped rows as drawn
    int DebugCols() const { return debugCols_; }
    const DebugOverlay* DebugOverlayWidget() const { return debug_; }
    const std::string& LastTabDecision() const { return lastTabDecision_; }

    // --- the playlist panel splitter (App::SplitterAt / BeginSplitterDrag / DragSplitter / EndSplitterDrag) -------------
    /// The gap between the video column and the docked panel at a client DIP point (the only panel resize handle).
    bool SplitterAt(pui::PointF dip) const;
    /// A press on the splitter: starts the drag, or (double click) resets the width to 320 and saves. False = not on it.
    bool BeginSplitterDrag(pui::PointF dip, bool doubleClick);
    void DragSplitter(pui::PointF dip);
    /// Release: saves playlistPanelWidthDip. False when no drag was running.
    bool EndSplitterDrag();
    bool SplitterDragging() const { return splitDrag_; }
    /// Applies a panel width (relayout) and stores what the layout allowed (App::SetPanelWidth).
    void SetPanelWidth(int widthDip);

    // --- App's power, cursor and auto-hide rules ---------------------------------------------------------------------
    /// App::UpdateKeepAwake: while playing, an IOPMAssertion keeps the display awake (video) or only the system (audio);
    /// paused, stopped or closed: none.
    enum class KeepAwake { None, System, Display };
    void UpdateKeepAwake();
    KeepAwake KeepAwakeState() const { return keepAwake_; }
    /// App::SetCursorHidden over CursorAutoHide: the cursor is hidden over the full-screen video after 2 s without a move.
    bool CursorHidden() const { return cursorHidden_; }
    /// The mini player's bar (App::autoHide_): 0 = shown, 1 = hidden; it slides kBarSlideDip down while it fades.
    float MiniBarHidden() const { return miniHide_.P(host.Now()); }
    bool MiniBarAnimating() const { return miniHide_.Animating(host.Now()); }
    /// RefreshChips calls so far (App::ChipsTick: +0.5 s, +1.5 s after PREPARED, then every 2 s while playing).
    int ChipRefreshes() const { return chipRefreshes_; }
    /// MiniAspectForMedia of the last PREPARED (16:9 without video): the mini player's resize aspect.
    double MediaAspect() const { return mediaAspect_; }

private:
    class Toast;
    static constexpr int kKeyStepFrame = 0, kKeyStepKeyframe = 1, kKeyStepSkip = 100;   // App.h's step actions
    void ApplyMode();
    void SyncModel();
    void TogglePlaylistPanel();
    void FocusPlaylistSearch();
    /// App::OpenPanelOverlay: the playlist panel as a light-dismiss popup at the stored width (min(width, W - 16)) over
    /// the video's right side; full screen: the full-height column at the right edge with square corners, not active.
    void OpenPanelOverlay();
    void ForPanels(const std::function<void(app::PlaylistPanel&)>& f);
    /// App::SelectionPanel: the panel whose selection Play starts (the overlay one, else the docked one).
    app::PlaylistPanel* SelectionPanel() const;
    /// App::NonPanelPopups: user popups other than the full-screen edge panel (the Mac menus are popups of this host).
    size_t NonPanelPopups() const;
    /// App::PanelInteractionActive: the edge panel stays while the pointer is over it, a menu / popup is open, a
    /// press inside it holds the capture, or the search box has the keyboard.
    bool PanelInteractionActive() const;
    void UpdateOverlays();
    void RefreshChips();
    void TogglePause();
    void SetVolume(int v);
    void CloseMedia();
    void Capture(bool withSubtitles);
    bool Loaded() const;
    void RunKeyCommand(int vk, app::CommandId c, bool repeat);
    wvp::FrameStepController::Hooks MakeFrameStepHooks();
    // AppPlayback.cpp: the current playlist and the playback history
    std::string UnnamedPath() const;
    void RestoreCurrentPlaylist();
    bool PersistCurrentPlaylist();
    void SetCurrentPlaylistName(const std::string& name);
    void AppendItems(const std::string& name, const std::vector<wvp::MediaItem>& items, bool startIfIdle);
    void SavePlaybackSnapshot(const std::string& playlist, const std::string& url, int index, double position, double duration,
                              bool seekable, bool force, bool ended = false);
    void SavePlaybackHistory(bool force, bool ended = false, double positionOverride = -1);

    puimac::CGRenderer* measurer_;
    std::string dataDir_;
    app::Services svc_;
    std::unique_ptr<Engine> engine_;
    wvp::FfPlayer* player_ = nullptr;   // owned by engine_
    bool subsShown_ = false, subsDirty_ = true;
    std::uint64_t subsCheckedMs_ = 0;
    std::unique_ptr<wvp::PlaybackController> ctl_;
    app::ShellLayout layout_;
    app::ModeState modes_;
    app::BarModel model_;
    wvp::PlayerState state_ = wvp::PlayerState::Idle;
    std::optional<wvp::MediaPreparedInfo> prepared_;
    std::string currentPlaylist_;   // UTF-8 playlist name; empty = the unnamed playlist
    std::vector<wvp::MediaItem> currentPlaylistItems_;
    bool suppressPlaylistPersistence_ = false;
    std::string historyCurrentUrl_, historyCurrentPlaylistName_;
    int historyCurrentIndex_ = -1;
    std::uint64_t historyWriteAt_ = 0, playbackProgressUiAt_ = 0;
    std::string resumeDecision_;
    std::string sizedForUrl_;
    int cw_ = 1280, ch_ = 720;
    float scale_ = 2.f;
    pui::ThemeMode themeMode_ = pui::ThemeMode::System;
    std::uint32_t accent_ = 0;
    std::optional<pui::PointF> pointer_;
    app::PlaylistPanel* panelPopup_ = nullptr;   // owned by the host's popup stack
    std::set<int> heldKeys_;
    wvp::FrameStepController frameStep_{MakeFrameStepHooks()};
    Toast* toast_ = nullptr;
    std::wstring lastToast_;
    DebugOverlay* debug_ = nullptr;
    bool debugVisible_ = false;
    std::vector<app::dbg::OverlayLine> debugLines_;
    std::uint64_t debugNextMs_ = 0;
    int debugCols_ = 0;
    std::string lastTabDecision_;
    bool splitDrag_ = false;
    float splitGrabPx_ = 0;
    void RefreshDebugOverlay();
    void LayoutDebugOverlay();
    app::dbg::OverlayContext BuildDebugContext();
    std::vector<app::dbg::OverlayLine> PresenterLines() const;
    pui::Widget* infoBar_ = nullptr;
    std::uint64_t infoBarUntil_ = 0;
    bool started_ = false;

    // App's power, cursor and auto-hide rules (MainView.mm, "keep awake, cursor, mini bar")
    class MiniBarFade;
    void ReleaseKeepAwake();
    void SetCursorHidden(bool hide);
    void CursorTick(std::uint64_t now);
    bool SpaceIsPlayPause() const;
    void ApplyAutoHideMode();
    void MiniBarActivity();
    void MiniAutoHideTick(std::uint64_t now);
    void StartMiniBarAnim(app::AutoHide::Action a);
    void PlaceMiniBar();
    bool MiniBarInteractionActive() const;
    void ApplyMiniAspect();
    void ChipsTick(std::uint64_t now);
    bool WindowZoomedOrMinimized() const;
    KeepAwake keepAwake_ = KeepAwake::None;
    std::uint32_t keepAwakeId_ = 0;                // IOPMAssertionID
    app::CursorAutoHide cursorIdle_;
    bool cursorHidden_ = false;
    std::optional<pui::PointF> cursorLast_;
    app::AutoHide miniHide_;
    std::optional<app::ShellMode> autoHideMode_;   // the mode ApplyAutoHideMode last applied
    bool keyInput_ = false;                        // App::keyInput_: inside a key-down
    MiniBarFade* miniFadeBegin_ = nullptr;
    float wheelRemainder_ = 0;
    int chipsStage_ = 0, chipRefreshes_ = 0;
    std::uint64_t chipsPreparedMs_ = 0, chipsNextMs_ = 0;
    double mediaAspect_ = 16.0 / 9.0;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}
