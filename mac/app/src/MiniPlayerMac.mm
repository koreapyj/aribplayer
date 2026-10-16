// macOS implementation of src/App/features/MiniPlayer.h (the original restyles the HWND with Win32 / DWM calls): the
// player window becomes the mini player and back. Placement through the pure features/MiniLogic (MiniResolvePlacement /
// MiniMakePlacement / MiniMinSizePx), in screen points (scale 1: 1 DIP = 1 point) with a top-left origin; the shell
// side (ComputeMiniShellLayout, the MiniBar) is MainView's. The window keeps its (transparent) title bar so it stays
// resizable and rounded; the traffic lights are hidden, the level floats above other windows, the aspect is locked to
// the media (MainView::MediaAspect = MiniAspectForMedia of the last PREPARED) like the Windows WM_SIZING handler.
#import <AppKit/AppKit.h>

#include "features/MiniPlayer.h"

#include <vector>

#include "AppContext.h"
#include "MainView.h"
#include "features/MiniLogic.h"

namespace app {

namespace {
MiniSession g_session;
NSSize g_normalMinSize;
NSWindowLevel g_normalLevel = NSNormalWindowLevel;

CGFloat PrimaryTop() { return NSMaxY(NSScreen.screens.firstObject.frame); }
PxRect ToPx(NSRect r) {
    const CGFloat top = PrimaryTop();
    return {(int)std::lround(NSMinX(r)), (int)std::lround(top - NSMaxY(r)), (int)std::lround(NSMaxX(r)), (int)std::lround(top - NSMinY(r))};
}
NSRect FromPx(const PxRect& r) { return NSMakeRect(r.l, PrimaryTop() - r.b, r.w(), r.h()); }

void SetTrafficLightsHidden(NSWindow* w, bool hidden) {
    for (NSWindowButton b : {NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton})
        [w standardWindowButton:b].hidden = hidden;
}
}

void EnterMiniPlayer(AppContext& ctx) {
    NSWindow* w = (__bridge NSWindow*)(void*)ctx.MainHwnd();
    if (!w || g_session.active) return;
    g_session.Begin(ToPx(w.frame), w.zoomed, w.level != NSNormalWindowLevel);
    g_normalMinSize = w.minSize;
    g_normalLevel = w.level;

    std::vector<PxRect> works;
    for (NSScreen* s in NSScreen.screens) works.push_back(ToPx(s.visibleFrame));
    const PxRect current = ToPx((w.screen ?: NSScreen.mainScreen).visibleFrame);
    const PxRect target = MiniResolvePlacement(ctx.Svc().settings.miniWindow, works, current, 1.f);
    const PxSize mn = MiniMinSizePx(1.f);
    w.minSize = NSMakeSize(mn.w, mn.h);
    SetTrafficLightsHidden(w, true);
    w.level = NSFloatingWindowLevel;
    [w setFrame:FromPx(target) display:YES animate:NO];
    [w orderFront:nil];   // SetWindowPos(HWND_TOPMOST, SWP_NOACTIVATE): the top of the floating band, not activated
    // App's WM_SIZING (MiniConstrainSizing with mediaAspect_): resizing keeps the playing media's aspect; MainView
    // re-applies it when another item is prepared while the mini player is up.
    const auto* mv = dynamic_cast<const mainview::MainView*>(&ctx);
    const double aspect = mv ? mv->MediaAspect() : 16.0 / 9.0;
    w.contentAspectRatio = NSMakeSize(aspect * 1000, 1000);
}

void ExitMiniPlayer(AppContext& ctx) {
    NSWindow* w = (__bridge NSWindow*)(void*)ctx.MainHwnd();
    if (!w || !g_session.active) return;
    MiniSession s = g_session;
    g_session.End();
    ctx.Svc().settings.miniWindow = MiniMakePlacement(ToPx(w.frame), 1.f);
    w.resizeIncrements = NSMakeSize(1, 1);   // clears contentAspectRatio
    w.minSize = g_normalMinSize;
    w.level = g_normalLevel;
    SetTrafficLightsHidden(w, false);
    [w setFrame:FromPx(s.normalRect) display:YES animate:NO];
    if (s.normalMaximized && !w.zoomed) [w zoom:nil];
}

}
