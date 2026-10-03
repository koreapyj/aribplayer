#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <thread>

#include "App.h"
#include "WindowSizing.h"

#include <dwmapi.h>
#include "Localize.h"
#include "features/FiltersPanel.h"
#include "features/FxModel.h"
#include "features/Logging.h"
#include "features/LoggingFormat.h"
#include "features/Menus.h"
#include "features/DisplayInfo.h"
#include "features/MiniLogic.h"
#include "features/SettingsService.h"
#include "features/OpenFlows.h"
#include "features/PlaybackExtras.h"
#include "features/PropertiesDialog.h"
#include "wvp/FrameCapture.h"
#include "wvp/M3uWriter.h"
#include "wvp/PlaylistLoader.h"
#include "wvp/Utf.h"

namespace app {

namespace {
std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

bool SameModel(const BarModel& a, const BarModel& b) {
    return a.loaded == b.loaded && a.canPlaySelection == b.canPlaySelection && a.prepared == b.prepared && a.seekable == b.seekable && a.pos == b.pos && a.dur == b.dur &&
           a.playing == b.playing && a.hasPrev == b.hasPrev && a.hasNext == b.hasNext && a.shuffle == b.shuffle && a.repeat == b.repeat &&
           a.volume == b.volume && a.muted == b.muted && a.subCount == b.subCount && a.fullscreen == b.fullscreen && a.mini == b.mini &&
           a.panelOpen == b.panelOpen && a.panelAvailable == b.panelAvailable && a.controlPanelOpen == b.controlPanelOpen &&
           a.timeMode == b.timeMode && a.chips == b.chips && a.title == b.title;
}

int CycleableSubtitleCount(const wvp::MediaInfo& mi) {
    return (int)std::count_if(mi.Streams.begin(), mi.Streams.end(), fx::IsCycleableSubtitle);
}
}

void App::RestoreCurrentPlaylist() {
    currentPlaylistName_ = svc_->settings.currentPlaylist;
    if (!currentPlaylistName_.empty() && !svc_->playlists.Exists(currentPlaylistName_)) currentPlaylistName_.clear();

    currentPlaylistItems_.clear();
    if (!currentPlaylistName_.empty()) {
        currentPlaylistItems_ = svc_->playlists.Load(currentPlaylistName_);
    } else {
        const std::wstring path = dataDir_ + L"\\unnamed.m3u8";
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            try { currentPlaylistItems_ = wvp::PlaylistLoader().Load(path).items; }
            catch (...) { currentPlaylistItems_.clear(); }
        }
    }
    svc_->settings.currentPlaylist = currentPlaylistName_;
    svc_->settingsStore.Save(svc_->settings);
    ForPanels([](PlaylistPanel& p) { p.OnCurrentPlaylistChanged(); });   // a panel laid out before the restore still shows the default list
}

bool App::PersistCurrentPlaylist() {
    if (currentPlaylistName_.empty()) return wvp::M3uWriter::Save(dataDir_ + L"\\unnamed.m3u8", currentPlaylistItems_, false);
    if (svc_->playlists.IsReference(currentPlaylistName_)) return true;  // referenced sources are never overwritten
    return svc_->playlists.Save(currentPlaylistName_, currentPlaylistItems_);
}

void App::SetCurrentPlaylistName(const std::wstring& name) {
    if (currentPlaylistName_ == name) return;
    currentPlaylistName_ = name;
    svc_->settings.currentPlaylist = name;
    svc_->settingsStore.Save(svc_->settings);
    ForPanels([](PlaylistPanel& p) { p.OnCurrentPlaylistChanged(); });
}

void App::SetResumeDecision(const std::wstring& decision) {
    resumeDecision_ = wvp::ToUtf8(decision);
    if (debugVisible_) RefreshDebugOverlay();
}

void App::SavePlaybackSnapshot(const std::wstring& playlist, const std::wstring& url, int index, double position, double duration,
                               bool seekable, bool force, bool ended) {
    if (url.empty()) return;
    const uint64_t now = Now();
    if (!force && now < historyWriteAt_) return;
    if (!std::isfinite(position) || position < 0) position = 0;
    if (!std::isfinite(duration) || duration < 0) duration = 0;
    const bool canResume = seekable && duration > 0;
    const bool played = wvp::PlaylistStore::IsPlayedPosition(position, duration, ended);
    // Off the UI thread (pause, seek, once a second while playing): the history file's read-modify-write can stall for
    // hundreds of ms (antivirus scanning) and the next key press would wait for it. An item already marked played stays
    // played (the writer checks); the readers (resume decisions) wait for pending writes.
    svc_->playlists.SavePlaybackStateAsync(playlist, url, position, duration, played, canResume, index);
    historyWriteAt_ = now + 1000;
    if (played) ForPanels([&](PlaylistPanel& p) { p.OnItemPlayed(url); });   // the writer keeps "played" sticky
    if (played) resumeDecision_ = "played: reset to start (near end / EOF)";
}

void App::SavePlaybackHistory(bool force, bool ended, double positionOverride) {
    if (!ctl_ || !ctl_->Current()) return;
    double position = positionOverride >= 0 ? positionOverride : model_.pos;
    SavePlaybackSnapshot(currentPlaylistName_, ctl_->Current()->url, ctl_->CurrentIndex(), position, model_.dur,
                         model_.seekable, force, ended);
}

std::vector<wvp::MediaItem> App::PlaylistItems(const std::wstring& name) {
    if (!name.empty() && svc_->playlists.IsReference(name)) return svc_->playlists.Load(name);
    if (name == currentPlaylistName_) return currentPlaylistItems_;
    if (!name.empty()) return svc_->playlists.Load(name);
    const std::wstring path = dataDir_ + L"\\unnamed.m3u8";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return {};
    try { return wvp::PlaylistLoader().Load(path).items; }
    catch (...) { return {}; }
}

bool App::SavePlaylistItems(const std::wstring& name, const std::vector<wvp::MediaItem>& items) {
    if (!name.empty() && svc_->playlists.IsReadOnly(name)) return false;
    bool saved = name.empty() ? wvp::M3uWriter::Save(dataDir_ + L"\\unnamed.m3u8", items, false)
                              : svc_->playlists.Save(name, items);
    if (name == currentPlaylistName_) {
        std::wstring currentUrl = ctl_ && ctl_->Current() ? ctl_->Current()->url : std::wstring();
        int current = -1;
        if (!currentUrl.empty()) {
            for (size_t i = 0; i < items.size(); ++i)
                if (items[i].url == currentUrl) { current = (int)i; break; }
        }
        currentPlaylistItems_ = items;
        if (ctl_) {
            if (!currentUrl.empty() && current < 0) ctl_->Stop();
            suppressPlaylistPersistence_ = true;
            ctl_->SetQueue(items, current, false);
            suppressPlaylistPersistence_ = false;
        } else if (pending_) {
            pending_->items = items;
            pending_->start = current;
        }
    }
    if (!saved) ShowInfoBar(InfoSeverity::Error, Tr(L"Could not save the playlist", L"재생목록을 저장할 수 없습니다"), name.empty() ? Tr(L"Unnamed playlist", L"이름 없는 재생목록") : name);
    return saved;
}

