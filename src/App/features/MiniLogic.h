#pragma once
// Pure (no Win32/D2D) logic of the mini player: size/placement clamping per monitor work area and DPI, default placement,
// the reduced bar layout, hit-testing (drag area vs. controls), enter/exit session state and the mini keyboard remap.
// Everything here is covered by --logictest (features/MiniLogicTests.cpp).
#include <algorithm>
#include <cmath>
#include <vector>

#include "Commands.h"
#include "ShellLogic.h"
#include "pui/Geometry.h"
#include "wvp/AppSettings.h"

namespace app {

constexpr float kMiniDefaultWDip = 400, kMiniDefaultHDip = 225;
constexpr float kMiniMinWDip = 320, kMiniMinHDip = 180;
constexpr float kMiniMarginDip = 16;
constexpr float kMiniBarDip = 56;

struct PxRect {
    int l = 0, t = 0, r = 0, b = 0;
    int w() const { return r - l; }
    int h() const { return b - t; }
    bool Empty() const { return r <= l || b <= t; }
    bool operator==(const PxRect& o) const { return l == o.l && t == o.t && r == o.r && b == o.b; }
};
struct PxSize { int w = 0, h = 0; };

long long PxIntersectArea(const PxRect& a, const PxRect& b);

PxSize MiniMinSizePx(float scale);
/// Default size (400x225 DIP) shrunk to fit a work area (never below the minimum unless the work area itself is smaller).
PxSize MiniDefaultSizePx(const PxRect& work, float scale);
/// Default placement: bottom-right corner of `work`, 16 DIP margin.
PxRect MiniDefaultRect(const PxRect& work, float scale);
PxRect MiniClampRect(PxRect want, const PxRect& work, float scale);
/// Index of the work area that overlaps `rect` the most; -1 when it overlaps none.
int MiniPickWorkArea(const PxRect& rect, const std::vector<PxRect>& works);
/// Placement to use on entering: the saved settings.miniWindow (x,y in device px, w,h in DIP) clamped to the monitor
/// it overlaps, otherwise the default placement on `currentWork` (the work area of the monitor the window is on).
PxRect MiniResolvePlacement(const wvp::WindowPlacement& saved, const std::vector<PxRect>& works, const PxRect& currentWork, float scale);
/// Value stored into settings.miniWindow (size converted to DIP so it survives DPI changes).
wvp::WindowPlacement MiniMakePlacement(const PxRect& windowRect, float scale);

/// Display aspect (w/h) of the current media for the mini player; 16:9 when unknown or implausible (audio only, 0x0).
double MiniAspectForMedia(int displayW, int displayH);
/// WM_SIZING for the mini player (client area == window rect, the video fills it): returns the proposed rect adjusted so
/// that w/h == aspect (to within a pixel), anchored on the edge/corner opposite to the dragged one. `edge` is the WMSZ_*
/// value (1 left, 2 right, 3 top, 4 top-left, 5 top-right, 6 bottom, 7 bottom-left, 8 bottom-right). Left/right edges are
/// width-driven (height follows at the bottom), top/bottom edges height-driven (width follows at the right), corners are
/// width-driven. The result never goes below `minSz` on either axis (the other axis grows to keep the aspect).
PxRect MiniConstrainSizing(PxRect proposed, int edge, double aspect, PxSize minSz);

/// Bar-local rects in DIP snapped to whole device pixels. Hidden elements have an empty rect and show* == false.
/// Hiding order when narrow: time, previous/next, play. The seek slider and the exit button stay as long as they fit.
struct MiniBarLayout {
    float scale = 1, widthDip = 0, heightDip = 0;
    pui::RectF slider, prev, play, next, exit, time;
    bool showTime = false, showPrev = false, showPlay = false, showNext = false, showExit = false;
    /// Every visible rect is non-empty and inside [0,widthDip]x[0,heightDip]; hidden ones are empty.
    bool Sane() const;
};
MiniBarLayout ComputeMiniBarLayout(int widthPx, float scale);
inline int MiniBarHeightPx(float scale) { return std::max(1, (int)std::lround(kMiniBarDip * (scale > 0 ? scale : 1.f))); }

/// The reduced bar overlays the bottom of the video and auto-hides like Now playing.
ShellLayout ComputeMiniShellLayout(int cwPx, int chPx, float scale);

enum class MiniZone { None, Caption, Control };
/// What is under a client point (device px). Control = a visible bar control while the bar is hittable (the shell then
/// returns HTCLIENT); Caption = everything else in the client (the shell returns HTCAPTION so the window can be dragged;
/// the resize borders are tested by the shell before this). None = outside the client.
MiniZone ClassifyMiniPoint(int xPx, int yPx, int cwPx, int chPx, float scale, bool barHittable);

struct MiniSession {
    bool active = false;
    PxRect normalRect;                  // restored (non-maximized) window rect before entering
    bool normalMaximized = false;
    bool normalTopmost = false;

    void Begin(const PxRect& rect, bool maximized, bool topmost) {
        active = true; normalRect = rect; normalMaximized = maximized; normalTopmost = topmost;
    }
    void End() { active = false; }
};
/// In the mini player Esc leaves it and the control panel command is dropped (the panel does not fit); every other command is unchanged.
CommandId MiniRemapCommand(CommandId c, bool mini);

}
