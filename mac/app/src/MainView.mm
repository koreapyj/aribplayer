#include "MainView.h"

#import <AppKit/AppKit.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <stdexcept>

#include "Commands.h"
#include "DebugOverlayMac.h"
#include "Localize.h"
#include "LoggingMac.h"
#include "Engine.h"
#include "NativeMenuMac.h"
#include "features/BadgeLogic.h"
#include "features/Logging.h"
#include "features/LoggingFormat.h"
#include "features/FiltersPanel.h"
#include "features/FxModel.h"
#include "features/Menus.h"
#include "features/MiniLogic.h"
#include "features/MiniPlayer.h"
#include "features/OpenFlows.h"
#include "features/PlaybackExtras.h"
#include "features/PropertiesDialog.h"
#include "features/SettingsLogic.h"
#include "features/SettingsService.h"
#include "pui/Buttons.h"
#include "pui/Glyphs.h"
#include "pui/InfoBar.h"
#include "pui/TextBox.h"
#include "pui/ToolTipStyle.h"
#include "puimac/CGRenderer.h"
#include "puimac/MacTheme.h"
#include "wvp/DebugInfo.h"
#include "wvp/FfPlayer.h"
#include "wvp/JsonLite.h"
#include "wvp/M3uWriter.h"
#include "wvp/Platform.h"
#include "wvp/PlaybackController.h"
#include "wvp/PlaylistLoader.h"
#include "wvp/win32/Utf.h"   // ToWide / ToUtf8 over shim/windows.h (what the App files use)