void App::PlayPlaylist(const std::wstring& name, const std::vector<wvp::MediaItem>& items, int start) {
    PlayPlaylistAtPosition(name, items, start, -1);
}

void App::PlayPlaylistAtPosition(const std::wstring& name, const std::vector<wvp::MediaItem>& items, int start, double position) {
    if (ctl_ && ctl_->Current()) SavePlaybackHistory(true);
    const bool keepPlaying = ctl_ && currentPlaylistName_ != name && currentPlaylistItems_ == items &&
                             ctl_->CurrentIndex() == start && ctl_->Current() && position <= 0;
    SetCurrentPlaylistName(name);
    currentPlaylistItems_ = items;
    if (!PersistCurrentPlaylist()) ShowInfoBar(InfoSeverity::Error, Tr(L"Could not save the playlist", L"재생목록을 저장할 수 없습니다"), name.empty() ? Tr(L"Unnamed playlist", L"이름 없는 재생목록") : name);
    if (ctl_) { if (!keepPlaying) ctl_->SetQueue(currentPlaylistItems_, start, true, position); }
    else pending_ = PendingPlay{currentPlaylistItems_, start, !items.empty(), position};
}

void App::AppendPlaylistItems(const std::wstring& name, const std::vector<wvp::MediaItem>& items) {
    AppendItems(name, items, false);   // playlist views (drops, Add files): append only, never start or change playback
}

void App::AppendItems(const std::wstring& name, const std::vector<wvp::MediaItem>& items, bool startIfIdle) {
    if (items.empty()) return;
    if (!name.empty() && svc_->playlists.IsReadOnly(name)) {
        ShowInfoBar(InfoSeverity::Warning, Tr(L"Playlist is read-only", L"재생목록이 읽기 전용입니다"), Tr(L"This playlist references a source file or URL. Import it to make an editable copy.", L"이 재생목록은 원본 파일 또는 주소를 참조합니다. 가져오기로 편집 가능한 사본을 만드세요."));
        return;
    }
    if (name != currentPlaylistName_) {
        auto all = PlaylistItems(name);
        all.insert(all.end(), items.begin(), items.end());
        SavePlaylistItems(name, all);
        return;
    }
    if (!ctl_) {
        currentPlaylistItems_.insert(currentPlaylistItems_.end(), items.begin(), items.end());
        if (!PersistCurrentPlaylist()) ShowInfoBar(InfoSeverity::Error, Tr(L"Could not save the playlist", L"재생목록을 저장할 수 없습니다"), name.empty() ? Tr(L"Unnamed playlist", L"이름 없는 재생목록") : name);
        if (startIfIdle) pending_ = PendingPlay{currentPlaylistItems_, 0, true};
        return;
    }
    bool startAtFirst = startIfIdle && ctl_->CurrentIndex() < 0;
    ctl_->Enqueue(items);   // QueueChanged writes the current playlist atomically
    if (startAtFirst && !ctl_->Items().empty()) ctl_->PlayAt(0);
}

void App::AppendToCurrentPlaylist(std::vector<wvp::MediaItem> items) {
    AppendItems(currentPlaylistName_, items, true);   // Open commands: if nothing has played yet, start the first item
}

void App::ReplaceUnnamedPlaylistAtPosition(std::vector<wvp::MediaItem> items, double position) {
    if (items.empty()) return;
    if (ctl_ && ctl_->Current()) SavePlaybackHistory(true);
    SetCurrentPlaylistName({});
    currentPlaylistItems_ = std::move(items);
    if (!PersistCurrentPlaylist()) ShowInfoBar(InfoSeverity::Error, Tr(L"Could not save the unnamed playlist", L"이름 없는 재생목록을 저장할 수 없습니다"), Tr(L"The playlist could not be written.", L"재생목록을 쓸 수 없습니다."));
    if (ctl_) ctl_->SetQueue(currentPlaylistItems_, 0, true, position);
    else pending_ = PendingPlay{currentPlaylistItems_, 0, true, position};
}

void App::PlaylistRenamed(const std::wstring& from, const std::wstring& to) {
    ForPanels([&](PlaylistPanel& p) { p.OnPlaylistRenamed(from, to); });
    if (currentPlaylistName_ == from) SetCurrentPlaylistName(to);
}

void App::PlaylistsChanged() {
    ForPanels([](PlaylistPanel& p) { p.OnPlaylistsChanged(); });
}

void App::PlaylistDeleted(const std::wstring& name) {
    ForPanels([&](PlaylistPanel& p) { p.OnPlaylistDeleted(name); });
    if (currentPlaylistName_ != name) return;
    std::vector<wvp::MediaItem> unnamed = PlaylistItems({});
    SetCurrentPlaylistName({});
    currentPlaylistItems_ = std::move(unnamed);
    if (!ctl_) { pending_.reset(); return; }
    ctl_->Stop();
    suppressPlaylistPersistence_ = true;
    ctl_->SetQueue(currentPlaylistItems_, -1, false);
    suppressPlaylistPersistence_ = false;
}

void App::UpdateKeepAwake() {
    // Only while playing: video keeps the display on, audio only keeps the system from sleeping. Paused / stopped: nothing.
    EXECUTION_STATE es = ES_CONTINUOUS;
    if (state_ == wvp::PlayerState::Playing) es |= ES_SYSTEM_REQUIRED | (acrylicMediaHasVideo_ ? ES_DISPLAY_REQUIRED : 0);
    SetThreadExecutionState(es);
}

