#include "features/SettingsService.h"

#include <windows.h>
#include <shellscalingapi.h>
#pragma comment(lib, "shcore.lib")

#include <algorithm>
#include <filesystem>
#include <thread>
#include <utility>

#include <winrt/Windows.Media.ClosedCaptioning.h>
#pragma comment(lib, "windowsapp.lib")

#include "AppContext.h"
#include "Localize.h"
#include "features/Logging.h"
#include "features/PlaybackExtras.h"
#include "features/FxModel.h"
#include "features/FxUtil.h"
#include "features/SettingsLogic.h"
#include "wvp/win32/Utf.h"

namespace app {

namespace {
struct CaptionStyle {
    bool ignoreBackground = false;
    bool forceOutline = false;
    bool operator==(const CaptionStyle&) const = default;
};

CaptionStyle ReadSystemCaptionStyle() {
    CaptionStyle style;
    try {
        using namespace winrt::Windows::Media::ClosedCaptioning;
        // ARIB draws one box per character cell, which is the Windows text background; the caption
        // window (region) has no ARIB counterpart, so only a transparent text background drops the box.
        style.ignoreBackground = ClosedCaptionProperties::BackgroundOpacity() == ClosedCaptionOpacity::ZeroPercent;
        const auto effect = ClosedCaptionProperties::FontEffect();
        style.forceOutline = effect == ClosedCaptionEdgeEffect::Uniform || effect == ClosedCaptionEdgeEffect::DropShadow;
    } catch (...) {
        // Windows may reject the property read; preserve libaribcaption's broadcast defaults.
    }
    return style;
}

std::uint32_t g_appliedAccent = 0;
wvp::AppSettings g_appliedSettings;   // what the running playback was last configured with (diff base for ApplySettingsLive)
CaptionStyle g_appliedCaptionStyle{};
bool g_captionStyleKnown = false;
bool g_hooked = false;
UINT_PTR g_filterSaveTimer = 0;
using PreparedToken = wvp::Event<const wvp::MediaPreparedInfo&>::Token;
AppContext* g_restoreHooksContext = nullptr;
AppContext* g_pendingRestoreContext = nullptr;
PreparedToken g_pendingRestoreToken = 0;

void CancelPendingTrackRestore(AppContext& ctx) {
    if (g_pendingRestoreContext != &ctx) return;
    if (g_pendingRestoreToken) ctx.OnMediaPrepared.remove(g_pendingRestoreToken);
    g_pendingRestoreToken = 0;
    g_pendingRestoreContext = nullptr;
}

void EnsureTrackRestoreCancellationHooks(AppContext& ctx) {
    if (g_restoreHooksContext == &ctx) return;
    g_restoreHooksContext = &ctx;
    ctx.OnMediaOpened.add([&ctx](const wvp::MediaItem*) { CancelPendingTrackRestore(ctx); });
    ctx.OnMediaError.add([&ctx](const wvp::PlayerErrorInfo&) { CancelPendingTrackRestore(ctx); });
}

void ScheduleTrackRestore(AppContext& ctx, int audioSelected, int subtitleSelected) {
    CancelPendingTrackRestore(ctx);
    EnsureTrackRestoreCancellationHooks(ctx);
    auto tok = std::make_shared<PreparedToken>(0);
    *tok = ctx.OnMediaPrepared.add([&ctx, tok, audioSelected, subtitleSelected](const wvp::MediaPreparedInfo&) {
        ctx.OnMediaPrepared.remove(*tok);
        if (g_pendingRestoreContext == &ctx && g_pendingRestoreToken == *tok) {
            g_pendingRestoreContext = nullptr;
            g_pendingRestoreToken = 0;
        }
        if (audioSelected > 0) SelectTrack(ctx, wvp::StreamType::Audio, audioSelected);
        if (subtitleSelected >= 0) SelectTrack(ctx, wvp::StreamType::Subtitle, subtitleSelected);
    });
    g_pendingRestoreContext = &ctx;
    g_pendingRestoreToken = *tok;
}

int SelectedTrackIndex(AppContext& ctx, wvp::StreamType type) {
    auto* player = ctx.Player();
    auto mi = player ? player->GetMediaInfo() : std::nullopt;
    if (!mi) return -1;
    if (type == wvp::StreamType::Audio && mi->AribAudio.Present) return player->AudioPreference();
    auto streams = mi->Of(type);
    if (type == wvp::StreamType::Subtitle) return fx::SelectedCycleableSubtitleIndex(streams);
    for (size_t i = 0; i < streams.size(); ++i) if (streams[i].Selected) return (int)i;
    return -1;
}

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH * 2]{};
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    std::filesystem::path p(std::wstring(buf, n));
    return p.parent_path().wstring();
}