namespace mainview {

using app::CommandId;
namespace G = pui::glyph;

/// Short pill above the bar (ShowToast): the Mac counterpart of AppRender's NoticeWidget toast.
class MainView::Toast final : public pui::Widget {
public:
    Toast() : pui::Widget(pui::Role::Text) { SetHitTestVisible(false); SetVisible(false); }
    void Show(std::wstring text, uint64_t now) {
        text_ = std::move(text);
        until_ = now + 2500;
        SetVisible(true);
        Invalidate();
        if (pui::UiHost* h = Host()) h->StartAnimating(this);
    }
    void Paint(pui::IRenderer& r, const pui::Theme& t) override {
        const pui::RectF b = Bounds();
        r.FillRoundRect(b, 16, t.flyoutSurface);
        r.StrokeRoundRect(b.Deflate(0.5f, 0.5f, 0.5f, 0.5f), 16, t.flyoutStroke, 1);
        r.Text(text_, b.Deflate(12, 0, 12, 0), Style(), t.textPrimary);
    }
    static pui::TextStyle Style() {
        pui::TextStyle ts;
        ts.size = 13;
        ts.align = pui::TextAlign::Center;
        ts.ellipsis = true;
        return ts;
    }
    bool OnTick(uint64_t now) override {
        if (now < until_) return true;
        SetVisible(false);
        return false;
    }
    const std::wstring& Text() const { return text_; }
private:
    std::wstring text_;
    uint64_t until_ = 0;
};

namespace {
constexpr float kTrafficInsetDip = 80;   // 12 + 3 x 14 + 2 x 6 + 12: the native traffic lights sit in the strip's left part

/// ShellRoot with the title bar's zones taken in the bar's own coordinates: TitleLayout::Classify's rectangles start at
/// 0, 0 (the window's left edge on Windows), while here the title bar starts after the traffic lights (kTrafficInsetDip).
class MacShellRoot final : public app::ShellRoot {
public:
    using app::ShellRoot::ShellRoot;
    pui::Widget* HitTest(pui::PointF p) override {
        pui::Widget* w = app::ShellRoot::HitTest(p);
        if (w && w != this) return w;
        app::TitleBar* t = Title();
        if (!t || !t->Visible() || !t->Bounds().Contains(p)) return w;
        const pui::RectF b = t->Bounds();
        const app::TitleZone z = t->Zone({p.x - b.l, p.y - b.t});
        if (z == app::TitleZone::None || z == app::TitleZone::Drag) return w;
        for (auto it = t->Children().rbegin(); it != t->Children().rend(); ++it)
            if (pui::Widget* c = (*it)->HitTest(p)) return c;
        return w;
    }
};
}

MainView::MainView(puimac::CGRenderer* measurer, void* videoView, const std::string& dataDir, const Options& options)
    : measurer_(measurer), dataDir_(dataDir.empty() ? wvp::platform::AppDataDir() : dataDir) {
    // App::App: Services with the default paths (AppDataDir) or an injected directory. The Services(dir)
    // constructor joins with "\\" (a Windows path), so an injected directory is set on the stores directly.
    std::error_code ec;
    std::filesystem::create_directories(dataDir_, ec);
    if (!dataDir.empty()) {
        svc_.settingsStore = wvp::SettingsStore(dataDir_ + "/settings.json");
        svc_.playlists = wvp::PlaylistStore(dataDir_ + "/Playlists");
    }
    pui::SetDefaultMeasurer(measurer);
    pui::InstallToolTipStyle(host);

    // App::Create + InitPlayer: the platform and its adapter probe (off the main thread; the first open waits for the
    // plan it makes possible), the presenter on the display adapter, FfPlayer over libffplay, the controller over it.
    engine_ = std::make_unique<Engine>(svc_, videoView, [this](std::function<void()> fn) { PostToUi(std::move(fn)); });
    engine_->StartProbe("startup");
    std::string error;
    if (!engine_->CreatePlayer(options.audioDriver, &error)) throw std::runtime_error(error);
    player_ = engine_->Player();
    ctl_ = std::make_unique<wvp::PlaybackController>(*player_, [this](std::function<void()> fn) { PostToUi(std::move(fn)); });
    engine_->Wire(*ctl_);   // plan provider (FilterManager over the probed capabilities), caption options, pipeline events
    engine_->onProblem = [this](const std::wstring& title, const std::wstring& message) { ShowInfoBar(app::InfoSeverity::Error, title, message); };
    engine_->onPipelineContentChanged = [this] { app::RefreshFiltersPanel(*this); };

    auto root = std::make_unique<MacShellRoot>(*this);
    shell = root.get();
    host.SetRoot(std::move(root));
    toast_ = shell->Add<Toast>();
    debug_ = shell->Add<DebugOverlay>();   // above the shell widgets, below popups (Layer::DebugOverlay)
    app::nativemenu::SetMacHost(&host, measurer_);
    // Player views hold no widget focus except the playlist search box; popups (menus, dialogs) focus normally.
    host.focusFilter = [this](const pui::Widget* w) {
        const pui::Widget* top = w;
        while (top && top->Parent()) top = top->Parent();
        return top != shell || dynamic_cast<const pui::TextBox*>(w) != nullptr;
    };

    // Title bar (TitleBar): the native traffic lights replace Minimize / Maximize / Close.
    app::TitleBar* title = shell->Title();
    title->MinimizeButton()->SetVisible(false);
    title->MaximizeButton()->SetVisible(false);
    title->CloseButton()->SetVisible(false);
    title->onMainMenu = [this] { ShowMainMenu(shell->Title()->MainMenuButton()); };
    title->onPin = [this] { Execute(CommandId::CycleAlwaysOnTop); };
    title->onExitFullscreen = [this] { SetFullscreen(false); };

    app::PlayerBar::Actions a;
    a.command = [this](CommandId c) { Execute(c); };
    a.seekTo = [this](double f) {
        if (model_.dur > 0) {
            const double t = std::clamp(f, 0.0, 1.0) * model_.dur;
            player_->Seek(t, false);
            SavePlaybackHistory(true, false, t);
        }
    };
    a.setVolume = [this](int v) { SetVolume(v); };
    a.subtitleMenu = [this](pui::Widget* anchor) { app::ShowSubtitleMenu(*this, anchor); };
    shell->Bar()->SetActions(a);
    app::MiniBar::Actions ma;
    ma.command = a.command;
    ma.seekTo = a.seekTo;
    shell->Mini()->SetActions(ma);

    // Controller / player events (App::WirePlayer, AppPlayback.cpp).
    ctl_->resolveResumePosition = [this](const wvp::MediaItem& item, double requestedPosition) {
        const auto state = svc_.playlists.GetItemPlaybackState(item.url);
        const double duration = state.duration > 0 ? state.duration : item.duration.value_or(0.0);
        const double candidate = std::isfinite(requestedPosition) && requestedPosition >= 0 ? requestedPosition : state.position;
        wvp::PlaylistStore::ItemPlaybackState decisionState = state;
        decisionState.position = candidate;
        decisionState.duration = duration;
        const double result = wvp::PlaylistStore::ResumePosition(decisionState);
        std::string reason;
        if (state.played) reason = "played item: reset to start";
        else if (!state.seekable || duration <= 0) reason = "unseekable or live stream: start at 0";
        else if (result <= 0) reason = "within 5 seconds of start/end: start at 0";
        else reason = "resume at " + std::to_string((int)result) + " seconds";
        SetResumeDecision(currentPlaylist_.empty() ? reason : currentPlaylist_ + ": " + reason);
        Log("resume", item.url + ": " + reason);
        return result;
    };
    ctl_->beforeLeavingCurrent = [this] {
        if (!ctl_ || !ctl_->Current()) return;
        const bool haveHistoryIdentity = !historyCurrentUrl_.empty();
        const std::string url = haveHistoryIdentity ? historyCurrentUrl_ : ctl_->Current()->url;
        const std::string playlist = haveHistoryIdentity ? historyCurrentPlaylistName_ : currentPlaylist_;
        const int index = haveHistoryIdentity ? historyCurrentIndex_ : ctl_->CurrentIndex();
        double position = model_.pos, duration = model_.dur;
        const wvp::PlayerState playerState = player_->State();
        if (playerState == wvp::PlayerState::Playing || playerState == wvp::PlayerState::Paused || playerState == wvp::PlayerState::Ended) {
            const double playerPosition = player_->Position(), playerDuration = player_->Duration();
            if (std::isfinite(playerPosition) && playerPosition >= 0) position = playerPosition;
            if (std::isfinite(playerDuration) && playerDuration > 0) duration = playerDuration;
        }
        SavePlaybackSnapshot(playlist, url, index, position, duration, model_.seekable, true);
    };
    ctl_->CurrentChanged.add([this](const wvp::MediaItem* it, int idx) {
        historyCurrentUrl_ = it ? it->url : std::string();
        historyCurrentIndex_ = idx;
        historyCurrentPlaylistName_ = currentPlaylist_;
        Log("queue", "current #" + std::to_string(idx) + ": " + (it ? it->url : std::string("(none)")));
        model_.title = it ? wvp::ToWide(it->DisplayTitle()) : std::wstring();
        if (!it) sizedForUrl_.clear();
        model_.prepared = false; model_.seekable = false; model_.pos = 0; model_.dur = 0; model_.chips.clear();
        subsDirty_ = true;
        prepared_.reset();
        frameStep_.Reset();
        if (!it && currentPlaylistItems_.empty()) {   // nothing left to play: leave the video-only modes
            if (modes_.mini) SetMiniPlayer(false);
            else if (modes_.fullscreen) SetFullscreen(false);
        }
        shell->Title()->SetTitle(model_.title);
        OnMediaOpened.raise(it);
        shell->Panel()->OnCurrentChanged();
        SyncModel();
    });
    ctl_->QueueChanged.add([this] {
        currentPlaylistItems_ = ctl_->Items();
        if (!suppressPlaylistPersistence_ && !PersistCurrentPlaylist())
            ShowInfoBar(app::InfoSeverity::Error, app::Tr(L"Could not save the current playlist", L"현재 재생목록을 저장할 수 없습니다"),
                        currentPlaylist_.empty() ? app::Tr(L"Unnamed playlist", L"이름 없는 재생목록") : wvp::ToWide(currentPlaylist_));
        shell->Panel()->OnQueueChanged();
        SyncModel();
    });
    ctl_->ModeChanged.add([this] { SyncModel(); });
    player_->Prepared.add([this](const wvp::MediaPreparedInfo& p) {
        model_.prepared = true;
        model_.seekable = p.Seekable;
        model_.dur = p.Duration;
        prepared_ = p;
        if (!p.Title.empty()) { model_.title = wvp::ToWide(p.Title); shell->Title()->SetTitle(model_.title); }
        subsDirty_ = true;
        RefreshChips();
        const std::uint64_t now = NowMs();   // the presenter / decoder settle after PREPARED: refresh at +0.5 s and +1.5 s
        chipsAt_[0] = now + 500;
        chipsAt_[1] = now + 1500;
        OnMediaPrepared.raise(p);
        SyncModel();
        // Like Windows' Prepared handler: 100%, once per opened URL, only in the windowed player view.
        const wvp::MediaItem* it = ctl_->Current();
        if (p.HasVideo && it && sizedForUrl_ != it->url && modes_.Mode() == app::ShellMode::NowPlaying &&
            !modes_.mini && !modes_.fullscreen && hooks.sizeWindowToVideo && hooks.sizeWindowToVideo(p, app::WindowScale(CommandId::WindowSize100)))
            sizedForUrl_ = it->url;
    });
    player_->FrameStepDone.add([this](int, bool moved) { frameStep_.StepDone(moved); });
    player_->KeyframeSeekDone.add([this](int, bool moved, double, double) { frameStep_.StepDone(moved); });
    player_->StateChanged.add([this](wvp::PlayerState s) {
        static const char* names[] = {"Idle", "Opening", "Playing", "Paused", "Stopped", "Ended", "Error"};
        Log("player", std::string("state: ") + names[(int)s]);
        state_ = s;
        model_.playing = s == wvp::PlayerState::Playing;
        ApplyWindowTopmost();   // "Enable when playing" follows the state
        SyncModel();
        if (s == wvp::PlayerState::Paused) SavePlaybackHistory(true);
    });
    player_->PositionChanged.add([this](double pos, double dur) {
        model_.pos = std::isnan(pos) ? 0 : pos;
        if (dur > 0) model_.dur = dur;
        SavePlaybackHistory(false);
        if (NowMs() >= playbackProgressUiAt_) { playbackProgressUiAt_ = NowMs() + 1000; OnPlaybackProgress.raise(); }
        SyncModel();
    });
    player_->Error.add([this](const wvp::PlayerErrorInfo& e) {
        if (e.DeviceLost) return;   // Engine reopens on a fresh device
        Log("player", "ERROR code " + std::to_string(e.Code) + ": " + e.Message + " (" + e.Url + ")");
        OnMediaError.raise(e);
        app::ShowPlaybackError(*this, e);
    });
    player_->Ended.add([this] { Log("player", "ended"); SavePlaybackHistory(true, true); OnMediaEnded.raise(); });
    SyncModel();
}

MainView::~MainView() {
    *alive_ = false;
    app::StopSystemCaptionSettingsWatch();
    app::nativemenu::SetMacHost(nullptr, nullptr);
    ctl_->Dispose();
    ctl_.reset();
    engine_->ReleasePlayer();   // ffp_destroy (blocks until libffplay stopped), then the presenter; main thread
    engine_.reset();
    pui::SetDefaultMeasurer(nullptr);
}

void MainView::Start() {
    if (started_) return;
    started_ = true;
    app::LoadSettingsAtStartup(*this);   // theme, window placement, filters
    RestoreCurrentPlaylist();            // load the saved current list before inputs (Finder, command line) use it
    shell->Title()->SetTopmostMode(svc_.settings.alwaysOnTop);
    app::InitPlaybackExtras(*this);
    OnPlayerReady.raise();               // LoadSettingsAtStartup's hook: captions, shuffle, repeat on the controller
    // App::Create: Settings > Debug logging (the Mac folder: LoggingMac.h), libffplay's log callback while a sink is
    // active, the system caption style and its watch.
    logmac::ApplyDebugLogging(svc_.settings.debugLogging, logmac::LogsDir(svc_.settingsStore.Path()));
    app::log::InstallEngineHooks();
    Log("app", "start: libffplay " + wvp::FfPlayer::Version() + ", debug overlay: Tab");
    app::OnSystemCaptionSettingsChanged(*this);
    app::StartSystemCaptionSettingsWatch(*this);
    ApplyWindowTopmost();
    ApplyMode();
}

void MainView::Shutdown() {
    if (!started_) return;
    app::StopSystemCaptionSettingsWatch();
    if (ctl_->Current()) SavePlaybackHistory(true);
    if (modes_.mini) SetMiniPlayer(false);   // so settings.window is the normal placement, not the mini rect
    app::SaveSettingsAtShutdown(*this);
    svc_.playlists.FlushPlaybackWrites();
}

pui::IRenderer& MainView::Measurer() { return *measurer_; }

pui::Theme MainView::MakeTheme(float scale) const {
    const bool dark = themeMode_ == pui::ThemeMode::Dark || (themeMode_ == pui::ThemeMode::System && puimac::SystemIsDark());
    return puimac::MakeMacTheme(dark, accent_, scale);
}

/// App::PostToUi: always queued on the main thread, never inline (FfPlayer's UiPost contract: it is called on
/// libffplay's threads and from inside event dispatch).
void MainView::PostToUi(std::function<void()> fn) {
    auto alive = alive_;
    __block std::function<void()> f = std::move(fn);
    dispatch_async(dispatch_get_main_queue(), ^{ if (*alive && f) f(); });
}

bool MainView::Loaded() const { return ctl_ && ctl_->Current() != nullptr; }

/// App::RefreshChips: H/W | S/W, HDR, codecs, channels from the engine's debug info + media info.
void MainView::RefreshChips() {
    std::vector<app::badge::Chip> next;
    if (Loaded()) {
        if (auto mi = player_->GetMediaInfo()) {
            wvp::DebugInfo debug;
            if (auto json = player_->GetDebugInfoJson()) debug = wvp::ParseDebugInfo(*json);
            next = app::badge::BuildChips(debug, *mi);
        }
    }
    if (next == model_.chips) return;
    model_.chips = std::move(next);
    SyncModel();
}

void MainView::SetDeinterlace(wvp::DeinterlaceMode mode) {
    wvp::FilterSettings s = svc_.filters.Settings();
    if (s.deinterlace == mode) return;
    s.deinterlace = mode;
    engine_->ApplyFilterSettings(s);
    app::QueueFilterSettingsSave(*this);
}

// --- layout -----------------------------------------------------------------------------------------------------------

void MainView::Resize(int wPx, int hPx, float scale) {
    cw_ = std::max(1, wPx); ch_ = std::max(1, hPx);
    if (scale_ != scale) {
        scale_ = scale;
        shell->Bar()->SetScale(scale);
        shell->Title()->SetScale(scale);
        shell->Mini()->SetScale(scale);
    }
    host.SetSize({cw_ / scale_, ch_ / scale_});
    ApplyMode();
}

void MainView::ApplyMode() {
    if (modes_.mini) {
        layout_ = app::ComputeMiniShellLayout(cw_, ch_, scale_);
    } else {
        app::ShellLayoutIn in;
        in.cwPx = cw_; in.chPx = ch_; in.scale = scale_; in.mode = modes_.Mode();
        in.panelWanted = svc_.settings.playlistPanel;
        in.panelWidthDip = (float)svc_.settings.playlistPanelWidthDip;
        layout_ = app::ComputeShellLayout(in);
    }
    host.CloseAllPopups();
    shell->ApplyLayout(layout_, modes_.Mode());
    // macOS: the strip's left part belongs to the native traffic lights; the TitleBar widget starts after them.
    if (!layout_.title.Empty() && !modes_.mini) {
        pui::RectF t = layout_.title;
        if (!modes_.fullscreen) t.l += kTrafficInsetDip;
        // TitleLayout reserves Minimize / Maximize / Close (3 x kTitleButtonDip) at the right edge and puts Pin left of
        // them. They are native here, so the widget extends past the window by that much: Pin lands at the right edge
        // and the hidden caption slots (and full screen's exit button: Mac full screen exits natively) fall outside.
        t.r += 3 * app::kTitleButtonDip;
        shell->Title()->SetBounds(t);
        shell->Title()->MinimizeButton()->SetVisible(false);
        shell->Title()->MaximizeButton()->SetVisible(false);
        shell->Title()->CloseButton()->SetVisible(false);
    }
    LayoutToast();
    if (hooks.videoRect) hooks.videoRect(layout_.video);
    UpdateOverlays();
    LayoutDebugOverlay();
    SyncModel();
    if (hooks.layoutChanged) hooks.layoutChanged();
}

void MainView::SetFullscreenState(bool on) {
    if (modes_.fullscreen == on) return;
    modes_ = app::WithFullscreen(modes_, on);
    ApplyMode();
    ApplyWindowTopmost();
    OnModeChanged.raise(modes_.Mode());
}

void MainView::SetFullscreen(bool on) {
    if (modes_.fullscreen == on) return;
    if (on && modes_.mini) SetMiniPlayer(false);
    if (hooks.toggleFullScreen) hooks.toggleFullScreen();
}

/// App::SetMiniPlayer: needs media; the window shape is the host's (MiniPlayerMac.mm), the layout ComputeMiniShellLayout.
void MainView::SetMiniPlayer(bool on) {
    if (on && !Loaded()) return;
    if (on == modes_.mini) return;
    if (on && modes_.fullscreen) {   // leave full screen first; AppKit animates out, then the mini shape applies
        SetFullscreen(false);
        auto alive = alive_;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.8 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            if (*alive && !modes_.fullscreen) SetMiniPlayer(true);
        });
        return;
    }
    modes_ = app::WithMini(modes_, on);
    if (on) app::EnterMiniPlayer(*this); else app::ExitMiniPlayer(*this);   // features/MiniPlayer.h (MiniPlayerMac.mm)
    if (hooks.miniPlayer) hooks.miniPlayer(on);
    ApplyWindowTopmost();
    ApplyMode();
    OnModeChanged.raise(modes_.Mode());
}

