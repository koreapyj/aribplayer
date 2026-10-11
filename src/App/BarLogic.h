#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <string>
#include <vector>

#include "features/BadgeLogic.h"
#include "Localize.h"
#include "features/SettingsLogic.h"
#include "pui/Geometry.h"
#include "pui/Glyphs.h"

namespace app {

inline std::wstring FormatTime(double s) {
    if (!(s >= 0) || std::isnan(s) || std::isinf(s)) s = 0;
    long long total = (long long)std::floor(s);
    long long h = total / 3600, m = (total / 60) % 60, sec = total % 60;
    wchar_t buf[48];
    if (h > 0) swprintf(buf, 48, L"%lld:%02lld:%02lld", h, m, sec);
    else swprintf(buf, 48, L"%02lld:%02lld", m, sec);
    return buf;
}

/// Volume button glyph: E74F mute, E992 (0), E993 (1-33), E994 (34-66), E995 (67-100).
inline const wchar_t* VolumeGlyph(int vol, bool muted) {
    if (muted) return L"";
    if (vol <= 0) return L"";
    if (vol <= 33) return L"";
    if (vol <= 66) return L"";
    return L"";
}

constexpr float kStripDip = 44;
constexpr float kDockDip = 86;          // seek row (10 pad + 20) + transport row (6 pad + 40 + 10 pad)
constexpr float kBarSlideDip = 64;      // full screen / mini player: the auto-hide bar slides this far down while fading
constexpr int kBarIdleHideMs = 3000, kBarShowMs = 300, kBarHideMs = 500;
// Full screen: each overlay appears (instantly) only when the cursor enters its screen edge - title strip at the top
// (kStripDip), control bar at the bottom (kDockDip), playlist panel at the right (a thin band) - and hides (instantly) as soon as
// the cursor leaves the overlay's own rect.
constexpr float kEdgeRightDip = 12;

// Dock metrics (DIP).
constexpr float kDockSeekTopDip = 10, kDockSeekH = 20, kDockRowTopDip = 36, kDockBtnDip = 40, kDockBtnGapDip = 2;
constexpr float kDockSeekInsetDip = 16, kDockSeekGapDip = 12, kDockTimeLabelDip = 52, kDockThumbDip = 20;
constexpr float kDockSideDip = 12, kDockGroupGapDip = 8, kDockChipLeadDip = 8;
constexpr float kChipHDip = 22, kChipPadDip = 8, kChipGapDip = 6;
constexpr float kVolumeWideDip = 96, kVolumeNarrowDip = 64, kVolumeMarginDip = 8;

enum Btn { B_Play, B_Stop, B_Prev, B_Next, B_Open, B_Mute, B_Subs, B_Control, B_Panel, B_Full, B_Count };

/// Progressive hide as the bar narrows, in order: chips (last first) -> volume slider 96 -> 64 -> Open / Subtitle / Control.
enum class BarTier { Full, VolumeNarrow, Compact };

struct BarLayout {   // bar-local coordinates (origin = bar top-left), device pixels
    static constexpr int kMaxChips = 6;
    float s = 1, W = 0, H = 0;
    pui::RectF stroke, elapsed, total, seek, volume, btn[B_Count], chip[kMaxChips];
    bool buttonVisible[B_Count]{}, chipVisible[kMaxChips]{};
    bool volumeVisible = false;
    int chipCount = 0, visibleChips = 0;
    BarTier tier = BarTier::Full;

    static float R(float dip, float s) { return std::round(dip * s); }
    float R(float dip) const { return R(dip, s); }

