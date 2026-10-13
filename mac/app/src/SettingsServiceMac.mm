// macOS implementation of src/App/features/SettingsService.h (the original SettingsService.cpp needs WinRT for the
// Windows caption style, the registry watch and the Win32 monitor API). Same load / save / live-apply logic as the
// original (LoadSettingsAtStartup, OnSettingsChanged + ApplySettingsLive, the debounced filter save, the track restore
// after a reopen); the Mac differences:
//  * window placement: HWND is the NSWindow (AppContext::MainHwnd); WindowPlacement holds the frame in screen points,
//    x / y = top-left corner measured from the top-left of the primary screen, dpi 72 (1 point = 1 unit);
//  * caption style: the macOS caption settings (MediaAccessibility, CaptionStyleMac.h) instead of Windows' Closed Caption
//    properties, for the text-subtitle style and the same arib_ignore_background / arib_force_outline; the watch observes
//    kMACaptionAppearanceSettingsChangedNotification instead of the ClosedCaptioning registry key (same 250 ms debounce,
//    same OnSystemCaptionSettingsChanged: new base options + FfPlayer::RefreshCaptionStyle on the playing item);
//  * the caption font is the registered CoreText family of the bundled TTF (CaptionFont.h), not a file: path;
//  * debug logging: features/Logging.cpp with the Mac folder (LoggingMac.h: ~/Library/Logs/aribplayer).
#import <AppKit/AppKit.h>

#include "features/SettingsService.h"

#include <algorithm>
#include <ctime>
#include <memory>
#include <sys/stat.h>
#include <utility>

#include "AppContext.h"
#include "CaptionFont.h"
#include "CaptionStyleMac.h"
#include "Localize.h"
#include "LoggingMac.h"
#include "features/Logging.h"
#include "features/FxModel.h"
#include "features/FxUtil.h"
#include "features/PlaybackExtras.h"
#include "features/SettingsLogic.h"
#include "wvp/win32/Utf.h"

