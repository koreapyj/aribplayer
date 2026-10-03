#pragma once
// Pure window-size math for the 1-5 keys (window size from the video) and the video click rules. No Win32 calls; tested
// by --logictest (LogicTests.cpp).
#include <algorithm>
#include <cmath>
#include <optional>

#include "Commands.h"
#include "features/MiniLogic.h"   // PxRect / PxSize

namespace app {

/// Display size of a video stream: SAR-corrected (the width is scaled, the height kept), rotated by 90/270 when
/// `swapAxes`. nullopt when there is no picture (no video stream / zero size).
inline std::optional<PxSize> VideoDisplaySize(int width, int height, int sarNum, int sarDen, bool swapAxes) {
    if (width <= 0 || height <= 0) return std::nullopt;
    double w = width;
    if (sarNum > 0 && sarDen > 0 && sarNum != sarDen) w = width * (double)sarNum / sarDen;
    PxSize s{(int)std::lround(w), height};
    if (swapAxes) std::swap(s.w, s.h);
    return s;
}

/// Minimum size of the normal (not mini) window in DIP: WM_GETMINMAXINFO and the 1-5 keys both use it (one source of truth).
constexpr int kMinWindowWidthDip = 500, kMinWindowHeightDip = 374;

/// Nominal video size the 1-5 keys use when no video dimensions are known (nothing open, or audio only).
constexpr int kFallbackVideoW = 1024, kFallbackVideoH = 768;

struct VideoWindowIn {
    PxSize video;              // display size of the video (VideoDisplaySize), physical px
    double scale = 1.0;        // 0.5 / 1 / 1.5 / 2; <= 0 = the largest window that fits the work area ("Max")
    int extraW = 0, extraH = 0;  // window size minus the video rect size (frame + title strip), physical px
    PxRect window;             // the current (restored) window rect
    PxRect work;               // work area of the window's monitor (monitor minus taskbar)
    // Invisible resize borders of the window rect (DWM extended frame bounds); the visible frame must fit the work area.
    int invL = 0, invT = 0, invR = 0, invB = 0;
    int minW = 0, minH = 0;    // minimum window size, physical px (kMinWindow*Dip x scale, as WM_GETMINMAXINFO)
};

/// Client width of the windowed player view whose video rect is exactly `videoW` px wide (UI scale `uiScale` = DPI / 96):
/// the 8 DIP gutters, and - when the playlist panel is wanted and `videoW` reaches the docked minimum - the panel (its stored
/// width) and the gap. Solved against ComputeShellLayout itself (its pixel rounding) so the two never drift apart.
inline int ClientWidthForVideo(int videoW, float uiScale, bool panelWanted, float panelWidthDip) {
    const float s = uiScale > 0 ? uiScale : 1.f;
    auto px = [&](float dip) { return (int)std::lround(dip * s); };
    const int gut = px(kBodyGutterDip);
    int w = videoW + 2 * gut;
    if (panelWanted && videoW >= px(kVideoMinDockDip)) w += px(kBodyGapDip) + std::max(px(panelWidthDip), px(kPanelMinDip));
    for (int i = 0; i < 4; ++i) {   // rounding: nudge until the layout's video width matches
        ShellLayoutIn in;
        in.cwPx = w; in.chPx = 1000; in.scale = s; in.mode = ShellMode::NowPlaying; in.panelWanted = panelWanted; in.panelWidthDip = panelWidthDip;
        const int d = videoW - ComputeShellLayout(in).video.w();
        if (d == 0) break;
        w += d;
    }
    return w;
}

/// Client height of the windowed player view whose video rect is `videoH` px tall: title strip + video + docked bar.
inline int ClientHeightForVideo(int videoH, float uiScale) {
    const float s = uiScale > 0 ? uiScale : 1.f;
    return videoH + (int)std::lround(kStripDip * s) + (int)std::lround(kDockDip * s);
}

/// New window rect, best effort: the window whose video rect is video x scale, then each axis clamped INDEPENDENTLY so the
/// visible frame fits the work area (no aspect-keeping shrink of the other axis: the video letterboxes inside); "Max" = the
/// largest window that fits on both axes. Never below the minimum size. Centred on the current window, then moved inside
/// the work area (same monitor).
inline PxRect VideoWindowRect(const VideoWindowIn& in) {
    const int maxW = in.work.w() + in.invL + in.invR, maxH = in.work.h() + in.invT + in.invB;   // visible frame == work area
    int ww = maxW, wh = maxH;
    if (in.scale > 0) {
        ww = std::min(maxW, (int)std::lround(in.video.w * in.scale) + in.extraW);
        wh = std::min(maxH, (int)std::lround(in.video.h * in.scale) + in.extraH);
    }
    ww = std::max(ww, in.minW);
    wh = std::max(wh, in.minH);
    int l = (in.window.l + in.window.r) / 2 - ww / 2, t = (in.window.t + in.window.b) / 2 - wh / 2;
    int visL = l + in.invL, visT = t + in.invT, visR = l + ww - in.invR, visB = t + wh - in.invB;
    if (visR > in.work.r) l -= visR - in.work.r;
    if (visB > in.work.b) t -= visB - in.work.b;
    visL = l + in.invL; visT = t + in.invT;
    if (visL < in.work.l) l += in.work.l - visL;
    if (visT < in.work.t) t += in.work.t - visT;
    return {l, t, l + ww, t + wh};
}

/// Always on top guard (WM_WINDOWPOSCHANGING): while topmost is wanted, foreign z-order changes that would move the
/// main window out of the topmost band are rewritten to HWND_TOPMOST. An owned dialog (or another app window) must be
/// allowed to remain above the player. The message carries a resolved HWND, never HWND_NOTOPMOST / HWND_TOP; 0x0004 =
/// SWP_NOZORDER.
inline bool ForceTopmostInsertAfter(bool topmostWanted, unsigned swpFlags, bool insertAfterIsTopmost,
                                    bool insertAfterBelongsToOurProcess) {
    return topmostWanted && !(swpFlags & 0x0004u) && !insertAfterIsTopmost && !insertAfterBelongsToOurProcess;
}

inline CommandId VideoClickCommand(int clickCount) { return clickCount == 2 ? CommandId::PlayPause : CommandId::None; }

/// Space in a video mode (Now playing, full screen, mini player) is play/pause on key DOWN, before the focused widget sees it:
/// a clicked bar button keeps the focus, and a focused pui button only acts when Space is released (the pause came as late
/// as the key was held) while the bar's other buttons would run their own command instead. Not while a popup / menu is open,
/// nor while a widget that types or navigates with Space has focus (text, list, combo box).
inline bool SpaceTogglesPlayback(bool popupOpen, bool focusUsesSpace) {
    return !popupOpen && !focusUsesSpace;
}

}