    /// `chipWidthsDip`: outer width of each chip in chip order (the first `kMaxChips` count).
    void Compute(float widthDip, float scale, const std::vector<float>& chipWidthsDip = {}) {
        s = scale > 0 ? scale : 1.f; W = std::round(widthDip * s); H = R(kDockDip);
        stroke = {0, 0, W, std::max(1.f, R(1))};
        for (bool& v : buttonVisible) v = true;
        for (bool& v : chipVisible) v = false;
        for (auto& r : btn) r = {};
        for (auto& r : chip) r = {};
        chipCount = (int)std::min<size_t>(chipWidthsDip.size(), kMaxChips);
        float chipW[kMaxChips]{};
        for (int i = 0; i < chipCount; i++) chipW[i] = R(chipWidthsDip[(size_t)i]);

        // seek row: elapsed label | slider | total label (the slider rect overhangs its track by half a thumb)
        const float seekT = R(kDockSeekTopDip), seekB = seekT + R(kDockSeekH);
        const float lab = R(kDockTimeLabelDip), inset = R(kDockSeekInsetDip), gap = R(kDockSeekGapDip), hang = R(kDockThumbDip / 2);
        elapsed = {inset, seekT, inset + lab, seekB};
        total = {W - inset - lab, seekT, W - inset, seekB};
        seek = {elapsed.r + gap - hang, seekT, total.l - gap + hang, seekB};

        // transport row
        const float rowT = R(kDockRowTopDip), bs = R(kDockBtnDip), bg = R(kDockBtnGapDip);
        const float cy = rowT + bs * 0.5f;
        auto box = [&](float l) { return pui::RectF{l, rowT, l + bs, rowT + bs}; };
        const float side = R(kDockSideDip), groupGap = R(kDockGroupGapDip), chipGap = R(kChipGapDip);

        struct Cfg { int chips; float volDip; bool secondary; };
        std::vector<Cfg> cfgs;
        for (int n = chipCount; n >= 0; n--) cfgs.push_back({n, kVolumeWideDip, true});
        cfgs.push_back({0, kVolumeNarrowDip, true});
        cfgs.push_back({0, kVolumeNarrowDip, false});
        cfgs.push_back({0, 0, false});
        auto leftEnd = [&](bool secondary) { int n = secondary ? 5 : 4; return side + n * bs + (n - 1) * bg; };
        auto rightSpan = [&](const Cfg& c) {
            float w = bs + (c.volDip > 0 ? bg + R(c.volDip) + R(kVolumeMarginDip) : 0.f);
            if (c.secondary) w += 2 * (bg + bs);
            return w + 2 * (bg + bs);   // Panel, Full
        };
        auto fits = [&](const Cfg& c) {
            float end = leftEnd(c.secondary), rl = W - side - rightSpan(c);
            if (c.chips > 0) {
                float span = (float)(c.chips - 1) * chipGap;
                for (int i = 0; i < c.chips; i++) span += chipW[i];
                end += groupGap + R(kDockChipLeadDip) + span;
            }
            return end + groupGap <= rl;
        };
        Cfg use = cfgs.back();
        for (const Cfg& c : cfgs) if (fits(c)) { use = c; break; }
        visibleChips = use.chips;
        tier = use.secondary ? (use.volDip == kVolumeWideDip ? BarTier::Full : BarTier::VolumeNarrow) : BarTier::Compact;

        float x = side;
        for (int b : {B_Play, B_Stop, B_Prev, B_Next, B_Open}) {
            if (b == B_Open && !use.secondary) { buttonVisible[b] = false; continue; }
            btn[b] = box(x);
            x += bs + bg;
        }
        x = x - bg + groupGap + R(kDockChipLeadDip);
        for (int i = 0; i < use.chips; i++) {
            float top = std::floor(cy - R(kChipHDip) * 0.5f);
            chip[i] = {x, top, x + chipW[i], top + R(kChipHDip)};
            chipVisible[i] = true;
            x += chipW[i] + chipGap;
        }

        x = W - side - rightSpan(use);
        btn[B_Mute] = box(x);
        x += bs;
        volumeVisible = use.volDip > 0;
        volume = {};
        if (volumeVisible) {
            float vt = std::floor(cy - R(kDockSeekH) * 0.5f);
            x += bg;
            volume = {x, vt, x + R(use.volDip), vt + R(kDockSeekH)};
            x += R(use.volDip) + R(kVolumeMarginDip);
        }
        for (int b : {B_Subs, B_Control}) {
            if (!use.secondary) { buttonVisible[b] = false; continue; }
            x += bg;
            btn[b] = box(x);
            x += bs;
        }
        for (int b : {B_Panel, B_Full}) {
            x += bg;
            btn[b] = box(x);
            x += bs;
        }
    }
    bool Sane() const {
        auto in = [&](const pui::RectF& r) { return !r.Empty() && r.l >= 0 && r.t >= 0 && r.r <= W + 0.5f && r.b <= H + 0.5f; };
        auto overlaps = [](const pui::RectF& a, const pui::RectF& b) {
            return !a.Empty() && !b.Empty() && a.l < b.r && b.l < a.r && a.t < b.b && b.t < a.b;
        };
        std::vector<pui::RectF> all;
        if (!in(stroke) || !in(elapsed) || !in(total) || !in(seek)) return false;
        if (elapsed.r > seek.l || seek.r > total.l) return false;
        for (int i = 0; i < B_Count; i++) {
            if (!buttonVisible[i]) continue;
            if (!in(btn[i])) return false;
            all.push_back(btn[i]);
        }
        for (int i = 0; i < chipCount; i++) {
            if (!chipVisible[i]) continue;
            if (!in(chip[i])) return false;
            all.push_back(chip[i]);
        }
        if (volumeVisible) {
            if (!in(volume)) return false;
            all.push_back(volume);
        }
        for (size_t i = 0; i < all.size(); i++) {
            if (overlaps(all[i], elapsed) || overlaps(all[i], total) || overlaps(all[i], seek)) return false;
            for (size_t j = i + 1; j < all.size(); j++)
                if (overlaps(all[i], all[j])) return false;
        }
        return buttonVisible[B_Play] && buttonVisible[B_Full];
    }
};


struct ColorFade {
    static constexpr int kMs = 83;
    pui::Color from, to;
    uint64_t t0 = 0;
    bool init = false;

