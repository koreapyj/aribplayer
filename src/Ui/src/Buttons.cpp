#include "pui/Buttons.h"

#include <algorithm>

namespace pui {


namespace {
IRenderer* g_measurer = nullptr;
}
IRenderer* DefaultMeasurer() {
    static NullMeasurer fallback;
    return g_measurer ? g_measurer : &fallback;
}
void SetDefaultMeasurer(IRenderer* r) { g_measurer = r; }


ButtonBase::ButtonBase(Role r) : Widget(r) { SetFocusable(true); }

bool ButtonBase::OnPointerDown(const PointerEvent& e) { return e.button == PointerButton::Left; }

bool ButtonBase::OnPointerUp(const PointerEvent& e) {
    if (e.button != PointerButton::Left) return false;
    if (Bounds().Contains(e.pt) && IsEffectivelyEnabled()) Activate();   // may destroy this: touch nothing after
    return true;
}

bool ButtonBase::OnKeyDown(int vk, unsigned mods) {
    if (mods & (ModCtrl | ModAlt)) return false;
    if (vk == key::Enter) { Activate(); return true; }
    if (vk == key::Space) {
        if (!spaceDown_) { spaceDown_ = true; Invalidate(); }
        return true;
    }
    return false;
}

bool ButtonBase::OnKeyUp(int vk, unsigned) {
    if (vk == key::Space && spaceDown_) {
        spaceDown_ = false;
        Invalidate();
        Activate();
        return true;
    }
    return false;
}

void ButtonBase::OnFocusChanged(bool focused) {
    if (!focused && spaceDown_) { spaceDown_ = false; Invalidate(); }
}


Button::Button() : ButtonBase(Role::Button) {}
Button::Button(std::wstring text, ButtonStyle style) : ButtonBase(Role::Button), style_(style) { SetText(std::move(text)); }

void Button::SetText(std::wstring t) {
    if (t == text_) return;
    std::wstring old = std::move(text_);
    text_ = std::move(t);
    SyncAutoName(*this, old, text_);
    RelayoutAncestors(*this);
    Invalidate();
}

void Button::SetGlyph(wchar_t g) { glyph_ = g; Invalidate(); }

SizeF Button::DesiredSize(IRenderer* m) {
    float w = 0;
    if (glyph_) w += 16;
    if (glyph_ && !text_.empty()) w += 8;
    if (!text_.empty()) w += MeasurerOr(m).MeasureText(text_, BodyTextStyle(14)).w;
    return {std::max(32.f, w + 22), 32};
}

void Button::Paint(IRenderer& r, const Theme& t) {
    RectF b = Bounds();
    bool en = IsEffectivelyEnabled(), pr = VisuallyPressed(), accent = AccentLook();
    Color fill, fg;
    if (accent) {
        fill = !en ? t.ctlStrongFillDisabled : pr ? t.accentPressed : IsHovered() ? t.accentHover : t.accent;
        fg = !en ? t.textDisabled : pr ? MulAlpha(t.textOnAccent, 0.8f) : t.textOnAccent;
    } else {
        fill = !en ? t.ctlDisabled : pr ? t.ctlTertiary : IsHovered() ? t.ctlSecondary : t.ctlDefault;
        fg = !en ? t.textDisabled : pr ? t.textSecondary : t.textPrimary;
    }
    r.FillRoundRect(b, t.controlRadius, fill);
    if (!accent) r.StrokeRoundRect(b.Deflate(0.5f, 0.5f, 0.5f, 0.5f), t.controlRadius, t.ctlStrokeDefault, 1);
    TextStyle ts = BodyTextStyle(14, 400, TextAlign::Leading);
    float tw = text_.empty() ? 0 : MeasurerOr(&r).MeasureText(text_, ts).w;
    float total = tw + (glyph_ ? 16.f + (text_.empty() ? 0.f : 8.f) : 0.f);
    float x = b.cx() - total * 0.5f;
    if (glyph_) {
        r.Text(glyph::Str(glyph_), RectF::XYWH(x, b.t, 16, b.h()), IconTextStyle(16), fg);
        x += 24;
    }
    if (!text_.empty()) r.Text(text_, {x, b.t, std::max(x, b.r - 4), b.b}, ts, fg);
    PaintFocusVisual(r, t, b, t.controlRadius);
}

void Button::Invoke() { if (IsEffectivelyEnabled()) Activate(); }

void Button::Activate() {
    Raise(UiEvent::Invoked);
    if (onClick) onClick();
}


IconButton::IconButton() : ButtonBase(Role::Button) {}
IconButton::IconButton(wchar_t glyph, std::wstring label) : ButtonBase(Role::Button), glyph_(glyph) {
    if (!label.empty()) SetLabel(std::move(label));
}

void IconButton::SetLabel(std::wstring label) {
    SetToolTip(label);
    SetName(std::move(label));
}

void IconButton::Paint(IRenderer& r, const Theme& t) {
    RectF b = Bounds();
    bool en = IsEffectivelyEnabled(), pr = VisuallyPressed();
    float rad = std::min(b.w(), b.h()) * 0.5f;
    Color fill;
    if (controlFill_) fill = !en ? t.ctlDisabled : pr ? t.ctlTertiary : IsHovered() ? t.ctlSecondary : t.ctlDefault;
    else fill = !en ? t.subtleDisabled : pr ? t.subtleTertiary : IsHovered() ? t.subtleSecondary : t.subtleTransparent;
    if (fill.v >> 24) r.FillRoundRect(b, rad, fill);
    Color fg = !en ? t.textDisabled : pr ? t.textSecondary : t.textPrimary;
    if (glyph_) r.Text(glyph::Str(glyph_), b, IconTextStyle(glyphSize_), fg);
    PaintFocusVisual(r, t, b, rad);
}

void IconButton::Invoke() { if (IsEffectivelyEnabled()) Activate(); }
void IconButton::Activate() {
    Raise(UiEvent::Invoked);
    if (onClick) onClick();
}


ToggleButton::ToggleButton() { SetRole(Role::ToggleButton); }
ToggleButton::ToggleButton(std::wstring text) : Button(std::move(text)) { SetRole(Role::ToggleButton); }

void ToggleButton::SetChecked(bool c) {
    if (c == checked_) return;
    checked_ = c;
    Raise(UiEvent::ToggleChanged);
    Invalidate();
    if (onToggled) onToggled(checked_);
}

void ToggleButton::Toggle() { if (IsEffectivelyEnabled()) SetChecked(!checked_); }
void ToggleButton::Activate() { SetChecked(!checked_); }


HyperlinkButton::HyperlinkButton() : ButtonBase(Role::Hyperlink) {}
HyperlinkButton::HyperlinkButton(std::wstring text) : ButtonBase(Role::Hyperlink) { SetText(std::move(text)); }

void HyperlinkButton::SetText(std::wstring t) {
    if (t == text_) return;
    std::wstring old = std::move(text_);
    text_ = std::move(t);
    SyncAutoName(*this, old, text_);
    RelayoutAncestors(*this);
    Invalidate();
}

SizeF HyperlinkButton::DesiredSize(IRenderer* m) {
    return {MeasurerOr(m).MeasureText(text_, BodyTextStyle(14)).w + 22, 32};
}

void HyperlinkButton::Paint(IRenderer& r, const Theme& t) {
    RectF b = Bounds();
    bool en = IsEffectivelyEnabled(), pr = VisuallyPressed();
    Color fg = !en ? t.textDisabled : pr ? t.accentPressed : IsHovered() ? t.accentHover : t.accent;
    TextStyle ts = BodyTextStyle(14, 400, TextAlign::Center);
    r.Text(text_, b, ts, fg);
    if (IsHovered() && en) {
        float tw = MeasurerOr(&r).MeasureText(text_, ts).w;
        float y = b.cy() + 9;
        r.Line({b.cx() - tw * 0.5f, y}, {b.cx() + tw * 0.5f, y}, fg, 1);
    }
    PaintFocusVisual(r, t, b, t.controlRadius);
}

void HyperlinkButton::Invoke() { if (IsEffectivelyEnabled()) Activate(); }
void HyperlinkButton::Activate() {
    Raise(UiEvent::Invoked);
    if (onClick) onClick();
}

}