namespace app {

namespace {
using CaptionStyle = mainview::CaptionStyleFlags;
mainview::CaptionAppearance g_captionAppearance{};
bool g_captionAppearanceKnown = false;

std::uint32_t g_appliedAccent = 0;
CaptionStyle g_appliedCaptionStyle{};
bool g_captionStyleKnown = false;
wvp::AppSettings g_appliedSettings;   // what the running playback was last configured with (diff base for ApplySettingsLive)
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

NSWindow* WindowOf(HWND h) { return (__bridge NSWindow*)(void*)h; }

/// Screen frames as sl::MonitorRect in the placement coordinates (points, top-left of the primary screen = 0,0).
std::vector<sl::MonitorRect> Monitors() {
    std::vector<sl::MonitorRect> v;
    const CGFloat top = NSMaxY(NSScreen.screens.firstObject.frame);
    for (NSScreen* s in NSScreen.screens) {
        const NSRect f = s.visibleFrame;
        v.push_back({(int)NSMinX(f), (int)(top - NSMaxY(f)), (int)NSMaxX(f), (int)(top - NSMinY(f))});
    }
    return v;
}

void ApplyPlayback(AppContext& ctx) {
    auto* c = ctx.Controller();
    if (!c) return;
    const wvp::AppSettings& s = ctx.Svc().settings;
    c->Player().SetSubtitleVisible(wvp::SubtitleLayer::Caption, s.showCaptions);
    c->Player().SetSubtitleVisible(wvp::SubtitleLayer::Superimpose, s.showSuperimpose);
    ApplySubtitleStyle(ctx);
    c->SetShuffle(s.shuffle);
    c->SetRepeat(static_cast<wvp::RepeatMode>(std::clamp(s.repeat, 0, 2)));
}

std::wstring Wide(NSString* s) { return s ? wvp::ToWide(s.UTF8String) : std::wstring(); }
}

std::optional<wvp::WindowPlacement> RestorablePlacement(wvp::WindowPlacement saved) {
    if (saved.valid && saved.dpi != 72) return std::nullopt;   // not a Mac placement
    return sl::ValidatePlacement(saved, Monitors());
}

std::optional<wvp::WindowPlacement> CaptureWindowPlacement(HWND hwnd) {
    NSWindow* w = WindowOf(hwnd);
    if (!w) return std::nullopt;
    if (w.styleMask & NSWindowStyleMaskFullScreen) return std::nullopt;
    const CGFloat top = NSMaxY(NSScreen.screens.firstObject.frame);
    const NSRect f = w.frame;
    wvp::WindowPlacement p;
    p.x = (int)std::lround(NSMinX(f));
    p.y = (int)std::lround(top - NSMaxY(f));
    p.w = (int)std::lround(NSWidth(f));
    p.h = (int)std::lround(NSHeight(f));
    p.maximized = w.zoomed;
    p.dpi = 72;
    p.valid = p.w > 0 && p.h > 0;
    if (!p.valid) return std::nullopt;
    return p;
}

namespace {
void RestoreWindow(AppContext& ctx) {
    NSWindow* w = WindowOf(ctx.MainHwnd());
    if (!w) return;
    auto v = RestorablePlacement(ctx.Svc().settings.window);
    if (!v) return;
    const CGFloat top = NSMaxY(NSScreen.screens.firstObject.frame);
    [w setFrame:NSMakeRect(v->x, top - v->y - v->h, v->w, v->h) display:NO];
    if (v->maximized && !w.zoomed) [w zoom:nil];
}

void SaveWindow(AppContext& ctx) {
    HWND hwnd = ctx.MainHwnd();
    if (!hwnd || ctx.IsFullscreen() || ctx.IsMiniPlayer()) return;   // keep the last normal geometry
    if (auto p = CaptureWindowPlacement(hwnd)) ctx.Svc().settings.window = *p;
}
}

std::wstring LibffplayPath() {
    NSString* fw = NSBundle.mainBundle.privateFrameworksPath;
    NSString* p = [fw stringByAppendingPathComponent:@"libffplay.dylib"];
    return [NSFileManager.defaultManager fileExistsAtPath:p] ? Wide(p) : std::wstring();
}

std::wstring BundledAribFontPath() { return wvp::ToWide(mainview::BundledAribFontPath()); }

void ApplySubtitleStyle(AppContext& ctx) {
    auto* player = ctx.Player();
    if (!player) return;
    if (!g_captionAppearanceKnown) {
        g_captionAppearance = mainview::ReadCaptionAppearance();
        g_captionAppearanceKnown = true;
    }
    player->SetSubtitleStyle(mainview::ResolveMacSubtitleStyle(ctx.Svc().settings.subtitle, g_captionAppearance));
}

void ApplyCaptionOpenOptions(wvp::PlaybackController& ctl, const wvp::AppSettings& settings) {
    g_captionAppearance = mainview::ReadCaptionAppearance();
    g_captionAppearanceKnown = true;
    CaptionStyle style = mainview::MapCaptionAppearance(g_captionAppearance);
    g_appliedCaptionStyle = style;
    g_captionStyleKnown = true;
    const mainview::AribFont& font = mainview::BundledCaptionFont();
    ctl.SetBaseOption("arib_font", font.option);
    NSString* fontsDir = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"fonts"];
    ctl.SetBaseOption("sub_fontsdir", wvp::ToUtf8(Wide(fontsDir)));
    ctl.SetBaseOption("superimpose", settings.showSuperimpose ? "1" : "0");
    ctl.SetBaseOption("arib_ignore_background", style.ignoreBackground ? "1" : "0");
    ctl.SetBaseOption("arib_force_outline", style.forceOutline ? "1" : "0");
}

