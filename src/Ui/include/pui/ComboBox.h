#pragma once
// ComboBox: closed box (text + chevron) and a light-dismiss popup list opened with UiHost::OpenPopup.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Buttons.h"

namespace pui {

/// Popup rectangle for a combo box: the selected item overlays the box (item centre = box centre), width at least the
/// box width (and minWidth), height n * itemH + 2 * padV limited to the client (the popup then scrolls), the whole
/// rectangle clamped inside the client with `margin`. selectedIndex < 0 behaves like 0.
RectF ComputeComboPopupRect(const RectF& box, int itemCount, int selectedIndex, float itemH, float padV, float minWidth,
                            SizeF client, float margin = 4);

/// Explorer-style type-ahead. Characters typed within kTimeoutMs of each other form a prefix; the first item at or after
/// `current` starting with it (case-insensitive, wrapping) is the match. A prefix of one repeated character instead
/// cycles through the items starting with that character. Returns -1 when nothing matches.
class TypeAhead {
public:
    static constexpr uint64_t kTimeoutMs = 1000;
    int Next(wchar_t ch, uint64_t now, const std::vector<std::wstring>& items, int current);
private:
    std::wstring prefix_;
    uint64_t last_ = 0;
};

class ComboBox : public Widget {
public:
    static constexpr float kHeight = 32, kItemHeight = 32, kPopupPad = 2;
    ComboBox();

    void AddItem(std::wstring text);
    void SetItems(std::vector<std::wstring> items);
    int ItemCount() const { return (int)items_.size(); }
    int SelectedIndex() const { return selected_; }
    /// Programmatic selection: raises SelectionChanged + ValueChanged, does not call onSelectionChanged.
    void SetSelectedIndex(int i);
    std::wstring SelectedText() const { return selected_ >= 0 ? items_[(size_t)selected_] : std::wstring(); }

    /// User-driven selection change (keyboard, popup, UIA Value set).
    std::function<void(int index)> onSelectionChanged;

    void Open();
    void Close();
    bool IsOpen() const { return popup_ != nullptr; }
    Widget* PopupWidget() const { return popup_; }

    SizeF DesiredSize(IRenderer* m) override;
    void Paint(IRenderer& r, const Theme& t) override;
    bool OnPointerDown(const PointerEvent& e) override;
    bool OnPointerUp(const PointerEvent& e) override;
    bool OnKeyDown(int vk, unsigned mods) override;
    bool OnChar(wchar_t ch) override;

    bool SupportsExpandCollapse() const override { return true; }
    bool IsExpanded() const override { return IsOpen(); }
    void Expand() override { Open(); }
    void Collapse() override { Close(); }
    bool SupportsSelection() const override { return true; }
    bool IsSelectionRequired() const override { return false; }
    bool SupportsValue() const override { return true; }
    std::wstring GetValue() const override { return SelectedText(); }
    void SetValue(const std::wstring& v) override;
    bool ValueReadOnly() const override { return false; }

private:
    friend class ComboPopup;
    void UserSelect(int i);
    std::vector<std::wstring> items_;
    int selected_ = -1;
    Widget* popup_ = nullptr;
    TypeAhead typeAhead_;
};

}
