#pragma once
// Pure edge-snapping math for moving / resizing the main window (magnetic edges). No Win32 calls; tested by
// --logictest (LogicTests.cpp). All rects are VISIBLE frame rects (DWM extended frame bounds), physical px.
// Snapping has hysteresis: it works on the cursor-implied RAW position, a snap holds until the raw position has travelled
// `thr` from where it engaged (either direction), and a released target cannot re-grab until the raw position left its reach.
#include <algorithm>
#include <cstdlib>
#include <vector>

#include "features/MiniLogic.h"   // PxRect

namespace app {

/// Snap distance in DIP: an edge within this distance of a target edge jumps onto it,
/// and leaves it again after the same distance of travel.
constexpr int kSnapThresholdDip = 12;

struct SnapEdges { bool l = false, t = false, r = false, b = false; };

namespace snap_detail {
/// One snappable coordinate (a window origin or one dragged edge).
struct Axis {
    bool snapped = false, suppressed = false;
    int target = 0, rawAtSnap = 0, suppTarget = 0;

    /// Output coordinate for the raw one; `cands` are coordinates the raw value may snap to.
    int Step(int raw, const std::vector<int>& cands, int thr) {
        if (snapped) {
            if (std::abs(raw - rawAtSnap) <= thr) return target;
            snapped = false; suppressed = true; suppTarget = target;   // escaped: this target stays quiet until left behind
        }
        if (suppressed && std::abs(raw - suppTarget) > thr) suppressed = false;
        bool found = false;
        int best = thr + 1, pick = 0;
        for (int c : cands) {
            if (suppressed && c == suppTarget) continue;
            const int d = std::abs(c - raw);
            if (d < best) { best = d; pick = c; found = true; }
        }
        if (!found) return raw;
        snapped = true; target = pick; rawAtSnap = raw;
        return pick;
    }
};
}

/// State of one move / size loop (WM_ENTERSIZEMOVE .. WM_EXITSIZEMOVE). Begin records the window and cursor; Step turns each
/// proposed rect into the snapped one. The raw position comes from the cursor delta (the proposal may be derived from the
/// already snapped rect), anchored once on the first proposal so a drag that started at a press point stays consistent.
struct SnapTracker {
    bool active = false;

    void Begin(PxRect vis, int cursorX, int cursorY) {
        *this = SnapTracker{};
        active = true; start_ = vis; cx0_ = cursorX; cy0_ = cursorY;
    }
    void End() { active = false; }

    /// `areas`: monitor work areas (their edges act from inside); `windows`: other players (outer edges, to sit adjacent or
    /// align). A target only counts when it overlaps the rect on the other axis (grown by `thr`), so a window far above never
    /// grabs a horizontal move. `move` shifts the whole rect (nearest of its two edges per axis wins); otherwise only the
    /// edges set in `drag` move, each independently. `minW` / `minH` drop a resize snap that would shrink below the minimum.
    PxRect Step(int cursorX, int cursorY, PxRect proposed, bool move, SnapEdges drag, const std::vector<PxRect>& areas,
                const std::vector<PxRect>& windows, int thr, int minW = 0, int minH = 0) {
        if (thr <= 0 || !active) return proposed;
        const int dx = cursorX - cx0_, dy = cursorY - cy0_;
        if (!anchored_) {
            anchored_ = true;
            corr_[0] = proposed.l - (start_.l + dx); corr_[1] = proposed.t - (start_.t + dy);
            corr_[2] = proposed.r - (start_.r + dx); corr_[3] = proposed.b - (start_.b + dy);
        }
        PxRect raw = proposed;
        if (move) {
            raw = {start_.l + dx + corr_[0], start_.t + dy + corr_[1], start_.r + dx + corr_[2], start_.b + dy + corr_[3]};
        } else {
            if (drag.l) raw.l = start_.l + dx + corr_[0];
            if (drag.r) raw.r = start_.r + dx + corr_[2];
            if (drag.t) raw.t = start_.t + dy + corr_[1];
            if (drag.b) raw.b = start_.b + dy + corr_[3];
        }
        std::vector<int> xs, ys;
        auto addTarget = [&](const PxRect& t) {
            const bool overlapY = t.t < raw.b + thr && t.b > raw.t - thr, overlapX = t.l < raw.r + thr && t.r > raw.l - thr;
            if (overlapY) { xs.push_back(t.l); xs.push_back(t.r); }
            if (overlapX) { ys.push_back(t.t); ys.push_back(t.b); }
        };
        for (const auto& a : areas) addTarget(a);
        for (const auto& w : windows) addTarget(w);
        PxRect out = raw;
        if (move) {
            // the origin snaps; only the LEADING edge (the side being moved toward) may, so a window barely narrower than the
            // gap cannot flip between its left- and right-snapped positions. Not yet moving on an axis: either edge.
            const int w = raw.w(), h = raw.h();
            auto lead = [](int& dir, int v, int prev) { if (v != prev) dir = v > prev ? 1 : -1; };
            if (seen_) { lead(dir_[0], raw.l, prevL_); lead(dir_[1], raw.t, prevT_); }
            auto origins = [](const std::vector<int>& cs, int ext, int dir) {
                std::vector<int> o;
                if (dir <= 0) o = cs;                                  // leading edge: left / top
                if (dir >= 0) for (int c : cs) o.push_back(c - ext);   // leading edge: right / bottom
                return o;
            };
            seen_ = true; prevL_ = raw.l; prevT_ = raw.t;
            const int nl = ax_[0].Step(raw.l, origins(xs, w, dir_[0]), thr), nt = ax_[1].Step(raw.t, origins(ys, h, dir_[1]), thr);
            out = {nl, nt, nl + w, nt + h};
        } else {
            auto edge = [&](int i, bool dragged, int rawV, std::vector<int> cands, auto valid) {
                if (!dragged) return rawV;
                cands.erase(std::remove_if(cands.begin(), cands.end(), [&](int c) { return !valid(c); }), cands.end());
                return ax_[i].Step(rawV, cands, thr);
            };
            out.l = edge(0, drag.l, raw.l, xs, [&](int c) { return raw.r - c >= minW; });
            out.r = edge(2, drag.r, raw.r, xs, [&](int c) { return c - out.l >= minW; });
            out.t = edge(1, drag.t, raw.t, ys, [&](int c) { return raw.b - c >= minH; });
            out.b = edge(3, drag.b, raw.b, ys, [&](int c) { return c - out.t >= minH; });
        }
        return out;
    }

private:
    PxRect start_{};
    int cx0_ = 0, cy0_ = 0;
    bool anchored_ = false;
    bool seen_ = false;
    int prevL_ = 0, prevT_ = 0, dir_[2]{};   // move: last raw origin and the last non-zero direction per axis
    int corr_[4]{};                    // l, t, r, b
    snap_detail::Axis ax_[4];          // l, t, r, b (move: l = x origin, t = y origin)
};

}
