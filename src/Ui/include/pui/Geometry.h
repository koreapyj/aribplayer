#pragma once
// All PlayerUi coordinates are DIPs in window-client space (the renderer applies the DPI scale).
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pui {

struct PointF {
    float x = 0, y = 0;
};

struct SizeF {
    float w = 0, h = 0;
};

struct RectF {
    float l = 0, t = 0, r = 0, b = 0;
    static RectF XYWH(float x, float y, float w, float h) { return {x, y, x + w, y + h}; }
    float w() const { return r - l; }
    float h() const { return b - t; }
    float cx() const { return (l + r) * 0.5f; }
    float cy() const { return (t + b) * 0.5f; }
    bool Empty() const { return !(r > l && b > t); }
    bool Contains(PointF p) const { return p.x >= l && p.x < r && p.y >= t && p.y < b; }
    bool Contains(float x, float y) const { return Contains(PointF{x, y}); }
    RectF Offset(float dx, float dy) const { return {l + dx, t + dy, r + dx, b + dy}; }
    RectF Inflate(float d) const { return {l - d, t - d, r + d, b + d}; }
    RectF Deflate(float dl, float dt, float dr, float db) const { return {l + dl, t + dt, r - dr, b - db}; }
    RectF Intersect(const RectF& o) const {
        RectF x{std::max(l, o.l), std::max(t, o.t), std::min(r, o.r), std::min(b, o.b)};
        if (x.r < x.l) x.r = x.l;
        if (x.b < x.t) x.b = x.t;
        return x;
    }
    bool Intersects(const RectF& o) const { return l < o.r && o.l < r && t < o.b && o.t < b; }
    bool operator==(const RectF& o) const { return l == o.l && t == o.t && r == o.r && b == o.b; }
    bool operator!=(const RectF& o) const { return !(*this == o); }
};

/// Straight (non-premultiplied) ARGB colour, 0xAARRGGBB.
struct Color {
    uint32_t v = 0;
    constexpr Color() = default;
    constexpr Color(uint32_t x) : v(x) {}
    float a() const { return ((v >> 24) & 0xFF) / 255.f; }
    float r() const { return ((v >> 16) & 0xFF) / 255.f; }
    float g() const { return ((v >> 8) & 0xFF) / 255.f; }
    float b() const { return (v & 0xFF) / 255.f; }
    bool operator==(const Color& o) const { return v == o.v; }
    bool operator!=(const Color& o) const { return v != o.v; }
};

inline Color WithAlpha(Color c, float alpha01) {
    uint32_t a = (uint32_t)std::lround(std::clamp(alpha01, 0.f, 1.f) * 255.f) & 0xFFu;
    return Color((a << 24) | (c.v & 0x00FFFFFF));
}
inline Color MulAlpha(Color c, float m) {
    uint32_t a = (uint32_t)std::lround(((c.v >> 24) & 0xFF) * std::clamp(m, 0.f, 1.f)) & 0xFFu;
    return Color((a << 24) | (c.v & 0x00FFFFFF));
}

/// A DIP coordinate rounded to a whole device pixel at `scale` (device px per DIP): 1 px lines on it stay crisp at 125 / 150 %.
inline float SnapDip(float dip, float scale) {
    if (scale <= 0) return dip;
    return std::round(dip * scale) / scale;
}

inline float EaseOut(float s) { s = std::clamp(s, 0.f, 1.f); float u = 1 - s; return 1 - u * u * u; }
inline float EaseIn(float s) { s = std::clamp(s, 0.f, 1.f); return s * s * s; }

enum Mods : unsigned { ModNone = 0, ModShift = 1, ModCtrl = 2, ModAlt = 4 };

/// Virtual-key codes used by PlayerUi (identical to the Win32 VK_* values, so callers can pass VKs through).
namespace key {
constexpr int Back = 0x08, Tab = 0x09, Enter = 0x0D, Escape = 0x1B, Space = 0x20, PageUp = 0x21, PageDown = 0x22,
              End = 0x23, Home = 0x24, Left = 0x25, Up = 0x26, Right = 0x27, Down = 0x28, Delete = 0x2E, Apps = 0x5D,
              F2 = 0x71, F4 = 0x73, F6 = 0x75, F10 = 0x79, A = 0x41;
}

}
