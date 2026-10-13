#import <AppKit/AppKit.h>

#include "puimac/MacTheme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
using pui::Color;

double Linear(double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }

double Luminance(Color c) {
    return 0.2126 * Linear(c.r()) + 0.7152 * Linear(c.g()) + 0.0722 * Linear(c.b());
}

struct Lab { double L, a, b; };
Lab Oklab(Color c) {
    const double r = Linear(c.r()), g = Linear(c.g()), b = Linear(c.b());
    const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    return {0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
            1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
            0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s};
}

double Contrast(Color a, Color b) {
    const double x = Luminance(a), y = Luminance(b);
    return (std::max(x, y) + 0.05) / (std::min(x, y) + 0.05);
}

Color Over(Color fg, Color bg, bool calibrated) {
    // AppKit can composite in a calibrated destination, not sRGB. Measure the actual CoreGraphics source-over result
    // in both spaces, then convert it to sRGB for WCAG luminance; sRGB-only arithmetic can overestimate selection contrast.
    CGColorSpaceRef space = CGColorSpaceCreateWithName(calibrated ? kCGColorSpaceGenericRGB : kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(nullptr, 1, 1, 8, 4, space, kCGImageAlphaPremultipliedLast);
    for (Color c : {bg, fg}) {
        CGColorRef color = CGColorCreateSRGB(c.r(), c.g(), c.b(), c.a());
        CGContextSetFillColorWithColor(ctx, color);
        CGContextFillRect(ctx, CGRectMake(0, 0, 1, 1));
        CGColorRelease(color);
    }
    CGImageRef image = CGBitmapContextCreateImage(ctx);
    uint8_t pixel[4]{};
    CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef dest = CGBitmapContextCreate(pixel, 1, 1, 8, 4, srgb, kCGImageAlphaPremultipliedLast);
    CGContextDrawImage(dest, CGRectMake(0, 0, 1, 1), image);
    CGContextRelease(dest);
    CGColorSpaceRelease(srgb);
    CGImageRelease(image);
    CGContextRelease(ctx);
    CGColorSpaceRelease(space);
    return Color(0xFF000000u | (uint32_t(pixel[0]) << 16) | (uint32_t(pixel[1]) << 8) | pixel[2]);
}
}

int main() {
    @autoreleasepool {
        [NSApplication sharedApplication];
        int failures = 0;
        auto check = [&](bool ok, const char* message) {
            printf("%s %s\n", ok ? "PASS" : "FAIL", message);
            if (!ok) ++failures;
        };
        for (bool calibrated : {false, true}) for (bool dark : {true, false}) {
            const auto raw = puimac::SystemAccentColor(dark);
            const auto t = puimac::MakeMacTheme(dark);
            // The dark bar is checked on a brighter native material than solidBackground. The panel is opaque in both
            // appearances, so its text contrast does not depend on the native material or anything behind the window.
            const Color material = dark ? Color(0xFF4A4A4A) : t.solidBackground;
            const Color renderedAccent = Over(t.accent, material, calibrated);   // calibrated gamut mapping also affects opaque text
            const Color panel = Over(t.panelFill, material, calibrated);
            const Color current = Over(t.ctlSecondary, panel, calibrated);
            const Color selected = Over(pui::WithAlpha(t.accent, 0x38 / 255.f), panel, calibrated);
            const Color lit = Over(t.ctlSecondary, material, calibrated);
            printf("%s %s raw=%08X accent=%08X row=%.3f selected=%.3f subtitle=%.3f seek=%.3f onAccent=%.3f\n",
                   calibrated ? "calibrated" : "sRGB", dark ? "dark" : "light", raw.v, t.accent.v, Contrast(renderedAccent, current), Contrast(renderedAccent, selected),
                   Contrast(renderedAccent, lit), Contrast(renderedAccent, material), Contrast(t.textOnAccent, renderedAccent));
            check(Contrast(renderedAccent, current) >= 4.5, "current row text contrast >= 4.5");
            check(Contrast(renderedAccent, selected) >= 4.5, "selected current row text contrast >= 4.5");
            check(Contrast(renderedAccent, lit) >= 3, "lit subtitle contrast >= 3");
            check(Contrast(renderedAccent, material) >= 3, "seek indicator contrast >= 3");
            check(Contrast(renderedAccent, t.solidBackground) >= 4.5, "link text contrast >= 4.5");
            check(Contrast(t.textOnAccent, renderedAccent) >= 4.5, "text on accent contrast >= 4.5");
            check(dark ? Luminance(t.accent) >= Luminance(raw) : Luminance(t.accent) <= Luminance(raw), "appearance shade has the correct luminance direction");
            check(t.textOnAccent == (dark ? Color(0xFF000000) : Color(0xFFFFFFFF)), "text on accent follows the shared theme");
            check(t.accentHover == pui::MulAlpha(t.accent, 0.9f) && t.accentPressed == pui::MulAlpha(t.accent, 0.8f), "accent state alpha follows the shared theme");
            check(puimac::MakeMacTheme(dark, raw).accent == raw, "explicit accent override passes through unchanged");
            check(t.panelFill == pui::Theme::Make(!dark).solidBackground, "opaque panel follows the shared solid background");
            const Lab source = Oklab(raw), shade = Oklab(t.accent);
            const double target = dark ? std::max(source.L, 0.80) : std::min(source.L, 0.45);
            check(std::abs(shade.L - target) < 0.003, "accent lightness stays at its moderate appearance target");
            const double sourceChroma = std::hypot(source.a, source.b), shadeChroma = std::hypot(shade.a, shade.b);
            if (sourceChroma > 1e-5) {
                check(shadeChroma > 1e-5 && (source.a * shade.a + source.b * shade.b) / (sourceChroma * shadeChroma) > 0.999390827,
                      "quantized accent hue stays within two degrees of the system accent");
                check(std::min({t.accent.r(), t.accent.g(), t.accent.b()}) <= 1.f / 255 ||
                      std::max({t.accent.r(), t.accent.g(), t.accent.b()}) >= 254.f / 255, "chromatic accent reaches the sRGB gamut boundary");
            } else check(t.accent.r() == t.accent.g() && t.accent.g() == t.accent.b(), "neutral accent stays neutral");
            if (dark) check(t.ctlSecondary == pui::Theme::Make(false).ctlSecondary, "dark secondary fill follows the shared theme");
        }
        return failures ? 1 : 0;
    }
}
