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
namespace cc = winrt::Windows::Media::ClosedCaptioning;
static_assert((int)cc::ClosedCaptionStyle::Default == 0 && (int)cc::ClosedCaptionStyle::SmallCapitals == 7);
static_assert((int)cc::ClosedCaptionSize::Default == 0 && (int)cc::ClosedCaptionSize::TwoHundredPercent == 4);
static_assert((int)cc::ClosedCaptionColor::Default == 0 && (int)cc::ClosedCaptionColor::White == 1 &&
              (int)cc::ClosedCaptionColor::Black == 2 && (int)cc::ClosedCaptionColor::Cyan == 8);
static_assert((int)cc::ClosedCaptionOpacity::Default == 0 && (int)cc::ClosedCaptionOpacity::OneHundredPercent == 1 &&
              (int)cc::ClosedCaptionOpacity::ZeroPercent == 4);
static_assert((int)cc::ClosedCaptionEdgeEffect::Default == 0 && (int)cc::ClosedCaptionEdgeEffect::None == 1 &&
              (int)cc::ClosedCaptionEdgeEffect::Uniform == 4 && (int)cc::ClosedCaptionEdgeEffect::DropShadow == 5);

wvp::WindowsCaptionProperties ReadWindowsCaptionProperties() {
    wvp::WindowsCaptionProperties p;
    try {
        using P = cc::ClosedCaptionProperties;
        p.fontStyle = (int)P::FontStyle();
        p.fontSize = (int)P::FontSize();
        p.fontColor = (int)P::FontColor();
        p.fontOpacity = (int)P::FontOpacity();
        p.fontEffect = (int)P::FontEffect();
        p.backgroundColor = (int)P::BackgroundColor();
        p.backgroundOpacity = (int)P::BackgroundOpacity();
    } catch (...) {
        p = {};   // Windows may reject the property read: everything "Default"
    }
    return p;
}

struct CaptionStyle {
    bool ignoreBackground = false;
    bool forceOutline = false;
    bool operator==(const CaptionStyle&) const = default;
};

CaptionStyle AribCaptionStyle(const wvp::WindowsCaptionProperties& p) {
    // ARIB draws one box per character cell, which is the Windows text background; the caption
    // window (region) has no ARIB counterpart, so only a transparent text background drops the box.
    CaptionStyle style;
    style.ignoreBackground = p.backgroundOpacity == (int)cc::ClosedCaptionOpacity::ZeroPercent;
    style.forceOutline = p.fontEffect == (int)cc::ClosedCaptionEdgeEffect::Uniform ||
                         p.fontEffect == (int)cc::ClosedCaptionEdgeEffect::DropShadow;
    return style;
}

std::uint32_t g_appliedAccent = 0;
wvp::AppSettings g_appliedSettings;   // what the running playback was last configured with (diff base for ApplySettingsLive)
wvp::WindowsCaptionProperties g_windowsCaption{};
bool g_windowsCaptionKnown = false;
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

struct TrackRestore {
    int arib = -1;                 // ARIB audio preference, or -1
    fx::TrackRef audio, subtitle;
};

TrackRestore CaptureTrackRestore(AppContext& ctx) {
    TrackRestore r;
    auto* player = ctx.Player();
    auto mi = player ? player->GetMediaInfo() : std::nullopt;
    if (!mi) return r;
    if (mi->AribAudio.Present) r.arib = player->AudioPreference();
    else r.audio = fx::CaptureTrackRef(mi->Of(wvp::StreamType::Audio), false);
    r.subtitle = fx::CaptureTrackRef(mi->Of(wvp::StreamType::Subtitle), true);
    return r;
}

