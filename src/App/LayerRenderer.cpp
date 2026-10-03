#include "LayerRenderer.h"

#include <d2d1helper.h>

namespace app {

using pui::Color;
using pui::PointF;
using pui::RectF;

void LayerRenderer::Mix(const void* p, size_t n) {
    const unsigned char* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; i++) { h_ ^= b[i]; h_ *= 1099511628211ull; }
}

void LayerRenderer::BeginHash() {
    hashing_ = true;
    enabled_ = true;
    h_ = 1469598103934665603ull;
}

uint64_t LayerRenderer::EndHash() {
    hashing_ = false;
    enabled_ = true;
    return h_;
}

void LayerRenderer::BeginDraw(ID2D1DeviceContext* dc, PointF originDip) {
    hashing_ = false;
    enabled_ = true;
    d2d_.Begin(dc, originDip);
}

void LayerRenderer::EndDraw() {
    d2d_.End();
    enabled_ = true;
}

void LayerRenderer::FillRect(const RectF& r, Color c) {
    if (!enabled_) return;
    if (hashing_) { MixOp(1); MixRect(r); MixV(c.v); return; }
    d2d_.FillRect(r, c);
}

void LayerRenderer::FillRoundRect(const RectF& r, float radius, Color c) {
    if (!enabled_) return;
    if (hashing_) { MixOp(2); MixRect(r); MixV(radius); MixV(c.v); return; }
    d2d_.FillRoundRect(r, radius, c);
}

void LayerRenderer::StrokeRoundRect(const RectF& r, float radius, Color c, float width) {
    if (!enabled_) return;
    if (hashing_) { MixOp(3); MixRect(r); MixV(radius); MixV(c.v); MixV(width); return; }
    d2d_.StrokeRoundRect(r, radius, c, width);
}

void LayerRenderer::FillEllipse(PointF p, float rx, float ry, Color c) {
    if (!enabled_) return;
    if (hashing_) { MixOp(4); MixV(p.x); MixV(p.y); MixV(rx); MixV(ry); MixV(c.v); return; }
    d2d_.FillEllipse(p, rx, ry, c);
}

void LayerRenderer::StrokeEllipse(PointF p, float rx, float ry, Color c, float width) {
    if (!enabled_) return;
    if (hashing_) { MixOp(5); MixV(p.x); MixV(p.y); MixV(rx); MixV(ry); MixV(c.v); MixV(width); return; }
    d2d_.StrokeEllipse(p, rx, ry, c, width);
}

void LayerRenderer::Line(PointF a, PointF b, Color c, float width) {
    if (!enabled_) return;
    if (hashing_) { MixOp(6); MixV(a.x); MixV(a.y); MixV(b.x); MixV(b.y); MixV(c.v); MixV(width); return; }
    d2d_.Line(a, b, c, width);
}

void LayerRenderer::Text(std::wstring_view text, const RectF& r, const pui::TextStyle& s, Color c) {
    if (!enabled_) return;
    if (hashing_) {
        MixOp(7); MixRect(r); MixV(c.v);
        MixV((int)s.font); MixV(s.size); MixV(s.weight); MixV((int)s.align); MixV((int)s.valign); MixV(s.wrap); MixV(s.ellipsis);
        Mix(text.data(), text.size() * sizeof(wchar_t));
        return;
    }
    d2d_.Text(text, r, s, c);
}

pui::SizeF LayerRenderer::MeasureText(std::wstring_view text, const pui::TextStyle& style, float maxWidth) {
    return d2d_.MeasureText(text, style, maxWidth);
}

void LayerRenderer::PushClip(const RectF& r) {
    if (hashing_) { MixOp(8); MixRect(r); return; }
    d2d_.PushClip(r);
}

void LayerRenderer::PopClip() {
    if (hashing_) { MixOp(9); return; }
    d2d_.PopClip();
}

void LayerRenderer::PushOpacity(float opacity) {
    if (hashing_) { MixOp(10); MixV(opacity); return; }
    d2d_.PushOpacity(opacity);
}

void LayerRenderer::PopOpacity() {
    if (hashing_) { MixOp(11); return; }
    d2d_.PopOpacity();
}

}