void App::InitPlayer() {
    playerCreating_ = true;
    // SDL would keep the display awake for as long as its video subsystem lives (paused, idle); UpdateKeepAwake owns that.
    SetEnvironmentVariableW(L"SDL_VIDEO_ALLOW_SCREENSAVER", L"1");
    // ffp_create runs on a worker thread while this thread keeps pumping messages
    wvp::FfPlayer::CreateAsync(video_, hwnd_, WM_NOTIFY_PLAYER, [this](std::unique_ptr<wvp::FfPlayer> p, int err) {
        playerCreating_ = false;
        if (!p) {
            playerFailed_ = true;
            ShowInfoBar(InfoSeverity::Error, Tr(L"Player unavailable", L"플레이어를 사용할 수 없습니다"), TrW(L"libffplay failed to start (error " + std::to_wstring(err) + L")", L"libffplay를 시작하지 못했습니다 (오류 " + std::to_wstring(err) + L")"));
            return;
        }
        player_ = std::move(p);
        ctl_ = std::make_unique<wvp::PlaybackController>(*player_, [this](std::function<void()> f) { PostToUi(std::move(f)); });
        ctl_->SetFilters(svc_->filters.Provider());
        WirePipeline();
        if (opt_.stats) ctl_->SetBaseOption(L"stats", L"1");   // --stats: ffplay status line (A-V, fd, aq, vq, sq) via the log callback
        for (const auto& [k, v] : LibffplayDeviceOptions(std::wstring(playAdapter_.luid.begin(), playAdapter_.luid.end()), opt_))
            ctl_->SetBaseOption(k, v);
        WirePlayer();
        EnsureVideoDropHook();
        ctl_->SetQueue(currentPlaylistItems_, -1, false);  // restore without autoplay; pending opens are applied below
        OnPlayerReady.raise();
        ApplyPendingQueue();
        SyncModel();
        Flush();
    });
}