void ScheduleTrackRestore(AppContext& ctx, TrackRestore saved) {
    CancelPendingTrackRestore(ctx);
    EnsureTrackRestoreCancellationHooks(ctx);
    auto tok = std::make_shared<PreparedToken>(0);
    *tok = ctx.OnMediaPrepared.add([&ctx, tok, saved](const wvp::MediaPreparedInfo&) {
        ctx.OnMediaPrepared.remove(*tok);
        if (g_pendingRestoreContext == &ctx && g_pendingRestoreToken == *tok) {
            g_pendingRestoreContext = nullptr;
            g_pendingRestoreToken = 0;
        }
        auto* player = ctx.Player();
        auto mi = player ? player->GetMediaInfo() : std::nullopt;
        if (!mi) return;
        if (saved.arib > 0) SelectTrack(ctx, wvp::StreamType::Audio, saved.arib);
        if (auto a = fx::ResolveTrackRef(mi->Of(wvp::StreamType::Audio), saved.audio)) SelectTrack(ctx, wvp::StreamType::Audio, *a);
        if (auto s = fx::ResolveTrackRef(mi->Of(wvp::StreamType::Subtitle), saved.subtitle)) SelectTrack(ctx, wvp::StreamType::Subtitle, *s);
    });
    g_pendingRestoreContext = &ctx;
    g_pendingRestoreToken = *tok;
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
    ApplySubtitleStyle(ctx);
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

void ApplySubtitleStyle(AppContext& ctx) {
    auto* player = ctx.Player();
    if (!player) return;
    if (!g_windowsCaptionKnown) {
        g_windowsCaption = ReadWindowsCaptionProperties();
        g_windowsCaptionKnown = true;
    }
    player->SetSubtitleStyle(wvp::ResolveSubtitleStyle(ctx.Svc().settings.subtitle, g_windowsCaption));
}

void ApplyCaptionOpenOptions(wvp::PlaybackController& ctl, const wvp::AppSettings& settings) {
    g_windowsCaption = ReadWindowsCaptionProperties();   // the text subtitle style is sent with it at the next Prepared
    g_windowsCaptionKnown = true;
    CaptionStyle style = AribCaptionStyle(g_windowsCaption);
    g_appliedCaptionStyle = style;
    g_captionStyleKnown = true;
    ctl.SetBaseOption("arib_font", wvp::AribFontOption(wvp::ToUtf8(BundledAribFontPath())));
    std::wstring fontsDir = ExeDir() + L"\\fonts";
    std::replace(fontsDir.begin(), fontsDir.end(), L'\\', L'/');
    ctl.SetBaseOption("sub_fontsdir", wvp::ToUtf8(fontsDir));
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

LiveApplyResult ApplySettingsLive(wvp::PlaybackController& ctl, const wvp::AppSettings& before, const wvp::AppSettings& after) {
    LiveApplyResult r;
    const bool captionDecoderChanged = before.showSuperimpose != after.showSuperimpose;
    const auto state = ctl.Player().State();
    if (captionDecoderChanged && ctl.Current() && (state == wvp::PlayerState::Playing || state == wvp::PlayerState::Paused)) {
        r.reopened = ctl.ReopenCurrentHere();
        if (r.reopened) r.toast = Tr(L"Caption settings applied", L"자막 설정을 적용했습니다");
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
    const wvp::WindowsCaptionProperties props = ReadWindowsCaptionProperties();
    if (!g_windowsCaptionKnown || !(props == g_windowsCaption)) {
        g_windowsCaption = props;
        g_windowsCaptionKnown = true;
        if (ctx.Svc().settings.subtitle.useSystemCaptionStyle) ApplySubtitleStyle(ctx);
    }
    const CaptionStyle current = AribCaptionStyle(props);
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
        if (!(before.subtitle == s.subtitle)) ApplySubtitleStyle(ctx);
        // live: caption decoder changes re-open the current item at its position
        TrackRestore saved = CaptureTrackRestore(ctx);
        EnsureTrackRestoreCancellationHooks(ctx);
        LiveApplyResult r = ApplySettingsLive(*c, before, s);
        g_appliedSettings = s;
        if (r.reopened) ScheduleTrackRestore(ctx, std::move(saved));
        if (!r.toast.empty()) ctx.ShowToast(r.toast);
    }
    log::ApplyDebugLogging(s.debugLogging, log::LogsDir(svc.settingsStore.Path()));
    ctx.ApplyWindowTopmost();
}

}