BOOL CALLBACK MonitorProc(HMONITOR m, HDC, LPRECT, LPARAM lp) {
    MONITORINFO mi{sizeof mi};
    if (GetMonitorInfoW(m, &mi))
        reinterpret_cast<std::vector<sl::MonitorRect>*>(lp)->push_back({mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom});
    return TRUE;
}
std::vector<sl::MonitorRect> Monitors() {
    std::vector<sl::MonitorRect> v;
    EnumDisplayMonitors(nullptr, nullptr, MonitorProc, reinterpret_cast<LPARAM>(&v));
    return v;
}

}

std::optional<wvp::WindowPlacement> RestorablePlacement(wvp::WindowPlacement saved) {
    if (saved.valid) {   // rescale the saved device-pixel size when the monitor it will appear on has another DPI
        POINT c{saved.x + saved.w / 2, saved.y + saved.h / 2};
        UINT dx = 0, dy = 0;
        if (SUCCEEDED(GetDpiForMonitor(MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &dx, &dy)) && dx > 0)
            saved = sl::RescalePlacement(saved, (int)dx);
    }
    return sl::ValidatePlacement(saved, Monitors());
}

std::optional<wvp::WindowPlacement> CaptureWindowPlacement(HWND hwnd) {
    WINDOWPLACEMENT wp{sizeof wp};
    if (!hwnd || !GetWindowPlacement(hwnd, &wp)) return std::nullopt;
    wvp::WindowPlacement p;
    p.maximized = wp.showCmd == SW_SHOWMAXIMIZED || IsZoomed(hwnd);
    RECT r{};
    if (p.maximized || IsIconic(hwnd)) {
        // rcNormalPosition is in work-area coordinates; the settings keep screen coordinates (SetWindowPos on restore).
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        r = wp.rcNormalPosition;
        OffsetRect(&r, mi.rcWork.left - mi.rcMonitor.left, mi.rcWork.top - mi.rcMonitor.top);
    } else if (!GetWindowRect(hwnd, &r)) {
        return std::nullopt;
    }
    p.x = r.left; p.y = r.top; p.w = r.right - r.left; p.h = r.bottom - r.top;
    p.valid = p.w > 0 && p.h > 0;
    p.dpi = (int)GetDpiForWindow(hwnd);
    if (!p.valid) return std::nullopt;
    return p;
}

namespace {

void RestoreWindow(AppContext& ctx) {
    HWND hwnd = ctx.MainHwnd();
    if (!hwnd) return;
    auto v = RestorablePlacement(ctx.Svc().settings.window);
    if (!v) return;
    SetWindowPos(hwnd, nullptr, v->x, v->y, v->w, v->h, SWP_NOZORDER | SWP_NOACTIVATE);
    if (v->maximized) {
        // The shell shows the window right after LoadSettingsAtStartup; maximise once it is visible (never for hidden runs).
        ctx.PostToUi([hwnd] { if (IsWindow(hwnd) && IsWindowVisible(hwnd) && !IsZoomed(hwnd)) ShowWindow(hwnd, SW_MAXIMIZE); });
    }
}

void SaveWindow(AppContext& ctx) {
    HWND hwnd = ctx.MainHwnd();
    if (!hwnd || ctx.IsFullscreen() || ctx.IsMiniPlayer()) return;   // keep the last normal geometry
    if (auto p = CaptureWindowPlacement(hwnd)) ctx.Svc().settings.window = *p;
}

void ApplyPlayback(AppContext& ctx) {
    auto* c = ctx.Controller();
    if (!c) return;
    const wvp::AppSettings& s = ctx.Svc().settings;
    c->Player().SetSubtitleVisible(wvp::SubtitleLayer::Caption, s.showCaptions);
    c->Player().SetSubtitleVisible(wvp::SubtitleLayer::Superimpose, s.showSuperimpose);
    // Volume is never remembered across launches: the controller keeps its default (100, unmuted).
    c->SetShuffle(s.shuffle);
    c->SetRepeat(static_cast<wvp::RepeatMode>(std::clamp(s.repeat, 0, 2)));
}
}