    pui::Color Cur(uint64_t now) const {
        if (!init) return pui::Color(0);
        float t = std::clamp((float)(now - t0) / (float)kMs, 0.f, 1.f);
        if (t0 == 0 || now < t0 || t >= 1.f) return to;
        auto pm = [](pui::Color c, float o[4]) { float a = c.a(); o[0] = a; o[1] = c.r() * a; o[2] = c.g() * a; o[3] = c.b() * a; };
        float a[4], b[4], m[4];
        pm(from, a); pm(to, b);
        for (int i = 0; i < 4; i++) m[i] = a[i] + (b[i] - a[i]) * t;
        auto q = [](float v) { return (uint32_t)std::lround(std::clamp(v, 0.f, 1.f) * 255.f); };
        if (m[0] <= 0.0001f) return pui::Color(0);
        return pui::Color((q(m[0]) << 24) | (q(m[1] / m[0]) << 16) | (q(m[2] / m[0]) << 8) | q(m[3] / m[0]));
    }
    void Target(pui::Color target, uint64_t now) {
        if (!init) { from = to = target; init = true; t0 = 0; return; }
        if (target.v != to.v) { from = Cur(now); to = target; t0 = now ? now : 1; }
    }
    bool Active(uint64_t now) const { return init && t0 != 0 && now >= t0 && now - t0 < (uint64_t)kMs; }
};

/// p: 0 = shown, 1 = hidden. Show = 300 ms ease-out, hide = 500 ms ease-in. All methods take `now` (ms).
class AutoHide {
public:
    struct Anim {   // description of the last started animation, for the DirectComposition layer
        float p0 = 0, p1 = 0;
        int durMs = 0;
        bool easeOut = true;
        unsigned serial = 0;
    };
    enum class Action { None, StartShow, StartHide, Snapped };   // Snapped: edge mode changed state with no animation

    bool Enabled() const { return enabled_; }
    /// Edge mode (full screen): only Enter shows the overlay; Activity / Reveal never do, EdgeTick hides it as soon as the
    /// pointer is off it, and Show / Hide switch instantly (Action::Snapped, no slide / fade).
    void SetEdge(bool e) { edge_ = e; }
    /// Disabled = docked (windowed player, pages): always shown, no animation.
    void SetEnabled(bool e, uint64_t now) {
        if (e == enabled_) return;
        enabled_ = e;
        Snap(0.f);
        lastActivity_ = now;
    }
    void Snap(float p) { from_ = to_ = p; dur_ = 0; }
    float P(uint64_t now) const {
        if (dur_ <= 0 || now >= t0_ + (uint64_t)dur_) return to_;
        float s = now <= t0_ ? 0.f : (float)(now - t0_) / (float)dur_;
        float e = out_ ? pui::EaseOut(s) : pui::EaseIn(s);
        return from_ + (to_ - from_) * e;
    }
    bool Hidden() const { return to_ >= 1.f; }
    bool Animating(uint64_t now) const { return dur_ > 0 && now < t0_ + (uint64_t)dur_; }
    /// Clicks/hover only reach the bar while it is fully committed to the shown state (not once the hide starts).
    bool Hittable(uint64_t now) const { return !enabled_ || (to_ == 0.f && P(now) < 0.5f); }
    uint64_t LastActivity() const { return lastActivity_; }
    void Touch(uint64_t now) { lastActivity_ = now; }