void App::WirePlayer() {
    ctl_->onOpening = [this](const std::wstring& url, const wvp::OpenOptions& o) {
        acrylicMediaHasVideo_ = false;
        acrylicD3D11_ = false;
        acrylicPresenterFailed_ = false;
        acrylicApiUnsupported_ = false;
        acrylicNeedsApply_ = true;
        lastOpenUrl_ = url;
        lastOpenOpts_ = o;
        effOpts_ = o;
        lastOpenCmd_ = log::FfplayCommandLine(url, o);
        log::EventW("open", L"url: " + url);
        log::EventW("open", L"options: " + log::FormatOptionsLine(o));
        log::EventW("open", L"ffplay: " + lastOpenCmd_);
        LogOpenPlan();
        UpdateAcrylicState(true);
        if (debugVisible_) RefreshDebugOverlay();
    };
    ctl_->onFilterChange = [this](const wvp::FilterChange& c) {
        std::wstring m = L"vf" + std::wstring(c.videoRebuild ? L" (rebuild)" : L" (unchanged)") + L"=" + c.videoFilter + L" | af" +
                         (c.audioRebuild ? L" (rebuild)" : L" (unchanged)") + L"=" + c.audioFilter;
        for (const auto& cmd : c.commands) m += L" | cmd " + cmd.target + L" " + cmd.command + L" " + cmd.argument;
        if (c.presenterParamsChanged) m += L" | presenter params " + wvp::PresenterParamsJson(c.presenterParams);
        log::EventW("filter", m);
        if (!lastOpenUrl_.empty()) {   // the playing graph differs from the open-time options: keep overlay / log / ffplay line in sync
            effOpts_ = log::EffectiveOptions(lastOpenOpts_, c.videoFilter, c.audioFilter);
            if (const wvp::Plan* pl = svc_->filters.CurrentPlan()) {   // presenter-applied rotation / flip / eq are not in the vf
                std::erase_if(effOpts_, [](const auto& e) { return e.first == L"presenter_params"; });
                if (pl->presenter != "sdl" && !pl->presenterParams.IsIdentity()) effOpts_.emplace_back(L"presenter_params", wvp::PresenterParamsJson(pl->presenterParams));
            }
            lastOpenCmd_ = log::FfplayCommandLine(lastOpenUrl_, effOpts_);
            log::EventW("filter", L"effective options: " + log::FormatOptionsLine(effOpts_));
            log::EventW("filter", L"ffplay: " + lastOpenCmd_);
            if (debugVisible_) RefreshDebugOverlay();
        }
    };
    player_->FilterError.add([](const wvp::FilterErrorInfo& e) {
        log::EventW("filter", L"ERROR " + e.Type + L" " + e.Filter + L" code " + std::to_wstring(e.Code) + L": " + e.Message, log::kError);
    });
    player_->Prepared.add([this](const wvp::MediaPreparedInfo& p) {
        frameStep_.Reset();
        log::Event("player", "prepared: duration " + std::to_string(p.Duration) + " s, " + std::to_string(p.Width) + "x" + std::to_string(p.Height) + " SAR " +
                                 std::to_string(p.SarNum) + ":" + std::to_string(p.SarDen) + ", video " + (p.HasVideo ? "yes" : "no") + ", audio " +
                                 (p.HasAudio ? "yes" : "no") + ", seekable " + (p.Seekable ? "yes" : "no") + ", title: " + wvp::ToUtf8(p.Title));
        model_.dur = p.Duration; model_.seekable = p.Seekable; model_.prepared = true;
        acrylicMediaHasVideo_ = p.HasVideo;
        UpdateKeepAwake();
        acrylicPresenterFailed_ = false;
        acrylicApiUnsupported_ = false;
        acrylicApiReady_ = false;
        acrylicAppliedRegions_.clear();
        acrylicD3D11_ = false;
        if (auto json = player_->GetDebugInfoJson()) {
            const auto debug = wvp::ParseDebugInfo(*json);
            acrylicD3D11_ = debug.Presenter.Present && debug.Presenter.Name == "d3d11";
        }
        acrylicNeedsApply_ = true;
        UpdateAcrylicState(true);
        chipsStage_ = 0;
        chipsPreparedMs_ = Now();
        chipsNextMs_ = chipsPreparedMs_ + 500;   // the presenter / decoder settle after PREPARED: refresh at +0.5 s and +1.5 s
        RefreshChips();
        mediaAspect_ = p.HasVideo ? MiniAspectForMedia(p.DisplayWidth(), p.DisplayHeight()) : 16.0 / 9.0;
        if (!p.Title.empty()) model_.title = p.Title;
        model_.subCount = 0;
        if (auto mi = player_->GetMediaInfo()) model_.subCount = CycleableSubtitleCount(*mi);
        CheckReady();
        UpdateTitleText();
        OnMediaPrepared.raise(p);
        SyncModel(); Flush();
        // An opened video sizes the window like key 2 (100%), once per opened item, in the windowed player view only
        if (p.HasVideo && sizedForUrl_ != lastOpenUrl_ && hwnd_ && modes_.Mode() == ShellMode::NowPlaying && !modes_.mini && !modes_.fullscreen &&
            IsWindowVisible(hwnd_) && !IsZoomed(hwnd_) && !IsIconic(hwnd_)) {
            sizedForUrl_ = lastOpenUrl_;
            const wvp::Rotation rot = svc_->filters.Settings().rotation;
            if (auto sz = VideoDisplaySize(p.Width, p.Height, p.SarNum, p.SarDen, rot == wvp::Rotation::Cw90 || rot == wvp::Rotation::Ccw90))
                SizeWindowToVideo(WindowScale(CommandId::WindowSize100), sz);
        }
    });
    player_->FrameStepDone.add([this](int, bool moved) { frameStep_.StepDone(moved); });
    player_->KeyframeSeekDone.add([this](int, bool moved, double, double) { frameStep_.StepDone(moved); });
    player_->PositionChanged.add([this](double p, double d) {
        positionEvents_++;
        model_.pos = std::isnan(p) ? 0 : p;
        model_.dur = d;
        SavePlaybackHistory(false);
        if (Now() >= playbackProgressUiAt_) { playbackProgressUiAt_ = Now() + 1000; OnPlaybackProgress.raise(); }
        SyncModel(); Flush();
    });
    player_->StateChanged.add([this](wvp::PlayerState s) {
        auto qpcUs = [] { LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); return c.QuadPart * 1e6 / f.QuadPart; };
        stateTiming_.enter = qpcUs();
        static const char* names[] = {"Idle", "Opening", "Playing", "Paused", "Stopped", "Ended", "Error"};
        log::Event("player", std::string("state: ") + names[(int)s]);
        state_ = s;
        model_.playing = s == wvp::PlayerState::Playing;
        CheckReady();
        const bool revealControls = s == wvp::PlayerState::Ended
            ? RestoreVideoControlsOnCompletion(Loaded())
            : s != wvp::PlayerState::Playing;
        ApplyWindowTopmost();   // "Enable when playing" follows the state
        UpdateKeepAwake();
        stateTiming_.topmost = qpcUs();
        if (revealControls) {   // Ended/Error always come back; a keyboard pause/stop/next leaves a hidden bar hidden
            bool forced = s == wvp::PlayerState::Ended || s == wvp::PlayerState::Error;
            StartBarAnim(forced ? autoHide_.Enter(GetTickCount64()) : autoHide_.Reveal(GetTickCount64()));
        }
        SyncModel(); Flush();
        stateTiming_.synced = qpcUs();
        if (s == wvp::PlayerState::Paused) SavePlaybackHistory(true);   // file I/O after the bar shows the new state
        stateTiming_.saved = qpcUs();
    });
    player_->Error.add([this](const wvp::PlayerErrorInfo& e) {
        if (e.DeviceLost) {   // AppPipeline reopens on a fresh device
            log::EventW("player", L"decoder device lost: " + e.Message, log::kWarning);
            return;
        }
        log::EventW("player", L"ERROR code " + std::to_wstring(e.Code) + L": " + e.Message + L" (" + e.Url + L")", log::kError);
        OnMediaError.raise(e);
        ShowPlaybackError(*this, e);
    });
    player_->PresenterError.add([this](const wvp::PresenterErrorInfo& e) {
        log::EventW("presenter", L"fallback to " + e.Fallback + L": " + e.Error, log::kWarning);
        if (e.DeviceLost) return;   // AppPipeline reopens on a fresh device; the presenter itself is fine
        acrylicD3D11_ = false;
        acrylicPresenterFailed_ = true;
        acrylicNeedsApply_ = true;
        UpdateAcrylicState(true);
        if (debugVisible_) RefreshDebugOverlay();
    });
    player_->Ended.add([this] { log::Event("player", "ended"); SavePlaybackHistory(true, true); OnMediaEnded.raise(); });
    player_->Input.add([this](const wvp::InputEventArgs& e) { OnVideoInput(e); });
    ctl_->resolveResumePosition = [this](const wvp::MediaItem& item, double requestedPosition) {
        const auto state = svc_->playlists.GetItemPlaybackState(item.url);
        const double duration = state.duration > 0 ? state.duration : item.duration.value_or(0.0);
        const double candidate = std::isfinite(requestedPosition) && requestedPosition >= 0 ? requestedPosition : state.position;
        wvp::PlaylistStore::ItemPlaybackState decisionState = state;
        decisionState.position = candidate;
        decisionState.duration = duration;
        const double result = wvp::PlaylistStore::ResumePosition(decisionState);
        std::wstring reason;
        if (state.played) reason = L"played item: reset to start";
        else if (!state.seekable || duration <= 0) reason = L"unseekable or live stream: start at 0";
        else if (result <= 0) reason = L"within 5 seconds of start/end: start at 0";
        else reason = L"resume at " + std::to_wstring((int)result) + L" seconds";
        SetResumeDecision(currentPlaylistName_.empty() ? reason : currentPlaylistName_ + L": " + reason);
        return result;
    };
    ctl_->beforeLeavingCurrent = [this] {
        if (!ctl_ || !ctl_->Current()) return;
        const bool haveHistoryIdentity = !historyCurrentUrl_.empty();
        const std::wstring& url = haveHistoryIdentity ? historyCurrentUrl_ : ctl_->Current()->url;
        const std::wstring& playlist = haveHistoryIdentity ? historyCurrentPlaylistName_ : currentPlaylistName_;
        const int index = haveHistoryIdentity ? historyCurrentIndex_ : ctl_->CurrentIndex();
        double position = model_.pos;
        double duration = model_.dur;
        if (player_) {
            const wvp::PlayerState playerState = player_->State();
            if (playerState == wvp::PlayerState::Playing || playerState == wvp::PlayerState::Paused || playerState == wvp::PlayerState::Ended) {
                const double playerPosition = player_->Position();
                const double playerDuration = player_->Duration();
                if (std::isfinite(playerPosition) && playerPosition >= 0) position = playerPosition;
                if (std::isfinite(playerDuration) && playerDuration > 0) duration = playerDuration;
            }
        }
        SavePlaybackSnapshot(playlist, url, index, position, duration, model_.seekable, true);
    };
    ctl_->CurrentChanged.add([this](const wvp::MediaItem* it, int idx) {
        historyCurrentUrl_ = it ? it->url : std::wstring();
        historyCurrentIndex_ = idx;
        historyCurrentPlaylistName_ = currentPlaylistName_;
        log::EventW("queue", L"current #" + std::to_wstring(idx) + L": " + (it ? it->url : std::wstring(L"(none)")));
        acrylicMediaHasVideo_ = false;
        acrylicD3D11_ = false;
        acrylicPresenterFailed_ = false;
        acrylicNeedsApply_ = true;
        model_.title = it ? it->DisplayTitle() : std::wstring();
        model_.prepared = false; model_.seekable = false; model_.pos = 0; model_.dur = 0; model_.subCount = 0;
        if (!it && !pending_ && currentPlaylistItems_.empty()) {   // nothing left to play: leave the video-only modes, stay in the player view
            if (modes_.mini) SetMiniPlayer(false);
            else if (modes_.fullscreen) SetFullscreen(false);
        }
        UpdateTitleText();
        chips_.clear();
        chipsNextMs_ = 0;
        OnMediaOpened.raise(it);
        ForPanels([](PlaylistPanel& p) { p.OnCurrentChanged(); });
        UpdateAcrylicState(true);
        SyncModel(); Flush();
    });
    ctl_->ModeChanged.add([this]() { SyncModel(); Flush(); });
    ctl_->QueueChanged.add([this]() {
        currentPlaylistItems_ = ctl_->Items();
        if (!suppressPlaylistPersistence_ && !PersistCurrentPlaylist())
            ShowInfoBar(InfoSeverity::Error, Tr(L"Could not save the current playlist", L"현재 재생목록을 저장할 수 없습니다"), currentPlaylistName_.empty() ? Tr(L"Unnamed playlist", L"이름 없는 재생목록") : currentPlaylistName_);
        ForPanels([](PlaylistPanel& p) { p.OnQueueChanged(); });
        SyncModel(); Flush();
    });
}