std::wstring FormatBuildTime(unsigned long long ft) {
    if (!ft) return L"";
    const std::time_t t = (std::time_t)(ft / 10000000ULL - 11644473600ULL);   // FILETIME (100 ns since 1601) -> Unix
    std::tm lt{};
    localtime_r(&t, &lt);
    wchar_t b[64];
    std::swprintf(b, 64, L"built %04d-%02d-%02d %02d:%02d:%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
    return b;
}

namespace {
std::wstring StampOf(const std::wstring& path) {
    struct stat sb{};
    if (path.empty() || stat(wvp::ToUtf8(path).c_str(), &sb) != 0) return L"";
    const unsigned long long ft = ((unsigned long long)sb.st_mtimespec.tv_sec + 11644473600ULL) * 10000000ULL;
    size_t i = path.find_last_of(L"/");
    return (i == std::wstring::npos ? path : path.substr(i + 1)) + L" " + FormatBuildTime(ft);
}
}

std::wstring ExeBuildStamp() { return StampOf(Wide(NSBundle.mainBundle.executablePath)); }
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
    (void)svc.filters.Apply(s.filters);
    fx::Zoom().on = s.filters.customVideoFilter.find(fx::kZoomInstance) != std::string::npos;
    fx::Zoom().aspect = 16.0 / 9.0;   // refreshed from the actual video area on media prepare

    g_appliedAccent = s.accentColor;
    ctx.ApplyTheme(sl::ToThemeMode(s.theme), s.accentColor);
    RestoreWindow(ctx);

    g_appliedSettings = s;
    ApplyPlayback(ctx);
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

void StartSystemCaptionSettingsWatch(AppContext& ctx) {
    AppContext* c = &ctx;
    mainview::StartCaptionAppearanceWatch([c] { OnSystemCaptionSettingsChanged(*c); });   // already on the main queue
}

void StopSystemCaptionSettingsWatch() { mainview::StopCaptionAppearanceWatch(); }

/// Applies the macOS caption settings to text subtitles and the ARIB decoder.
void OnSystemCaptionSettingsChanged(AppContext& ctx) {
    const mainview::CaptionAppearance appearance = mainview::ReadCaptionAppearance();
    if (!g_captionAppearanceKnown || !(appearance == g_captionAppearance)) {
        g_captionAppearance = appearance;
        g_captionAppearanceKnown = true;
        if (ctx.Svc().settings.subtitle.useSystemCaptionStyle) ApplySubtitleStyle(ctx);
    }
    const CaptionStyle current = mainview::MapCaptionAppearance(appearance);
    log::Event("settings", "caption appearance: " + mainview::DescribeCaptionAppearance(appearance) + " -> ignore_background " +
                               (current.ignoreBackground ? "1" : "0") + ", force_outline " + (current.forceOutline ? "1" : "0"));
    if (!g_captionStyleKnown) {
        g_appliedCaptionStyle = current;
        g_captionStyleKnown = true;
        return;
    }
    if (current == g_appliedCaptionStyle) return;
    g_appliedCaptionStyle = current;

    auto* ctl = ctx.Controller();
    if (!ctl || !ctl->Current()) return;   // the next open uses the new style
    const auto state = ctl->Player().State();
    if (state != wvp::PlayerState::Playing && state != wvp::PlayerState::Paused) return;

    ApplyCaptionOpenOptions(*ctl, ctx.Svc().settings);   // persist the new style for subsequent media opens
    auto* player = ctx.Player();
    if (!player) return;

    const int rc = player->RefreshCaptionStyle(current.ignoreBackground, current.forceOutline);
    mainview::CaptionRefreshRecord& rec = mainview::LastCaptionRefresh();
    rec.count++;
    rec.flags = current;
    rec.rc = rc;
    if (rc == 0) {
        ctx.ShowToast(Tr(L"Caption appearance updated", L"자막 모양을 업데이트했습니다"));
    } else {
        log::Event("settings", "Could not queue caption style refresh (error " + std::to_string(rc) + ")", log::kWarning);
    }
}

void OnSettingsChanged(AppContext& ctx) {
    Services& svc = ctx.Svc();
    const wvp::AppSettings& s = svc.settings;
    svc.settingsStore.Save(s);
    pui::ThemeMode mode = sl::ToThemeMode(s.theme);
    if (mode != ctx.CurrentThemeMode() || s.accentColor != g_appliedAccent) {
        g_appliedAccent = s.accentColor;
        ctx.ApplyTheme(mode, s.accentColor);
    }
    if (auto* c = ctx.Controller()) {
        wvp::AppSettings before = g_appliedSettings;
        if (before.showCaptions != s.showCaptions)
            c->Player().SetSubtitleVisible(wvp::SubtitleLayer::Caption, s.showCaptions);
        if (before.showSuperimpose != s.showSuperimpose)
            c->Player().SetSubtitleVisible(wvp::SubtitleLayer::Superimpose, s.showSuperimpose);
        if (!(before.subtitle == s.subtitle)) ApplySubtitleStyle(ctx);
        TrackRestore saved = CaptureTrackRestore(ctx);
        EnsureTrackRestoreCancellationHooks(ctx);
        LiveApplyResult r = ApplySettingsLive(*c, before, s);
        g_appliedSettings = s;
        if (r.reopened) ScheduleTrackRestore(ctx, std::move(saved));
        if (!r.toast.empty()) ctx.ShowToast(r.toast);
    }
    mainview::logmac::ApplyDebugLogging(s.debugLogging, mainview::logmac::LogsDir(svc.settingsStore.Path()));
    ctx.ApplyWindowTopmost();
}

}
