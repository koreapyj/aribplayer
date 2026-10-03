#pragma once
// The App owns one instance (std::make_unique<Services>()) and exposes it through AppContext::Svc().
//  * settingsStore/settings: loaded by features/SettingsService (LoadSettingsAtStartup) and saved at shutdown/on change.
//  * filters: the live FilterManager; FiltersPanel applies changes and forwards the FilterChange to
//    PlaybackController::ApplyFilterChange; the controller's FilterProvider is filters.Provider().
//  * playlists: user playlists (.m3u8) in %LOCALAPPDATA%\aribplayer\Playlists.
// Tests / --logictest may construct Services with injected paths through the constructor arguments.
#include <string>

#include "wvp/AppSettings.h"
#include "wvp/FilterManager.h"
#include "wvp/PlaylistStore.h"

namespace app {

struct Services {
    Services() = default;
    /// dataDir empty = %LOCALAPPDATA%\aribplayer (default paths).
    explicit Services(const std::wstring& dataDir)
        : settingsStore(dataDir.empty() ? std::wstring{} : dataDir + L"\\settings.json"),
          playlists(dataDir.empty() ? std::wstring{} : dataDir + L"\\Playlists") {}

    wvp::SettingsStore settingsStore;
    wvp::AppSettings settings;
    wvp::FilterManager filters;
    wvp::PlaylistStore playlists;
};

}