void App::UpdateTitleText() {
    if (shell_) shell_->Title()->SetTitle(model_.title);
}

void App::SyncModel() {
    if (!shell_) return;
    shell_->Title()->SetTopmostMode(svc_->settings.alwaysOnTop);
    BarModel m = model_;
    m.loaded = Loaded();
    m.canPlaySelection = CanPlaySelection();
    m.hasPrev = ctl_ && ctl_->HasPrevious();
    m.hasNext = ctl_ && ctl_->HasNext();
    m.shuffle = ctl_ && ctl_->Shuffle();
    m.repeat = ctl_ ? (int)ctl_->Repeat() : 0;
    m.volume = ctl_ ? ctl_->Volume() : 100;
    m.muted = ctl_ && ctl_->IsMuted();
    m.playing = state_ == wvp::PlayerState::Playing;
    m.fullscreen = modes_.fullscreen;
    m.mini = modes_.mini;
    m.panelOpen = svc_->settings.playlistPanel && (!layout_.panelOverlay || panelPopup_);   // narrow window: pressed only while the popup is open
    m.panelAvailable = modes_.Mode() == ShellMode::NowPlaying;
    m.controlPanelOpen = IsFiltersPanelOpen(*this);
    m.timeMode = svc_->settings.timeRemaining ? sl::TimeDisplayMode::Remaining : sl::TimeDisplayMode::Elapsed;
    m.chips = m.loaded ? chips_ : std::vector<badge::Chip>{};
    if (!m.loaded) { m.prepared = false; m.pos = 0; m.dur = 0; }
    if (SameModel(m, shell_->Bar()->Model())) { model_.loaded = m.loaded; return; }
    model_.loaded = m.loaded;
    shell_->Bar()->SetModel(m);
    shell_->Mini()->SetModel(m);
}

void App::RefreshChips() {
    std::vector<badge::Chip> next;
    if (player_ && Loaded()) {
        if (auto mi = player_->GetMediaInfo()) {
            wvp::DebugInfo debug;
            if (debugCache_.Valid && Now() - debugCacheMs_ < 250) {
                debug = debugCache_;   // parsed by the acrylic poll a moment ago
            } else if (auto json = player_->GetDebugInfoJson()) {
                debug = wvp::ParseDebugInfo(*json);
                debugCache_ = debug;
                debugCacheMs_ = Now();
            }
            next = badge::BuildChips(debug, *mi);
        }
    }
    if (next == chips_) return;
    chips_ = std::move(next);
    SyncModel();
}

void App::ChipsTick(uint64_t now) {
    if (!Loaded() || chipsNextMs_ == 0 || now < chipsNextMs_) return;
    if (chipsStage_ == 0) { RefreshChips(); chipsStage_ = 1; chipsNextMs_ = std::max(now + 1, chipsPreparedMs_ + 1500); return; }
    if (chipsStage_ == 1) { RefreshChips(); chipsStage_ = 2; chipsNextMs_ = now + 2000; return; }
    if (state_ == wvp::PlayerState::Playing) RefreshChips();
    chipsNextMs_ = now + 2000;
}

void App::CloseMedia() {
    if (!ctl_ || (!Loaded() && !pending_)) return;
    pending_.reset();
    sizedForUrl_.clear();
    ctl_->Stop();   // beforeLeavingCurrent saves the resume position
    suppressPlaylistPersistence_ = true;
    ctl_->SetQueue(currentPlaylistItems_, -1, false);   // no current item: the idle view
    suppressPlaylistPersistence_ = false;
}

void App::TogglePlaylistPanel() {
    if (modes_.fullscreen || modes_.mini) return;
    // Row too narrow to dock the panel (layout_.panelOverlay): the toggle opens / closes it as a light-dismiss popup over the video.
    // Closing it with the toggle also clears the persisted setting; a light dismiss keeps it (the panel docks once the window is wide enough).
    const bool narrow = modes_.Mode() == ShellMode::NowPlaying && layout_.panelOverlay;
    if (narrow && !panelPopup_) { OpenPanelOverlay(); SyncModel(); return; }
    if (narrow && panelPopup_) host_.ClosePopup(panelPopup_);
    // The window grows / shrinks by the panel so the video keeps its size. Only a free-standing window with a docked
    // panel (or room for one) does this; maximized, full screen and the narrow overlay keep the video-resizing behaviour.
    const bool resizeWindow = modes_.Mode() == ShellMode::NowPlaying && !narrow && !layout_.video.Empty() && hwnd_ && !IsZoomed(hwnd_) && !IsIconic(hwnd_);
    const int videoW = layout_.video.w();
    svc_->settings.playlistPanel = !svc_->settings.playlistPanel;
    svc_->settingsStore.Save(svc_->settings);
    if (resizeWindow) ResizeWindowForPlaylistPanel(videoW, svc_->settings.playlistPanel);
    if (modes_.Mode() == ShellMode::NowPlaying) {
        ApplyMode();
        if (!narrow && layout_.panelOverlay) OpenPanelOverlay();   // switched on in a window too narrow to dock it
    } else {
        SyncModel();
    }
}

