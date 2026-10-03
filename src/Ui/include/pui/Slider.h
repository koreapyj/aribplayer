#pragma once
#include <functional>
#include <optional>

#include "Buttons.h"

namespace pui {

float SliderValueToX(double v, double mn, double mx, float trackL, float trackR);
/// Snap v to the step grid starting at mn (step <= 0: no snapping) and clamp to [mn, mx].
double SliderSnap(double v, double mn, double mx, double step);
/// Value at x (clamped to the track), snapped to step.
double SliderXToValue(float x, double mn, double mx, float trackL, float trackR, double step);

class Slider : public Widget {
public:
    static constexpr float kHeight = 32, kThumbOuter = 20, kTrackHeight = 4;
    Slider();

    void SetRange(double mn, double mx);
    double Minimum() const { return min_; }
    double Maximum() const { return max_; }
    void SetStep(double s) { step_ = s; }
    double Step() const { return step_; }
    void SetLargeStep(double s) { largeStep_ = s; }
    double LargeStep() const { return largeStep_ > 0 ? largeStep_ : (max_ - min_) / 10; }
    /// Programmatic set: clamps+snaps, raises ValueChanged, does NOT call onChanged.
    void SetValue(double v);
    double Value() const { return value_; }
    bool IsDragging() const { return dragging_; }

    /// User-driven change (pointer, keyboard, wheel, UIA). dragging=true while the pointer is held.
    std::function<void(double value, bool dragging)> onChanged;
    /// Pointer hover position as a value (nullopt when the pointer leaves or the slider is disabled/dragging).
    std::function<void(std::optional<double>)> onHoverValue;
    /// Formats a value for the tooltip; when set the tooltip text follows the hovered/dragged value.
    std::function<std::wstring(double)> valueFormatter;

    RectF TrackRect() const;    // 4 DIP track between the 10 DIP thumb insets, centred vertically
    RectF ThumbRect() const;    // 20 DIP outer thumb around the current value
    double ValueAt(float x) const;

    SizeF DesiredSize(IRenderer*) override { return {Bounds().w() > 0 ? Bounds().w() : 200.f, kHeight}; }
    void Paint(IRenderer& r, const Theme& t) override;
    bool OnPointerDown(const PointerEvent& e) override;
    bool OnPointerMove(const PointerEvent& e) override;
    bool OnPointerUp(const PointerEvent& e) override;
    bool OnPointerWheel(const PointerEvent& e) override;
    void OnPointerLeave() override;
    bool OnKeyDown(int vk, unsigned mods) override;

    bool SupportsRangeValue() const override { return true; }
    RangeInfo GetRange() const override;
    void SetRangeValue(double v) override;

private:
    double SmallStep() const { return step_ > 0 ? step_ : (max_ - min_) / 100; }
    void UserSet(double v, bool dragging, bool force = false);
    void UpdateToolTip(double v);
    double min_ = 0, max_ = 100, value_ = 0, step_ = 1, largeStep_ = 10;
    bool dragging_ = false;
};

}
