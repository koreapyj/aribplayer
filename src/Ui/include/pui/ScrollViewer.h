#pragma once
// ScrollBarController (geometry + hover/fade state + painting) is reusable: VirtualList uses it too.
#include <functional>
#include <memory>

#include "Widget.h"

namespace pui {

/// Overlay scrollbar logic. Pure geometry + state; owners feed it pointer events and the host clock.
/// Look: thin 2 DIP indicator that expands to a 6 DIP thumb with arrow buttons while the pointer is over the
/// 12 DIP wide zone at the right edge; fades out kFadeDelayMs after the last activity.
class ScrollBarController {
public:
    static constexpr float kZoneW = 12, kArrowH = 12, kThumbCollapsedW = 2, kThumbExpandedW = 6, kMinThumb = 24, kEdgeInset = 2;
    static constexpr uint64_t kFadeDelayMs = 1200, kFadeMs = 300, kExpandMs = 150;
    enum class Part { None, ArrowUp, TrackUp, Thumb, TrackDown, ArrowDown };

    void SetView(const RectF& view) { view_ = view; }
    void SetMetrics(float viewport, float extent, float offset) { viewport_ = viewport; extent_ = extent; offset_ = offset; }
    bool Needed() const { return viewport_ > 0 && extent_ > viewport_ + 0.5f; }
    float MaxOffset() const { return std::max(0.f, extent_ - viewport_); }

    RectF ZoneRect() const { return {view_.r - kZoneW, view_.t, view_.r, view_.b}; }
    RectF InnerTrack() const { return {view_.r - kZoneW, view_.t + kArrowH, view_.r, view_.b - kArrowH}; }
    /// Thumb rect for the current metrics and expand state (y/height do not depend on the expand state).
    RectF ThumbRect() const;
    Part HitPart(PointF p) const;

    /// Returns true when the press was consumed. newOffset is set when the press scrolls (page/arrow) or starts a drag.
    bool PointerDown(PointF p, uint64_t now, float arrowStep, float& newOffset);
    /// Returns true while dragging (newOffset valid). Also updates the hover state.
    bool PointerMove(PointF p, uint64_t now, float& newOffset);
    void PointerUp(uint64_t now);
    void Leave(uint64_t now);
    bool Dragging() const { return dragging_; }
    bool Hover() const { return hover_; }

    void Poke(uint64_t now) { active_ = true; lastActivity_ = now; }
    /// Advances the expand animation. Returns true while the owner must keep ticking.
    bool Update(uint64_t now);
    float Opacity(uint64_t now) const;
    float Expand() const { return expand_; }
    void Paint(IRenderer& r, const Theme& t, uint64_t now) const;

    static float ClampOffset(float offset, float viewport, float extent) { return std::clamp(offset, 0.f, std::max(0.f, extent - viewport)); }

private:
    RectF view_{};
    float viewport_ = 0, extent_ = 0, offset_ = 0;
    bool hover_ = false, dragging_ = false, active_ = false;
    float grab_ = 0, expand_ = 0;
    uint64_t lastActivity_ = 0, lastUpdate_ = 0;
};

/// Child widget that only paints through a callback (drawn after its siblings, never hit-tested).
class OverlayWidget : public Widget {
public:
    OverlayWidget() : Widget(Role::Custom) { SetHitTestVisible(false); }
    std::function<void(IRenderer&, const Theme&)> onPaint;
    void Paint(IRenderer& r, const Theme& t) override { if (onPaint) onPaint(r, t); }
};

class ScrollViewer : public Widget {
public:
    ScrollViewer();

    /// Replaces the content widget (placed below the overlay scrollbar).
    Widget* SetContent(std::unique_ptr<Widget> content);
    template <class T, class... A> T* EmplaceContent(A&&... args) {
        auto p = std::make_unique<T>(std::forward<A>(args)...);
        T* raw = p.get();
        SetContent(std::move(p));
        return raw;
    }
    Widget* Content() const { return content_; }

    /// Explicit content height (DIP); negative = ask the content's DesiredSize (measurer may be null).
    void SetExtentHeight(float h);
    void SetMeasurer(IRenderer* m) { measurer_ = m; }
    /// Call after the content changed size.
    void UpdateLayout() { OnLayout(); Invalidate(); }

    float Offset() const { return offset_; }
    float MaxOffset() const { return std::max(0.f, extent_ - Bounds().h()); }
    void SetOffset(float o);
    void ScrollBy(float dy) { SetOffset(offset_ + dy); }
    void SetWheelStep(float s) { wheelStep_ = s; }
    /// rect in current client (absolute) coordinates; scrolls the minimum distance to make it visible.
    void ScrollIntoView(const RectF& rect);
    void ScrollIntoView(Widget* child) override { if (child) ScrollIntoView(child->Bounds()); }

    const ScrollBarController& ScrollBar() const { return sb_; }

    void OnLayout() override;
    Widget* HitTest(PointF p) override;
    bool OnPointerDown(const PointerEvent& e) override;
    bool OnPointerMove(const PointerEvent& e) override;
    bool OnPointerUp(const PointerEvent& e) override;
    bool OnPointerWheel(const PointerEvent& e) override;
    void OnPointerLeave() override;
    bool OnKeyDown(int vk, unsigned mods) override;
    bool OnTick(uint64_t nowMs) override;
    bool SupportsScroll() const override { return true; }
    double VerticalScrollPercent() const override;
    double VerticalViewSize() const override;
    void SetVerticalScrollPercent(double pct) override;

private:
    uint64_t Now() const;
    void Animate();
    float ComputeExtent();
    Widget* content_ = nullptr;
    OverlayWidget* overlay_ = nullptr;
    IRenderer* measurer_ = nullptr;
    float explicitExtent_ = -1, extent_ = 0, offset_ = 0, wheelStep_ = 48;
    ScrollBarController sb_;
};

}