/// Sets the window width so the client has room for a video rect of `videoWidthPx` with / without the docked panel: the left edge
/// stays, the right edge moves; clamped into the monitor work area (shifting left first, then shrinking - the video gives way).
void App::ResizeWindowForPlaylistPanel(int videoWidthPx, bool panelWanted) {
    RECT wr{}, cr{};
    if (!GetWindowRect(hwnd_, &wr) || !GetClientRect(hwnd_, &cr)) return;
    const int newCw = ClientWidthForVideo(videoWidthPx, scale_, panelWanted, (float)svc_->settings.playlistPanelWidthDip);
    int ww = (wr.right - wr.left) + (newCw - (int)cr.right);
    ww = std::max(ww, S(kMinWindowWidthDip));
    RECT vis = wr;
    if (FAILED(DwmGetWindowAttribute(hwnd_, DWMWA_EXTENDED_FRAME_BOUNDS, &vis, sizeof vis))) vis = wr;
    const int invL = std::max(0L, vis.left - wr.left), invR = std::max(0L, wr.right - vis.right);
    MONITORINFO mon{sizeof mon};
    GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mon);
    const RECT& work = mon.rcWork;
    ww = std::min<int>(ww, (work.right - work.left) + invL + invR);
    int l = wr.left;
    if (l + ww - invR > work.right) l = work.right + invR - ww;
    if (l + invL < work.left) l = work.left - invL;
    if (l == wr.left && ww == wr.right - wr.left) return;
    SetWindowPos(hwnd_, nullptr, l, wr.top, ww, wr.bottom - wr.top, SWP_NOZORDER | SWP_NOACTIVATE);   // WM_SIZE relayouts
}

/// Ctrl+S from anywhere in the player view: opens the panel when hidden and puts the keyboard focus in its search box.
/// The video child (SDL) may own the Win32 focus: take it back to the main window so keys reach the widget host.
void App::FocusPlaylistSearch() {
    if (modes_.Mode() != ShellMode::NowPlaying) return;
    if (!svc_->settings.playlistPanel) TogglePlaylistPanel();
    else if (layout_.panelOverlay && !panelPopup_) OpenPanelOverlay();
    PlaylistPanel* p = panelPopup_ ? panelPopup_ : (shell_ && shell_->Panel()->Visible() ? shell_->Panel() : nullptr);
    if (!p) return;
    if (hwnd_ && IsWindowVisible(hwnd_)) SetFocus(hwnd_);
    p->FocusSearch();
}

void App::OpenPanelOverlay() {
    if (panelPopup_ || !shell_ || layout_.video.Empty()) return;
    const bool fs = modes_.Mode() == ShellMode::FullScreen;
    if (!fs && modes_.Mode() != ShellMode::NowPlaying) return;
    // Narrow window: over the video column. Full screen: the full-height column on the right edge.
    const float top = layout_.video.t / scale_, bottom = layout_.video.b / scale_, right = layout_.video.r / scale_;
    const float w = std::max(0.f, std::min((float)svc_->settings.playlistPanelWidthDip, cw_ / scale_ - 16.f));
    auto panel = std::make_unique<PlaylistPanel>(*this, true);
    PlaylistPanel* raw = panel.get();
    raw->SetBounds({right - w, top, right, bottom});
    pui::PopupOptions po;
    po.lightDismiss = true;
    po.takeFocus = false;
    po.onClosed = [this, raw] {
        if (panelPopup_ == raw) panelPopup_ = nullptr;
        SyncModel();
    };
    panelPopup_ = raw;
    host_.OpenPopup(std::move(panel), po);
    raw->Refresh();
    raw->SetActive(!fs);   // full screen: the hover reveals it; keys stay with the player until the user clicks into it
    SyncModel();
}

void App::ForPanels(const std::function<void(PlaylistPanel&)>& f) {
    if (!shell_) return;
    f(*shell_->Panel());
    if (panelPopup_) f(*panelPopup_);
}

void App::CycleAlwaysOnTop() {
    svc_->settings.alwaysOnTop = sl::NextAlwaysOnTop(svc_->settings.alwaysOnTop);
    OnSettingsChanged(*this);   // saves and re-applies HWND_TOPMOST
    const wchar_t* names[] = {Tr(L"Always on top: Off", L"항상 위에: 사용 안 함"), Tr(L"Always on top: On", L"항상 위에: 항상"),
                            Tr(L"Always on top: While playing", L"항상 위에: 재생 중"), Tr(L"Always on top: While fullscreen", L"항상 위에: 전체 화면")};
    ShowToast(names[sl::AlwaysOnTopIndex(svc_->settings.alwaysOnTop)]);
}

void App::ToggleTimeMode() {
    svc_->settings.timeRemaining = !svc_->settings.timeRemaining;
    svc_->settingsStore.Save(svc_->settings);
    SyncModel();
}

void App::CheckReady() {
    if (readyWritten_ || !model_.prepared || state_ != wvp::PlayerState::Playing) return;
    readyWritten_ = true;
    wchar_t path[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"WVP_READY_FILE", path, ARRAYSIZE(path));
    if (n == 0 || n >= ARRAYSIZE(path)) return;
    try {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        std::time_t t = (std::time_t)(ms / 1000);
        std::tm tmv{}; gmtime_s(&tmv, &t);
        char ts[64];
        std::snprintf(ts, sizeof ts, "ready %04d-%02d-%02dT%02d:%02d:%02d.%03lldZ", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                      tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (long long)(ms % 1000));
        std::ofstream f(path, std::ios::trunc);
        f << ts << "\n";
    } catch (...) {}
}

void App::ApplyPendingQueue() {
    if (!ctl_ || !pending_) return;
    PendingPlay p = std::move(*pending_);
    pending_.reset();
    ctl_->SetQueue(currentPlaylistItems_, p.start, p.replace, p.startAt);
}

void App::Capture(bool withSubtitles) {
    auto frame = player_ ? player_->Snapshot(withSubtitles) : std::nullopt;
    if (!frame) { ShowToast(Tr(L"No frame to copy", L"복사할 프레임이 없습니다")); return; }
    std::thread([this, f = std::move(*frame)]() {
        std::wstring msg;
        try {
            wvp::FrameCapture::CopyToClipboard(f);
            msg = Tr(L"Copied", L"복사됨");
        } catch (const std::exception& ex) { msg = Tr(L"Copy failed: ", L"복사 실패: ") + Widen(ex.what()); }
        PostToUi([this, msg]() { ShowToast(msg); });
    }).detach();
}