    Action Show(uint64_t now) {
        if (!enabled_ || to_ == 0.f) return Action::None;
        if (edge_) { Snap(0.f); return Action::Snapped; }
        Start(P(now), 0.f, kBarShowMs, true, now);
        return Action::StartShow;
    }
    Action Hide(uint64_t now) {
        if (!enabled_ || to_ == 1.f) return Action::None;
        if (edge_) { Snap(1.f); return Action::Snapped; }
        Start(P(now), 1.f, kBarHideMs, false, now);
        return Action::StartHide;
    }
    /// Pointer activity: restart the idle countdown and show the bar (ends a keyboard-quiet period).
    Action Activity(uint64_t now) { return edge_ ? Action::None : Enter(now); }
    /// Shows the overlay and restarts its idle countdown (the cursor is in its edge zone / on it; or a forced reveal).
    Action Enter(uint64_t now) { lastActivity_ = now; keyQuiet_ = false; pointerOut_ = false; return Show(now); }
    /// Key press: never shows the bar and never restarts the idle countdown. Until the next pointer activity, the bar also
    /// stays as it is when playback stops or pauses (a keyboard pause must not bring the bar back through Tick/Reveal).
    Action KeyPress() { keyQuiet_ = true; return Action::None; }
    bool KeyQuiet() const { return keyQuiet_; }
    /// Playback-state reveal (paused/stopped/opening): shows the bar like Activity unless the last input was a key press.
    Action Reveal(uint64_t now) { return keyQuiet_ || pointerOut_ || edge_ ? Action::None : Enter(now); }
    /// The pointer left the window: hide at once, playing or paused, unless the user is interacting (popup open, seek
    /// drag). Until the pointer comes back (Activity) the bar also stays hidden while paused.
    Action PointerLeft(uint64_t now, bool interactionActive) {
        if (!enabled_ || interactionActive) return Action::None;
        pointerOut_ = true;
        return Hide(now);
    }
    bool PointerOut() const { return pointerOut_; }
    /// Periodic rule: hide after 3 s idle while playing unless the user is interacting; show whenever not playing
    /// (unless the last input was a key press, see KeyPress).
    Action Tick(uint64_t now, bool playing, bool interactionActive, bool minimized) {
        if (!enabled_ || minimized) return Action::None;
        if (playing) {
            if (to_ == 0.f && !interactionActive && now - lastActivity_ >= (uint64_t)kBarIdleHideMs) return Hide(now);
        } else if (to_ != 0.f && !keyQuiet_ && !pointerOut_) return Show(now);
        return Action::None;
    }
    /// Full-screen rule: hide at once when the pointer is neither over the overlay nor the user interacting with it (menu, drag).
    Action EdgeTick(uint64_t now, bool pointerOver, bool interactionActive, bool minimized) {
        return !enabled_ || minimized || to_ != 0.f || pointerOver || interactionActive ? Action::None : Hide(now);
    }
    const Anim& LastAnim() const { return anim_; }

private:
    void Start(float p0, float p1, int dur, bool easeOut, uint64_t now) {
        from_ = p0; to_ = p1; t0_ = now; dur_ = dur; out_ = easeOut;
        anim_ = {p0, p1, dur, easeOut, anim_.serial + 1};
    }
    bool enabled_ = true;
    bool edge_ = false;
    bool keyQuiet_ = false;
    bool pointerOut_ = false;
    float from_ = 0, to_ = 0;
    uint64_t t0_ = 0, lastActivity_ = 0;
    int dur_ = 0;
    bool out_ = true;
    Anim anim_;
};

/// Full-screen edge zones containing the cursor (client DIP); the three overlays are independent and may overlap in a corner.
struct EdgeZones { bool top = false, bottom = false, right = false; };
inline EdgeZones EdgeZonesAt(float x, float y, float wDip, float hDip) {
    EdgeZones z;
    if (x < 0 || y < 0 || x > wDip || y > hDip) return z;
    z.top = y < kStripDip;
    z.bottom = y >= hDip - kDockDip;
    z.right = x >= wDip - kEdgeRightDip;
    return z;
}

// Independent of the bar: the cursor has its own idle timer and reappears on the first real pointer move, whatever
// the bar is doing.
constexpr int kCursorIdleHideMs = 2000;

class CursorAutoHide {
public:
    void Moved(uint64_t now) { lastMove_ = now; }
    /// Key press: leaves the cursor as it is (an auto-hidden cursor stays hidden; only a real pointer move shows it).
    void KeyPress() {}
    /// Hide over the video in full screen only (never in a normal/maximized window, the mini player or pages) after
    /// kCursorIdleHideMs without a pointer move; never over the bar or a popup, never while minimized (`minimized` also
    /// covers a disabled main window, i.e. a modal dialog is open).
    bool ShouldHide(uint64_t now, bool fullscreen, bool overChrome, bool popupOpen, bool minimized) const {
        return fullscreen && !overChrome && !popupOpen && !minimized && now - lastMove_ >= (uint64_t)kCursorIdleHideMs;
    }
private:
    uint64_t lastMove_ = 0;
};


struct BarModel {
    bool loaded = false;        // controller has a current item
    bool canPlaySelection = false;   // nothing loaded, but a playlist row is selected: Play starts it
    bool prepared = false;      // PREPARED received for it
    bool seekable = false;
    double pos = 0, dur = 0;    // seconds; dur <= 0 = live/unknown
    bool playing = false;
    bool hasPrev = false, hasNext = false;
    bool shuffle = false;       // context menu
    int repeat = 0;             // 0 off, 1 all, 2 one (context menu)
    int volume = 100;
    bool muted = false;
    int subCount = 0;
    bool fullscreen = false, mini = false;
    bool panelOpen = false;         // playlist panel wanted (setting)
    bool panelAvailable = false;    // the toggle works: windowed player view
    bool controlPanelOpen = false;  // 제어창 (filters panel) is open
    sl::TimeDisplayMode timeMode = sl::TimeDisplayMode::Elapsed;
    std::vector<badge::Chip> chips;
    std::wstring title;

