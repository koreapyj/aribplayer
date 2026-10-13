#include "wvp/SubtitleStyle.h"

namespace wvp {

namespace {

// Windows documents only the style classes; these are the families Windows' own caption rendering uses (the same
// table as Chromium's caption_style_win.cc). Small capitals has no small-caps rendering here: Tahoma.
std::optional<std::string> CaptionFamily(int style) {
    switch (style) {
    case 1: return "Courier New";        // MonospacedWithSerifs
    case 2: return "Times New Roman";    // ProportionalWithSerifs
    case 3: return "Consolas";           // MonospacedWithoutSerifs
    case 4: return "Tahoma";             // ProportionalWithoutSerifs
    case 5: return "Segoe Print";        // Casual
    case 6: return "Segoe Script";       // Cursive
    case 7: return "Tahoma";             // SmallCapitals
    default: return std::nullopt;
    }
}

std::optional<double> CaptionScale(int size) {
    switch (size) {
    case 1: return 0.5;
    case 2: return 1.0;
    case 3: return 1.5;
    case 4: return 2.0;
    default: return std::nullopt;
    }
}

std::optional<std::uint32_t> CaptionRgb(int color) {   // 0xRRGGBB00
    switch (color) {
    case 1: return 0xFFFFFF00u;   // White
    case 2: return 0x00000000u;   // Black
    case 3: return 0xFF000000u;   // Red
    case 4: return 0x00FF0000u;   // Green
    case 5: return 0x0000FF00u;   // Blue
    case 6: return 0xFFFF0000u;   // Yellow
    case 7: return 0xFF00FF00u;   // Magenta
    case 8: return 0x00FFFF00u;   // Cyan
    default: return std::nullopt;
    }
}

std::optional<std::uint32_t> CaptionAlpha(int opacity) {
    switch (opacity) {
    case 1: return 0xFFu;   // 100 %
    case 2: return 0xBFu;   // 75 %
    case 3: return 0x40u;   // 25 %
    case 4: return 0x00u;   // 0 %
    default: return std::nullopt;
    }
}

// A colour and its opacity are one renderer value: either one set defines it, the other keeps the Windows default look
// (`rgb` / opaque).
std::optional<std::uint32_t> CaptionRgba(int color, int opacity, std::uint32_t rgb) {
    auto c = CaptionRgb(color);
    auto a = CaptionAlpha(opacity);
    if (!c && !a) return std::nullopt;
    return c.value_or(rgb) | a.value_or(0xFFu);
}

}

SystemCaptionStyle MapWindowsCaptionProperties(const WindowsCaptionProperties& p) {
    SystemCaptionStyle s;
    s.font = CaptionFamily(p.fontStyle);
    s.fontScale = CaptionScale(p.fontSize);
    s.textRgba = CaptionRgba(p.fontColor, p.fontOpacity, 0xFFFFFF00u);
    switch (p.fontEffect) {   // Raised (2) and Depressed (3) are embossed edges the renderer has no style for
    case 1: s.edge = CaptionEdge::None; break;
    case 4: s.edge = CaptionEdge::Outline; break;
    case 5: s.edge = CaptionEdge::DropShadow; break;
    default: break;
    }
    s.backRgba = CaptionRgba(p.backgroundColor, p.backgroundOpacity, 0x00000000u);
    return s;
}

SubtitleRenderStyle ResolveSubtitleStyle(const SubtitleStyle& settings, const WindowsCaptionProperties& windows) {
    SubtitleRenderStyle r;
    r.applyToAss = settings.applyToAss;
    unsigned defined = 0;
    if (settings.useSystemCaptionStyle) {
        const SystemCaptionStyle w = MapWindowsCaptionProperties(windows);
        if (w.font) { r.font = *w.font; defined |= kSubFont; }
        if (w.fontScale) { r.fontScale = *w.fontScale; defined |= kSubSize; }
        if (w.textRgba) { r.primaryRgba = *w.textRgba; defined |= kSubTextColor; }
        if (w.edge) {
            r.outlineWidth = *w.edge == CaptionEdge::Outline ? kSystemCaptionOutlinePx : 0.0;
            r.shadow = *w.edge == CaptionEdge::DropShadow ? kSystemCaptionShadowPx : 0.0;
            defined |= kSubBorder;
        }
        if (w.backRgba) { r.backRgba = *w.backRgba; defined |= kSubBackground; }
    } else {
        r.font = settings.font;
        r.fontWeight = settings.fontWeight;
        r.italic = settings.italic;
        r.fontSizePt = settings.fontSizePt;
        r.primaryRgba = settings.primaryRgba;
        r.outlineRgba = settings.outlineRgba;
        r.outlineWidth = settings.outlineWidth;
        defined = kSubFont | kSubFontStyle | kSubSize | kSubTextColor | kSubOutlineColor | kSubBorder;
    }
    const unsigned switched = (settings.overrideFont ? kSubFont | kSubFontStyle : 0u) | (settings.overrideSize ? kSubSize : 0u) |
                              (settings.overrideColors ? kSubTextColor | kSubOutlineColor | kSubBackground : 0u) |
                              (settings.overrideBorder ? kSubBorder : 0u);
    r.fallbackFlags = defined;
    r.overrideFlags = defined & switched;
    return r;
}

}
