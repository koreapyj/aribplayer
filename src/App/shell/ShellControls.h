#pragma once
// Small controls owned by the shell (title bar, player bar dock, playlist panel): GlyphButton, TextButton, Chip and
// ShellSlider. They are intentionally independent of the shared PlayerUi controls (Buttons.h / Slider.h) so the shell has no
// build dependency on them; pages use the shared controls.
#include <functional>
#include <string>

#include "BarLogic.h"
#include "pui/Widget.h"

namespace app {

/// Glyph button with the spec's 83 ms fill fade. Rounded = 4 DIP radius bar button, Rect = square title-bar button.
/// Accent style = accent fill + black glyph (play / pause); toggled = subtle 8 % fill + accent glyph + accent underline
/// (playlist panel toggle). A right click (or Shift+F10 / Menu key) raises onRightClick.
class GlyphButton : public pui::Widget {
public:
    enum class Shape { Rounded, Rect };
    GlyphButton(std::wstring name, std::wstring glyph, float glyphSize, Shape shape);

    void SetGlyph(const std::wstring& g);
    const std::wstring& Glyph() const { return glyph_; }
    void SetGlyphSize(float s) { glyphSize_ = s; Invalidate(); }
    /// Accent fill with the on-accent glyph colour (play / pause).
    void SetAccentStyle(bool accent);
    /// Pressed-in look of a toggle button (panel open): subtle fill, accent glyph, accent underline.
    void SetToggled(bool on);
    /// Windows caption close button uses the system red hover/pressed fill and white glyph.
    void SetCaptionCloseStyle(bool close) { captionClose_ = close; }
    /// Keeps the pressed look while the button's drop-down menu is open (main-menu button).
    void SetMenuOpen(bool on);
    std::function<void()> onInvoke;
    std::function<void()> onRightClick;

    void Paint(pui::IRenderer& r, const pui::Theme& t) override;
    bool OnPointerDown(const pui::PointerEvent& e) override;
    bool OnPointerUp(const pui::PointerEvent& e) override;
    bool OnContextMenu(pui::PointF pt, bool keyboard) override;
    bool OnKeyDown(int vk, unsigned mods) override;
    bool OnTick(uint64_t nowMs) override;
    bool SupportsInvoke() const override { return true; }
    void Invoke() override;

private:
    std::wstring glyph_;
    float glyphSize_;
    Shape shape_;
    bool accent_ = false, toggled_ = false, captionClose_ = false, menuOpen_ = false;
    ColorFade fade_;
};

/// Text button: [leading glyph] label. Plain = subtle hover / pressed fill (footer buttons); selected = semibold label with a 2 DIP accent underline (tab header). `SetMenuOpen` keeps the pressed look
/// while its drop-down menu is open.
class TextButton : public pui::Widget {
public:
    TextButton(std::wstring label, std::wstring leadGlyph = {});

    void SetLabel(const std::wstring& label);
    void SetLeadGlyph(const std::wstring& g);
    void SetSelected(bool on);
    void SetMenuOpen(bool on);
    void SetFontSize(float size);
    /// Playlist chip look: selected = 8 % fill + 1 px chipStroke outline (no underline), unselected label in secondary text.
    void SetChipStyle(bool on);
    /// Natural width (DIP) for `r`'s font metrics: 10 DIP padding, glyph column, label.
    float DesiredWidth(pui::IRenderer& r) const;
    std::function<void()> onInvoke;
    std::function<void()> onRightClick;

    void Paint(pui::IRenderer& r, const pui::Theme& t) override;
    bool OnPointerDown(const pui::PointerEvent& e) override;
    bool OnPointerUp(const pui::PointerEvent& e) override;
    bool OnContextMenu(pui::PointF pt, bool keyboard) override;
    bool OnKeyDown(int vk, unsigned mods) override;
    bool OnTick(uint64_t nowMs) override;
    bool SupportsInvoke() const override { return true; }
    void Invoke() override;

private:
    std::wstring label_, glyph_;
    bool selected_ = false, menuOpen_ = false, chipStyle_ = false;
    float fontSize_ = 14;
    ColorFade fade_;
};

/// Codec / decoder info chip: 22 DIP tall, 1 px outline (Theme::chipStroke), 4 DIP radius, 11 px secondary text. Display-only
/// by default; clickable chips (the H/W decoder chip) get hover / pressed fills and keep the pressed look while their menu is open.
class Chip : public pui::Widget {
public:
    explicit Chip(std::wstring text, bool clickable = false);

    void SetText(const std::wstring& text);
    const std::wstring& Text() const { return text_; }
    void SetClickable(bool on);
    bool Clickable() const { return clickable_; }
    void SetMenuOpen(bool on);
    /// Outer width (DIP): text + 2 x 8 DIP padding + the 1 px borders.
    static float WidthFor(pui::IRenderer& r, const std::wstring& text);
    std::function<void()> onInvoke;

    void Paint(pui::IRenderer& r, const pui::Theme& t) override;
    bool OnPointerDown(const pui::PointerEvent& e) override;
    bool OnPointerUp(const pui::PointerEvent& e) override;
    bool OnKeyDown(int vk, unsigned mods) override;
    bool SupportsInvoke() const override { return clickable_; }
    void Invoke() override;

private:
    std::wstring text_;
    bool clickable_, menuOpen_ = false;
};

/// Horizontal slider with the prototype look: 4 DIP track, accent fill, 20 DIP thumb with a 10 DIP accent dot. Value is a
/// fraction 0..1; UIA RangeValue reports value*scale. The track is inset from the control rect by half a
/// thumb so the thumb never leaves the widget.
class ShellSlider : public pui::Widget {
public:
    explicit ShellSlider(std::wstring name);

    void SetFraction(double f);                 // programmatic (ignored while dragging)
    double Fraction() const { return dragging_ ? dragFrac_ : frac_; }
    bool Dragging() const { return dragging_; }
    void SetRangeScale(double maxValue) { scale_ = maxValue > 0 ? maxValue : 1; }
    void SetSteps(double smallFrac, double largeFrac) { small_ = smallFrac; large_ = largeFrac; }
    /// Track rectangle (DIP): the control rect inset horizontally, 4 DIP tall.
    pui::RectF Track() const;
    double FractionAt(float x) const;

    std::function<void(double)> onChanging;     // during pointer drag
    std::function<void(double)> onCommit;       // drag released / keyboard / UIA
    std::function<void(int)> onWheelSteps;      // mouse wheel over the slider: whole notches (+ = up); unset = wheel falls through
    std::function<std::wstring(double)> valueText;

    void Paint(pui::IRenderer& r, const pui::Theme& t) override;
    bool OnPointerDown(const pui::PointerEvent& e) override;
    bool OnPointerMove(const pui::PointerEvent& e) override;
    bool OnPointerUp(const pui::PointerEvent& e) override;
    bool OnPointerWheel(const pui::PointerEvent& e) override;
    bool OnKeyDown(int vk, unsigned mods) override;
    bool SupportsRangeValue() const override { return true; }
    pui::RangeInfo GetRange() const override;
    void SetRangeValue(double v) override;
    bool SupportsValue() const override { return true; }
    std::wstring GetValue() const override;

    /// Shared drawing helper (also used by tests through RecordingRenderer): track, accent fill and thumb.
    static void DrawSlider(pui::IRenderer& r, const pui::Theme& t, const pui::RectF& track, double frac, bool enabled, bool hover,
                           bool pressed);

private:
    double frac_ = 0, dragFrac_ = 0, scale_ = 1, small_ = 0.02, large_ = 0.1;
    float wheelAcc_ = 0;
    bool dragging_ = false;
};

}
