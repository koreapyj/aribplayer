#include "pui/SelectableText.h"

#include <algorithm>

#include "pui/Clipboard.h"

namespace pui {

namespace {
constexpr int kVkA = 0x41, kVkC = 0x43, kVkInsert = 0x2D;

TextStyle Plain(TextStyle s) {
    s.wrap = false;
    s.ellipsis = false;
    s.valign = ParaAlign::Top;
    return s;
}

bool BreaksAfter(wchar_t c) { return c == L' ' || c == L'\\' || c == L'/' || c == L'-' || c == L'_' || c == L'.' || c == L',' || c == L';'; }
}

SelectableText::SelectableText(std::wstring text, TextStyle style) : Widget(Role::Edit), text_(std::move(text)), style_(Plain(style)) {
    SetFocusable(true);
}

std::vector<SelectableText::Line> SelectableText::Wrap(IRenderer& r, std::wstring_view text, const TextStyle& style, float width) {
    const TextStyle st = Plain(style);
    std::vector<Line> out;
    size_t pos = 0;
    while (true) {
        size_t nl = text.find(L'\n', pos);
        size_t paraEnd = nl == std::wstring_view::npos ? text.size() : nl;
        size_t lineEnd = paraEnd;
        if (lineEnd > pos && text[lineEnd - 1] == L'\r') lineEnd--;
        size_t start = pos;
        while (true) {
            if (r.MeasureText(text.substr(start, lineEnd - start), st).w <= width || lineEnd - start <= 1) {
                out.push_back({start, lineEnd});
                break;
            }
            size_t lo = 1, hi = lineEnd - start;   // largest n in [1, hi) that fits
            while (hi - lo > 1) {
                size_t mid = (lo + hi) / 2;
                if (r.MeasureText(text.substr(start, mid), st).w <= width) lo = mid; else hi = mid;
            }
            size_t n = lo;
            for (size_t k = n; k > n / 2 && k > 0; k--)
                if (BreaksAfter(text[start + k - 1])) { n = k; break; }
            out.push_back({start, start + n});
            start += n;
        }
        if (nl == std::wstring_view::npos) break;
        pos = nl + 1;
    }
    return out;
}

float SelectableText::HeightFor(IRenderer& r, std::wstring_view text, const TextStyle& style, float width) {
    const float lh = r.MeasureText(L"Ag", Plain(style)).h;
    return lh * (float)Wrap(r, text, style, width).size();
}

void SelectableText::Layout(IRenderer& r) {
    const float w = Bounds().w();
    if (w == laidWidth_ && !lines_.empty()) return;
    laidWidth_ = w;
    lineH_ = r.MeasureText(L"Ag", style_).h;
    lines_ = Wrap(r, text_, style_, w);
    adv_.assign(lines_.size(), {});
    for (size_t li = 0; li < lines_.size(); li++) {
        const Line& ln = lines_[li];
        auto& a = adv_[li];
        a.assign(ln.end - ln.begin + 1, 0.f);
        for (size_t i = 1; i < a.size(); i++)
            a[i] = r.MeasureText(std::wstring_view(text_).substr(ln.begin, i), style_).w;
    }
}

float SelectableText::XOf(size_t line, size_t idx) const {
    const Line& ln = lines_[line];
    const auto& a = adv_[line];
    return Bounds().l + a[std::clamp(idx, ln.begin, ln.end) - ln.begin];
}

size_t SelectableText::IndexAt(PointF p) const {
    if (lines_.empty()) return 0;
    long li = lineH_ > 0 ? (long)((p.y - Bounds().t) / lineH_) : 0;
    li = std::clamp(li, 0L, (long)lines_.size() - 1);
    const Line& ln = lines_[(size_t)li];
    size_t best = ln.begin;
    float bestD = 1e9f;
    for (size_t i = ln.begin; i <= ln.end; i++) {
        float d = std::fabs(XOf((size_t)li, i) - p.x);
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

void SelectableText::SetSelection(size_t anchor, size_t caret) {
    anchor_ = std::min(anchor, text_.size());
    caret_ = std::min(caret, text_.size());
    Invalidate();
}

void SelectableText::CopySelection() const {
    if (caret_ != anchor_) SetClipboardText(SelectedText());
}

void SelectableText::Paint(IRenderer& r, const Theme& t) {
    Layout(r);
    const RectF b = Bounds();
    const bool showSel = IsFocused() && caret_ != anchor_;
    for (size_t li = 0; li < lines_.size(); li++) {
        const Line& ln = lines_[li];
        const float y = b.t + (float)li * lineH_;
        if (showSel) {
            size_t s = std::max(SelectionStart(), ln.begin), e = std::min(SelectionEnd(), ln.end);
            if (s < e) r.FillRect({XOf(li, s), y, XOf(li, e), y + lineH_}, WithAlpha(t.accent, 0.4f));
        }
        r.Text(std::wstring_view(text_).substr(ln.begin, ln.end - ln.begin), {b.l, y, b.r + 4, y + lineH_}, style_, t.textPrimary);
    }
}

bool SelectableText::OnPointerDown(const PointerEvent& e) {
    if (e.button != PointerButton::Left) return false;
    if (e.clickCount >= 2) { SelectAll(); selecting_ = false; return true; }
    size_t i = IndexAt(e.pt);
    SetSelection((e.mods & ModShift) ? anchor_ : i, i);
    selecting_ = true;
    return true;
}

bool SelectableText::OnPointerMove(const PointerEvent& e) {
    if (!selecting_) return false;
    SetSelection(anchor_, IndexAt(e.pt));
    return true;
}

bool SelectableText::OnPointerUp(const PointerEvent& e) {
    (void)e;
    selecting_ = false;
    return true;
}

bool SelectableText::OnKeyDown(int vk, unsigned mods) {
    if (!(mods & ModCtrl) || (mods & ModAlt)) return false;
    if (vk == kVkA) { SelectAll(); return true; }
    if (vk == kVkC || vk == kVkInsert) { CopySelection(); return true; }
    return false;
}

bool SelectableText::OnContextMenu(PointF pt, bool keyboard) {
    (void)keyboard;
    if (!onContextMenu) return false;
    onContextMenu(pt);
    return true;
}

}
