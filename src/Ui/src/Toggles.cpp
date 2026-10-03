#include "pui/Toggles.h"

#include <algorithm>

#include "pui/UiText.h"

namespace pui {

namespace {
Color LabelColor(const Theme& t, bool en, bool pr) { return !en ? t.textDisabled : pr ? t.textSecondary : t.textPrimary; }
std::wstring OnLabel() { return UiText(L"On", L"켜짐"); }
std::wstring OffLabel() { return UiText(L"Off", L"꺼짐"); }
}


ToggleSwitch::ToggleSwitch() : ButtonBase(Role::ToggleButton) {}

void ToggleSwitch::SetOn(bool on) {
    if (on == on_) return;
    on_ = on;
    Raise(UiEvent::ToggleChanged);
    Invalidate();
    if (onToggled) onToggled(on_);
}

RectF ToggleSwitch::TrackRect() const {
    RectF b = Bounds();
    return RectF::XYWH(b.l, b.cy() - kTrackH * 0.5f, kTrackW, kTrackH);
}

SizeF ToggleSwitch::DesiredSize(IRenderer* m) {
    IRenderer& mr = MeasurerOr(m);
    float w = std::max(mr.MeasureText(OnLabel(), BodyTextStyle(14)).w, mr.MeasureText(OffLabel(), BodyTextStyle(14)).w);
    return {kTrackW + kGap + w, kHeight};
}

void ToggleSwitch::Paint(IRenderer& r, const Theme& t) {
    RectF tr = TrackRect();
    bool en = IsEffectivelyEnabled(), pr = VisuallyPressed(), hv = IsHovered();
    float rad = kTrackH * 0.5f;
    Color knob;
    if (on_) {
        Color fill = !en ? t.ctlStrongFillDisabled : pr ? t.accentPressed : hv ? t.accentHover : t.accent;
        r.FillRoundRect(tr, rad, fill);
        knob = en ? t.textOnAccent : t.textDisabled;
    } else {
        r.FillRoundRect(tr, rad, t.ctlAltSecondary);
        r.StrokeRoundRect(tr.Deflate(0.5f, 0.5f, 0.5f, 0.5f), rad - 0.5f, en ? t.ctlStrongStroke : t.ctlStrongStrokeDisabled, 1);
        knob = en ? t.textSecondary : t.textDisabled;
    }
    float kr = pr ? 7.f : hv ? 7.f : 6.f;
    float cx = on_ ? tr.r - 10.f : tr.l + 10.f;
    r.FillEllipse({cx, tr.cy()}, kr, kr, knob);
    RectF lb = {tr.r + kGap, Bounds().t, Bounds().r, Bounds().b};
    r.Text(on_ ? OnLabel() : OffLabel(), lb, BodyTextStyle(14), LabelColor(t, en, false));
    PaintFocusVisual(r, t, Bounds(), t.controlRadius);
}


CheckBox::CheckBox() : ButtonBase(Role::CheckBox) {}
CheckBox::CheckBox(std::wstring text) : ButtonBase(Role::CheckBox) { SetText(std::move(text)); }

void CheckBox::SetText(std::wstring t) {
    if (t == text_) return;
    std::wstring old = std::move(text_);
    text_ = std::move(t);
    SyncAutoName(*this, old, text_);
    RelayoutAncestors(*this);
    Invalidate();
}

void CheckBox::SetChecked(bool c) {
    if (c == checked_) return;
    checked_ = c;
    Raise(UiEvent::ToggleChanged);
    Invalidate();
    if (onToggled) onToggled(checked_);
}

SizeF CheckBox::DesiredSize(IRenderer* m) {
    float w = kBox;
    if (!text_.empty()) w += kGap + MeasurerOr(m).MeasureText(text_, BodyTextStyle(14)).w;
    return {w, kHeight};
}

void CheckBox::Paint(IRenderer& r, const Theme& t) {
    RectF b = Bounds();
    bool en = IsEffectivelyEnabled(), pr = VisuallyPressed(), hv = IsHovered();
    RectF box = RectF::XYWH(b.l, b.cy() - kBox * 0.5f, kBox, kBox);
    if (checked_) {
        Color fill = !en ? t.ctlStrongFillDisabled : pr ? t.accentPressed : hv ? t.accentHover : t.accent;
        r.FillRoundRect(box, t.controlRadius, fill);
        r.Text(glyph::Str(glyph::CheckMark), box, IconTextStyle(12), en ? t.textOnAccent : t.textDisabled);
    } else {
        r.FillRoundRect(box, t.controlRadius, pr ? t.ctlTertiary : hv ? t.ctlSecondary : t.ctlAltSecondary);
        r.StrokeRoundRect(box.Deflate(0.5f, 0.5f, 0.5f, 0.5f), t.controlRadius, en ? t.ctlStrongStroke : t.ctlStrongStrokeDisabled, 1);
    }
    if (!text_.empty())
        r.Text(text_, {box.r + kGap, b.t, b.r, b.b}, BodyTextStyle(14), LabelColor(t, en, false));
    PaintFocusVisual(r, t, b, t.controlRadius);
}


RadioButton::RadioButton() : ButtonBase(Role::RadioButton) {}
RadioButton::RadioButton(std::wstring text) : ButtonBase(Role::RadioButton) { SetText(std::move(text)); }

void RadioButton::SetText(std::wstring t) {
    if (t == text_) return;
    std::wstring old = std::move(text_);
    text_ = std::move(t);
    SyncAutoName(*this, old, text_);
    RelayoutAncestors(*this);
    Invalidate();
}

RadioGroup* RadioButton::Group() const { return dynamic_cast<RadioGroup*>(Parent()); }

SizeF RadioButton::DesiredSize(IRenderer* m) {
    float w = kDot;
    if (!text_.empty()) w += kGap + MeasurerOr(m).MeasureText(text_, BodyTextStyle(14)).w;
    return {w, kHeight};
}

void RadioButton::SetCheckedInternal(bool c) {
    if (c == checked_) return;
    checked_ = c;
    Raise(UiEvent::SelectionChanged);
    Invalidate();
}

void RadioButton::Activate() { SelectItem(); }

void RadioButton::SelectItem() {
    if (!IsEffectivelyEnabled()) return;
    if (RadioGroup* g = Group()) { g->Select(this, true); return; }
    if (!checked_) { SetCheckedInternal(true); SetFocusable(true); }
    Focus();
}

void RadioButton::RemoveFromSelection() {
    if (RadioGroup* g = Group()) { if (checked_) g->SetSelectedIndex(-1); }
    else SetCheckedInternal(false);
}

bool RadioButton::OnKeyDown(int vk, unsigned mods) {
    if (!(mods & (ModCtrl | ModAlt))) {
        if (RadioGroup* g = Group()) {
            switch (vk) {
            case key::Up: case key::Left: g->MoveSelection(-1); return true;
            case key::Down: case key::Right: g->MoveSelection(+1); return true;
            default: break;
            }
        }
    }
    return ButtonBase::OnKeyDown(vk, mods);
}

void RadioButton::Paint(IRenderer& r, const Theme& t) {
    RectF b = Bounds();
    bool en = IsEffectivelyEnabled(), pr = VisuallyPressed(), hv = IsHovered();
    PointF c{b.l + kDot * 0.5f, b.cy()};
    float R = kDot * 0.5f;
    if (checked_) {
        Color fill = !en ? t.ctlStrongFillDisabled : pr ? t.accentPressed : hv ? t.accentHover : t.accent;
        r.FillEllipse(c, R, R, fill);
        float ir = pr ? 4.f : hv ? 6.f : 5.f;
        r.FillEllipse(c, ir, ir, en ? t.textOnAccent : t.textDisabled);
    } else {
        r.FillEllipse(c, R, R, pr ? t.ctlTertiary : hv ? t.ctlSecondary : t.ctlAltSecondary);
        r.StrokeEllipse(c, R - 0.5f, R - 0.5f, en ? t.ctlStrongStroke : t.ctlStrongStrokeDisabled, 1);
    }
    if (!text_.empty())
        r.Text(text_, {b.l + kDot + kGap, b.t, b.r, b.b}, BodyTextStyle(14), LabelColor(t, en, false));
    PaintFocusVisual(r, t, b, t.controlRadius);
}


RadioGroup::RadioGroup() : Widget(Role::Group) {}

RadioButton* RadioGroup::AddOption(std::wstring text) {
    RadioButton* rb = Add<RadioButton>(std::move(text));
    rb->SetFocusable(false);
    UpdateRoving();
    RequestLayout();
    return rb;
}

int RadioGroup::OptionCount() const { return (int)Children().size(); }

RadioButton* RadioGroup::Option(int i) const {
    if (i < 0 || i >= (int)Children().size()) return nullptr;
    return dynamic_cast<RadioButton*>(Children()[(size_t)i].get());
}

int RadioGroup::SelectedIndex() const {
    for (int i = 0; i < OptionCount(); i++)
        if (RadioButton* r = Option(i); r && r->checked_) return i;
    return -1;
}

std::vector<Widget*> RadioGroup::SelectedChildren() const {
    std::vector<Widget*> v;
    if (RadioButton* r = Option(SelectedIndex())) v.push_back(r);
    return v;
}

void RadioGroup::UpdateRoving() {
    int sel = SelectedIndex();
    int tabStop = sel;
    if (tabStop < 0)
        for (int i = 0; i < OptionCount(); i++)
            if (RadioButton* r = Option(i); r && r->Enabled()) { tabStop = i; break; }
    for (int i = 0; i < OptionCount(); i++)
        if (RadioButton* r = Option(i)) r->SetFocusable(i == tabStop);
}

void RadioGroup::Select(RadioButton* rb, bool focus) {
    int old = SelectedIndex();
    int idx = rb ? rb->IndexInParent() : -1;
    if (rb && idx < 0) return;
    for (int i = 0; i < OptionCount(); i++)
        if (RadioButton* r = Option(i)) r->SetCheckedInternal(r == rb);
    UpdateRoving();
    if (focus && rb) {
        if (UiHost* h = Host()) h->SetFocus(rb, h->FocusVisible());
    }
    if (idx != old) {
        Raise(UiEvent::SelectionChanged);
        Invalidate();
        if (onSelectionChanged) onSelectionChanged(idx);
    }
}

void RadioGroup::SetSelectedIndex(int i) { Select(i < 0 ? nullptr : Option(i), false); }

void RadioGroup::MoveSelection(int dir) {
    int n = OptionCount();
    if (n == 0) return;
    int cur = SelectedIndex();
    if (UiHost* h = Host())
        for (int i = 0; i < n; i++)
            if (Option(i) == h->Focused()) { cur = i; break; }
    if (cur < 0) cur = dir > 0 ? -1 : 0;
    for (int step = 1; step <= n; step++) {
        int j = ((cur + dir * step) % n + n) % n;
        RadioButton* r = Option(j);
        if (r && r->Enabled()) {
            if (UiHost* h = Host()) h->SetFocusVisible(true);
            Select(r, true);
            return;
        }
    }
}

void RadioGroup::OnLayout() {
    RectF b = Bounds();
    float x = b.l, y = b.t;
    for (auto& c : Children()) {
        if (!c->Visible()) continue;
        SizeF d = c->DesiredSize(nullptr);
        if (horizontal_) { c->SetBounds(RectF::XYWH(x, y, d.w, d.h)); x += d.w + spacing_ + 16; }
        else { c->SetBounds(RectF::XYWH(x, y, std::max(d.w, b.w()), d.h)); y += d.h + spacing_; }
    }
}

SizeF RadioGroup::DesiredSize(IRenderer* m) {
    float w = 0, h = 0;
    for (auto& c : Children()) {
        if (!c->Visible()) continue;
        SizeF d = c->DesiredSize(m);
        if (horizontal_) { w += d.w + spacing_ + 16; h = std::max(h, d.h); }
        else { w = std::max(w, d.w); h += d.h + spacing_; }
    }
    return {w, h};
}

}
