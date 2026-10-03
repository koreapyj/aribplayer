#pragma once
// Characters arrive through UiHost::Char (the app forwards WM_CHAR); no IME composition UI.
#include <functional>
#include <string>
#include <vector>

#include "Widget.h"

namespace pui {

class TextBox : public Widget {
public:
    static constexpr float kHeight = 32;
    TextBox();

    void SetText(std::wstring t);                     // programmatic: caret to end, raises ValueChanged, no onChanged
    const std::wstring& Text() const { return text_; }
    void SetPlaceholder(std::wstring p) { placeholder_ = std::move(p); Invalidate(); }
    size_t SelectionStart() const { return std::min(caret_, anchor_); }
    size_t SelectionEnd() const { return std::max(caret_, anchor_); }
    void SetSelection(size_t anchor, size_t caret);
    void SelectAll() { SetSelection(0, text_.size()); }

    std::function<void(const std::wstring&)> onChanged;   // user edits only
    std::function<void(const std::wstring&)> onSubmit;

    /// True when the host's focused widget is a TextBox (the app should then not treat plain keys as shortcuts).
    static bool IsTextInputFocused(const UiHost& host);

    void Paint(IRenderer& r, const Theme& t) override;
    bool OnPointerDown(const PointerEvent& e) override;
    bool OnPointerMove(const PointerEvent& e) override;
    bool OnPointerUp(const PointerEvent& e) override;
    bool OnKeyDown(int vk, unsigned mods) override;
    bool OnChar(wchar_t ch) override;
    void OnFocusChanged(bool focused) override { (void)focused; Invalidate(); }
    bool SupportsValue() const override { return true; }
    std::wstring GetValue() const override { return text_; }
    void SetValue(const std::wstring& v) override;
    bool ValueReadOnly() const override { return false; }

private:
    RectF TextArea() const;
    size_t IndexAt(float x) const;
    float XOf(size_t idx) const;
    void Measure(IRenderer& r, const Theme& t);
    void Move(size_t to, bool extend);
    void Insert(std::wstring_view s);
    void EraseRange(size_t a, size_t b);
    void Changed();
    void EnsureCaretVisible();

    std::wstring text_, placeholder_;
    size_t caret_ = 0, anchor_ = 0;
    float scrollX_ = 0;
    bool selecting_ = false;
    std::vector<float> adv_;      // adv_[i] = width of text_[0, i); refreshed when painting
    bool advDirty_ = true;
};

}
