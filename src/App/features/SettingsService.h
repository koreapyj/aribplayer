#pragma once
// Settings load/save glue. The shell calls LoadSettingsAtStartup after Services and the widget
// tree exist and before the window is shown (and before the player is created), SaveSettingsAtShutdown from WM_CLOSE.
// Pages call OnSettingsChanged after changing ctx.Svc().settings (it saves and applies live).
#include <windows.h>

#include "wvp/FilterManager.h"
#include "wvp/AppSettings.h"
#include <optional>
#include <string>

#include "wvp/PlaybackController.h"

namespace app {
class AppContext;

/// Subscribes to OnPlayerReady to apply hardware decoding, volume, shuffle and repeat to the controller.
void LoadSettingsAtStartup(AppContext& ctx);
void SaveSettingsAtShutdown(AppContext& ctx);
/// Updates the in-memory saved filter state immediately and debounces disk writes while sliders are dragged.
void QueueFilterSettingsSave(AppContext& ctx);
/// Writes the current filter state now (panel close and shutdown); cancels any pending debounce.
void FlushFilterSettings(AppContext& ctx);
/// Saves the settings and applies theme/accent immediately; hardware decoding applies to the next opened item.
void OnSettingsChanged(AppContext& ctx);

/// A window's normal placement (screen coordinates; maximized flag; its DPI); nullopt when it cannot be read.
std::optional<wvp::WindowPlacement> CaptureWindowPlacement(HWND hwnd);
/// A saved placement rescaled to the DPI of the monitor it will appear on and validated against the monitors; nullopt = use
/// the default geometry.
std::optional<wvp::WindowPlacement> RestorablePlacement(wvp::WindowPlacement saved);

/// Full path of the loaded libffplay.dll.
std::wstring LibffplayPath();

/// "aribplayer.exe built 2026-10-04 20:00:12" from the exe's last-write time (never stale like __TIME__ of one unit).
/// Shown in the debug overlay header so an old binary is recognisable.
std::wstring ExeBuildStamp();
/// Same for the loaded libffplay.dll ("libffplay.dll built ...").
std::wstring LibffplayBuildStamp();
/// Pure formatter used by both (FILETIME -> "built YYYY-MM-DD HH:MM:SS" local time), "" for a zero time.
std::wstring FormatBuildTime(unsigned long long fileTime100ns);

/// Sets the ARIB font, superimpose and Windows caption-style open options before every media open.
void ApplyCaptionOpenOptions(wvp::PlaybackController& ctl, const wvp::AppSettings& settings);
/// Refreshes the Windows Closed Caption Properties mapping and ARIB caption decoder when the ClosedCaptioning registry key
/// changes (StartSystemCaptionSettingsWatch).
void OnSystemCaptionSettingsChanged(AppContext& ctx);
/// Windows Settings > Accessibility > Captions broadcasts no WM_SETTINGCHANGE for caption changes, so a background thread
/// watches HKCU\Software\Microsoft\Windows\CurrentVersion\ClosedCaptioning (RegNotifyChangeKeyValue, subtree) and posts
/// OnSystemCaptionSettingsChanged to the UI thread, debounced 250 ms (Settings writes several values per change).
void StartSystemCaptionSettingsWatch(AppContext& ctx);
/// Stops and joins the watcher (before the window / AppContext goes away). Safe when it never started.
void StopSystemCaptionSettingsWatch();
/// Bundled font location next to the executable (empty override means this path).
std::wstring BundledAribFontPath();

/// What ApplySettingsLive did, for the toast and the tests.
struct LiveApplyResult {
    bool reopened = false;          // the current item was re-opened at its position (hwaccel / decoder change)
    std::wstring toast;             // "" = nothing to say
};
/// Applies the playback-affecting differences between `before` and `after` to the running playback:
///  * hwDecoder -> the planner's decoder preference; when the plan's decoder changes and media is playing/paused:
///    ReopenCurrentHere (position, pause kept; audio/subtitle selection is restored by the app through SelectTrack after
///    the new Prepared).
LiveApplyResult ApplySettingsLive(wvp::PlaybackController& ctl, wvp::FilterManager& fm, const wvp::AppSettings& before,
                                  const wvp::AppSettings& after);
}
