#pragma once
// Everything here is covered by --logictest.
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "BarLogic.h"
#include "pui/Geometry.h"

namespace app {

/// The player view is the only view (Settings is its own window, shell/SettingsWindow); full screen and the mini player replace its layout.
enum class ShellMode { NowPlaying, FullScreen, MiniPlayer };

/// PlaylistPanel: the docked playlist panel (windowed player view) - files are added to the viewed playlist.
enum class DropRoute { Rejected, VideoView, PlaylistPanel };
/// One routing rule for OLE DragOver and Drop. Popup areas always reject; the auto-hide title and bar are treated as
/// part of the video view.
inline DropRoute RouteDrop(bool pointInVideo, bool pointOverPopup, bool pointOverBar, bool pointOverTitle) {
    if (pointOverPopup) return DropRoute::Rejected;
    return pointInVideo || pointOverBar || pointOverTitle ? DropRoute::VideoView : DropRoute::Rejected;
}

/// Completion does not navigate away: only an existing file needs its auto-hidden controls restored.
inline bool RestoreVideoControlsOnCompletion(bool hasFile) { return hasFile; }

struct ModeState {
    bool fullscreen = false, mini = false;
    ShellMode Mode() const {
        if (mini) return ShellMode::MiniPlayer;
        if (fullscreen) return ShellMode::FullScreen;
        return ShellMode::NowPlaying;
    }
    bool operator==(const ModeState& o) const { return fullscreen == o.fullscreen && mini == o.mini; }
};
/// Full screen leaves the mini player.
inline ModeState WithFullscreen(ModeState s, bool v) {
    s.fullscreen = v;
    if (v) s.mini = false;
    return s;
}
/// The mini player leaves full screen. Leaving it returns to the windowed player.
inline ModeState WithMini(ModeState s, bool v) {
    s.mini = v;
    if (v) s.fullscreen = false;
    return s;
}

struct RectPx {
    int l = 0, t = 0, r = 0, b = 0;
    int w() const { return r - l; }
    int h() const { return b - t; }
    bool Empty() const { return r <= l || b <= t; }
};

struct ShellLayoutIn {
    int cwPx = 1280, chPx = 720;
    float scale = 1.f;
    ShellMode mode = ShellMode::NowPlaying;
    bool panelWanted = false;     // playlist panel setting (windowed player view only)
    float panelWidthDip = 320;    // user panel width (AppSettings::playlistPanelWidthDip); clamped to what the row allows
};

// Windowed player body row (DIP): 8 gutters, 8 gap. The panel has a user width (default 320, at least kPanelMinDip) and the
// video column takes the rest, so resizing the window only changes the video. The wanted panel stays docked while even the
// minimum panel leaves the video kVideoMinDockDip; narrower than that it opens over the video instead (panelOverlay). The gap
// between video and panel is the splitter (the only resize handle).
constexpr float kBodyGutterDip = 8, kBodyGapDip = 8, kVideoMinDockDip = 240, kPanelMinDip = 240, kPanelDefaultDip = 320;

/// All DIP rects are snapped to whole device pixels (px / scale). Empty rect = element not shown in that mode.
struct ShellLayout {
    pui::RectF title, bar;
    pui::RectF panel;               // playlist panel beside the video (windowed player view, wide enough); empty otherwise
    pui::RectF contentLayer;        // region of the content surface (the title strip); empty in full screen
    RectPx video;                   // device pixels, client coords (video child window); Empty = hidden
    RectPx contentPx, barPx, panelPx;
    RectPx splitterPx;              // the gap between video column and docked panel (resize handle, panel column height); Empty = none
    bool panelOverlay = false;      // panel wanted but the row is too narrow to dock it: it opens over the video instead
    bool barOverlaysVideo = false;  // auto-hide bar (full screen, mini); the windowed player docks the bar under the video
    bool titleSlides = false;       // the title strip slides/fades with the bar
    float widthDip = 0, heightDip = 0;
};

inline ShellLayout ComputeShellLayout(const ShellLayoutIn& in) {
    ShellLayout o;
    float s = in.scale > 0 ? in.scale : 1.f;
    auto px = [&](float dip) { return (int)std::lround(dip * s); };
    auto dip = [&](int p) { return (float)p / s; };
    int W = std::max(1, in.cwPx), H = std::max(1, in.chPx);
    int strip = px(kStripDip), bar = px(kDockDip);
    o.widthDip = dip(W); o.heightDip = dip(H);
    auto rect = [&](int l, int t, int r, int b) { return pui::RectF{dip(l), dip(t), dip(r), dip(b)}; };
    int barTop = std::max(0, H - bar);
    o.barPx = {0, barTop, W, H};
    o.bar = rect(0, barTop, W, H);
    switch (in.mode) {
    case ShellMode::NowPlaying: {
        // title strip | body row: video column (video + docked bar) [+ panel column spanning the full body height]
        o.title = rect(0, 0, W, strip);
        int rowB = std::max(strip, barTop);
        int gut = px(kBodyGutterDip), gap = px(kBodyGapDip);
        int rowL = std::min(gut, W / 2), rowR = std::max(rowL + 1, W - gut);
        int rowW = rowR - rowL;
        const int minP = px(kPanelMinDip), minV = px(kVideoMinDockDip);
        const bool fits = in.panelWanted && rowW - gap - minP >= minV;
        const int pw = fits ? std::clamp(px(in.panelWidthDip), minP, rowW - gap - minV) : 0;
        const int vw = fits ? rowW - gap - pw : 0;
        o.panelOverlay = in.panelWanted && !fits;
        if (fits) {
            o.video = {rowL, strip, rowL + vw, rowB};
            // The dock sits under the video only; the panel spans the whole body height.
            o.barPx = {rowL, barTop, rowL + vw, H};
            o.bar = rect(o.barPx.l, o.barPx.t, o.barPx.r, o.barPx.b);
            o.panelPx = {rowL + vw + gap, strip, rowR, std::max(strip + 1, H - gut)};
            o.splitterPx = {rowL + vw, strip, rowL + vw + gap, o.panelPx.b};
            o.panel = rect(o.panelPx.l, o.panelPx.t, o.panelPx.r, o.panelPx.b);
        } else {
            o.video = {rowL, strip, rowR, rowB};
        }
        o.contentPx = {0, 0, W, strip};
        o.contentLayer = o.title;
        break;
    }
    case ShellMode::MiniPlayer:
        o.title = rect(0, 0, W, strip);
        o.video = {0, strip, W, H};
        o.contentPx = {0, 0, W, strip};
        o.contentLayer = o.title;
        o.barOverlaysVideo = true;
        o.titleSlides = false;
        break;
    case ShellMode::FullScreen:
        o.title = rect(0, 0, W, strip);
        o.video = {0, 0, W, H};
        o.barOverlaysVideo = true;
        o.titleSlides = true;
        break;
    }
    return o;
}

enum class TitleZone { None, MainMenu, Drag, Pin, Min, Max, Close };

constexpr float kTitleButtonDip = 46;      // pin / minimize / maximize / close, full strip height
constexpr float kTitlePadDip = 8;          // gap between the main menu button and the file name

struct TitleLayoutIn {
    float widthDip = 1280, heightDip = kStripDip;
    bool fullscreen = false;        // no pin and no maximize; the maximize slot holds "exit full screen"
};

/// Rects of the title strip (DIP, client coords; empty = element absent). Single source of truth for the TitleBar widget
/// and the window's non-client hit test: [Menu] name ... [Pin] [Min] [Max/Restore] [Close].
struct TitleLayout {
    pui::RectF mainMenu, name, pin, min, max, close;
    float widthDip = 0, heightDip = kStripDip;