std::wstring LibffplayPath() {
    wchar_t p[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(GetModuleHandleW(L"libffplay.dll"), p, (DWORD)std::size(p));
    return std::wstring(p, n);
}

std::wstring BundledAribFontPath() { return ExeDir() + L"\\fonts\\WLCMARU2004ARIBU.TTF"; }

void ApplyCaptionOpenOptions(wvp::PlaybackController& ctl, const wvp::AppSettings& settings) {
    CaptionStyle style = ReadSystemCaptionStyle();
    g_appliedCaptionStyle = style;
    g_captionStyleKnown = true;
    std::string fontPath = settings.aribFont.empty() ? wvp::ToUtf8(BundledAribFontPath()) : settings.aribFont;
    ctl.SetBaseOption("arib_font", wvp::AribFontOption(fontPath));
    ctl.SetBaseOption("superimpose", settings.showSuperimpose ? "1" : "0");
    ctl.SetBaseOption("arib_ignore_background", style.ignoreBackground ? "1" : "0");
    ctl.SetBaseOption("arib_force_outline", style.forceOutline ? "1" : "0");
}

std::wstring FormatBuildTime(unsigned long long ft) {
    if (!ft) return L"";
    FILETIME f{(DWORD)(ft & 0xFFFFFFFFu), (DWORD)(ft >> 32)}, l;
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&f, &l) || !FileTimeToSystemTime(&l, &st)) return L"";
    wchar_t b[64];
    swprintf_s(b, L"built %04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}

namespace {
std::wstring StampOf(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return L"";
    unsigned long long t = ((unsigned long long)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
    size_t i = path.find_last_of(L"\\/");
    return (i == std::wstring::npos ? path : path.substr(i + 1)) + L" " + FormatBuildTime(t);
}
}

std::wstring ExeBuildStamp() {
    wchar_t p[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, p, (DWORD)std::size(p));
    return n ? StampOf(std::wstring(p, n)) : L"";
}
std::wstring LibffplayBuildStamp() { return StampOf(LibffplayPath()); }

LiveApplyResult ApplySettingsLive(wvp::PlaybackController& ctl, wvp::FilterManager& fm, const wvp::AppSettings& before,
                                  const wvp::AppSettings& after) {
    LiveApplyResult r;
    if (before.hwDecoder != after.hwDecoder) {
        // the plan encodes the decoder preference: a decoder change reopens the item (before capabilities it is remembered)
        wvp::PlanPrefs pp = wvp::PlanPrefsFor(after.hwDecoder);
        wvp::FilterChange fc = fm.SetPlanPreferences(pp.decoder, pp.forcedDecoder);
        auto st = ctl.Player().State();
        if (fc.reopen && ctl.Current() && (st == wvp::PlayerState::Playing || st == wvp::PlayerState::Paused)) {
            r.reopened = ctl.ReopenCurrentHere();
        } else {
            ctl.ApplyFilterChange(fc);
        }
        const auto& hw = sl::HwDecoderChoices()[(size_t)sl::HwDecoderIndex(after.hwDecoder)];
        r.toast = std::wstring(Tr(L"Hardware decoding: ", L"하드웨어 디코딩: ")) + hw.label + (r.reopened ? Tr(L"  — reopened", L"  — 다시 열림") : L"");
    }
    const bool captionDecoderChanged = before.showSuperimpose != after.showSuperimpose || before.aribFont != after.aribFont;
    const auto state = ctl.Player().State();
    if (captionDecoderChanged && ctl.Current() &&
        (state == wvp::PlayerState::Playing || state == wvp::PlayerState::Paused) && !r.reopened) {
        r.reopened = ctl.ReopenCurrentHere();
        if (r.reopened && r.toast.empty()) r.toast = Tr(L"Caption settings applied", L"자막 설정을 적용했습니다");
    }
    return r;
}

void LoadSettingsAtStartup(AppContext& ctx) {
    Services& svc = ctx.Svc();
    svc.settings = svc.settingsStore.Load();
    const wvp::AppSettings& s = svc.settings;
    // Populate the provider before any path can open the first media item.
    (void)svc.filters.Apply(s.filters);
    fx::Zoom().on = s.filters.customVideoFilter.find(fx::kZoomInstance) != std::string::npos;
    fx::Zoom().aspect = 16.0 / 9.0;   // refreshed from the actual video area on media prepare

    g_appliedAccent = s.accentColor;
    ctx.ApplyTheme(sl::ToThemeMode(s.theme), s.accentColor);
    RestoreWindow(ctx);

    // playback settings once the controller exists (ffp_create is asynchronous)
    g_appliedSettings = s;
    {   // planner preference (decoder); it only matters once capabilities arrive
        wvp::PlanPrefs pp = wvp::PlanPrefsFor(s.hwDecoder);
        svc.filters.SetPlanPreferences(pp.decoder, pp.forcedDecoder);
    }
    ApplyPlayback(ctx);   // no-op now; the controller does not exist yet
    if (!g_hooked) {
        g_hooked = true;
        ctx.OnPlayerReady.add([&ctx] { ApplyPlayback(ctx); });
    }
}

void FlushFilterSettings(AppContext& ctx) {
    fx::KillUiTimer(ctx.MainHwnd(), g_filterSaveTimer);
    g_filterSaveTimer = 0;
    auto& svc = ctx.Svc();
    svc.settings.filters = svc.filters.Settings();
    svc.settingsStore.Save(svc.settings);
}

void QueueFilterSettingsSave(AppContext& ctx) {
    auto& svc = ctx.Svc();
    wvp::FilterSettings current = svc.filters.Settings();
    if (svc.settings.filters == current) return;
    svc.settings.filters = std::move(current);
    fx::KillUiTimer(ctx.MainHwnd(), g_filterSaveTimer);
    g_filterSaveTimer = fx::SetUiTimer(ctx.MainHwnd(), 350, [&ctx] { FlushFilterSettings(ctx); }, false);
    if (!g_filterSaveTimer) svc.settingsStore.Save(svc.settings);
}

void SaveSettingsAtShutdown(AppContext& ctx) {
    Services& svc = ctx.Svc();
    wvp::AppSettings& s = svc.settings;
    SaveWindow(ctx);
    s.currentPlaylist = ctx.CurrentPlaylistName();
    if (auto* c = ctx.Controller()) {
        s.shuffle = c->Shuffle();
        s.repeat = static_cast<int>(c->Repeat());
    }
    FlushFilterSettings(ctx);
}

namespace {
HANDLE g_ccStop = nullptr;
std::thread g_ccThread;
}

void StartSystemCaptionSettingsWatch(AppContext& ctx) {
    if (g_ccThread.joinable()) return;
    HKEY key = nullptr;
    // Create-or-open: the key is missing until the user first changes a caption setting.
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\ClosedCaptioning", 0, nullptr, 0,
                        KEY_NOTIFY | KEY_READ, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        log::Event("settings", "caption settings watch: cannot open the ClosedCaptioning key", log::kWarning);
        return;
    }
    g_ccStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE changed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_ccStop || !changed) {
        if (g_ccStop) CloseHandle(g_ccStop);
        if (changed) CloseHandle(changed);
        g_ccStop = nullptr;
        RegCloseKey(key);
        return;
    }
    AppContext* c = &ctx;
    HANDLE stop = g_ccStop;
    g_ccThread = std::thread([c, key, changed, stop] {
        constexpr DWORD kFilter = REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET;
        for (;;) {
            if (RegNotifyChangeKeyValue(key, TRUE, kFilter, changed, TRUE) != ERROR_SUCCESS) break;
            HANDLE h[2] = {stop, changed};
            if (WaitForMultipleObjects(2, h, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) break;   // stop (or failure)
            // debounce: Settings writes one value per property; wait until it has been quiet for 250 ms
            for (;;) {
                if (RegNotifyChangeKeyValue(key, TRUE, kFilter, changed, TRUE) != ERROR_SUCCESS) break;
                DWORD w = WaitForMultipleObjects(2, h, FALSE, 250);
                if (w == WAIT_OBJECT_0) { RegCloseKey(key); CloseHandle(changed); return; }
                if (w != WAIT_OBJECT_0 + 1) break;   // quiet (timeout): apply
            }
            c->PostToUi([c] { OnSystemCaptionSettingsChanged(*c); });
            // the debounce loop left a notification armed; the outer loop re-arms (an extra RegNotify is harmless)
        }
        RegCloseKey(key);
        CloseHandle(changed);
    });
}

void StopSystemCaptionSettingsWatch() {
    if (!g_ccThread.joinable()) return;
    SetEvent(g_ccStop);
    g_ccThread.join();
    CloseHandle(g_ccStop);
    g_ccStop = nullptr;
}

void OnSystemCaptionSettingsChanged(AppContext& ctx) {
    const CaptionStyle current = ReadSystemCaptionStyle();
    if (!g_captionStyleKnown) {
        g_appliedCaptionStyle = current;
        g_captionStyleKnown = true;
        return;
    }
    if (current == g_appliedCaptionStyle) return;
    g_appliedCaptionStyle = current;

    auto* ctl = ctx.Controller();
    if (!ctl || !ctl->Current()) return;   // next open will use the new Windows style
    const auto state = ctl->Player().State();
    if (state != wvp::PlayerState::Playing && state != wvp::PlayerState::Paused) return;

    ApplyCaptionOpenOptions(*ctl, ctx.Svc().settings);  // persist the new style for subsequent media opens
    auto* player = ctx.Player();
    if (!player) return;

    const int rc = player->RefreshCaptionStyle(current.ignoreBackground, current.forceOutline);
    if (rc == 0) {
        ctx.ShowToast(Tr(L"Caption appearance updated", L"자막 모양을 업데이트했습니다"));
    } else {
        log::Event("settings", "Could not queue caption style refresh (error " + std::to_string(rc) + ")",
                   log::kWarning);
    }
}

void OnSettingsChanged(AppContext& ctx) {
    Services& svc = ctx.Svc();
    const wvp::AppSettings& s = svc.settings;
    svc.settingsStore.Save(s);
    pui::ThemeMode mode = sl::ToThemeMode(s.theme);
    if (mode != ctx.CurrentThemeMode() || s.accentColor != g_appliedAccent) {
        g_appliedAccent = s.accentColor;
        ctx.ApplyTheme(mode, s.accentColor);        // System mode keeps following WM_SETTINGCHANGE (the shell stores the mode)
    }
    if (auto* c = ctx.Controller()) {
        wvp::AppSettings before = g_appliedSettings;
        if (before.showCaptions != s.showCaptions)
            c->Player().SetSubtitleVisible(wvp::SubtitleLayer::Caption, s.showCaptions);
        if (before.showSuperimpose != s.showSuperimpose)
            c->Player().SetSubtitleVisible(wvp::SubtitleLayer::Superimpose, s.showSuperimpose);
        // live: decoder/planner changes re-open the current item at its position
        const int aSel = SelectedTrackIndex(ctx, wvp::StreamType::Audio);
        const int sSel = SelectedTrackIndex(ctx, wvp::StreamType::Subtitle);
        EnsureTrackRestoreCancellationHooks(ctx);
        LiveApplyResult r = ApplySettingsLive(*c, svc.filters, before, s);
        g_appliedSettings = s;
        if (r.reopened) ScheduleTrackRestore(ctx, aSel, sSel);
        if (!r.toast.empty()) ctx.ShowToast(r.toast);
    }
    log::ApplyDebugLogging(s.debugLogging, log::LogsDir(svc.settingsStore.Path()));
    ctx.ApplyWindowTopmost();
}

}
