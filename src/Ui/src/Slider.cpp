#include "pui/Slider.h"

#include <algorithm>
#include <cmath>

namespace pui {

float SliderValueToX(double v, double mn, double mx, float trackL, float trackR) {
    if (!(mx > mn)) return trackL;
    double f = std::clamp((v - mn) / (mx - mn), 0.0, 1.0);
    return trackL + (float)(f * (trackR - trackL));
}

double SliderSnap(double v, double mn, double mx, double step) {
    if (step > 0) v = mn + std::round((v - mn) / step) * step;
    return std::clamp(v, mn, mx);
}

double SliderXToValue(float x, double mn, double mx, float trackL, float trackR, double step) {
    if (!(trackR > trackL)) return mn;
    double f = std::clamp((double)(x - trackL) / (double)(trackR - trackL), 0.0, 1.0);
    return SliderSnap(mn + f * (mx - mn), mn, mx, step);
}

Slider::Slider() : Widget(Role::Slider) { SetFocusable(true); }

void Slider::SetRange(double mn, double mx) {
    if (mx < mn) mx = mn;
    min_ = mn; max_ = mx;
    double v = std::clamp(value_, min_, max_);
    if (v != value_) { value_ = v; Raise(UiEvent::ValueChanged); }
    Invalidate();
}

void Slider::SetValue(double v) {
    v = SliderSnap(v, min_, max_, step_);
    if (v == value_) return;
    value_ = v;
    Raise(UiEvent::ValueChanged);
    Invalidate();
}

RectF Slider::TrackRect() const {
    RectF b = Bounds();
    float inset = kThumbOuter * 0.5f;
    return {b.l + inset, b.cy() - kTrackHeight * 0.5f, b.r - inset, b.cy() + kTrackHeight * 0.5f};
}

RectF Slider::ThumbRect() const {
    RectF tr = TrackRect();
    float x = SliderValueToX(value_, min_, max_, tr.l, tr.r);
    float cy = Bounds().cy(), h = kThumbOuter * 0.5f;
    return {x - h, cy - h, x + h, cy + h};
}

double Slider::ValueAt(float x) const {
    RectF tr = TrackRect();
    return SliderXToValue(x, min_, max_, tr.l, tr.r, step_);
}

void Slider::UpdateToolTip(double v) {
    if (valueFormatter) SetToolTip(valueFormatter(v));
}

void Slider::UserSet(double v, bool dragging, bool force) {
    v = SliderSnap(v, min_, max_, step_);
    bool changed = v != value_;
    if (changed) {
        value_ = v;
        Raise(UiEvent::ValueChanged);
        Invalidate();
    }
    if (valueFormatter) UpdateToolTip(v);
    if (changed || dragging || force) {
        if (onChanged) onChanged(value_, dragging);
    }
}

void Slider::Paint(IRenderer& r, const Theme& t) {
    RectF tr = TrackRect();
    bool en = IsEffectivelyEnabled();
    float rad = kTrackHeight * 0.5f;
    r.FillRoundRect(tr, rad, en ? t.ctlStrongFill : t.ctlStrongFillDisabled);
    if (!en) return;
    RectF th = ThumbRect();
    float cx = th.cx(), cy = th.cy();
    if (cx > tr.l) r.FillRoundRect({tr.l, tr.t, std::max(cx, tr.l + 1), tr.b}, rad, t.accent);
    bool pressed = dragging_ || IsPressed();
    r.FillEllipse({cx, cy}, kThumbOuter * 0.5f, kThumbOuter * 0.5f, t.ctlSolidFill);
    r.StrokeEllipse({cx, cy}, kThumbOuter * 0.5f - 0.5f, kThumbOuter * 0.5f - 0.5f, t.ctlStrokeSecondary, 1);
    float ri = pressed ? 5.f : IsHovered() ? 7.f : 6.f;
    r.FillEllipse({cx, cy}, ri, ri, MulAlpha(t.accent, pressed ? 0.8f : IsHovered() ? 0.9f : 1.f));
    PaintFocusVisual(r, t, th, kThumbOuter * 0.5f);
}

bool Slider::OnPointerDown(const PointerEvent& e) {
    if (e.button != PointerButton::Left) return false;
    dragging_ = true;
    UserSet(ValueAt(e.pt.x), true);
    if (onHoverValue) onHoverValue(std::nullopt);
    return true;
}

bool Slider::OnPointerMove(const PointerEvent& e) {
    if (dragging_) {
        UserSet(ValueAt(e.pt.x), true);
        return true;
    }
    double v = ValueAt(e.pt.x);
    UpdateToolTip(v);
    if (onHoverValue) onHoverValue(v);
    return false;
}

bool Slider::OnPointerUp(const PointerEvent& e) {
    if (e.button != PointerButton::Left || !dragging_) return false;
    dragging_ = false;
    UserSet(ValueAt(e.pt.x), false, true);     // always report the end of the drag
    Invalidate();
    return true;
}

bool Slider::OnPointerWheel(const PointerEvent& e) {
    if (e.wheelDelta == 0) return false;
    UserSet(value_ + (e.wheelDelta > 0 ? 1 : -1) * SmallStep() * std::max(1.0, std::fabs((double)e.wheelDelta)), false);
    return true;
}

void Slider::OnPointerLeave() {
    if (!dragging_ && onHoverValue) onHoverValue(std::nullopt);
}

bool Slider::OnKeyDown(int vk, unsigned mods) {
    if (mods & (ModCtrl | ModAlt)) return false;
    switch (vk) {
    case key::Left: case key::Down: UserSet(value_ - SmallStep(), false); return true;
    case key::Right: case key::Up: UserSet(value_ + SmallStep(), false); return true;
    case key::PageDown: UserSet(value_ - LargeStep(), false); return true;
    case key::PageUp: UserSet(value_ + LargeStep(), false); return true;
    case key::Home: UserSet(min_, false); return true;
    case key::End: UserSet(max_, false); return true;
    default: return false;
    }
}

RangeInfo Slider::GetRange() const {
    RangeInfo i;
    i.minimum = min_; i.maximum = max_; i.value = value_;
    i.smallChange = SmallStep(); i.largeChange = LargeStep();
    i.readOnly = !IsEffectivelyEnabled();
    return i;
}

void Slider::SetRangeValue(double v) {
    if (!IsEffectivelyEnabled()) return;
    UserSet(v, false);
}

}