/// Full screen: the title strip and the bar appear only while the cursor is in their edge zone / on them (App's edge
/// mode: snap, no slide). Mini player: the reduced bar shows while the pointer is over the window. Windowed: both are
/// docked and always shown.
void MainView::UpdateOverlays() {
    const bool interacting = UserPopupOpen() || shell->Bar()->SeekDragging();
    if (modes_.mini) {
        app::MiniBar* mini = shell->Mini();
        mini->SetVisible(pointer_.has_value() || interacting || state_ != wvp::PlayerState::Playing);
        return;
    }
    if (!modes_.fullscreen) return;
    const pui::SizeF s = host.Size();
    bool top = false, bottom = false;
    if (pointer_) {
        app::EdgeZones z = app::EdgeZonesAt(pointer_->x, pointer_->y, s.w, s.h);
        top = z.top || (shell->Title()->Visible() && shell->Title()->Bounds().Contains(*pointer_));
        bottom = z.bottom || (shell->Bar()->Visible() && shell->Bar()->Bounds().Contains(*pointer_));
    }
    shell->Title()->SetVisible(top || (interacting && shell->Title()->Visible()));
    shell->Bar()->SetVisible(bottom || (interacting && shell->Bar()->Visible()));
}

void MainView::PointerAt(std::optional<pui::PointF> p) {
    pointer_ = p;
    UpdateOverlays();
}

bool MainView::IsTitleStripPoint(pui::PointF p) const {
    return !modes_.fullscreen && !modes_.mini && !layout_.title.Empty() && layout_.title.Contains(p);
}

bool MainView::IsVideoPoint(pui::PointF p) const {
    const app::RectPx& v = layout_.video;
    if (v.Empty()) return false;
    return p.x * scale_ >= v.l && p.x * scale_ < v.r && p.y * scale_ >= v.t && p.y * scale_ < v.b;
}

void MainView::PressAt(pui::Widget* hit) {
    app::PlaylistPanel* panel = shell->Panel();
    const bool inPanel = hit && panel->Visible() && (hit == panel || panel->IsAncestorOf(hit));
    panel->SetActive(inPanel);
    if (!inPanel && host.Focused() && host.Focused() == panel->Search()) host.SetFocus(nullptr);
}

