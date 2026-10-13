#pragma once
// JSON persistence in platform::AppDataDir()\settings.json (path injectable for tests).
// Unknown keys are ignored, out-of-range values clamped.
#include <cstdint>
#include <string>

#include "FilterManager.h"
#include "PlayerTypes.h"
#include "SubtitleStyle.h"
#include "VideoGpu.h"

namespace wvp {

enum class ThemeSetting { System, Light, Dark };
enum class AlwaysOnTop { Disable, Enable, WhenPlaying, WhenFullscreen };

struct WindowPlacement {
    int x = 0, y = 0, w = 0, h = 0;
    bool maximized = false;
    bool valid = false;
    int dpi = 96;                            // DPI of the monitor the size (device px) was saved on
    bool operator==(const WindowPlacement&) const = default;
};

struct AppSettings {
    ThemeSetting theme = ThemeSetting::System;
    std::uint32_t accentColor = 0;           // 0 = system accent, else ARGB
    AlwaysOnTop alwaysOnTop = AlwaysOnTop::Disable;   // JSON key "alwaysOnTop": "Disable"|"Enable"|"WhenPlaying"|"WhenFullscreen"; else Disable
    // Settings > Playback > Video GPU. JSON key "videoGpu": "auto" (also absent/invalid) or
    // {"luid": "HHHHHHHH:LLLLLLLL", "vendorId": N, "deviceId": N, "name": "..."}.
    VideoGpuPref videoGpu;
    FilterSettings filters;
    WindowPlacement window;
    WindowPlacement miniWindow;
    WindowPlacement settingsWindow;          // the separate Settings window (position / size in device px; JSON key "settingsWindow")
    bool shuffle = false;
    int repeat = 0;                          // 0 off, 1 all, 2 one
    bool playlistPanel = true;               // windowed player view shows the playlist panel (bar toggle / F6); JSON key "playlistPanel"
    int playlistPanelWidthDip = 320;         // docked / overlay playlist panel width (DIP), dragged at the video|panel splitter; JSON key "playlistPanelWidthDip"
    bool timeRemaining = false;
    bool debugLogging = false;               // write the libffplay + app log to %LOCALAPPDATA%\aribplayer\logs\aribplayer-YYYYMMDD.log
    bool showCaptions = true;                 // ARIB caption layer; missing key means on
    bool showSuperimpose = true;               // ARIB superimpose layer; missing key means on
    SubtitleStyle subtitle;                   // Settings > Subtitles; JSON key "subtitle"
    std::string currentPlaylist;              // active named playlist (PlaylistStore name); empty = the unnamed playlist; missing key = empty

    bool operator==(const AppSettings&) const = default;
};

class SettingsStore {
public:
    /// Empty path = DefaultPath().
    explicit SettingsStore(std::string path = {});

    static std::string DefaultPath();   // platform::AppDataDir() + settings.json

    const std::string& Path() const { return path_; }

    /// Missing or corrupt file -> defaults; never throws.
    AppSettings Load() const;
    /// Creates the directory, atomic write (temp file + replace). False on I/O failure.
    bool Save(const AppSettings& s) const;

    static std::string ToJson(const AppSettings& s);
    /// false (and defaults in `out`) when the text is not a JSON object.
    static bool FromJson(const std::string& utf8, AppSettings& out);

private:
    std::string path_;
};

}
