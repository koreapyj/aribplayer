#include "features/MiniPlayer.h"

#include <windows.h>

#include <dwmapi.h>

#include <vector>

#include "AppContext.h"
#include "features/MiniLogic.h"

namespace app {

namespace {
MiniSession g_session;
LONG_PTR g_normalStyle = 0;

constexpr DWORD kDwmaCornerPreference = 33;   // DWMWA_WINDOW_CORNER_PREFERENCE
constexpr int kCornerDefault = 0, kCornerRound = 2;

PxRect ToPx(const RECT& r) { return {r.left, r.top, r.right, r.bottom}; }

std::vector<PxRect> g_monitorWorks;
BOOL CALLBACK EnumMon(HMONITOR m, HDC, LPRECT, LPARAM) {
    MONITORINFO mi{sizeof mi};
    if (GetMonitorInfoW(m, &mi)) g_monitorWorks.push_back(ToPx(mi.rcWork));
    return TRUE;
}

void SetCorners(HWND h, int pref) { DwmSetWindowAttribute(h, kDwmaCornerPreference, &pref, sizeof pref); }
}

void EnterMiniPlayer(AppContext& ctx) {
    HWND h = ctx.MainHwnd();
    if (!h || g_session.active) return;
    const bool visible = IsWindowVisible(h) != FALSE;

    WINDOWPLACEMENT wp{sizeof wp};
    GetWindowPlacement(h, &wp);
    const bool maximized = wp.showCmd == SW_SHOWMAXIMIZED;
    PxRect normal = ToPx(wp.rcNormalPosition);
    if (!maximized) { RECT r; GetWindowRect(h, &r); normal = ToPx(r); }
    const bool topmost = (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    g_normalStyle = GetWindowLongPtrW(h, GWL_STYLE);
    g_session.Begin(normal, maximized, topmost);

    float scale = ctx.Scale();
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
    g_monitorWorks.clear();
    EnumDisplayMonitors(nullptr, nullptr, EnumMon, 0);
    PxRect target = MiniResolvePlacement(ctx.Svc().settings.miniWindow, g_monitorWorks, ToPx(mi.rcWork), scale);

    if (maximized && visible) ShowWindow(h, SW_RESTORE);   // never on a hidden window: that would show it
    LONG_PTR style = g_normalStyle;
    style &= ~(LONG_PTR)(WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
    style |= WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN;
    SetWindowLongPtrW(h, GWL_STYLE, style);
    SetCorners(h, kCornerRound);
    SetWindowPos(h, HWND_TOPMOST, target.l, target.t, target.w(), target.h(), SWP_FRAMECHANGED | SWP_NOACTIVATE);
}

void ExitMiniPlayer(AppContext& ctx) {
    HWND h = ctx.MainHwnd();
    if (!h || !g_session.active) return;
    MiniSession s = g_session;
    g_session.End();

    RECT cur{};
    GetWindowRect(h, &cur);
    ctx.Svc().settings.miniWindow = MiniMakePlacement(ToPx(cur), ctx.Scale());

    LONG_PTR style = g_normalStyle;
    style = (style & ~(LONG_PTR)WS_VISIBLE) | (GetWindowLongPtrW(h, GWL_STYLE) & WS_VISIBLE);
    SetWindowLongPtrW(h, GWL_STYLE, style);
    SetCorners(h, kCornerDefault);
    SetWindowPos(h, s.normalTopmost ? HWND_TOPMOST : HWND_NOTOPMOST, s.normalRect.l, s.normalRect.t, s.normalRect.w(), s.normalRect.h(),
                 SWP_FRAMECHANGED | SWP_NOACTIVATE);
    if (s.normalMaximized && IsWindowVisible(h)) ShowWindow(h, SW_MAXIMIZE);
}

}