    bool Seekable() const { return loaded && prepared && seekable && dur > 0; }
    bool SliderEnabled() const { return loaded && prepared && dur > 0; }
    bool Live() const { return loaded && prepared && !(dur > 0); }
};

struct BtnInfo {
    bool enabled = true, visible = true, toggled = false;
    std::wstring glyph, tip;
};

inline BtnInfo ButtonInfo(const BarModel& m, int b) {
    namespace G = pui::glyph;
    BtnInfo i;
    switch (b) {
    case B_Play: i.glyph = G::Str(m.playing ? G::Pause : G::Play); i.tip = m.playing ? Tr(L"Pause", L"일시 정지") : Tr(L"Play", L"재생"); i.enabled = m.loaded || m.canPlaySelection; break;
    case B_Stop: i.glyph = G::Str(G::Stop); i.tip = Tr(L"Stop", L"정지"); i.enabled = m.loaded; break;
    case B_Prev: i.glyph = G::Str(G::PreviousLegacy); i.tip = Tr(L"L: Back 5 s, R: Previous file", L"L: 5초 뒤로, R: 이전 파일"); i.enabled = m.Seekable() || m.hasPrev; break;
    case B_Next: i.glyph = G::Str(G::NextLegacy); i.tip = Tr(L"L: Forward 5 s, R: Next file", L"L: 5초 앞으로, R: 다음 파일"); i.enabled = m.Seekable() || m.hasNext; break;
    case B_Open: i.glyph = G::Str(G::OpenFile); i.tip = Tr(L"Open file", L"파일 열기"); break;
    case B_Mute: i.glyph = VolumeGlyph(m.volume, m.muted); i.tip = m.muted ? Tr(L"Unmute", L"음소거 해제") : Tr(L"Mute", L"음소거"); break;
    case B_Subs: i.glyph = G::Str(G::ClosedCaption); i.tip = Tr(L"Subtitles", L"자막"); i.enabled = m.loaded; break;
    case B_Control: i.glyph = G::Str(G::Sliders); i.tip = Tr(L"Control panel (F7)", L"제어창 (F7)"); i.toggled = m.controlPanelOpen; break;
    case B_Panel: i.glyph = G::Str(G::Playlist); i.tip = Tr(L"Playlist (F6)", L"재생목록 (F6)"); i.enabled = m.panelAvailable; i.toggled = m.panelOpen && m.panelAvailable; break;
    case B_Full: i.glyph = G::Str(m.fullscreen ? G::BackToWindow : G::FullScreen); i.tip = m.fullscreen ? Tr(L"Exit full screen (Enter)", L"전체 화면 해제 (Enter)") : Tr(L"Full screen (Enter)", L"전체 화면 (Enter)"); break;
    default: break;
    }
    return i;
}

}