    static TitleLayout Compute(const TitleLayoutIn& in) {
        TitleLayout o;
        const float W = std::max(0.f, in.widthDip), H = in.heightDip;
        o.widthDip = W; o.heightDip = H;
        o.close = {std::max(0.f, W - kTitleButtonDip), 0, W, H};
        o.max = {std::max(0.f, o.close.l - kTitleButtonDip), 0, o.close.l, H};
        o.min = {std::max(0.f, o.max.l - kTitleButtonDip), 0, o.max.l, H};
        float left = o.min.l;
        if (!in.fullscreen) {
            o.pin = {std::max(0.f, o.min.l - kTitleButtonDip), 0, o.min.l, H};
            left = o.pin.l;
        }
        float nl = kTitlePadDip;
        o.mainMenu = {0, 0, std::min(kTitleButtonDip, left), H};   // caption-sized hamburger button at the strip's left edge
        nl = o.mainMenu.r + kTitlePadDip;
        const float nr = left - kTitlePadDip;
        o.name = nr > nl ? pui::RectF{nl, 0, nr, H} : pui::RectF{};
        return o;
    }

    TitleZone Classify(pui::PointF p) const {
        if (p.y < 0 || p.y >= heightDip || p.x < 0 || p.x >= widthDip) return TitleZone::None;
        auto in = [&](const pui::RectF& r) { return !r.Empty() && r.Contains(p.x, p.y); };
        if (in(close)) return TitleZone::Close;
        if (in(max)) return TitleZone::Max;
        if (in(min)) return TitleZone::Min;
        if (in(pin)) return TitleZone::Pin;
        if (in(mainMenu)) return TitleZone::MainMenu;
        return TitleZone::Drag;
    }
};

/// A DIP rect in device pixels (rounded like the layout's snapped rects).
inline RectPx ToPx(const pui::RectF& r, float scale) {
    return {(int)std::lround(r.l * scale), (int)std::lround(r.t * scale), (int)std::lround(r.r * scale), (int)std::lround(r.b * scale)};
}

}