/// App::RouteDropAtClient.
bool MainView::AcceptsDropAt(pui::PointF dip) const {
    for (pui::Widget* popup : host.Popups())
        if (popup && popup->IsEffectivelyVisible() && (popup->Bounds().Contains(dip) || popup->HitTest(dip))) return false;
    if (host.HasModalPopup()) return false;
    if (modes_.Mode() == app::ShellMode::NowPlaying && shell->Panel()->Visible() && layout_.panel.Contains(dip.x, dip.y))
        return shell->Panel()->AcceptsDrops();
    return app::RouteDrop(IsVideoPoint(dip), false, shell->Bar()->Visible() && layout_.bar.Contains(dip.x, dip.y),
                          layout_.title.Contains(dip.x, dip.y)) != app::DropRoute::Rejected ||
           modes_.mini;
}

/// App::HandleDropAtClient.
bool MainView::DropAt(pui::PointF dip, const std::vector<std::wstring>& paths) {
    if (paths.empty() || !AcceptsDropAt(dip)) return false;
    if (modes_.Mode() == app::ShellMode::NowPlaying && shell->Panel()->Visible() && layout_.panel.Contains(dip.x, dip.y))
        return shell->Panel()->DropPaths(paths, dip);
    return app::DispatchDroppedPaths(*this, app::DropRoute::VideoView, paths);
}

// --- model ------------------------------------------------------------------------------------------------------------

void MainView::Tick() {
    const std::uint64_t now = NowMs();
    if (Loaded()) {
        const double pos = player_->Position(), dur = player_->Duration();
        if (!std::isnan(pos)) model_.pos = pos;
        if (dur > 0) model_.dur = dur;
        for (std::uint64_t& at : chipsAt_)
            if (at && now >= at) { at = 0; RefreshChips(); }
    }
    if (infoBar_ && now >= infoBarUntil_) {
        if (host.IsPopupOpen(infoBar_)) host.ClosePopup(infoBar_);
        infoBar_ = nullptr;
    }
    engine_->Poll(now);
    if (modes_.mini) UpdateOverlays();
    if (debugVisible_ && now >= debugNextMs_) {   // ~4 Hz while shown, nothing while hidden (App::Tick)
        debugNextMs_ = now + 250;
        RefreshDebugOverlay();
    }
    SyncModel();
}

void MainView::SyncModel() {
    app::BarModel m = model_;
    m.loaded = Loaded();
    m.canPlaySelection = CanPlaySelection();
    m.hasPrev = ctl_->HasPrevious();
    m.hasNext = ctl_->HasNext();
    m.shuffle = ctl_->Shuffle();
    m.repeat = (int)ctl_->Repeat();
    m.volume = ctl_->Volume();
    m.muted = ctl_->IsMuted();
    m.playing = state_ == wvp::PlayerState::Playing;
    m.fullscreen = modes_.fullscreen;
    m.mini = modes_.mini;
    m.panelOpen = svc_.settings.playlistPanel && !layout_.panelOverlay;
    m.panelAvailable = modes_.Mode() == app::ShellMode::NowPlaying;
    m.controlPanelOpen = app::IsFiltersPanelOpen(*this);
    m.timeMode = svc_.settings.timeRemaining ? app::sl::TimeDisplayMode::Remaining : app::sl::TimeDisplayMode::Elapsed;
    if (!m.loaded) { m.prepared = false; m.pos = 0; m.dur = 0; m.chips.clear(); }
    model_.loaded = m.loaded;
    shell->Bar()->SetModel(m);
    shell->Mini()->SetModel(m);
    shell->Title()->SetTopmostMode(svc_.settings.alwaysOnTop);
    // Mac: the subtitle button is lit with showCaptions on and a selectable subtitle track selected (embedded,
    // external or ARIB caption, never superimpose; BarLogic's B_Subs has no toggled state).
    // (media info is a JSON round trip: at most every 200 ms, at once after a command)
    const std::uint64_t now = NowMs();
    if (subsDirty_ || now - subsCheckedMs_ >= 200) {
        subsShown_ = m.loaded && SubtitlesShown();
        subsCheckedMs_ = now;
        subsDirty_ = false;
    }
    if (auto* subs = dynamic_cast<app::GlyphButton*>(shell->Bar()->ButtonWidget(app::B_Subs))) subs->SetToggled(m.loaded && subsShown_);
}

bool MainView::SubtitlesShown() {
    if (!svc_.settings.showCaptions) return false;
    auto mi = player_ ? player_->GetMediaInfo() : std::nullopt;
    return mi && std::any_of(mi->Streams.begin(), mi->Streams.end(), [](const wvp::StreamInfo& st) {
        return st.Selected && app::fx::IsCycleableSubtitle(st);
    });
}

bool MainView::CanPlaySelection() const {
    return !Loaded() && shell->Panel()->Visible() && shell->Panel()->CanPlaySelection();
}

// --- playlists + history (AppPlayback.cpp) ----------------------------------------------------------------------------

std::string MainView::UnnamedPath() const { return wvp::platform::Combine(dataDir_, "unnamed.m3u8"); }

void MainView::RestoreCurrentPlaylist() {
    currentPlaylist_ = svc_.settings.currentPlaylist;
    if (!currentPlaylist_.empty() && !svc_.playlists.Exists(currentPlaylist_)) currentPlaylist_.clear();
    currentPlaylistItems_.clear();
    if (!currentPlaylist_.empty()) {
        currentPlaylistItems_ = svc_.playlists.Load(currentPlaylist_);
    } else {
        const std::string path = UnnamedPath();
        if (wvp::platform::Exists(path)) {
            try { currentPlaylistItems_ = wvp::PlaylistLoader().Load(path).items; }
            catch (...) { currentPlaylistItems_.clear(); }
        }
    }
    svc_.settings.currentPlaylist = currentPlaylist_;
    svc_.settingsStore.Save(svc_.settings);
    // The restored list is the queue without a current item (the idle view), as on Windows.
    suppressPlaylistPersistence_ = true;
    ctl_->SetQueue(currentPlaylistItems_, -1, false);
    suppressPlaylistPersistence_ = false;
    shell->Panel()->OnCurrentPlaylistChanged();
}

bool MainView::PersistCurrentPlaylist() {
    if (currentPlaylist_.empty()) return wvp::M3uWriter::Save(UnnamedPath(), currentPlaylistItems_, false);
    if (svc_.playlists.IsReference(currentPlaylist_)) return true;   // referenced sources are never overwritten
    return svc_.playlists.Save(currentPlaylist_, currentPlaylistItems_);
}

void MainView::SetCurrentPlaylistName(const std::string& name) {
    if (currentPlaylist_ == name) return;
    currentPlaylist_ = name;
    svc_.settings.currentPlaylist = name;
    svc_.settingsStore.Save(svc_.settings);
    shell->Panel()->OnCurrentPlaylistChanged();
}

void MainView::SavePlaybackSnapshot(const std::string& playlist, const std::string& url, int index, double position, double duration,
                                    bool seekable, bool force, bool ended) {
    if (url.empty()) return;
    const std::uint64_t now = NowMs();
    if (!force && now < historyWriteAt_) return;
    if (!std::isfinite(position) || position < 0) position = 0;
    if (!std::isfinite(duration) || duration < 0) duration = 0;
    const bool canResume = seekable && duration > 0;
    const bool played = wvp::PlaylistStore::IsPlayedPosition(position, duration, ended);
    svc_.playlists.SavePlaybackStateAsync(playlist, url, position, duration, played, canResume, index);
    historyWriteAt_ = now + 1000;
    if (played) shell->Panel()->OnItemPlayed(url);
    if (played) resumeDecision_ = "played: reset to start (near end / EOF)";
}

void MainView::SavePlaybackHistory(bool force, bool ended, double positionOverride) {
    if (!Loaded()) return;
    const double position = positionOverride >= 0 ? positionOverride : model_.pos;
    SavePlaybackSnapshot(currentPlaylist_, ctl_->Current()->url, ctl_->CurrentIndex(), position, model_.dur, model_.seekable, force, ended);
}

