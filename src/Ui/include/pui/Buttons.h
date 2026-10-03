#pragma once
#include <functional>
#include <string>

#include "Glyphs.h"
#include "Widget.h"

namespace pui {


class NullMeasurer : public IRenderer {
public:
    void FillRect(const RectF&, Color) override {}
    void FillRoundRect(const RectF&, float, Color) override {}
    void StrokeRoundRect(const RectF&, float, Color, float) override {}
    void FillEllipse(PointF, float, float, Color) override {}
    void StrokeEllipse(PointF, float, float, Color, float) override {}
    void Line(PointF, PointF, Color, float) override {}
    void Text(std::wstring_view, const RectF&, const TextStyle&, Color) override {}
    SizeF MeasureText(std::wstring_view t, const TextStyle& s, float maxWidth) override {
        float w = 0.5f * s.size * (float)t.size(), lh = 1.33f * s.size;
        if (maxWidth > 0 && s.wrap && w > maxWidth) {
            float lines = (float)(int)((w + maxWidth - 1) / maxWidth);
            return {maxWidth, lh * lines};
        }
        return {maxWidth > 0 ? (w < maxWidth ? w : maxWidth) : w, lh};
    }
    void PushClip(const RectF&) override {}
    void PopClip() override {}
    void PushOpacity(float) override {}
    void PopOpacity() override {}
};

/// Process-wide measurer used when a layout call has no renderer at hand (menus, InfoBar layout...). The app sets
/// its D2D renderer here; the default is a NullMeasurer estimate.
IRenderer* DefaultMeasurer();
void SetDefaultMeasurer(IRenderer* r);   // nullptr restores the estimator
inline IRenderer& MeasurerOr(IRenderer* m) { return m ? *m : *DefaultMeasurer(); }

class IHeightForWidth {
public:
    virtual ~IHeightForWidth() = default;
    virtual float HeightForWidth(IRenderer* measurer, float width) = 0;
};

inline TextStyle BodyTextStyle(float size = 14, int weight = 400, TextAlign a = TextAlign::Leading) {
    TextStyle s; s.size = size; s.weight = weight; s.align = a; return s;
}
inline TextStyle IconTextStyle(float size = 16) {
    TextStyle s; s.font = FontKind::Icon; s.size = size; s.align = TextAlign::Center; s.valign = ParaAlign::Center; s.ellipsis = false; return s;
}
inline void SyncAutoName(Widget& w, const std::wstring& oldText, const std::wstring& newText) {
    if (w.Name().empty() || w.Name() == oldText) w.SetName(newText);
}
inline void RelayoutAncestors(Widget& w) {
    for (Widget* p = w.Parent(); p; p = p->Parent()) p->RequestLayout();
}


/// Pointer/keyboard activation shared by all button-like controls: pointer up inside after a left press, Enter on key
/// down, Space on key up (the pressed look shows while Space is held).
class ButtonBase : public Widget {
public:
    explicit ButtonBase(Role r);
    bool OnPointerDown(const PointerEvent& e) override;
    bool OnPointerUp(const PointerEvent& e) override;
    bool OnKeyDown(int vk, unsigned mods) override;
    bool OnKeyUp(int vk, unsigned mods) override;
    void OnFocusChanged(bool focused) override;
protected:
    virtual void Activate() = 0;
    bool VisuallyPressed() const { return IsPressed() || spaceDown_; }
    bool spaceDown_ = false;
};


enum class ButtonStyle { Standard, Accent };

class Button : public ButtonBase {
public:
    Button();
    explicit Button(std::wstring text, ButtonStyle style = ButtonStyle::Standard);
    void SetText(std::wstring t);
    const std::wstring& Text() const { return text_; }
    void SetGlyph(wchar_t g);
    wchar_t Glyph() const { return glyph_; }
    std::function<void()> onClick;

    SizeF DesiredSize(IRenderer* m) override;
    void Paint(IRenderer& r, const Theme& t) override;
    bool SupportsInvoke() const override { return true; }
    void Invoke() override;
protected:
    void Activate() override;
    virtual bool AccentLook() const { return style_ == ButtonStyle::Accent; }
    std::wstring text_;
    wchar_t glyph_ = 0;
    ButtonStyle style_ = ButtonStyle::Standard;
};


/// Round subtle glyph button (36 DIP like the player bar buttons).
class IconButton : public ButtonBase {
public:
    IconButton();
    explicit IconButton(wchar_t glyph, std::wstring label = {});
    void SetGlyph(wchar_t g) { glyph_ = g; Invalidate(); }
    wchar_t Glyph() const { return glyph_; }
    void SetGlyphSize(float s) { glyphSize_ = s; Invalidate(); }
    void SetSize(float diameter) { size_ = diameter; }
    float Size() const { return size_; }
    /// Sets the automation name and the tooltip text.
    void SetLabel(std::wstring label);
    /// Use ControlFill tokens (rest/hover/pressed) instead of the transparent subtle fills (e.g. the Play button).
    void SetControlFill(bool v) { controlFill_ = v; Invalidate(); }
    std::function<void()> onClick;

    SizeF DesiredSize(IRenderer*) override { return {size_, size_}; }
    void Paint(IRenderer& r, const Theme& t) override;
    bool SupportsInvoke() const override { return true; }
    void Invoke() override;
protected:
    void Activate() override;
private:
    wchar_t glyph_ = 0;
    float glyphSize_ = 16, size_ = 36;
    bool controlFill_ = false;
};


class ToggleButton : public Button {
public:
    ToggleButton();
    explicit ToggleButton(std::wstring text);
    void SetChecked(bool c);                 // raises ToggleChanged + onToggled when it changes
    bool Checked() const { return checked_; }
    std::function<void(bool)> onToggled;

    bool SupportsInvoke() const override { return false; }
    bool SupportsToggle() const override { return true; }
    int ToggleState() const override { return checked_ ? 1 : 0; }
    void Toggle() override;
protected:
    void Activate() override;
    bool AccentLook() const override { return checked_; }
private:
    bool checked_ = false;
};


class HyperlinkButton : public ButtonBase {
public:
    HyperlinkButton();
    explicit HyperlinkButton(std::wstring text);
    void SetText(std::wstring t);
    const std::wstring& Text() const { return text_; }
    std::function<void()> onClick;

    SizeF DesiredSize(IRenderer* m) override;
    void Paint(IRenderer& r, const Theme& t) override;
    bool SupportsInvoke() const override { return true; }
    void Invoke() override;
protected:
    void Activate() override;
private:
    std::wstring text_;
};

}
