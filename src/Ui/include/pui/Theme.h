#pragma once
// Theme tokens based on WinUI Fluent tokens. Values are straight ARGB.
#include "Geometry.h"

namespace pui {

enum class ThemeMode { System, Light, Dark };

struct Theme {
    bool light = false;
    Color accent = 0xFFFF8232;          // AccentFillColorDefault (dark: SystemAccentColorLight2, light: Dark1)
    Color accentHover = 0xE6FF8232;     // accent at 0.9
    Color accentPressed = 0xCCFF8232;   // accent at 0.8
    Color textOnAccent = 0xFF000000;    // TextOnAccentFillColorPrimary

    Color barFill = 0xB31C1C1C;          // ControlOnImageFillColorDefault
    Color barStroke = 0x19000000;        // CardStrokeColorDefault
    Color textPrimary = 0xFFFFFFFF, textSecondary = 0xC5FFFFFF, textTertiary = 0x8BFFFFFF, textDisabled = 0x5DFFFFFF;
    Color subtleTransparent = 0x00FFFFFF, subtleSecondary = 0x0FFFFFFF, subtleTertiary = 0x0AFFFFFF, subtleDisabled = 0x00FFFFFF;
    Color ctlDefault = 0x0FFFFFFF, ctlSecondary = 0x15FFFFFF, ctlTertiary = 0x08FFFFFF, ctlDisabled = 0x0BFFFFFF;
    Color ctlInputActive = 0xB31E1E1E;   // ControlFillColorInputActive (focused text box)
    Color ctlAltSecondary = 0x19000000;  // ControlAltFillColorSecondary (toggle switch off fill)
    Color ctlStrokeDefault = 0x12FFFFFF, ctlStrokeSecondary = 0x18FFFFFF;
    Color ctlStrongStroke = 0x8BFFFFFF, ctlStrongStrokeDisabled = 0x28FFFFFF;
    Color ctlStrongFill = 0x8BFFFFFF, ctlStrongFillDisabled = 0x28FFFFFF;   // slider track
    Color ctlSolidFill = 0xFF454545;     // slider thumb outer
    Color ringDisabled = 0x14FFFFFF;     // play ring disabled
    Color focusStrokeOuter = 0xFFFFFFFF, focusStrokeInner = 0xB3000000;
    Color solidBackground = 0xFF202020;  // SolidBackgroundFillColorBase (window fallback when Mica is unavailable)
    Color layerFill = 0x4C3A3A3A;        // LayerFillColorDefault (page/content layer over Mica)
    Color cardFill = 0x0DFFFFFF, cardFillSecondary = 0x08FFFFFF, cardStroke = 0x19000000;
    Color dividerStroke = 0x15FFFFFF;
    Color panelFill = 0x0AFFFFFF, panelStroke = 0x14FFFFFF;   // side panel surface (white 4%) and its 1px border (white 8%)
    float hairline = 1;                 // one DEVICE pixel in DIP (1 / scale; set by the host): 1 px borders at any DPI
    Color chipStroke = 0x24FFFFFF;                             // codec chip outline (white 14%)
    Color flyoutSurface = 0xFF2C2C2C, flyoutStroke = 0x33000000;
    // AcrylicInAppFillColorDefaultBrush (WinUI 3): the presenter applies these tokens over the video; flyoutSurface
    // remains the opaque FallbackColor whenever Acrylic is disabled or unavailable.
    Color acrylicFallbackColor = 0xFF2C2C2C;
    Color acrylicTintColor = 0xFF2C2C2C, acrylicLuminosityColor = 0xFF2C2C2C;
    float acrylicTintOpacity = 0.15f, acrylicLuminosityOpacity = 0.96f;
    bool acrylicEnabled = false;        // only tagged Acrylic surfaces use a transparent fill while this is true
    Color smokeFill = 0x4D000000;        // dialog backdrop
    Color critical = 0xFFFF99A4, criticalBg = 0xFF442726;
    Color caution = 0xFFFCE100, cautionBg = 0xFF433519;
    Color success = 0xFF6CCB5F, successBg = 0xFF393D1B;
    Color attentionBg = 0x08FFFFFF;      // informational InfoBar background; icon uses accent

    // DIP
    float controlRadius = 4, overlayRadius = 8, fontBody = 14, fontCaption = 12, fontSubtitle = 20, fontTitle = 28;

    /// Builds the full token set. accentArgb is the system accent colour for the theme (already the Light2/Dark1
    /// variant); 0 picks the fallback (Zest FFFF8232 dark / FFD03F0A light).
    static Theme Make(bool light, Color accentArgb = 0);
};

inline Theme Theme::Make(bool lightTheme, Color acc) {
    Theme t;
    t.light = lightTheme;
    if (acc.v == 0) acc = lightTheme ? Color(0xFFD03F0A) : Color(0xFFFF8232);
    t.accent = acc;
    t.accentHover = MulAlpha(acc, 0.9f);
    t.accentPressed = MulAlpha(acc, 0.8f);
    if (!lightTheme) return t;   // defaults above are the dark values
    t.textOnAccent = 0xFFFFFFFF;
    t.barFill = 0xC9FFFFFF; t.barStroke = 0x0F000000;
    t.textPrimary = 0xE4000000; t.textSecondary = 0x9E000000; t.textTertiary = 0x72000000; t.textDisabled = 0x5C000000;
    t.subtleTransparent = 0x00FFFFFF; t.subtleSecondary = 0x09000000; t.subtleTertiary = 0x06000000; t.subtleDisabled = 0x00000000;
    t.ctlDefault = 0xB3FFFFFF; t.ctlSecondary = 0x80F9F9F9; t.ctlTertiary = 0x4DF9F9F9; t.ctlDisabled = 0x4DF9F9F9;
    t.ctlInputActive = 0xFFFFFFFF; t.ctlAltSecondary = 0x06000000;
    t.ctlStrokeDefault = 0x0F000000; t.ctlStrokeSecondary = 0x29000000;
    t.ctlStrongStroke = 0x72000000; t.ctlStrongStrokeDisabled = 0x37000000;
    t.ctlStrongFill = 0x72000000; t.ctlStrongFillDisabled = 0x51000000;
    t.ctlSolidFill = 0xFFFFFFFF; t.ringDisabled = 0x0F000000;
    t.focusStrokeOuter = 0xE4000000; t.focusStrokeInner = 0xB3FFFFFF;
    t.solidBackground = 0xFFF3F3F3; t.layerFill = 0x80FFFFFF;
    t.cardFill = 0xB3FFFFFF; t.cardFillSecondary = 0x80F6F6F6; t.cardStroke = 0x0F000000;
    t.dividerStroke = 0x0F000000;
    t.panelFill = 0x0A000000; t.panelStroke = 0x14000000; t.chipStroke = 0x24000000;
    t.flyoutSurface = 0xFFF9F9F9; t.flyoutStroke = 0x0F000000;
    t.acrylicFallbackColor = 0xFFF9F9F9;
    t.acrylicTintColor = 0xFFFCFCFC; t.acrylicTintOpacity = 0.0f;
    t.acrylicLuminosityColor = 0xFFFCFCFC; t.acrylicLuminosityOpacity = 0.85f;
    t.smokeFill = 0x4D000000;
    t.critical = 0xFFC42B1C; t.criticalBg = 0xFFFDE7E9;
    t.caution = 0xFF9D5D00; t.cautionBg = 0xFFFFF4CE;
    t.success = 0xFF0F7B0F; t.successBg = 0xFFDFF6DD;
    t.attentionBg = 0x80F6F6F6;
    return t;
}

}