std::vector<wvp::MediaItem> MainView::PlaylistItems(const std::string& name) {
    if (!name.empty() && svc_.playlists.IsReference(name)) return svc_.playlists.Load(name);
    if (name == currentPlaylist_) return currentPlaylistItems_;
    if (!name.empty()) return svc_.playlists.Load(name);
    const std::string path = UnnamedPath();
    if (!wvp::platform::Exists(path)) return {};
    try { return wvp::PlaylistLoader().Load(path).items; }
    catch (...) { return {}; }
}

bool MainView::SavePlaylistItems(const std::string& name, const std::vector<wvp::MediaItem>& items) {
    if (!name.empty() && svc_.playlists.IsReadOnly(name)) return false;
    const bool saved = name.empty() ? wvp::M3uWriter::Save(UnnamedPath(), items, false) : svc_.playlists.Save(name, items);
    if (name == currentPlaylist_) {
        const std::string currentUrl = Loaded() ? ctl_->Current()->url : std::string();
        int current = -1;
        if (!currentUrl.empty())
            for (size_t i = 0; i < items.size(); ++i)
                if (items[i].url == currentUrl) { current = (int)i; break; }
        currentPlaylistItems_ = items;
        if (!currentUrl.empty() && current < 0) ctl_->Stop();
        suppressPlaylistPersistence_ = true;
        ctl_->SetQueue(items, current, false);
        suppressPlaylistPersistence_ = false;
    }
    if (!saved) ShowInfoBar(app::InfoSeverity::Error, app::Tr(L"Could not save the playlist", L"재생목록을 저장할 수 없습니다"),
                            name.empty() ? app::Tr(L"Unnamed playlist", L"이름 없는 재생목록") : wvp::ToWide(name));
    return saved;
}

void MainView::PlayPlaylist(const std::string& name, const std::vector<wvp::MediaItem>& items, int start) {
    PlayPlaylistAtPosition(name, items, start, -1);
}

void MainView::PlayPlaylistAtPosition(const std::string& name, const std::vector<wvp::MediaItem>& items, int start, double position) {
    if (Loaded()) SavePlaybackHistory(true);
    const bool keepPlaying = currentPlaylist_ != name && currentPlaylistItems_ == items && ctl_->CurrentIndex() == start && Loaded() && position <= 0;
    SetCurrentPlaylistName(name);
    currentPlaylistItems_ = items;
    if (!PersistCurrentPlaylist())
        ShowInfoBar(app::InfoSeverity::Error, app::Tr(L"Could not save the playlist", L"재생목록을 저장할 수 없습니다"),
                    name.empty() ? app::Tr(L"Unnamed playlist", L"이름 없는 재생목록") : wvp::ToWide(name));
    if (!keepPlaying) ctl_->SetQueue(currentPlaylistItems_, start, true, position);
}

void MainView::AppendPlaylistItems(const std::string& name, const std::vector<wvp::MediaItem>& items) {
    AppendItems(name, items, false);   // playlist views (drops, Add files): append only, never start or change playback
}

void MainView::AppendItems(const std::string& name, const std::vector<wvp::MediaItem>& items, bool startIfIdle) {
    if (items.empty()) return;
    if (!name.empty() && svc_.playlists.IsReadOnly(name)) {
        ShowInfoBar(app::InfoSeverity::Warning, app::Tr(L"Playlist is read-only", L"재생목록이 읽기 전용입니다"),
                    app::Tr(L"This playlist references a source file or URL. Import it to make an editable copy.",
                            L"이 재생목록은 원본 파일 또는 주소를 참조합니다. 가져오기로 편집 가능한 사본을 만드세요."));
        return;
    }
    if (name != currentPlaylist_) {
        auto all = PlaylistItems(name);
        all.insert(all.end(), items.begin(), items.end());
        SavePlaylistItems(name, all);
        return;
    }
    const bool startAtFirst = startIfIdle && ctl_->CurrentIndex() < 0;
    ctl_->Enqueue(items);   // QueueChanged writes the current playlist
    if (startAtFirst && !ctl_->Items().empty()) ctl_->PlayAt(0);
}

void MainView::AppendToCurrentPlaylist(std::vector<wvp::MediaItem> items) {
    AppendItems(currentPlaylist_, items, true);   // Open commands: if nothing has played yet, start the first item
}

void MainView::ReplaceUnnamedPlaylistAtPosition(std::vector<wvp::MediaItem> items, double position) {
    if (items.empty()) return;
    if (Loaded()) SavePlaybackHistory(true);
    SetCurrentPlaylistName({});
    currentPlaylistItems_ = std::move(items);
    if (!PersistCurrentPlaylist())
        ShowInfoBar(app::InfoSeverity::Error, app::Tr(L"Could not save the unnamed playlist", L"이름 없는 재생목록을 저장할 수 없습니다"),
                    app::Tr(L"The playlist could not be written.", L"재생목록을 쓸 수 없습니다."));
    ctl_->SetQueue(currentPlaylistItems_, 0, true, position);
}

void MainView::PlaylistRenamed(const std::string& from, const std::string& to) {
    shell->Panel()->OnPlaylistRenamed(from, to);
    if (currentPlaylist_ == from) SetCurrentPlaylistName(to);
}

void MainView::PlaylistDeleted(const std::string& name) {
    shell->Panel()->OnPlaylistDeleted(name);
    if (currentPlaylist_ != name) return;
    std::vector<wvp::MediaItem> unnamed = PlaylistItems({});
    SetCurrentPlaylistName({});
    currentPlaylistItems_ = std::move(unnamed);
    ctl_->Stop();
    suppressPlaylistPersistence_ = true;
    ctl_->SetQueue(currentPlaylistItems_, -1, false);
    suppressPlaylistPersistence_ = false;
}

void MainView::PlaylistsChanged() { shell->Panel()->OnPlaylistsChanged(); }

void MainView::CloseMedia() {
    if (!Loaded()) return;
    sizedForUrl_.clear();
    ctl_->Stop();   // beforeLeavingCurrent saves the resume position
    suppressPlaylistPersistence_ = true;
    ctl_->SetQueue(currentPlaylistItems_, -1, false);   // no current item: the idle view
    suppressPlaylistPersistence_ = false;
}

// --- notices / theme --------------------------------------------------------------------------------------------------

/// App::ShowInfoBar: a pui::InfoBar under the title strip, auto-dismissed after 12 s, replacing the previous one.
void MainView::ShowInfoBar(app::InfoSeverity severity, const std::wstring& title, const std::wstring& message) {
    Log("notice", wvp::ToUtf8(title + L": " + message));
    if (infoBar_ && host.IsPopupOpen(infoBar_)) host.ClosePopup(infoBar_);
    infoBar_ = nullptr;
    auto bar = std::make_unique<pui::InfoBar>();
    pui::InfoBar* raw = bar.get();
    bar->SetSeverity((pui::InfoBarSeverity)(int)severity);
    bar->SetTitle(title);
    bar->SetMessage(message);
    bar->SetClosable(true);
    bar->onClosed = [this, raw] { if (host.IsPopupOpen(raw)) host.ClosePopup(raw); if (infoBar_ == raw) infoBar_ = nullptr; };
    const pui::SizeF cs = host.Size();
    const float w = std::min(560.f, std::max(0.f, cs.w - 16.f));
    float y = modes_.fullscreen || modes_.mini ? 8.f : app::kStripDip + 8.f;
    const float h = std::min(raw->HeightForWidth(measurer_, w), std::max(0.f, cs.h - y - 8.f));
    const float x = std::clamp(std::round((cs.w - w) / 2), 0.f, std::max(0.f, cs.w - w));
    y = std::clamp(y, 0.f, std::max(0.f, cs.h - h));
    raw->SetBounds(pui::RectF::XYWH(x, y, w, h));
    pui::PopupOptions po;
    po.lightDismiss = false;
    po.takeFocus = false;
    po.onClosed = [this, raw] { if (infoBar_ == raw) infoBar_ = nullptr; };
    infoBar_ = host.OpenPopup(std::move(bar), po);
    infoBarUntil_ = NowMs() + 12000;
}

