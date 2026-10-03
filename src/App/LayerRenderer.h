#pragma once
// Renderer used for the composition layers. One object, two modes:
//   * Hash mode: nothing is drawn; every draw call is folded into a 64-bit FNV hash. The app paints a layer in this mode
//     first and only repaints the real DirectComposition surface when the hash changed ("redraw only when something
//     visible changed"), so UiHost invalidations caused by e.g. a hover change on another layer cost no GPU work.
//   * Draw mode: calls are forwarded to a pui::D2dRenderer (Begin/End around the surface BeginDraw/EndDraw).
// MeasureText always goes to the D2dRenderer so both passes lay text out identically.
// It also implements the optional renderer extension below (suppression) that widgets reach with
// dynamic_cast, so widgets stay testable with pui::RecordingRenderer.
#include <cstdint>

#include "pui/D2dRenderer.h"

namespace app {

/// Skip drawing for a span of the paint (used by ShellRoot to paint only popups through UiHost::Paint).
struct ISuppressible {
    virtual ~ISuppressible() = default;
    virtual void SetDrawEnabled(bool on) = 0;
    virtual bool DrawEnabled() const = 0;
};

class LayerRenderer final : public pui::IRenderer, public ISuppressible {
public:
    explicit LayerRenderer(pui::D2dRenderer& d2d) : d2d_(d2d) {}

    void BeginHash();
    uint64_t EndHash();
    void BeginDraw(ID2D1DeviceContext* dc, pui::PointF originDip);
    void EndDraw();

    void SetDrawEnabled(bool on) override { enabled_ = on; d2d_.SetDrawEnabled(on); }
    bool DrawEnabled() const override { return enabled_; }

    void FillRect(const pui::RectF& r, pui::Color c) override;
    void FillRoundRect(const pui::RectF& r, float radius, pui::Color c) override;
    void StrokeRoundRect(const pui::RectF& r, float radius, pui::Color c, float width = 1) override;
    void FillEllipse(pui::PointF center, float rx, float ry, pui::Color c) override;
    void StrokeEllipse(pui::PointF center, float rx, float ry, pui::Color c, float width = 1) override;
    void Line(pui::PointF a, pui::PointF b, pui::Color c, float width = 1) override;
    void Text(std::wstring_view text, const pui::RectF& r, const pui::TextStyle& style, pui::Color c) override;
    pui::SizeF MeasureText(std::wstring_view text, const pui::TextStyle& style, float maxWidth = 0) override;
    void PushClip(const pui::RectF& r) override;
    void PopClip() override;
    void PushOpacity(float opacity) override;
    void PopOpacity() override;

private:
    void Mix(const void* p, size_t n);
    template <class T> void MixV(const T& v) { Mix(&v, sizeof v); }
    void MixOp(int op) { MixV(op); }
    void MixRect(const pui::RectF& r) { MixV(r.l); MixV(r.t); MixV(r.r); MixV(r.b); }

    pui::D2dRenderer& d2d_;
    bool hashing_ = false, enabled_ = true;
    uint64_t h_ = 0;
};

}
