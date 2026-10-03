#pragma once
#include <functional>

#include "Buttons.h"

namespace pui {

class ToggleSwitch : public ButtonBase {
public:
    static constexpr float kTrackW = 40, kTrackH = 20, kGap = 12, kHeight = 32;
    ToggleSwitch();
    void SetOn(bool on);                     // raises ToggleChanged + onToggled when it changes
    bool IsOn() const { return on_; }
    std::function<void(bool)> onToggled;

    RectF TrackRect() const;
    SizeF DesiredSize(IRenderer* m) override;
    void Paint(IRenderer& r, const Theme& t) override;
    bool SupportsToggle() const override { return true; }
    int ToggleState() const override { return on_ ? 1 : 0; }
    void Toggle() override { if (IsEffectivelyEnabled()) SetOn(!on_); }
protected:
    void Activate() override { SetOn(!on_); }
private:
    bool on_ = false;
};

class CheckBox : public ButtonBase {
public:
    static constexpr float kBox = 20, kGap = 8, kHeight = 32;
    CheckBox();
    explicit CheckBox(std::wstring text);
    void SetText(std::wstring t);
    const std::wstring& Text() const { return text_; }
    void SetChecked(bool c);                 // raises ToggleChanged + onToggled when it changes
    bool Checked() const { return checked_; }
    std::function<void(bool)> onToggled;

    SizeF DesiredSize(IRenderer* m) override;
    void Paint(IRenderer& r, const Theme& t) override;
    bool SupportsToggle() const override { return true; }
    int ToggleState() const override { return checked_ ? 1 : 0; }
    void Toggle() override { if (IsEffectivelyEnabled()) SetChecked(!checked_); }
protected:
    void Activate() override { SetChecked(!checked_); }
private:
    bool checked_ = false;
    std::wstring text_;
};

class RadioGroup;

class RadioButton : public ButtonBase {
public:
    static constexpr float kDot = 20, kGap = 8, kHeight = 32;
    RadioButton();
    explicit RadioButton(std::wstring text);
    void SetText(std::wstring t);
    const std::wstring& Text() const { return text_; }
    bool Checked() const { return checked_; }
    RadioGroup* Group() const;

    SizeF DesiredSize(IRenderer* m) override;
    void Paint(IRenderer& r, const Theme& t) override;
    bool OnKeyDown(int vk, unsigned mods) override;
    bool SupportsSelectionItem() const override { return true; }
    bool IsSelected() const override { return checked_; }
    void SelectItem() override;
    void RemoveFromSelection() override;
protected:
    void Activate() override;
private:
    friend class RadioGroup;
    void SetCheckedInternal(bool c);         // no group bookkeeping
    bool checked_ = false;
    std::wstring text_;
};

/// Vertical (or horizontal) container of RadioButtons with roving focus: only the selected radio (or the first enabled
/// one when nothing is selected) is a Tab stop; arrow keys move selection and focus.
class RadioGroup : public Widget {
public:
    RadioGroup();
    RadioButton* AddOption(std::wstring text);
    int OptionCount() const;
    RadioButton* Option(int i) const;
    int SelectedIndex() const;
    /// -1 clears. Raises SelectionChanged (group + item) and calls onSelectionChanged when the index changes.
    void SetSelectedIndex(int i);
    void SetHorizontal(bool h) { horizontal_ = h; RequestLayout(); }
    void SetSpacing(float s) { spacing_ = s; RequestLayout(); }
    std::function<void(int)> onSelectionChanged;

    void OnLayout() override;
    SizeF DesiredSize(IRenderer* m) override;
    bool SupportsSelection() const override { return true; }
    bool IsSelectionRequired() const override { return false; }
    std::vector<Widget*> SelectedChildren() const override;

    /// Arrow-key navigation: select the next/previous enabled radio (wraps) and move focus to it.
    void MoveSelection(int dir);
    void UpdateRoving();
private:
    friend class RadioButton;
    void Select(RadioButton* rb, bool focus);
    bool horizontal_ = false;
    float spacing_ = 0;
};

}
