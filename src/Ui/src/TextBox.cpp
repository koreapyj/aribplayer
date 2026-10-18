#include "pui/TextBox.h"

#include <algorithm>

#include "pui/Clipboard.h"
#include "pui/Glyphs.h"

namespace pui {

namespace {
constexpr int kVkC = 0x43, kVkV = 0x56, kVkX = 0x58;
constexpr float kPadL = 10, kPadR = 8, kGlyphSize = 14, kGlyphGap = 8, kCharGuess = 7;
}

TextBox::TextBox() : Widget(Role::Edit) {
    SetFocusable(true);
}

bool TextBox::IsTextInputFocused(const UiHost& host) { return dynamic_cast<TextBox*>(host.Focused()) != nullptr; }

RectF TextBox::TextArea() const {
    RectF b = Bounds();
    float l = b.l + kPadL + kGlyphSize + kGlyphGap;
    return {l, b.t + 1, std::max(l, b.r - kPadR), b.b - 2};
}

float TextBox::XOf(size_t idx) const {
    float a = idx < adv_.size() ? adv_[idx] : (adv_.empty() ? (float)idx * kCharGuess : adv_.back());
    return TextArea().l - scrollX_ + a;
}

size_t TextBox::IndexAt(float x) const {
    size_t best = 0;
    float bestD = 1e9f;
    for (size_t i = 0; i <= text_.size(); i++) {
        float d = std::fabs(XOf(i) - x);
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

void TextBox::Measure(IRenderer& r, const Theme& t) {
    if (!advDirty_ && adv_.size() == text_.size() + 1) return;
    TextStyle st;
    st.size = t.fontBody;
    st.ellipsis = false;
    adv_.assign(text_.size() + 1, 0.f);
    for (size_t i = 1; i <= text_.size(); i++)
        adv_[i] = r.MeasureText(std::wstring_view(text_).substr(0, i), st).w;
    advDirty_ = false;
}

void TextBox::EnsureCaretVisible() {
    RectF ta = TextArea();
    float cx = caret_ < adv_.size() ? adv_[caret_] : (float)caret_ * kCharGuess;
    float total = adv_.empty() ? 0.f : adv_.back();
    if (cx - scrollX_ > ta.w() - 1) scrollX_ = cx - ta.w() + 1;
    if (cx - scrollX_ < 0) scrollX_ = cx;
    scrollX_ = std::clamp(scrollX_, 0.f, std::max(0.f, total - ta.w() + 2));
}

void TextBox::Paint(IRenderer& r, const Theme& t) {
    Measure(r, t);
    EnsureCaretVisible();
    RectF b = Bounds();
    bool focused = IsFocused();
    bool dis = !IsEffectivelyEnabled();
    Color fill = dis ? t.ctlDisabled : focused ? t.ctlInputActive : IsHovered() ? t.ctlSecondary : t.ctlDefault;
    r.FillRoundRect(b, t.controlRadius, fill);
    r.StrokeRoundRect(b.Deflate(0.5f, 0.5f, 0.5f, 0.5f), t.controlRadius, t.ctlStrokeDefault, 1);
    TextStyle gs;
    gs.font = FontKind::Icon;
    gs.size = kGlyphSize;
    gs.align = TextAlign::Center;
    gs.ellipsis = false;
    r.Text(glyph::Str(glyph::Search), {b.l + kPadL, b.t, b.l + kPadL + kGlyphSize, b.b}, gs, t.textSecondary);
    RectF ta = TextArea();
    TextStyle st;
    st.size = t.fontBody;
    st.ellipsis = false;
    r.PushClip(ta);
    if (text_.empty()) {
        r.Text(placeholder_, ta, st, dis ? t.textDisabled : t.textTertiary);
    } else {
        if (focused && caret_ != anchor_) {
            float x0 = XOf(SelectionStart()), x1 = XOf(SelectionEnd());
            r.FillRect({x0, ta.t + 5, x1, ta.b - 5}, WithAlpha(t.accent, 0.4f));
        }
        r.Text(text_, {ta.l - scrollX_, ta.t, ta.l - scrollX_ + adv_.back() + 4, ta.b}, st, dis ? t.textDisabled : t.textPrimary);
    }
    if (focused) {
        float x = XOf(caret_);
        r.Line({x, ta.t + 6}, {x, ta.b - 6}, t.textPrimary, 1);
    }
    r.PopClip();
    if (focused) r.Line({b.l + 3, b.b - 1}, {b.r - 3, b.b - 1}, t.accent, 2);
}

void TextBox::SetSelection(size_t anchor, size_t caret) {
    anchor_ = std::min(anchor, text_.size());
    caret_ = std::min(caret, text_.size());
    Invalidate();
}

void TextBox::SetText(std::wstring t) {
    if (t == text_) return;
    text_ = std::move(t);
    caret_ = anchor_ = text_.size();
    advDirty_ = true;
    Raise(UiEvent::ValueChanged);
    Invalidate();
}

void TextBox::SetValue(const std::wstring& v) {
    if (v == text_) return;
    SetText(v);
    if (onChanged) onChanged(text_);
}

void TextBox::Changed() {
    advDirty_ = true;
    Raise(UiEvent::ValueChanged);
    Invalidate();
    if (onChanged) onChanged(text_);
}

void TextBox::Move(size_t to, bool extend) {
    caret_ = std::min(to, text_.size());
    if (!extend) anchor_ = caret_;
    Invalidate();
}

void TextBox::EraseRange(size_t a, size_t b) {
    text_.erase(a, b - a);
    caret_ = anchor_ = a;
}

void TextBox::Insert(std::wstring_view s) {
    if (s.empty()) return;
    std::wstring t = text_;
    size_t pos = SelectionStart();
    t.erase(pos, SelectionEnd() - pos);
    t.insert(pos, s);
    text_ = std::move(t);
    caret_ = anchor_ = pos + s.size();
    Changed();
}

bool TextBox::OnChar(wchar_t ch) {
    if (!IsEffectivelyEnabled() || ch < 0x20 || ch == 0x7F) return false;
    Insert(std::wstring_view(&ch, 1));
    return true;
}

bool TextBox::OnKeyDown(int vk, unsigned mods) {
    bool shift = (mods & ModShift) != 0, ctrl = (mods & ModCtrl) != 0;
    if (mods & ModAlt) return false;
    switch (vk) {
    case key::Left:
        if (!shift && caret_ != anchor_) Move(SelectionStart(), false);
        else Move(caret_ > 0 ? caret_ - 1 : 0, shift);
        return true;
    case key::Right:
        if (!shift && caret_ != anchor_) Move(SelectionEnd(), false);
        else Move(caret_ + 1, shift);
        return true;
    case key::Home: Move(0, shift); return true;
    case key::End: Move(text_.size(), shift); return true;
    case key::Back:
        if (caret_ != anchor_) EraseRange(SelectionStart(), SelectionEnd());
        else if (caret_ > 0) EraseRange(caret_ - 1, caret_);
        else return true;
        Changed();
        return true;
    case key::Delete:
        if (caret_ != anchor_) EraseRange(SelectionStart(), SelectionEnd());
        else if (caret_ < text_.size()) EraseRange(caret_, caret_ + 1);
        else return true;
        Changed();
        return true;
    case key::Escape:
        if (text_.empty()) return false;
        text_.clear();
        caret_ = anchor_ = 0;
        Changed();
        return true;
    case key::Enter:
        if (!onSubmit) return false;
        onSubmit(text_);
        return true;
    default: break;
    }
    if (!ctrl) return false;
    if (vk == key::A) { SelectAll(); return true; }
    if (vk == kVkC || vk == kVkX) {
        if (caret_ != anchor_) {
            SetClipboardText(text_.substr(SelectionStart(), SelectionEnd() - SelectionStart()));
            if (vk == kVkX) { EraseRange(SelectionStart(), SelectionEnd()); Changed(); }
        }
        return true;
    }
    if (vk == kVkV) {
        std::wstring clean;
        for (wchar_t c : ClipboardText()) clean += (c < 0x20) ? L' ' : c;   // single line: CR/LF/Tab become spaces
        Insert(clean);
        return true;
    }
    return false;
}

bool TextBox::OnPointerDown(const PointerEvent& e) {
    if (e.button != PointerButton::Left) return false;
    if (e.clickCount >= 2) { SelectAll(); selecting_ = false; return true; }
    Move(IndexAt(e.pt.x), (e.mods & ModShift) != 0);
    selecting_ = true;
    return true;
}

bool TextBox::OnPointerMove(const PointerEvent& e) {
    if (!selecting_) return false;
    Move(IndexAt(e.pt.x), true);
    return true;
}

bool TextBox::OnPointerUp(const PointerEvent& e) {
    (void)e;
    selecting_ = false;
    return true;
}

}