PlaylistPanel* App::SelectionPanel() const {
    if (!shell_) return nullptr;
    PlaylistPanel* p = panelPopup_ ? panelPopup_ : (svc_->settings.playlistPanel && !layout_.panelOverlay ? shell_->Panel() : nullptr);
    return p && !p->Stale() && p->CanPlaySelection() ? p : nullptr;
}

bool App::CanPlaySelection() const { return !Loaded() && SelectionPanel(); }

void App::OnPanelSelectionChanged() { SyncModel(); }

void App::TogglePause() {
    if (!ctl_) return;
    if (!Loaded()) {
        if (PlaylistPanel* p = SelectionPanel()) { p->PlaySelection(); return; }
    }
    using wvp::PlayerState;
    if (state_ == PlayerState::Ended || state_ == PlayerState::Stopped || state_ == PlayerState::Idle || state_ == PlayerState::Error)
        PlayPlaylistEntry(*this, currentPlaylistName_, 0, true);
    else ctl_->TogglePause();
}

void App::LogSeek(const char* how, double seconds, bool relative) {
    char b[48];
    std::snprintf(b, sizeof b, "%.2f", seconds);
    log::Event("seek", std::string(how) + (relative ? " relative " : " to ") + b + " s");
}

void App::SetVolume(int v) {
    if (!ctl_) return;
    v = std::clamp(v, 0, 100);
    ctl_->SetVolume(v);
    if (v > 0 && ctl_->IsMuted()) ctl_->SetMuted(false);
    SyncModel();
}

void App::AdjustVolume(int d) {
    SetVolume((ctl_ ? ctl_->Volume() : 100) + d);
    if (!keyInput_) StartBarAnim(autoHide_.Activity(GetTickCount64()));   // wheel reveals the bar, volume keys do not
    Flush();
}

wvp::FrameStepController::Hooks App::MakeFrameStepHooks() {
    wvp::FrameStepController::Hooks h;
    h.step = [this](int action, int direction) {
        if (!player_ || !MediaActive()) return -1;
        if (action >= kKeyStepSkip) {
            if (!model_.Seekable()) return -1;
            const double sec = (action - kKeyStepSkip) * (direction > 0 ? 1.0 : -1.0);
            const double before = player_->Position();
            const int rc = player_->Skip(sec);
            if (rc == 0) {
                LogSeek("key", sec, true);
                SavePlaybackHistory(true, false, std::clamp(std::isfinite(before) ? before + sec : model_.pos + sec, 0.0, model_.dur));
            }
            return rc;
        }
        if (action != kKeyStepKeyframe) return player_->StepFrame(direction);
        const int rc = player_->SeekKeyframe(direction);
        if (rc == 0) log::Event("seek", direction > 0 ? "key next keyframe" : "key previous keyframe");
        return rc;
    };
    // The physical key state, not the queued WM_KEYUP: with the UI thread busy a release can still sit in the message
    // queue when a step completes, and an extra D step costs a GOP decode. Another application's keys do not count.
    h.held = [](int key) {
        if ((GetAsyncKeyState(key) & 0x8000) == 0) return false;
        DWORD pid = 0;
        HWND fg = GetForegroundWindow();
        return fg && GetWindowThreadProcessId(fg, &pid) && pid == GetCurrentProcessId();
    };
    h.nowMs = [] { return (uint64_t)GetTickCount64(); };
    return h;
}

void App::RunKeyCommand(int vk, CommandId c, bool repeat) {
    const CommandId m = MiniRemapCommand(c, modes_.mini);
    if (m == CommandId::FrameNext || m == CommandId::FramePrevious) {
        frameStep_.KeyDown(vk, kKeyStepFrame, m == CommandId::FrameNext ? +1 : -1, repeat);
        return;
    }
    if (m == CommandId::KeyframeNext || m == CommandId::KeyframePrevious) {
        frameStep_.KeyDown(vk, kKeyStepKeyframe, m == CommandId::KeyframeNext ? +1 : -1, repeat);
        return;
    }
    if (const int sec = SeekSeconds(m)) {
        frameStep_.KeyDown(vk, kKeyStepSkip + std::abs(sec), sec > 0 ? +1 : -1, repeat);
        return;
    }
    Execute(c);
}

