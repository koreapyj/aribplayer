#pragma once
// Everything runs on the UI thread unless a member says otherwise. Implemented by app::App (App.h).
#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Commands.h"
#include "Services.h"
#include "ShellLogic.h"
#include "pui/Renderer.h"
#include "pui/Theme.h"
#include "pui/Widget.h"
#include "wvp/Event.h"
#include "wvp/FfPlayer.h"
#include "wvp/MediaItem.h"
#include "wvp/PlaybackController.h"

namespace app {

enum class InfoSeverity { Informational, Success, Warning, Error };

class AppContext {
public:
    virtual ~AppContext() = default;

    virtual HWND MainHwnd() const = 0;
    /// The single UiHost of the shell: popups (OpenPopup), focus, UI events.
    virtual pui::UiHost& Ui() = 0;
    /// Playback controller / player: nullptr until the player is created (ffp_create runs asynchronously; subscribe to
    /// OnPlayerReady). Both are destroyed before the window closes.
    virtual wvp::PlaybackController* Controller() = 0;
    virtual wvp::FfPlayer* Player() = 0;
    /// App-wide services (settings, recent media, filters, playlists); owned by the App.
    virtual Services& Svc() = 0;
    /// Renderer for text measurement (DesiredSize, ellipsis decisions). Valid for the app lifetime.
    virtual pui::IRenderer& Measurer() = 0;

    /// Re-evaluates Settings > Always on top (state / maximized) and applies HWND_TOPMOST / HWND_NOTOPMOST. The mini
    /// player keeps its own topmost. Default: no-op (test stubs).
    virtual void ApplyWindowTopmost() {}

    /// Full screen: the window covers the monitor and the title strip is hidden (the player view is the only view).
    virtual void SetFullscreen(bool on) = 0;
    virtual bool IsFullscreen() const = 0;
    /// Mini player hook: updates the mode state and calls features/MiniPlayer (EnterMiniPlayer / ExitMiniPlayer).
    virtual void SetMiniPlayer(bool on) = 0;
    virtual bool IsMiniPlayer() const = 0;
    virtual ShellMode Mode() const = 0;

    /// Name of the current playlist; empty identifies the persistent unnamed playlist.
    virtual std::string CurrentPlaylistName() const { return {}; }
    virtual bool IsCurrentPlaylist(const std::string& name) const { return name == CurrentPlaylistName(); }
    /// Loads an arbitrary playlist. The unnamed list is supplied by App; the default supports named playlists in tests.
    virtual std::vector<wvp::MediaItem> PlaylistItems(const std::string& name) {
        if (name.empty()) {
            if (IsCurrentPlaylist(name)) if (auto* c = Controller()) return c->Items();
            return {};
        }
        return Svc().playlists.Load(name);
    }
    /// Persistent "played" flag per URL (one history read; may wait for pending history writes).
    virtual std::vector<bool> PlayedFlags(const std::vector<std::string>& urls) { return Svc().playlists.GetPlayedFlags(urls); }
    /// Saves an arbitrary playlist; App persists the unnamed file and syncs controller edits for the current list.
    virtual bool SavePlaylistItems(const std::string& name, const std::vector<wvp::MediaItem>& items) {
        return !name.empty() && Svc().playlists.Save(name, items);
    }
    /// Activates a playlist and starts at the requested item. The app persists its identity.
    virtual void PlayPlaylist(const std::string&, const std::vector<wvp::MediaItem>& items, int) {
        AppendToCurrentPlaylist(items);
    }
    /// Activates a playlist item at a remembered offset. Default test contexts may ignore the offset.
    virtual void PlayPlaylistAtPosition(const std::string& name, const std::vector<wvp::MediaItem>& items, int start, double position) {
        (void)position;
        PlayPlaylist(name, items, start);
    }
    /// Replaces the unnamed slot and starts its first item at a remembered offset.
    virtual void ReplaceUnnamedPlaylistAtPosition(std::vector<wvp::MediaItem> items, double position) = 0;
    virtual bool IsPlaylistReadOnly(const std::string& name) { return Svc().playlists.IsReadOnly(name); }
    virtual void SetResumeDecision(const std::string&) {}
    /// Appends without interrupting current playback. App owns current-list persistence and updates the controller if active.
    /// Playlist views (drops on details / overview, Add files): append only; never starts or changes playback.
    virtual void AppendPlaylistItems(const std::string& name, const std::vector<wvp::MediaItem>& items) {
        if (items.empty()) return;
        if (IsCurrentPlaylist(name)) { AppendToCurrentPlaylist(items); return; }
        auto all = PlaylistItems(name);
        all.insert(all.end(), items.begin(), items.end());
        SavePlaylistItems(name, all);
    }
    /// Appends to the current playlist; starts its first item only when nothing is current.
    virtual void AppendToCurrentPlaylist(std::vector<wvp::MediaItem> items) = 0;
    virtual void PlaylistRenamed(const std::string&, const std::string&) {}
    virtual void PlaylistDeleted(const std::string&) {}
    /// A playlist file was created or rewritten: the panel chips (and a viewed non-current list) refresh at once.
    virtual void PlaylistsChanged() {}

    /// Persistent banner under the title strip (auto-dismisses after ~12 s, click dismisses).
    virtual void ShowInfoBar(InfoSeverity severity, const std::wstring& title, const std::wstring& message) = 0;
    /// Short pill above the bar (~2.5 s) and the status line in the bar header.
    virtual void ShowToast(const std::wstring& text) = 0;
    /// Hardware adapters that can be the video GPU (from the capability probes), for Settings > Playback > Video GPU.
    virtual std::vector<wvp::VideoGpuPref> VideoGpus() const { return {}; }
    /// settings.videoGpu changed: select again and reopen the current item on the new adapter when it changed.
    virtual void VideoGpuSettingChanged() {}
    /// Shows the error notice when no video GPU could be probed or no pipeline plan exists for the current settings.
    virtual void ReportPlanProblem() {}

    /// mode System follows the OS app theme; accent 0 = system accent colour (0xAARRGGBB straight ARGB otherwise).
    virtual void ApplyTheme(pui::ThemeMode mode, uint32_t accent) = 0;
    virtual const pui::Theme& CurrentTheme() const = 0;
    virtual pui::ThemeMode CurrentThemeMode() const = 0;
    /// DIP -> device pixel scale of the main window (dpi / 96).
    virtual float Scale() const = 0;
    /// Coalesced.
    virtual void RequestRedraw() = 0;
    virtual void Execute(CommandId command) = 0;
    /// Nothing is loaded but the playlist panel has a row selected: Play starts that row. The panel reports selection changes.
    virtual bool CanPlaySelection() const { return false; }
    virtual void OnPanelSelectionChanged() {}
    /// Runs fn on the UI thread (thread-safe; use from worker threads).
    virtual void PostToUi(std::function<void()> fn) = 0;

    // Multicast hooks: .add(fn) -> token, .remove(token); UI thread unless noted.
    wvp::Event<> OnPlayerReady;                                   // controller/player were created
    wvp::Event<const wvp::MediaItem*> OnMediaOpened;              // current queue item changed (nullptr = none)
    wvp::Event<const wvp::MediaPreparedInfo&> OnMediaPrepared;    // PREPARED for the current item
    wvp::Event<const wvp::PlayerErrorInfo&> OnMediaError;
    wvp::Event<> OnPlaybackProgress;                                // throttled position/history update for resume markers
    wvp::Event<> OnMediaEnded;                                    // an item ended (before the controller auto-advances)
    wvp::Event<const pui::Theme&> OnThemeChanged;
    wvp::Event<ShellMode> OnModeChanged;
};

}
