#include "puimac/MacTheme.h"

#import <AppKit/AppKit.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

using pui::Color;
using pui::Theme;

namespace puimac {

namespace {
Color ArgbOf(NSColor* c) {
    NSColor* s = [c colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    if (!s) return 0;
    auto ch = [](CGFloat v) { return (uint32_t)std::lround(std::clamp((double)v, 0.0, 1.0) * 255.0); };
    return Color((ch(s.alphaComponent) << 24) | (ch(s.redComponent) << 16) | (ch(s.greenComponent) << 8) | ch(s.blueComponent));
}

struct Rgb { double r, g, b; };

double Linear(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
double Srgb(double c) { return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055; }

Rgb FromOklab(double L, double a, double b) {
    double l = L + 0.3963377774 * a + 0.2158037573 * b;
    double m = L - 0.1055613458 * a - 0.0638541728 * b;
    double s = L - 0.0894841775 * a - 1.2914855480 * b;
    l *= l * l; m *= m * m; s *= s * s;
    return {4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
           -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
           -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s};
}

Color AccentShade(Color raw, bool dark) {
    const double r = Linear(raw.r()), g = Linear(raw.g()), b = Linear(raw.b());
    const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    const double L = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
    const double a = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
    const double ob = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
    // OKLCH lightness supplies the light/dark accent roles; these are not Windows OS palette values.
    // Chromatic accents use the sRGB gamut boundary at constant hue and lightness; neutral accents stay neutral.
    const double target = dark ? std::max(L, 0.80) : std::min(L, 0.45);
    const double chroma = std::hypot(a, ob);
    const double hueA = chroma > 1e-5 ? a / chroma : 0, hueB = chroma > 1e-5 ? ob / chroma : 0;
    auto inGamut = [](Rgb c) { return c.r >= 0 && c.r <= 1 && c.g >= 0 && c.g <= 1 && c.b >= 0 && c.b <= 1; };
    double lo = 0, hi = chroma > 1e-5 ? 0.5 : 0;
    for (int i = 0; i < 24; ++i) {
        const double mid = (lo + hi) / 2;
        if (inGamut(FromOklab(target, hueA * mid, hueB * mid))) lo = mid;
        else hi = mid;
    }
    const Rgb c = FromOklab(target, hueA * lo, hueB * lo);
    auto ch = [](double v) { return (uint32_t)std::lround(std::clamp(Srgb(v), 0.0, 1.0) * 255); };
    return Color(0xFF000000u | (ch(c.r) << 16) | (ch(c.g) << 8) | ch(c.b));
}
}

bool SystemIsDark() {
    NSAppearance* ap = NSApp ? NSApp.effectiveAppearance : NSAppearance.currentDrawingAppearance;
    if (ap) {
        NSAppearanceName n = [ap bestMatchFromAppearancesWithNames:@[NSAppearanceNameAqua, NSAppearanceNameDarkAqua]];
        return [n isEqualToString:NSAppearanceNameDarkAqua];
    }
    NSString* style = [[NSUserDefaults standardUserDefaults] stringForKey:@"AppleInterfaceStyle"];
    return [style isEqualToString:@"Dark"];
}

Color SystemAccentColor(bool dark) {
    __block Color out = 0;
    NSAppearance* ap = [NSAppearance appearanceNamed:dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
    [ap performAsCurrentDrawingAppearance:^{ out = ArgbOf(NSColor.controlAccentColor); }];
    if ((out.v >> 24) == 0) out = dark ? Color(0xFF0A84FF) : Color(0xFF007AFF);   // system blue
    return Color(0xFF000000 | (out.v & 0x00FFFFFF));
}

// Rationale (macOS HIG + the system colours of macOS 14-26):
//  * Text: NSColor.labelColor is 85 % black/white, secondaryLabel 50 % / 55 %, tertiaryLabel 26 % / 25 %, and disabled
//    controls use the tertiary level.
//  * Surfaces: windowBackgroundColor is #ECECEC light / #323232 dark; text fields are white / #1E1E1E; separators are
//    10 % of the opposite colour. Buttons in light mode are white with a 12 % border (the bezel shadow), in dark mode a
//    ~17 % white fill with a 10 % white border. Menus/popovers are #F0F0F0 / #2E2E30 (opaque fallbacks for the
//    vibrant materials) with a 10 pt radius.
//  * Accent: a hue-preserving light/dark shade of controlAccentColor; text on accent is black in dark mode, white in
//    light mode. Explicit accent overrides pass through, as in the Windows host. The focus ring is the accent at ~55 %.
//  * Radii: 6 pt for controls (standard push button / text field radius for 28-32 pt tall controls), 10 pt for
//    menus, popovers and cards.
//  * Type (points): body 13, caption 11 (HIG "subheadline"/caption range), title 2 = 17 for subtitles, title 1 = 22
//    for titles. TextBox/VirtualList read these tokens; CGRenderer's size shim covers the other widgets.
//  * Semantic colours: systemRed / systemOrange (light) or systemYellow (dark) / systemGreen; their InfoBar backgrounds
//    are the same colour at ~15-20 % so they sit on any surface.
Theme MakeMacTheme(bool dark, Color accent, float scale) {
    if (accent.v == 0) accent = AccentShade(SystemAccentColor(dark), dark);
    Theme t = Theme::Make(!dark, accent);
    // The opaque panel uses the shared base (#202020 dark / #F3F3F3 light). Its selected-row tint stays bounded
    // independently of the native vibrant material, so the accent can retain its chroma at moderate lightness.
    t.panelFill = t.solidBackground;
    t.hairline = scale > 0 ? 1.f / scale : 1.f;
    t.controlRadius = 6;
    t.overlayRadius = 10;
    t.fontBody = 13;
    t.fontCaption = 11;
    t.fontSubtitle = 17;
    t.fontTitle = 22;
    t.acrylicEnabled = false;
    t.acrylicTintOpacity = 0.f;
    t.acrylicLuminosityOpacity = 1.f;
    t.focusStrokeOuter = pui::WithAlpha(accent, 0.55f);
    t.focusStrokeInner = 0x00000000;
    if (dark) {
        t.barFill = 0xBF1E1E1E; t.barStroke = 0x26000000;
        t.textPrimary = 0xD9FFFFFF; t.textSecondary = 0x8CFFFFFF; t.textTertiary = 0x40FFFFFF; t.textDisabled = 0x40FFFFFF;
        t.subtleTransparent = 0x00FFFFFF; t.subtleSecondary = 0x14FFFFFF; t.subtleTertiary = 0x0AFFFFFF; t.subtleDisabled = 0x00FFFFFF;
        // Current rows and lit bar buttons use the shared dark theme's 0x15FFFFFF secondary fill.
        t.ctlDefault = 0x2BFFFFFF; t.ctlTertiary = 0x1FFFFFFF; t.ctlDisabled = 0x12FFFFFF;
        t.ctlInputActive = 0xFF1E1E1E; t.ctlAltSecondary = 0x3DFFFFFF;
        t.ctlStrokeDefault = 0x1AFFFFFF; t.ctlStrokeSecondary = 0x26FFFFFF;
        t.ctlStrongStroke = 0x59FFFFFF; t.ctlStrongStrokeDisabled = 0x26FFFFFF;
        t.ctlStrongFill = 0x40FFFFFF; t.ctlStrongFillDisabled = 0x1FFFFFFF;
        t.ctlSolidFill = 0xFFCFCFCF; t.ringDisabled = 0x14FFFFFF;
        t.solidBackground = 0xFF323232; t.layerFill = 0x1AFFFFFF;
        t.cardFill = 0x0FFFFFFF; t.cardFillSecondary = 0x08FFFFFF; t.cardStroke = 0x1AFFFFFF;
        t.dividerStroke = 0x1AFFFFFF;
        t.panelStroke = 0x1AFFFFFF; t.chipStroke = 0x33FFFFFF;
        t.flyoutSurface = 0xFF2E2E30; t.flyoutStroke = 0x1AFFFFFF;
        t.acrylicFallbackColor = 0xFF2E2E30; t.acrylicTintColor = 0xFF2E2E30; t.acrylicLuminosityColor = 0xFF2E2E30;
        t.smokeFill = 0x4D000000;
        t.critical = 0xFFFF453A; t.criticalBg = 0x33FF453A;
        t.caution = 0xFFFFD60A; t.cautionBg = 0x2EFFD60A;
        t.success = 0xFF32D74B; t.successBg = 0x3332D74B;
        t.attentionBg = 0x14FFFFFF;
    } else {
        t.barFill = 0xD9F5F5F5; t.barStroke = 0x1A000000;
        t.textPrimary = 0xD9000000; t.textSecondary = 0x80000000; t.textTertiary = 0x42000000; t.textDisabled = 0x40000000;
        t.subtleTransparent = 0x00000000; t.subtleSecondary = 0x0F000000; t.subtleTertiary = 0x08000000; t.subtleDisabled = 0x00000000;
        t.ctlDefault = 0xFFFFFFFF; t.ctlSecondary = 0xFFF7F7F7; t.ctlTertiary = 0xFFE9E9E9; t.ctlDisabled = 0x80FFFFFF;
        t.ctlInputActive = 0xFFFFFFFF; t.ctlAltSecondary = 0xFFE3E3E3;
        t.ctlStrokeDefault = 0x1F000000; t.ctlStrokeSecondary = 0x33000000;
        t.ctlStrongStroke = 0x4D000000; t.ctlStrongStrokeDisabled = 0x26000000;
        t.ctlStrongFill = 0x29000000; t.ctlStrongFillDisabled = 0x14000000;
        t.ctlSolidFill = 0xFFFFFFFF; t.ringDisabled = 0x0F000000;
        t.solidBackground = 0xFFECECEC; t.layerFill = 0x66FFFFFF;
        t.cardFill = 0x99FFFFFF; t.cardFillSecondary = 0x66FFFFFF; t.cardStroke = 0x14000000;
        t.dividerStroke = 0x1A000000;
        t.panelStroke = 0x14000000; t.chipStroke = 0x33000000;
        t.flyoutSurface = 0xFFF0F0F0; t.flyoutStroke = 0x1F000000;
        t.acrylicFallbackColor = 0xFFF0F0F0; t.acrylicTintColor = 0xFFF0F0F0; t.acrylicLuminosityColor = 0xFFF0F0F0;
        t.smokeFill = 0x33000000;
        t.critical = 0xFFFF3B30; t.criticalBg = 0x26FF3B30;
        t.caution = 0xFFFF9500; t.cautionBg = 0x26FF9500;
        t.success = 0xFF28CD41; t.successBg = 0x2628CD41;
        t.attentionBg = 0x0A000000;
    }
    return t;
}

Theme MakeMacThemeForSystem(float scale) { return MakeMacTheme(SystemIsDark(), 0, scale); }

}

// --- observer -----------------------------------------------------------------------------------------------------------

@interface PuiMacThemeObserver : NSObject
@property(nonatomic, copy) void (^onChange)(void);
@end

@implementation PuiMacThemeObserver
- (instancetype)init {
    if ((self = [super init])) {
        [NSApp addObserver:self forKeyPath:@"effectiveAppearance" options:NSKeyValueObservingOptionNew context:nullptr];
        [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(fire:) name:NSSystemColorsDidChangeNotification object:nil];
        [[NSDistributedNotificationCenter defaultCenter] addObserver:self selector:@selector(fire:)
                                                                name:@"AppleColorPreferencesChangedNotification" object:nil];
        [[NSDistributedNotificationCenter defaultCenter] addObserver:self selector:@selector(fire:)
                                                                name:@"AppleInterfaceThemeChangedNotification" object:nil];
    }
    return self;
}
- (void)dealloc {
    [NSApp removeObserver:self forKeyPath:@"effectiveAppearance"];
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [[NSDistributedNotificationCenter defaultCenter] removeObserver:self];
}
- (void)observeValueForKeyPath:(NSString*)k ofObject:(id)o change:(NSDictionary*)c context:(void*)ctx {
    (void)k; (void)o; (void)c; (void)ctx;
    [self fire:nil];
}
- (void)fire:(NSNotification*)n {
    (void)n;
    dispatch_async(dispatch_get_main_queue(), ^{ if (self.onChange) self.onChange(); });
}
@end

namespace puimac {

ThemeObserver::ThemeObserver(std::function<void()> onChange) {
    PuiMacThemeObserver* o = [[PuiMacThemeObserver alloc] init];
    o.onChange = ^{ onChange(); };
    impl_ = (void*)CFBridgingRetain(o);
}

ThemeObserver::~ThemeObserver() {
    if (impl_) CFBridgingRelease(impl_);
}

}