void App::Execute(CommandId c) {
    using C = CommandId;
    c = MiniRemapCommand(c, modes_.mini);
    if (c != C::FrameNext && c != C::FramePrevious && c != C::KeyframeNext && c != C::KeyframePrevious)
        frameStep_.Cancel();   // another command ends a held D / F / Left / Right chain
    switch (c) {
    case C::PlayPause: TogglePause(); break;
    case C::Previous: if (ctl_) ctl_->Previous(); break;
    case C::Next: if (ctl_) ctl_->Next(); break;
    case C::SeekBack5: case C::SeekForward5: case C::SeekBack30: case C::SkipForward30:
    case C::SeekBack60: case C::SeekForward60: case C::SeekBack300: case C::SeekForward300:
        frameStep_.StepOnce(kKeyStepSkip + std::abs(SeekSeconds(c)), SeekSeconds(c) > 0 ? +1 : -1);
        break;
    case C::SpeedDown: SetPlaybackSpeed(*this, SteppedSpeed(svc_->filters.Settings().speed, -1, wvp::filters::kMaxSpeed)); break;
    case C::SpeedUp: SetPlaybackSpeed(*this, SteppedSpeed(svc_->filters.Settings().speed, +1, wvp::filters::kMaxSpeed)); break;
    case C::SpeedReset: SetPlaybackSpeed(*this, 1.0); break;
    case C::FrameNext: frameStep_.StepOnce(kKeyStepFrame, +1); break;
    case C::FramePrevious: frameStep_.StepOnce(kKeyStepFrame, -1); break;
    case C::KeyframeNext: frameStep_.StepOnce(kKeyStepKeyframe, +1); break;
    case C::KeyframePrevious: frameStep_.StepOnce(kKeyStepKeyframe, -1); break;
    case C::WindowSize50: case C::WindowSize100: case C::WindowSize150: case C::WindowSize200: case C::WindowSizeMax:
        SizeWindowToVideo(WindowScale(c));
        break;
    case C::CycleSubtitles: {
        auto mi = player_ ? player_->GetMediaInfo() : std::nullopt;
        if (mi && fx::ToggleAribCaptionVisibility(*mi, svc_->settings.showCaptions)) {
            OnSettingsChanged(*this);
            ShowToast(svc_->settings.showCaptions ? Tr(L"Captions on", L"자막 켜짐") : Tr(L"Captions off", L"자막 꺼짐"));
        } else if (model_.subCount > 0) {
            CycleTrack(*this, wvp::StreamType::Subtitle);
        }
        break;
    }
    case C::Stop:
        if (modes_.mini) SetMiniPlayer(false);   // the mini player needs media: Stop returns to the player view first
        CloseMedia();
        break;
    case C::ToggleControlPanel: ToggleFiltersPanel(*this); break;
    case C::TogglePlaylistPanel: TogglePlaylistPanel(); break;
    case C::FocusPlaylistSearch: FocusPlaylistSearch(); break;
    case C::CycleAlwaysOnTop: CycleAlwaysOnTop(); break;
    case C::ToggleTimeMode: ToggleTimeMode(); break;
    case C::ToggleHwDecoding: {   // bar H/W | S/W chip: the configured decoder <-> off, same path as Settings > Playback > Hardware decoding
        wvp::AppSettings& s = svc_->settings;
        if (s.hwDecoder != L"off") { s.hwDecoderLastOn = s.hwDecoder; s.hwDecoder = L"off"; }
        else s.hwDecoder = wvp::IsValidHwDecoder(s.hwDecoderLastOn) && s.hwDecoderLastOn != L"off" ? s.hwDecoderLastOn : L"auto";
        OnSettingsChanged(*this);
        SyncModel();
        break;
    }
    case C::ToggleMute: if (ctl_) { ctl_->SetMuted(!ctl_->IsMuted()); SyncModel(); } break;
    case C::VolumeUp: AdjustVolume(5); break;
    case C::VolumeDown: AdjustVolume(-5); break;
    case C::ToggleFullscreen: SetFullscreen(!modes_.fullscreen); break;
    case C::Escape:
        if (modes_.fullscreen) SetFullscreen(false);
        break;
    case C::ToggleMiniPlayer: SetMiniPlayer(!modes_.mini); break;
    case C::OpenFiles: OpenFiles(*this); break;
    case C::OpenFolder: OpenFolder(*this); break;
    case C::OpenUrl: OpenUrl(*this); break;
    case C::CaptureFrame: Capture(false); break;
    case C::CaptureFrameWithCaptions: Capture(true); break;
    case C::ShowProperties: ShowPropertiesDialog(*this); break;
    case C::ShowEqualizer: ShowFiltersPanel(*this, FiltersTab::Audio); break;
    case C::ZoomToFill:  ToggleZoomToFill(*this); break;
    case C::VideoEnhancements: ShowFiltersPanel(*this, FiltersTab::Video); break;
    case C::OpenSettings: ShowSettings(); break;
    case C::ToggleDebugOverlay: ToggleDebugOverlay(); break;
    case C::Exit: if (hwnd_) PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;
    default: break;
    }
    SyncModel();
    Flush();
}

dbg::OverlayContext App::BuildDebugContext() {
    dbg::OverlayContext c;
    c.mode = modes_.mini ? "mini player" : modes_.fullscreen ? "full screen" : "now playing";
    c.acrylicFallbackReason = std::string(acrylic::FallbackText(acrylicFallbackReason_));
    c.displayRefreshHz = disp::RefreshRateHz(hwnd_);
    {   // build stamps so a stale exe/DLL is visible
        std::wstring st = ExeBuildStamp(), ls = LibffplayBuildStamp();
        if (!ls.empty()) st += (st.empty() ? L"" : L"  |  ") + ls;
        c.buildStamp = wvp::ToUtf8(st);
    }
    if (player_) {
        if (auto j = player_->GetDebugInfoJson()) {
            c.haveJson = true;
            c.info = wvp::ParseDebugInfo(*j);
        }
    }
    if (const wvp::Plan* pl = svc_->filters.CurrentPlan()) c.plan = *pl;
    c.url = wvp::ToUtf8(lastOpenUrl_);
    c.resumeDecision = resumeDecision_;
    if (!lastOpenUrl_.empty()) {
        c.openOptions = wvp::ToUtf8(log::FormatOptionsLine(effOpts_));
        c.ffplayCmd = wvp::ToUtf8(lastOpenCmd_);
    }
    const wvp::FilterSettings& fs = svc_->filters.Settings();
    c.requestedSpeed = fs.speed;
    {
        auto nameOf = [&](const std::string& luid) {
            for (const auto& a : adapters_) if (a.luid == luid) return a.name;
            return luid.empty() ? std::string("unknown") : luid;
        };
        c.adapterNote = (playAdapter_.luid.empty() ? std::string("video GPU none") : "video GPU " + wvp::ToUtf8(playAdapter_.name) + " (luid " +
                         playAdapter_.luid + ")") + ", display " + nameOf(displayLuid_) +
                        (displayRawLuid_ != displayLuid_ && !displayRawLuid_.empty() ? " (alias: " + displayRawLuid_ + ")" : "") + " - " +
                        playAdapter_.note;
        if (haveVideoChoice_ && videoChoice_.runnerUp >= 0 && videoChoice_.runnerUp < (int)adapters_.size())
            c.adapterNote += "; runner-up " + adapters_[(size_t)videoChoice_.runnerUp].name + " [host " +
                             std::to_string(videoChoice_.runnerUpTotal.Weighted()) + ", device " + std::to_string(videoChoice_.runnerUpTotal.deviceCopies) + "]";
        c.adapterWarn = haveVideoChoice_ && (videoChoice_.crossAdapter || videoChoice_.manualMissing || videoChoice_.index < 0);
    }
    c.deinterlaceRequested = fs.deinterlace == wvp::DeinterlaceMode::On ? "deinterlace" : fs.deinterlace == wvp::DeinterlaceMode::Ivtc ? "ivtc" : "";
    return c;
}

void App::RefreshDebugOverlay() {
    auto lines = dbg::BuildOverlayLines(BuildDebugContext());
    if (lines != debugLines_) {
        debugLines_ = std::move(lines);
        debugDirty_ = true;
    }
}

void App::ToggleDebugOverlay(const char* src) {
    debugVisible_ = !debugVisible_;
    log::Event("debug", std::string(debugVisible_ ? "overlay on" : "overlay off") + " (" + src + ", thread " + std::to_string(GetCurrentThreadId()) + ")");
    debugDirty_ = true;
    if (debugVisible_) {
        RefreshDebugOverlay();
        debugNextMs_ = Now() + 250;
        SetTimerMs(50);   // ~4 Hz refresh (every 5th tick); back to 100 ms when hidden
    }
    Flush();
}

}