void MainView::ShowToast(const std::wstring& text) {
    if (!text.empty()) lastToast_ = text;
    if (toast_ && !text.empty()) {
        toast_->Show(text, host.Now());
        LayoutToast();
    }
}

/// The toast pill fits its text (24 DIP padding), centred near the video's bottom, never wider than the video - 32.
void MainView::LayoutToast() {
    if (!toast_) return;
    const pui::RectF v = layout_.video.Empty() ? pui::RectF{0, 0, host.Size().w, host.Size().h}
                                               : pui::RectF{layout_.video.l / scale_, layout_.video.t / scale_, layout_.video.r / scale_, layout_.video.b / scale_};
    float textW = 336;
    if (measurer_ && !toast_->Text().empty()) textW = measurer_->MeasureText(toast_->Text(), Toast::Style(), 0).w;
    const float w = std::max(0.f, std::min(v.w() - 32, std::max(120.f, std::ceil(textW) + 24))), h = 32;
    const float bottom = modes_.mini ? v.b - app::kMiniBarDip - 8 : v.b - 24;
    toast_->SetBounds({v.cx() - w / 2, bottom - h, v.cx() + w / 2, bottom});
}

void MainView::ApplyTheme(pui::ThemeMode mode, uint32_t accent) {
    themeMode_ = mode;
    accent_ = accent;
    if (hooks.applyTheme) hooks.applyTheme(mode, accent);
    OnThemeChanged.raise(host.GetTheme());
}

void MainView::ApplyWindowTopmost() {
    if (modes_.mini) return;   // the mini player keeps its own (floating) level
    const bool want = app::sl::ShouldBeTopmost(svc_.settings.alwaysOnTop, state_ == wvp::PlayerState::Playing, modes_.fullscreen);
    if (hooks.applyTopmost) hooks.applyTopmost(want);
    shell->Title()->SetTopmostMode(svc_.settings.alwaysOnTop);
}

std::vector<wvp::VideoGpuPref> MainView::VideoGpus() const {
    std::vector<wvp::VideoGpuPref> v;
    for (const auto& a : engine_->Candidates()) v.push_back({a.luid, a.name, a.vendorId, a.deviceId});
    return v;
}

void MainView::VideoGpuSettingChanged() { engine_->VideoGpuSettingChanged(); }

// --- input ------------------------------------------------------------------------------------------------------------

bool MainView::UserPopupOpen() const {
    // Only the app's own notice is excluded, as App::notices_; the toast is a shell child, not a popup.
    return host.PopupCount() > (infoBar_ && host.IsPopupOpen(infoBar_) ? 1u : 0u);
}

/// App::Proc WM_KEYDOWN: the host (popup, focused widget) first, then an active playlist panel, then the key table
/// unless a focused text box owns the key (App::TextInputOwnsKey: plain / Shift keys type there; Esc and F-keys pass).
bool MainView::Key(int vk, unsigned mods, bool repeat) {
    heldKeys_.insert(vk);
    if (vk == pui::key::Tab && !(mods & (pui::ModCtrl | pui::ModAlt))) {   // App's WM_KEYDOWN: before the host
        const bool userPopup = UserPopupOpen();   // App::UserPopupOpen
        const app::dbg::TabAction tab = app::dbg::DecideTab(userPopup, (mods & pui::ModShift) != 0);
        lastTabDecision_ = std::string(tab == app::dbg::TabAction::ToggleOverlay ? "toggle" : tab == app::dbg::TabAction::Ignore ? "ignore" : "traversal") +
                           " mode=" + std::to_string((int)modes_.Mode()) + " popups=" + std::to_string(host.PopupCount()) +
                           " userPopup=" + (userPopup ? "1" : "0") + " mods=" + std::to_string(mods);
        if (tab != app::dbg::TabAction::FocusTraversal) {
            if (tab == app::dbg::TabAction::ToggleOverlay && !repeat) ToggleDebugOverlay("keydown");
            return true;
        }
    }
    if (host.KeyDown(vk, mods)) return true;
    app::PlaylistPanel* panel = shell->Panel();
    if (panel->Visible() && panel->Active() && !UserPopupOpen() && panel->OnKeyDown(vk, mods)) return true;
    const bool fkey = vk >= 0x70 && vk <= 0x87;   // VK_F1 .. VK_F24
    if (vk != pui::key::Escape && !fkey && pui::TextBox::IsTextInputFocused(host) && !(mods & (pui::ModCtrl | pui::ModAlt))) return false;
    const CommandId c = app::LookupKey(vk, mods);
    if (c == CommandId::None) return false;
    RunKeyCommand(vk, c, repeat);
    return true;
}

void MainView::KeyUp(int vk) {
    heldKeys_.erase(vk);
    frameStep_.KeyUp(vk);
}

/// App::RunKeyCommand: frame / keyframe steps and timed seeks go through the held-key step controller.
void MainView::RunKeyCommand(int vk, CommandId c, bool repeat) {
    const CommandId m = app::MiniRemapCommand(c, modes_.mini);
    if (m == CommandId::FrameNext || m == CommandId::FramePrevious) {
        frameStep_.KeyDown(vk, kKeyStepFrame, m == CommandId::FrameNext ? +1 : -1, repeat);
        return;
    }
    if (m == CommandId::KeyframeNext || m == CommandId::KeyframePrevious) {
        frameStep_.KeyDown(vk, kKeyStepKeyframe, m == CommandId::KeyframeNext ? +1 : -1, repeat);
        return;
    }
    if (const int sec = app::SeekSeconds(m)) {
        frameStep_.KeyDown(vk, kKeyStepSkip + std::abs(sec), sec > 0 ? +1 : -1, repeat);
        return;
    }
    Execute(c);
}

wvp::FrameStepController::Hooks MainView::MakeFrameStepHooks() {
    wvp::FrameStepController::Hooks h;
    h.step = [this](int action, int direction) {
        if (!player_ || !Loaded()) return -1;
        if (action >= kKeyStepSkip) {
            if (!model_.Seekable()) return -1;
            const double sec = (action - kKeyStepSkip) * (direction > 0 ? 1.0 : -1.0);
            const double before = player_->Position();
            const int rc = player_->Skip(sec);
            if (rc == 0) SavePlaybackHistory(true, false, std::clamp(std::isfinite(before) ? before + sec : model_.pos + sec, 0.0, model_.dur));
            return rc;
        }
        if (action != kKeyStepKeyframe) return player_->StepFrame(direction);
        return player_->SeekKeyframe(direction);
    };
    // Keys of this app only: NSEvent key-down / key-up of the key window (a released key is erased in KeyUp).
    h.held = [this](int key) { return heldKeys_.count(key) > 0 && NSApp.isActive; };
    h.nowMs = [] { return NowMs(); };
    return h;
}

void MainView::VideoWheel(float notches) {
    if (notches == 0) return;
    SetVolume(ctl_->Volume() + (int)std::lround(notches * 5));
}

void MainView::SetVolume(int v) {
    v = std::clamp(v, 0, 100);
    ctl_->SetVolume(v);
    if (v > 0 && ctl_->IsMuted()) ctl_->SetMuted(false);
    SyncModel();
}

void MainView::TogglePlaylistPanel() {
    if (modes_.fullscreen || modes_.mini) return;
    const bool narrow = layout_.panelOverlay;
    const int videoW = layout_.video.w();
    svc_.settings.playlistPanel = !svc_.settings.playlistPanel;
    svc_.settingsStore.Save(svc_.settings);
    // The window grows / shrinks by the panel so the video keeps its size (App::ResizeWindowForPlaylistPanel).
    if (!narrow && !layout_.video.Empty() && hooks.resizeForPanel) hooks.resizeForPanel(videoW, svc_.settings.playlistPanel);
    else ApplyMode();
}

/// App::TogglePause.
void MainView::TogglePause() {
    if (!Loaded()) {
        if (CanPlaySelection()) { shell->Panel()->PlaySelection(); return; }
    }
    using wvp::PlayerState;
    if (state_ == PlayerState::Ended || state_ == PlayerState::Stopped || state_ == PlayerState::Idle || state_ == PlayerState::Error)
        app::PlayPlaylistEntry(*this, currentPlaylist_, 0, true);
    else ctl_->TogglePause();
}

