#pragma once
// Read-only wrapped text the user can select with the mouse and copy (Ctrl+A / Ctrl+C, double click selects all).
#include <functional>
#include <string>
#include <vector>

#include "Widget.h"

namespace pui {

class SelectableText : public Widget {
public:
    struct Line { size_t begin = 0, end = 0; };

    SelectableText(std::wstring text, TextStyle style);

    const std::wstring& Text() const { return text_; }
    void SelectAll() { SetSelection(0, text_.size()); }
    void SetSelection(size_t anchor, size_t caret);
    std::wstring SelectedText() const { return text_.substr(SelectionStart(), SelectionEnd() - SelectionStart()); }
    size_t SelectionStart() const { return std::min(caret_, anchor_); }
    size_t SelectionEnd() const { return std::max(caret_, anchor_); }
    void CopySelection() const;

    /// Right click / menu key: the owner shows its menu (Copy / Select all) at the point.
    std::function<void(PointF)> onContextMenu;

    /// Height of `text` wrapped to `width` (at least one line).
    static float HeightFor(IRenderer& r, std::wstring_view text, const TextStyle& style, float width);

    void Paint(IRenderer& r, const Theme& t) override;
    bool OnPointerDown(const PointerEvent& e) override;
    bool OnPointerMove(const PointerEvent& e) override;
    bool OnPointerUp(const PointerEvent& e) override;
    bool OnKeyDown(int vk, unsigned mods) override;
    bool OnContextMenu(PointF pt, bool keyboard) override;
    CursorKind Cursor() const override { return CursorKind::IBeam; }
    void OnFocusChanged(bool focused) override { (void)focused; Invalidate(); }
    bool SupportsValue() const override { return true; }
    std::wstring GetValue() const override { return text_; }

private:
    static std::vector<Line> Wrap(IRenderer& r, std::wstring_view text, const TextStyle& style, float width);
    void Layout(IRenderer& r);
    size_t IndexAt(PointF p) const;
    float XOf(size_t line, size_t idx) const;

    std::wstring text_;
    TextStyle style_;
    size_t caret_ = 0, anchor_ = 0;
    bool selecting_ = false;
    std::vector<Line> lines_;
    std::vector<std::vector<float>> adv_;   // adv_[line][i] = width of the first i characters of the line
    float lineH_ = 0, laidWidth_ = -1;
};

}
