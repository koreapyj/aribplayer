#include "features/MiniLogic.h"

namespace app {

namespace {
float SafeScale(float s) { return s > 0 ? s : 1.f; }
int Px(float dip, float s) { return (int)std::lround(dip * s); }
}

long long PxIntersectArea(const PxRect& a, const PxRect& b) {
    long long w = std::min(a.r, b.r) - std::max(a.l, b.l);
    long long h = std::min(a.b, b.b) - std::max(a.t, b.t);
    return (w > 0 && h > 0) ? w * h : 0;
}

PxSize MiniMinSizePx(float scale) {
    float s = SafeScale(scale);
    return {Px(kMiniMinWDip, s), Px(kMiniMinHDip, s)};
}

PxSize MiniDefaultSizePx(const PxRect& work, float scale) {
    float s = SafeScale(scale);
    return {std::min(Px(kMiniDefaultWDip, s), std::max(1, work.w())), std::min(Px(kMiniDefaultHDip, s), std::max(1, work.h()))};
}

PxRect MiniClampRect(PxRect want, const PxRect& work, float scale) {
    PxSize mn = MiniMinSizePx(scale);
    int ww = std::max(1, work.w()), wh = std::max(1, work.h());
    int w = std::clamp(want.w(), std::min(mn.w, ww), ww);
    int h = std::clamp(want.h(), std::min(mn.h, wh), wh);
    int l = std::clamp(want.l, work.l, work.r - w);
    int t = std::clamp(want.t, work.t, work.b - h);
    return {l, t, l + w, t + h};
}

PxRect MiniDefaultRect(const PxRect& work, float scale) {
    PxSize sz = MiniDefaultSizePx(work, scale);
    int m = Px(kMiniMarginDip, SafeScale(scale));
    PxRect r{work.r - m - sz.w, work.b - m - sz.h, work.r - m, work.b - m};
    return MiniClampRect(r, work, scale);
}

int MiniPickWorkArea(const PxRect& rect, const std::vector<PxRect>& works) {
    int best = -1;
    long long bestArea = 0;
    for (size_t i = 0; i < works.size(); i++) {
        long long a = PxIntersectArea(rect, works[i]);
        if (a > bestArea) { bestArea = a; best = (int)i; }
    }
    return best;
}

PxRect MiniResolvePlacement(const wvp::WindowPlacement& saved, const std::vector<PxRect>& works, const PxRect& currentWork, float scale) {
    if (!saved.valid || saved.w <= 0 || saved.h <= 0) return MiniDefaultRect(currentWork, scale);
    float s = SafeScale(scale);
    int w = Px((float)saved.w, s), h = Px((float)saved.h, s);
    PxRect want{saved.x, saved.y, saved.x + w, saved.y + h};
    int idx = MiniPickWorkArea(want, works);
    if (idx >= 0) return MiniClampRect(want, works[(size_t)idx], scale);
    // saved on a monitor that is gone: keep the size, use the default corner of the current monitor
    PxRect d = MiniDefaultRect(currentWork, scale);
    PxRect r{d.r - w, d.b - h, d.r, d.b};
    return MiniClampRect(r, currentWork, scale);
}

wvp::WindowPlacement MiniMakePlacement(const PxRect& r, float scale) {
    float s = SafeScale(scale);
    wvp::WindowPlacement p;
    p.x = r.l; p.y = r.t;
    p.w = (int)std::lround(r.w() / s);
    p.h = (int)std::lround(r.h() / s);
    p.maximized = false;
    p.valid = p.w > 0 && p.h > 0;
    return p;
}

bool MiniBarLayout::Sane() const {
    const float eps = 1e-3f;
    auto in = [&](const pui::RectF& r) { return !r.Empty() && r.l >= -eps && r.t >= -eps && r.r <= widthDip + eps && r.b <= heightDip + eps; };
    auto chk = [&](bool shown, const pui::RectF& r) { return shown ? in(r) : r.Empty(); };
    return chk(showTime, time) && chk(showPrev, prev) && chk(showPlay, play) && chk(showNext, next) && chk(showExit, exit) && in(slider);
}

MiniBarLayout ComputeMiniBarLayout(int widthPx, float scale) {
    float s = SafeScale(scale);
    MiniBarLayout o;
    o.scale = s;
    int W = std::max(1, widthPx), H = Px(kMiniBarDip, s);
    o.widthDip = (float)W / s;
    o.heightDip = (float)H / s;
    int margin = Px(12, s), btn = Px(32, s), gap = Px(8, s), outer = Px(8, s), timeW = Px(96, s);
    auto dip = [&](int l, int t, int r, int b) { return pui::RectF{(float)l / s, (float)t / s, (float)r / s, (float)b / s}; };

    int sl = (W >= 2 * margin + Px(40, s)) ? margin : 0;
    o.slider = dip(sl, Px(2, s), W - sl, Px(22, s));

    int top = Px(38, s) - btn / 2;
    o.showExit = W >= btn + 2 * outer;
    int exitL = W - outer - btn;
    if (o.showExit) o.exit = dip(exitL, top, exitL + btn, top + btn);
    int rightLimit = o.showExit ? exitL - gap : W - margin;

    int playL = W / 2 - btn / 2, playR = playL + btn;
    o.showPlay = playL >= margin && playR <= rightLimit;
    if (o.showPlay) {
        o.play = dip(playL, top, playR, top + btn);
        int prevL = playL - gap - btn, nextR = playR + gap + btn;
        bool pn = prevL >= margin && nextR <= rightLimit;
        o.showPrev = o.showNext = pn;
        if (pn) {
            o.prev = dip(prevL, top, prevL + btn, top + btn);
            o.next = dip(playR + gap, top, nextR, top + btn);
            o.showTime = prevL >= margin + timeW + gap;
            if (o.showTime) o.time = dip(margin, top, margin + timeW, top + btn);
        }
    }
    return o;
}

ShellLayout ComputeMiniShellLayout(int cwPx, int chPx, float scale) {
    float s = SafeScale(scale);
    ShellLayout o;
    int W = std::max(1, cwPx), H = std::max(1, chPx);
    int barH = std::min(H, MiniBarHeightPx(s));
    o.widthDip = (float)W / s;
    o.heightDip = (float)H / s;
    o.barPx = {0, H - barH, W, H};
    o.bar = pui::RectF{0, (float)(H - barH) / s, (float)W / s, (float)H / s};
    o.video = {0, 0, W, H};
    o.contentPx = {};
    o.contentLayer = {};
    o.barOverlaysVideo = true;
    o.titleSlides = false;
    return o;
}

MiniZone ClassifyMiniPoint(int x, int y, int cw, int ch, float scale, bool barHittable) {
    if (x < 0 || y < 0 || x >= cw || y >= ch) return MiniZone::None;
    if (!barHittable) return MiniZone::Caption;
    float s = SafeScale(scale);
    int barH = std::min(ch, MiniBarHeightPx(s));
    int barTop = ch - barH;
    if (y < barTop) return MiniZone::Caption;
    MiniBarLayout l = ComputeMiniBarLayout(cw, s);
    pui::PointF p{(float)x / s, (float)(y - barTop) / s};
    if (l.slider.Contains(p)) return MiniZone::Control;
    if ((l.showPrev && l.prev.Contains(p)) || (l.showPlay && l.play.Contains(p)) || (l.showNext && l.next.Contains(p)) ||
        (l.showExit && l.exit.Contains(p)))
        return MiniZone::Control;
    return MiniZone::Caption;
}

double MiniAspectForMedia(int dw, int dh) {
    if (dw <= 0 || dh <= 0) return 16.0 / 9.0;
    double a = (double)dw / dh;
    return (a < 0.25 || a > 4.0) ? 16.0 / 9.0 : a;
}

PxRect MiniConstrainSizing(PxRect r, int edge, double aspect, PxSize minSz) {
    if (!(aspect > 0.0)) aspect = 16.0 / 9.0;
    enum { Left = 1, Right = 2, Top = 3, TopLeft = 4, TopRight = 5, Bottom = 6, BottomLeft = 7, BottomRight = 8 };
    bool heightDriven = edge == Top || edge == Bottom;
    int w = r.w(), h = r.h();
    int minW = std::max(minSz.w, (int)std::ceil(minSz.h * aspect));
    if (heightDriven) {
        h = std::max(h, (int)std::ceil(minW / aspect));
        w = (int)std::lround(h * aspect);
    } else {
        w = std::max(w, minW);
        h = (int)std::lround(w / aspect);
    }
    switch (edge) {
    case Left: r.l = r.r - w; r.b = r.t + h; break;
    case Right: r.r = r.l + w; r.b = r.t + h; break;
    case Top: r.t = r.b - h; r.r = r.l + w; break;
    case Bottom: r.b = r.t + h; r.r = r.l + w; break;
    case TopLeft: r.l = r.r - w; r.t = r.b - h; break;
    case TopRight: r.r = r.l + w; r.t = r.b - h; break;
    case BottomLeft: r.l = r.r - w; r.b = r.t + h; break;
    case BottomRight: default: r.r = r.l + w; r.b = r.t + h; break;
    }
    return r;
}

CommandId MiniRemapCommand(CommandId c, bool mini) {
    if (!mini) return c;
    switch (c) {
    case CommandId::Escape:
        return CommandId::ToggleMiniPlayer;
    case CommandId::ToggleControlPanel:   // the filters panel does not fit the mini window
        return CommandId::None;
    default: return c;
    }
}

}