/// App::Capture: the engine's snapshot (optionally with the caption layers) to the clipboard.
void MainView::Capture(bool withSubtitles) {
    auto frame = player_->Snapshot(withSubtitles);
    if (!frame) { ShowToast(app::Tr(L"No frame to copy", L"복사할 프레임이 없습니다")); return; }
    if (hooks.copyFrame) hooks.copyFrame(*frame);
    ShowToast(app::Tr(L"Copied", L"복사됨"));
}

void MainView::Execute(CommandId c) {
    subsDirty_ = true;
    using C = CommandId;
    c = app::MiniRemapCommand(c, modes_.mini);
    if (c != C::FrameNext && c != C::FramePrevious && c != C::KeyframeNext && c != C::KeyframePrevious)
        frameStep_.Cancel();   // another command ends a held D / F / Left / Right chain
    switch (c) {
    case C::PlayPause: TogglePause(); break;
    case C::Previous: ctl_->Previous(); break;
    case C::Next: ctl_->Next(); break;
    case C::SeekBack5: case C::SeekForward5: case C::SeekBack30: case C::SkipForward30:
    case C::SeekBack60: case C::SeekForward60: case C::SeekBack300: case C::SeekForward300:
        frameStep_.StepOnce(kKeyStepSkip + std::abs(app::SeekSeconds(c)), app::SeekSeconds(c) > 0 ? +1 : -1);
        break;
    case C::SpeedDown: app::SetPlaybackSpeed(*this, app::SteppedSpeed(svc_.filters.Settings().speed, -1, wvp::filters::kMaxSpeed)); break;
    case C::SpeedUp: app::SetPlaybackSpeed(*this, app::SteppedSpeed(svc_.filters.Settings().speed, +1, wvp::filters::kMaxSpeed)); break;
    case C::SpeedReset: app::SetPlaybackSpeed(*this, 1.0); break;
    case C::FrameNext: frameStep_.StepOnce(kKeyStepFrame, +1); break;
    case C::FramePrevious: frameStep_.StepOnce(kKeyStepFrame, -1); break;
    case C::KeyframeNext: frameStep_.StepOnce(kKeyStepKeyframe, +1); break;
    case C::KeyframePrevious: frameStep_.StepOnce(kKeyStepKeyframe, -1); break;
    case C::WindowSize50: case C::WindowSize100: case C::WindowSize150: case C::WindowSize200: case C::WindowSizeMax:
        if (prepared_ && prepared_->HasVideo && hooks.sizeWindowToVideo) {
            if (modes_.mini) SetMiniPlayer(false);
            if (modes_.fullscreen) SetFullscreen(false);
            else hooks.sizeWindowToVideo(*prepared_, app::WindowScale(c));
        }
        break;
    case C::ToggleSubtitles: app::ToggleSubtitles(*this); break;
    case C::CycleSubtitleTrack: app::CycleSubtitleTrack(*this); break;
    case C::LoadSubtitleFile: app::LoadSubtitleFile(*this); break;
    case C::SubtitleDelayEarlier: app::AdjustSubtitleDelay(*this, -500); break;
    case C::SubtitleDelayLater: app::AdjustSubtitleDelay(*this, +500); break;
    case C::SubtitleDelayReset: app::AdjustSubtitleDelay(*this, 0); break;
    case C::Stop:
        if (modes_.mini) SetMiniPlayer(false);   // the mini player needs media: Stop returns to the player view first
        CloseMedia();
        break;
    case C::ToggleControlPanel: app::ToggleFiltersPanel(*this); break;
    case C::TogglePlaylistPanel: TogglePlaylistPanel(); break;
    case C::FocusPlaylistSearch:
        if (modes_.Mode() != app::ShellMode::NowPlaying) break;
        if (!svc_.settings.playlistPanel) TogglePlaylistPanel();
        if (shell->Panel()->Visible()) shell->Panel()->FocusSearch();
        break;
    case C::CycleAlwaysOnTop: {
        svc_.settings.alwaysOnTop = app::sl::NextAlwaysOnTop(svc_.settings.alwaysOnTop);
        app::OnSettingsChanged(*this);
        const wchar_t* names[] = {app::Tr(L"Always on top: Off", L"항상 위에: 사용 안 함"), app::Tr(L"Always on top: On", L"항상 위에: 항상"),
                                  app::Tr(L"Always on top: While playing", L"항상 위에: 재생 중"), app::Tr(L"Always on top: While fullscreen", L"항상 위에: 전체 화면")};
        ShowToast(names[app::sl::AlwaysOnTopIndex(svc_.settings.alwaysOnTop)]);
        break;
    }
    case C::ToggleTimeMode:
        svc_.settings.timeRemaining = !svc_.settings.timeRemaining;
        svc_.settingsStore.Save(svc_.settings);
        break;
    case C::ToggleMute: ctl_->SetMuted(!ctl_->IsMuted()); break;
    case C::VolumeUp: SetVolume(ctl_->Volume() + 5); break;
    case C::VolumeDown: SetVolume(ctl_->Volume() - 5); break;
    case C::ToggleFullscreen: SetFullscreen(!modes_.fullscreen); break;
    case C::Escape:
        if (host.PopupCount()) host.CloseAllPopups();
        else if (modes_.fullscreen) SetFullscreen(false);
        break;
    case C::ToggleMiniPlayer: SetMiniPlayer(!modes_.mini); break;
    case C::OpenFiles: app::OpenFiles(*this); break;
    case C::OpenFolder: app::OpenFolder(*this); break;
    case C::OpenUrl: app::OpenUrl(*this); break;
    case C::CaptureFrame: Capture(false); break;
    case C::CaptureFrameWithCaptions: Capture(true); break;
    case C::ShowProperties: app::ShowPropertiesDialog(*this); break;
    case C::ShowEqualizer: app::ShowFiltersPanel(*this, app::FiltersTab::Audio); break;
    case C::ZoomToFill: app::ToggleZoomToFill(*this); break;
    case C::VideoEnhancements: app::ShowFiltersPanel(*this, app::FiltersTab::Video); break;
    case C::OpenSettings: if (hooks.openSettings) hooks.openSettings(); break;
    case C::ToggleDebugOverlay: ToggleDebugOverlay(); break;
    case C::Exit: if (hooks.quit) hooks.quit(); break;
    default: break;
    }
    SyncModel();
}

// --- the Tab debug overlay (AppPlayback.cpp BuildDebugContext / RefreshDebugOverlay / ToggleDebugOverlay, AppRender.cpp
//     RenderDebugLayer) ---------------------------------------------------------------------------------------------------

namespace {
long long JsonInt(const wvp::json::Value* obj, const char* key) {
    const wvp::json::Value* m = obj ? obj->Find(key) : nullptr;
    return m && m->kind == wvp::json::Value::Kind::Number ? (long long)m->n : -1;
}
double JsonNum(const wvp::json::Value* obj, const char* key) {
    const wvp::json::Value* m = obj ? obj->Find(key) : nullptr;
    return m && m->kind == wvp::json::Value::Kind::Number ? m->n : NAN;
}
}

app::dbg::OverlayContext MainView::BuildDebugContext() {
    app::dbg::OverlayContext c;
    c.mode = modes_.mini ? "mini player" : modes_.fullscreen ? "full screen" : "now playing";
    // No Acrylic on macOS (vibrancy is AppKit's): no acrylic row, as when in-app Acrylic is active without regions.
    if (NSWindow* w = (__bridge NSWindow*)(void*)MainHwnd())
        if (NSScreen* screen = w.screen) c.displayRefreshHz = (double)screen.maximumFramesPerSecond;
    {   // build stamps so a stale app / libffplay is visible
        std::wstring st = app::ExeBuildStamp(), ls = app::LibffplayBuildStamp();
        if (!ls.empty()) st += (st.empty() ? L"" : L"  |  ") + ls;
        c.buildStamp = wvp::ToUtf8(st);
    }
    if (player_) {
        if (auto j = player_->GetDebugInfoJson()) {
            c.haveJson = true;
            c.info = wvp::ParseDebugInfo(*j);
        }
    }
    if (const wvp::Plan* pl = svc_.filters.CurrentPlan()) c.plan = *pl;
    c.url = engine_->LastOpenUrl();
    c.resumeDecision = resumeDecision_;
    if (!c.url.empty()) c.openOptions = app::log::FormatOptionsLine(engine_->EffectiveOpenOptions());
    const wvp::FilterSettings& fs = svc_.filters.Settings();
    c.requestedSpeed = fs.speed;
    c.adapterNote = engine_->AdapterNote(&c.adapterWarn);
    c.deinterlaceRequested = svc_.filters.Content().progressive ? "" : fs.deinterlace == wvp::DeinterlaceMode::On ? "deinterlace" : fs.deinterlace == wvp::DeinterlaceMode::Ivtc ? "ivtc" : "";
    return c;
}

