#pragma once
// The app implements it with Direct2D/DirectWrite
// (pui/D2dRenderer.h); tests use pui::RecordingRenderer (below), which needs no device.
// Coordinates are DIPs; the D2D implementation applies the DPI scale as a transform.
#include <string>
#include <string_view>
#include <vector>

#include "Geometry.h"

namespace pui {

enum class FontKind { Text, Icon };              // Text = Segoe UI Variable (fallback Segoe UI); Icon = Segoe Fluent Icons (fallback Segoe MDL2 Assets)
enum class TextAlign { Leading, Center, Trailing };
enum class ParaAlign { Top, Center, Bottom };

struct TextStyle {
    FontKind font = FontKind::Text;
    float size = 14;          // DIP
    int weight = 400;         // DWRITE_FONT_WEIGHT
    TextAlign align = TextAlign::Leading;
    ParaAlign valign = ParaAlign::Center;
    bool wrap = false;
    bool ellipsis = true;     // character ellipsis trimming when the text does not fit (no wrap)
};

class IRenderer {
public:
    virtual ~IRenderer() = default;
    virtual void FillRect(const RectF& r, Color c) = 0;
    virtual void FillRoundRect(const RectF& r, float radius, Color c) = 0;
    virtual void StrokeRoundRect(const RectF& r, float radius, Color c, float width = 1) = 0;
    virtual void FillEllipse(PointF center, float rx, float ry, Color c) = 0;
    virtual void StrokeEllipse(PointF center, float rx, float ry, Color c, float width = 1) = 0;
    virtual void Line(PointF a, PointF b, Color c, float width = 1) = 0;
    virtual void Text(std::wstring_view text, const RectF& r, const TextStyle& style, Color c) = 0;
    /// Size of the laid-out text (maxWidth <= 0: unconstrained).
    virtual SizeF MeasureText(std::wstring_view text, const TextStyle& style, float maxWidth = 0) = 0;
    virtual void PushClip(const RectF& r) = 0;
    virtual void PopClip() = 0;
    /// Opacity applied to everything drawn until the matching PopOpacity (nests multiplicatively).
    virtual void PushOpacity(float opacity) = 0;
    virtual void PopOpacity() = 0;
};

/// Test renderer: records every call; MeasureText is deterministic (0.5 * size per character, line height 1.33 * size).
class RecordingRenderer : public IRenderer {
public:
    enum class Op { FillRect, FillRoundRect, StrokeRoundRect, FillEllipse, StrokeEllipse, Line, Text, PushClip, PopClip, PushOpacity, PopOpacity };
    struct Call {
        Op op;
        RectF rect{};
        Color color{};
        std::wstring text;
        TextStyle style{};
        float value = 0;
    };
    std::vector<Call> calls;

    void FillRect(const RectF& r, Color c) override { calls.push_back({Op::FillRect, r, c}); }
    void FillRoundRect(const RectF& r, float rad, Color c) override { calls.push_back({Op::FillRoundRect, r, c, {}, {}, rad}); }
    void StrokeRoundRect(const RectF& r, float rad, Color c, float) override { calls.push_back({Op::StrokeRoundRect, r, c, {}, {}, rad}); }
    void FillEllipse(PointF p, float rx, float ry, Color c) override { calls.push_back({Op::FillEllipse, {p.x - rx, p.y - ry, p.x + rx, p.y + ry}, c}); }
    void StrokeEllipse(PointF p, float rx, float ry, Color c, float) override { calls.push_back({Op::StrokeEllipse, {p.x - rx, p.y - ry, p.x + rx, p.y + ry}, c}); }
    void Line(PointF a, PointF b, Color c, float w) override { calls.push_back({Op::Line, {a.x, a.y, b.x, b.y}, c, {}, {}, w}); }
    void Text(std::wstring_view t, const RectF& r, const TextStyle& s, Color c) override { calls.push_back({Op::Text, r, c, std::wstring(t), s}); }
    SizeF MeasureText(std::wstring_view t, const TextStyle& s, float maxWidth) override {
        float w = 0.5f * s.size * (float)t.size(), lh = 1.33f * s.size;
        if (maxWidth > 0 && s.wrap && w > maxWidth) {
            float lines = (float)(int)((w + maxWidth - 1) / maxWidth);
            return {maxWidth, lh * lines};
        }
        return {maxWidth > 0 ? (w < maxWidth ? w : maxWidth) : w, lh};
    }
    void PushClip(const RectF& r) override { calls.push_back({Op::PushClip, r}); }
    void PopClip() override { calls.push_back({Op::PopClip}); }
    void PushOpacity(float o) override { calls.push_back({Op::PushOpacity, {}, {}, {}, {}, o}); }
    void PopOpacity() override { calls.push_back({Op::PopOpacity}); }

    void Clear() { calls.clear(); }
    bool HasText(std::wstring_view t) const {
        for (auto& c : calls) if (c.op == Op::Text && c.text == t) return true;
        return false;
    }
    int Count(Op op) const {
        int n = 0;
        for (auto& c : calls) if (c.op == op) n++;
        return n;
    }
};

}
