#pragma once
// pui::IRenderer on CoreGraphics + CoreText.
//  * Coordinates are points (= pui DIPs). The host passes a CGContext whose CTM already maps points to device pixels
//    and is flipped (origin top-left, y down) - what a flipped NSView's drawRect: gives.
//  * FontKind::Text -> the system font (SF Pro) at the requested weight; FontKind::Icon -> each Glyphs.h code point is
//    mapped to an SF Symbol (SymbolMap.h) rasterised through NSImage at the backing scale and tinted with the colour.
//  * PushClip/PopClip = CGContextSaveGState + ClipToRect / RestoreGState; PushOpacity multiplies every colour's alpha.
#include <CoreGraphics/CoreGraphics.h>

#include <string_view>
#include <vector>

#include "pui/Renderer.h"

namespace puimac {

class CGRenderer : public pui::IRenderer {
public:
    CGRenderer();
    ~CGRenderer() override;
    CGRenderer(const CGRenderer&) = delete;
    CGRenderer& operator=(const CGRenderer&) = delete;

    /// Draw calls between Begin and End go to `ctx`. `scale` = device pixels per point (backing scale factor): symbol
    /// rasterisation and baseline snapping use it. MeasureText works outside Begin/End as well.
    void Begin(CGContextRef ctx, float scale);
    void End();
    bool Drawing() const { return ctx_ != nullptr; }
    void SetScale(float s) { scale_ = s > 0 ? s : 1; }
    float Scale() const { return scale_; }

    // pui::IRenderer
    void FillRect(const pui::RectF& r, pui::Color c) override;
    void FillRoundRect(const pui::RectF& r, float radius, pui::Color c) override;
    void StrokeRoundRect(const pui::RectF& r, float radius, pui::Color c, float width) override;
    void FillEllipse(pui::PointF center, float rx, float ry, pui::Color c) override;
    void StrokeEllipse(pui::PointF center, float rx, float ry, pui::Color c, float width) override;
    void Line(pui::PointF a, pui::PointF b, pui::Color c, float width) override;
    void Text(std::wstring_view text, const pui::RectF& r, const pui::TextStyle& style, pui::Color c) override;
    pui::SizeF MeasureText(std::wstring_view text, const pui::TextStyle& style, float maxWidth) override;
    void PushClip(const pui::RectF& r) override;
    void PopClip() override;
    void PushOpacity(float opacity) override;
    void PopOpacity() override;

    /// Monospaced text (the system monospaced face, SF Mono) for diagnostics: pui::FontKind has no monospace kind, and
    /// the app's debug overlay lays out fixed columns (Windows: Cascadia Mono / Consolas through DirectWrite). One line,
    /// no wrapping or ellipsis, top-aligned in `r` and clipped to it; `size` in points.
    void MonoText(std::wstring_view text, const pui::RectF& r, float size, pui::Color c);
    /// The advance of one character ("0") and the line height (ascent + descent + leading, rounded up) of that face.
    pui::SizeF MonoCell(float size);

private:
    struct Impl;
    Impl* impl_;
    CGContextRef ctx_ = nullptr;
    float scale_ = 1;
    float opacity_ = 1;
    std::vector<float> opacityStack_;
    int clipDepth_ = 0;

    /// The widgets hard-code Fluent sizes (14 / 12 / 20 / 28) instead of reading Theme.font*; render and measure
    /// FontKind::Text at the macOS HIG sizes (13 / 11 / 17 / 22). Other sizes pass through.
    float EffectiveSize(const pui::TextStyle& s) const;
    CGColorRef MakeColor(pui::Color c) const;   // caller releases
    void DrawIcon(std::wstring_view text, const pui::RectF& r, const pui::TextStyle& s, pui::Color c);
};

}
