#include "shell/ShellControls.h"

#include <algorithm>
#include <cmath>

namespace app {

using pui::Color;
using pui::RectF;
namespace G = pui::glyph;

namespace {
pui::TextStyle IconStyle(float size) {
    pui::TextStyle s;
    s.font = pui::FontKind::Icon;
    s.size = size;
    s.align = pui::TextAlign::Center;
    s.ellipsis = false;
    return s;
}

// Transparent variants keep the hover RGB so premultiplied fades do not darken.
Color Clear(Color c) { return Color(c.v & 0x00FFFFFFu); }

Color FillFor(const pui::Theme& t, bool accent, bool toggled, bool captionClose, bool enabled, bool hov, bool prs) {
    if (captionClose) {
        if (!enabled || !hov) return Color(0x00000000);
        return prs ? Color(0xFF9E1E14) : Color(0xFFC42B1C);
    }
    if (accent) return !enabled ? t.ctlDisabled : prs ? t.accentPressed : hov ? t.accentHover : t.accent;
    if (toggled) return !enabled ? Clear(t.ctlSecondary) : prs ? t.ctlTertiary : t.ctlSecondary;
    if (!enabled) return Clear(t.subtleSecondary);
    return prs ? t.subtleTertiary : hov ? t.subtleSecondary : Clear(t.subtleSecondary);
}

constexpr float kTextBtnPad = 10, kTextBtnGlyphCol = 16, kTextBtnGap = 6;
}

GlyphButton::GlyphButton(std::wstring name, std::wstring glyph, float glyphSize, Shape shape)
    : pui::Widget(pui::Role::Button), glyph_(std::move(glyph)), glyphSize_(glyphSize), shape_(shape) {
    SetName(std::move(name));
    SetFocusable(true);
}

void GlyphButton::SetGlyph(const std::wstring& g) {
    if (g == glyph_) return;
    glyph_ = g;
    Invalidate();
}

void GlyphButton::SetAccentStyle(bool accent) {
    if (accent == accent_) return;
    accent_ = accent;
    Invalidate();
}

void GlyphButton::SetToggled(bool on) {
    if (on == toggled_) return;
    toggled_ = on;
    Invalidate();
}

void GlyphButton::SetMenuOpen(bool on) {
    if (on == menuOpen_) return;
    menuOpen_ = on;
    Invalidate();
}

void GlyphButton::Paint(pui::IRenderer& r, const pui::Theme& t) {
    pui::UiHost* host = Host();
    uint64_t now = host ? host->Now() : 1;
    bool en = IsEffectivelyEnabled(), hov = IsHovered() && en, prs = (IsPressed() && hov) || menuOpen_;
    fade_.Target(FillFor(t, accent_, toggled_, captionClose_, en, hov, prs), now);
    const RectF& b = Bounds();
    const float radius = shape_ == Shape::Rounded ? t.controlRadius : 0.f;
    Color f = fade_.Cur(now);
    if (f.a() > 0) {
        if (shape_ == Shape::Rounded) r.FillRoundRect(b, radius, f);
        else r.FillRect(b, f);
    }
    Color gc = !en ? t.textDisabled : captionClose_ && hov ? Color(0xFFFFFFFF) : accent_ ? t.textOnAccent : toggled_ ? t.accent
             : prs ? t.textSecondary : t.textPrimary;
    r.Text(glyph_, b, IconStyle(glyphSize_), gc);
    if (toggled_ && en) r.FillRoundRect(RectF{b.l + 12, b.b - 6, b.r - 12, b.b - 3}, 1.5f, t.accent);   // 3 DIP underline
    PaintFocusVisual(r, t, b, shape_ == Shape::Rounded ? radius : 2.f);
    if (fade_.Active(now) && host) host->StartAnimating(this);
}

bool GlyphButton::OnTick(uint64_t nowMs) { return fade_.Active(nowMs); }

bool GlyphButton::OnPointerDown(const pui::PointerEvent& e) { return e.button == pui::PointerButton::Left; }

bool GlyphButton::OnPointerUp(const pui::PointerEvent& e) {
    if (e.button == pui::PointerButton::Left && Bounds().Contains(e.pt)) Invoke();
    return true;
}

bool GlyphButton::OnContextMenu(pui::PointF, bool) {
    if (!onRightClick || !IsEffectivelyEnabled()) return false;
    onRightClick();
    return true;
}

bool GlyphButton::OnKeyDown(int vk, unsigned mods) {
    if (mods == 0 && (vk == pui::key::Enter || vk == pui::key::Space)) { Invoke(); return true; }
    return false;
}

void GlyphButton::Invoke() {
    if (!IsEffectivelyEnabled()) return;
    Raise(pui::UiEvent::Invoked);
    if (onInvoke) onInvoke();
}

TextButton::TextButton(std::wstring label, std::wstring leadGlyph)
    : pui::Widget(pui::Role::Button), label_(std::move(label)), glyph_(std::move(leadGlyph)) {
    SetName(label_);
    SetFocusable(true);
}

void TextButton::SetLabel(const std::wstring& label) {
    if (label == label_) return;
    label_ = label;
    SetName(label_);
    Invalidate();
}

void TextButton::SetLeadGlyph(const std::wstring& g) {
    if (g == glyph_) return;
    glyph_ = g;
    Invalidate();
}

void TextButton::SetSelected(bool on) {
    if (on == selected_) return;
    selected_ = on;
    Invalidate();
}

void TextButton::SetMenuOpen(bool on) {
    if (on == menuOpen_) return;
    menuOpen_ = on;
    Invalidate();
}

void TextButton::SetChipStyle(bool on) {
    if (on == chipStyle_) return;
    chipStyle_ = on;
    Invalidate();
}

void TextButton::SetFontSize(float size) {
    fontSize_ = size;
    Invalidate();
}

float TextButton::DesiredWidth(pui::IRenderer& r) const {
    pui::TextStyle ts;
    ts.size = fontSize_;
    ts.weight = selected_ && !chipStyle_ ? 600 : 400;
    ts.ellipsis = false;
    float w = 2 * kTextBtnPad + r.MeasureText(label_, ts).w;
    if (!glyph_.empty()) w += kTextBtnGlyphCol + kTextBtnGap;
    return std::ceil(w);
}

void TextButton::Paint(pui::IRenderer& r, const pui::Theme& t) {
    pui::UiHost* host = Host();
    uint64_t now = host ? host->Now() : 1;
    bool en = IsEffectivelyEnabled(), hov = IsHovered() && en, prs = (IsPressed() && hov) || menuOpen_;
    fade_.Target(!en ? Clear(t.subtleSecondary) : prs ? t.subtleTertiary : hov ? t.subtleSecondary : Clear(t.subtleSecondary), now);
    const RectF& b = Bounds();
    Color f = fade_.Cur(now);
    if (chipStyle_ && selected_) {
        r.FillRoundRect(b, t.controlRadius, t.ctlSecondary);
        const float h = t.hairline, hh = h / 2;
        r.StrokeRoundRect(RectF{b.l + hh, b.t + hh, b.r - hh, b.b - hh}, t.controlRadius, t.chipStroke, h);
    }
    if (f.a() > 0) r.FillRoundRect(b, t.controlRadius, f);
    Color tc = !en ? t.textDisabled : prs ? t.textSecondary : (chipStyle_ && !selected_) ? t.textSecondary : t.textPrimary;
    float x = b.l + kTextBtnPad, right = b.r - kTextBtnPad;
    if (!glyph_.empty()) {
        r.Text(glyph_, RectF{x, b.t, x + kTextBtnGlyphCol, b.b}, IconStyle(14), tc);
        x += kTextBtnGlyphCol + kTextBtnGap;
    }
    pui::TextStyle ts;
    ts.size = fontSize_;
    ts.weight = selected_ && !chipStyle_ ? 600 : 400;
    ts.ellipsis = true;
    r.Text(label_, RectF{x, b.t, std::max(x, right), b.b}, ts, tc);
    if (selected_ && !chipStyle_) r.FillRoundRect(RectF{b.l + kTextBtnPad, b.b - 3, b.r - kTextBtnPad, b.b - 1}, 1.f, t.accent);   // 2 DIP underline
    PaintFocusVisual(r, t, b, t.controlRadius);
    if (fade_.Active(now) && host) host->StartAnimating(this);
}

bool TextButton::OnTick(uint64_t nowMs) { return fade_.Active(nowMs); }

bool TextButton::OnPointerDown(const pui::PointerEvent& e) { return e.button == pui::PointerButton::Left; }

bool TextButton::OnPointerUp(const pui::PointerEvent& e) {
    if (e.button == pui::PointerButton::Left && Bounds().Contains(e.pt)) Invoke();
    return true;
}

bool TextButton::OnContextMenu(pui::PointF, bool) {
    if (!onRightClick || !IsEffectivelyEnabled()) return false;
    onRightClick();
    return true;
}

bool TextButton::OnKeyDown(int vk, unsigned mods) {
    if (mods == 0 && (vk == pui::key::Enter || vk == pui::key::Space)) { Invoke(); return true; }
    return false;
}

void TextButton::Invoke() {
    if (!IsEffectivelyEnabled()) return;
    Raise(pui::UiEvent::Invoked);
    if (onInvoke) onInvoke();
}

Chip::Chip(std::wstring text) : pui::Widget(pui::Role::Text), text_(std::move(text)) {
    SetName(text_);
    SetHitTestVisible(false);
}

void Chip::SetText(const std::wstring& text) {
    if (text == text_) return;
    text_ = text;
    SetName(text_);
    Invalidate();
}

float Chip::WidthFor(pui::IRenderer& r, const std::wstring& text) {
    pui::TextStyle ts;
    ts.size = 11;
    ts.ellipsis = false;
    return std::ceil(r.MeasureText(text, ts).w) + 2 * kChipPadDip + 2;
}

void Chip::Paint(pui::IRenderer& r, const pui::Theme& t) {
    const RectF& b = Bounds();
    const float hh = t.hairline / 2;
    r.StrokeRoundRect(RectF{b.l + hh, b.t + hh, b.r - hh, b.b - hh}, t.controlRadius, t.chipStroke, t.hairline);
    pui::TextStyle ts;
    ts.size = 11;
    ts.align = pui::TextAlign::Center;
    ts.ellipsis = false;
    r.Text(text_, b, ts, IsEffectivelyEnabled() ? t.textSecondary : t.textDisabled);
}

ShellSlider::ShellSlider(std::wstring name) : pui::Widget(pui::Role::Slider) {
    SetName(std::move(name));
    SetFocusable(true);
}

void ShellSlider::SetFraction(double f) {
    f = std::clamp(f, 0.0, 1.0);
    if (dragging_) return;
    if (f == frac_) return;
    frac_ = f;
    Raise(pui::UiEvent::ValueChanged);
    Invalidate();
}

RectF ShellSlider::Track() const {
    const RectF& b = Bounds();
    constexpr float kInset = 10;
    return {b.l + kInset, b.cy() - 2, std::max(b.l + kInset + 1, b.r - kInset), b.cy() + 2};
}

double ShellSlider::FractionAt(float x) const {
    RectF t = Track();
    return std::clamp((double)(x - t.l) / std::max(1.0, (double)t.w()), 0.0, 1.0);
}

void ShellSlider::DrawSlider(pui::IRenderer& r, const pui::Theme& t, const RectF& track, double frac, bool enabled, bool hover, bool pressed) {
    const float rad = 2;
    r.FillRoundRect(track, rad, enabled ? t.ctlStrongFill : t.ctlStrongFillDisabled);
    if (!enabled) return;
    float f = (float)std::clamp(frac, 0.0, 1.0);
    float cx = track.l + f * track.w(), cy = track.cy();
    if (cx > track.l) r.FillRoundRect({track.l, track.t, std::max(cx, track.l + 1), track.b}, rad, t.accent);
    float ro = 10;
    r.FillEllipse({cx, cy}, ro, ro, t.ctlSolidFill);
    r.StrokeEllipse({cx, cy}, ro - 0.5f, ro - 0.5f, t.ctlStrokeSecondary, 1);
    float ri = pressed ? 4.f : hover ? 6.f : 5.f;
    r.FillEllipse({cx, cy}, ri, ri, pui::MulAlpha(t.accent, pressed ? 0.8f : hover ? 0.9f : 1.f));
}

void ShellSlider::Paint(pui::IRenderer& r, const pui::Theme& t) {
    bool en = IsEffectivelyEnabled();
    DrawSlider(r, t, Track(), Fraction(), en, IsHovered() && en, (IsPressed() || dragging_) && en);
    if (en) {
        RectF tr = Track();
        float cx = tr.l + (float)Fraction() * tr.w();
        if (ShowFocusVisual()) PaintFocusVisual(r, t, RectF{cx - 11, Bounds().cy() - 11, cx + 11, Bounds().cy() + 11}, 11);
    }
}

bool ShellSlider::OnPointerDown(const pui::PointerEvent& e) {
    if (e.button != pui::PointerButton::Left || !IsEffectivelyEnabled()) return false;
    dragging_ = true;
    dragFrac_ = FractionAt(e.pt.x);
    if (onChanging) onChanging(dragFrac_);
    Invalidate();
    return true;
}

bool ShellSlider::OnPointerMove(const pui::PointerEvent& e) {
    if (!dragging_) return false;
    dragFrac_ = FractionAt(e.pt.x);
    if (onChanging) onChanging(dragFrac_);
    Invalidate();
    return true;
}

bool ShellSlider::OnPointerUp(const pui::PointerEvent& e) {
    if (!dragging_) return false;
    dragging_ = false;
    dragFrac_ = FractionAt(e.pt.x);
    frac_ = dragFrac_;
    Raise(pui::UiEvent::ValueChanged);
    if (onCommit) onCommit(frac_);
    Invalidate();
    return true;
}

bool ShellSlider::OnPointerWheel(const pui::PointerEvent& e) {
    if (!onWheelSteps || !IsEffectivelyEnabled()) return false;
    wheelAcc_ += e.wheelDelta;
    int n = (int)std::trunc(wheelAcc_);
    wheelAcc_ -= (float)n;
    if (n != 0) onWheelSteps(n);
    return true;
}

bool ShellSlider::OnKeyDown(int vk, unsigned mods) {
    if (mods & (pui::ModCtrl | pui::ModAlt)) return false;
    double f = Fraction(), n = f;
    switch (vk) {
    case pui::key::Left: case pui::key::Down: n = f - small_; break;
    case pui::key::Right: case pui::key::Up: n = f + small_; break;
    case pui::key::PageDown: n = f - large_; break;
    case pui::key::PageUp: n = f + large_; break;
    case pui::key::Home: n = 0; break;
    case pui::key::End: n = 1; break;
    default: return false;
    }
    n = std::clamp(n, 0.0, 1.0);
    frac_ = n;
    Raise(pui::UiEvent::ValueChanged);
    if (onCommit) onCommit(n);
    Invalidate();
    return true;
}

pui::RangeInfo ShellSlider::GetRange() const {
    pui::RangeInfo ri;
    ri.minimum = 0;
    ri.maximum = scale_;
    ri.value = Fraction() * scale_;
    ri.smallChange = small_ * scale_;
    ri.largeChange = large_ * scale_;
    ri.readOnly = !IsEffectivelyEnabled();
    return ri;
}

void ShellSlider::SetRangeValue(double v) {
    if (!IsEffectivelyEnabled()) return;
    double f = std::clamp(v / scale_, 0.0, 1.0);
    frac_ = f;
    Raise(pui::UiEvent::ValueChanged);
    if (onCommit) onCommit(f);
    Invalidate();
}

std::wstring ShellSlider::GetValue() const {
    if (valueText) return valueText(Fraction());
    return std::to_wstring((int)std::lround(Fraction() * scale_));
}

}