/// Mac rows: the Metal presenter's own counters (FFPPresenter.stats_json), since the last PREPARED (the current open): frames presented,
/// presented directly (VideoToolbox IOSurface, zero-copy) / uploaded, copies, IOSurface identity checks; then its drops
/// and timing. libffplay's debug info carries the presenter's name and frame path, not these.
std::vector<app::dbg::OverlayLine> MainView::PresenterLines() const {
    std::vector<app::dbg::OverlayLine> out;
    if (!engine_->Presenter()) return out;
    auto now = wvp::json::Parse(engine_->PresenterStats()), base = wvp::json::Parse(engine_->PresenterStatsAtPrepared());
    if (!now || !now->IsObject()) return out;
    const wvp::json::Value* b = base && base->IsObject() ? &*base : nullptr;
    auto delta = [&](const char* key) { const long long a = JsonInt(&*now, key), z = JsonInt(b, key); return a < 0 ? -1 : a - std::max(0LL, z); };
    const long long presented = delta("frames_presented"), direct = delta("frames_direct"), uploaded = delta("frames_uploaded"),
                    copies = delta("copies"), verified = delta("zero_copy_verified"), mismatch = delta("zero_copy_mismatch");
    char t[400];
    std::snprintf(t, sizeof t, "metal     presented %lld | zero-copy %lld (verified %lld, mismatch %lld) | uploaded %lld | copies %lld",
                  presented, direct, verified, mismatch, uploaded, copies);
    out.push_back({mismatch > 0 ? app::dbg::LineKind::Warn : app::dbg::LineKind::Normal, t});
    std::snprintf(t, sizeof t, "          skipped timeout %lld gpu %lld display %lld | dropped no-drawable %lld hidden %lld | gpu %.2f ms | latency %.1f ms",
                  delta("skipped_timeout"), delta("skipped_gpu"), delta("skipped_display"), delta("dropped_no_drawable"), delta("dropped_hidden"),
                  JsonNum(&*now, "gpu_ms_avg"), JsonNum(&*now, "present_latency_ms_avg"));
    out.push_back({app::dbg::LineKind::Dim, t});
    return out;
}

void MainView::RefreshDebugOverlay() {
    auto lines = app::dbg::BuildOverlayLines(BuildDebugContext());
    // The presenter rows go right after the "output" row (the presenter libffplay reports).
    auto at = std::find_if(lines.begin(), lines.end(), [](const app::dbg::OverlayLine& l) { return l.text.rfind("output", 0) == 0; });
    auto extra = PresenterLines();
    lines.insert(at == lines.end() ? lines.end() : at + 1, extra.begin(), extra.end());
    if (lines != debugLines_) {
        debugLines_ = std::move(lines);
        LayoutDebugOverlay();
    }
}

/// AppRender.cpp RenderDebugLayer: geometry (ComputeOverlayGeometry), wrap + clip, the height of the rows.
void MainView::LayoutDebugOverlay() {
    if (!debug_) return;
    debug_->SetScale(*measurer_, scale_);
    const app::dbg::OverlayGeometry g = app::dbg::ComputeOverlayGeometry(cw_, ch_, scale_, modes_.Mode(), debug_->CharW(), debug_->LineH(),
                                                                         layout_.barPx.h());
    if (!debugVisible_ || !g.visible) {
        debug_->SetVisible(false);
        return;
    }
    debugCols_ = g.cols;
    std::vector<app::dbg::OverlayLine> rows = app::dbg::ClipRows(app::dbg::WrapLines(debugLines_, g.cols), g.rows);
    const float pad = (float)std::lround(app::dbg::kPadDip * scale_);
    const float hPx = std::ceil((float)rows.size() * debug_->LineH() + 2 * pad);
    debug_->SetBounds({g.x / scale_, g.y / scale_, (g.x + g.w) / scale_, (g.y + hPx) / scale_});
    debug_->SetRows(std::move(rows));
    debug_->SetVisible(true);
    debug_->Invalidate();
}

void MainView::ToggleDebugOverlay(const char* src) {
    debugVisible_ = !debugVisible_;
    Log("debug", std::string(debugVisible_ ? "overlay on" : "overlay off") + " (" + src + ")");
    if (debugVisible_) {
        RefreshDebugOverlay();
        debugNextMs_ = NowMs() + 250;
    }
    LayoutDebugOverlay();
}

bool MainView::DebugOverlayDrawn() const { return debug_ && debug_->Visible() && !debug_->Rows().empty(); }
std::string MainView::DebugText() const { return app::dbg::PlainText(debugLines_); }
const std::vector<app::dbg::OverlayLine>& MainView::DebugRows() const { return debug_->Rows(); }

// --- the playlist panel splitter (App.cpp SplitterAt / BeginSplitterDrag / DragSplitter / SetPanelWidth / EndSplitterDrag)

bool MainView::SplitterAt(pui::PointF dip) const {
    const app::RectPx& r = layout_.splitterPx;
    const bool userPopup = UserPopupOpen();
    const float x = dip.x * scale_, y = dip.y * scale_;
    return shell && modes_.Mode() == app::ShellMode::NowPlaying && !layout_.panelOverlay && !r.Empty() && !userPopup &&
           x >= r.l && x < r.r && y >= r.t && y < r.b;
}

bool MainView::BeginSplitterDrag(pui::PointF dip, bool doubleClick) {
    if (!SplitterAt(dip)) return false;
    if (doubleClick) {   // reset to the default width
        SetPanelWidth((int)app::kPanelDefaultDip);
        svc_.settingsStore.Save(svc_.settings);
        return true;
    }
    splitDrag_ = true;
    splitGrabPx_ = dip.x * scale_ - layout_.splitterPx.l;
    return true;
}

void MainView::DragSplitter(pui::PointF dip) {
    if (!splitDrag_) return;
    // The panel's left edge follows the splitter's left edge plus the gap; its right edge stays.
    const float panelW = (float)layout_.panelPx.r - (dip.x * scale_ - splitGrabPx_ + std::round(app::kBodyGapDip * scale_));
    SetPanelWidth((int)std::lround(panelW / scale_));
}

void MainView::SetPanelWidth(int widthDip) {
    if (widthDip == svc_.settings.playlistPanelWidthDip) return;
    svc_.settings.playlistPanelWidthDip = widthDip;
    ApplyMode();
    if (!layout_.panelPx.Empty()) svc_.settings.playlistPanelWidthDip = (int)std::lround(layout_.panelPx.w() / scale_);   // as clamped by the layout
}

bool MainView::EndSplitterDrag() {
    if (!splitDrag_) return false;
    splitDrag_ = false;
    svc_.settingsStore.Save(svc_.settings);
    Log("panel", "playlist panel width " + std::to_string(svc_.settings.playlistPanelWidthDip) + " DIP");
    return true;
}

// --- main menu: features/Menus.cpp ---------------------------------------------------------------------

void MainView::ShowMainMenu(pui::PointF at) { app::ShowMainMenu(*this, at); }

void MainView::ShowMainMenu(pui::Widget* anchor) {
    app::GlyphButton* btn = shell->Title()->MainMenuButton();
    app::ShowMainMenu(*this, anchor, {[btn] { btn->SetMenuOpen(true); }, [btn] { btn->SetMenuOpen(false); }});
}

}
