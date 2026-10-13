#include "CaptionStyleMac.h"

#import <AppKit/AppKit.h>
#import <MediaAccessibility/MediaAccessibility.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mainview {

namespace {
std::optional<CaptionAppearance> g_override;
std::function<void()> g_changed;
bool g_watching = false;
std::uint64_t g_generation = 0;
int g_observerToken = 0;   // its address identifies the observer

std::uint32_t Alpha(double opacity) {
    return (std::uint32_t)std::lround(std::clamp(opacity, 0.0, 1.0) * 255.0);
}

std::uint32_t CaptionRgb(CGColorRef color, std::uint32_t fallback) {
    if (!color) return fallback;
    NSColor* rgb = [[NSColor colorWithCGColor:color] colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    CFRelease(color);
    if (!rgb) return fallback;
    return (Alpha(rgb.redComponent) << 24) | (Alpha(rgb.greenComponent) << 16) | (Alpha(rgb.blueComponent) << 8);
}

void Changed(CFNotificationCenterRef, void*, CFNotificationName, const void*, CFDictionaryRef) {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!g_watching) return;
        const std::uint64_t gen = ++g_generation;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.25 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            if (g_watching && gen == g_generation && g_changed) g_changed();   // quiet for 250 ms: apply
        });
    });
}

CFNotificationCenterRef Centers(int i) {
    switch (i) {
    case 0: return CFNotificationCenterGetLocalCenter();
    case 1: return CFNotificationCenterGetDistributedCenter();
    default: return CFNotificationCenterGetDarwinNotifyCenter();
    }
}
}

CaptionAppearance ReadCaptionAppearance() {
    if (g_override) return *g_override;
    CaptionAppearance a;
    MACaptionAppearanceBehavior b = kMACaptionAppearanceBehaviorUseValue;
    a.backgroundOpacity = MACaptionAppearanceGetBackgroundOpacity(kMACaptionAppearanceDomainUser, &b);
    a.backgroundUseValue = b == kMACaptionAppearanceBehaviorUseValue;
    b = kMACaptionAppearanceBehaviorUseValue;
    a.windowOpacity = MACaptionAppearanceGetWindowOpacity(kMACaptionAppearanceDomainUser, &b);
    a.windowUseValue = b == kMACaptionAppearanceBehaviorUseValue;
    b = kMACaptionAppearanceBehaviorUseValue;
    a.edgeStyle = (int)MACaptionAppearanceGetTextEdgeStyle(kMACaptionAppearanceDomainUser, &b);
    a.edgeUseValue = b == kMACaptionAppearanceBehaviorUseValue;
    a.displayType = (int)MACaptionAppearanceGetDisplayType(kMACaptionAppearanceDomainUser);
    CTFontDescriptorRef font = MACaptionAppearanceCopyFontDescriptorForStyle(kMACaptionAppearanceDomainUser, nullptr,
                                                                            kMACaptionAppearanceFontStyleDefault);
    if (font) {
        NSString* family = CFBridgingRelease(CTFontDescriptorCopyAttribute(font, kCTFontFamilyNameAttribute));
        // libass resolves public family names; private system-UI families use the platform's subtitle default.
        if (family.length && ![family hasPrefix:@"."]) a.font = family.UTF8String;
        CFRelease(font);
    }
    const double scale = MACaptionAppearanceGetRelativeCharacterSize(kMACaptionAppearanceDomainUser, nullptr);
    if (std::isfinite(scale) && scale > 0) a.fontScale = scale;
    a.textRgba = CaptionRgb(MACaptionAppearanceCopyForegroundColor(kMACaptionAppearanceDomainUser, nullptr), 0xFFFFFF00u) |
                 Alpha(MACaptionAppearanceGetForegroundOpacity(kMACaptionAppearanceDomainUser, nullptr));
    a.backgroundRgb = CaptionRgb(MACaptionAppearanceCopyBackgroundColor(kMACaptionAppearanceDomainUser, nullptr), 0);
    a.windowRgb = CaptionRgb(MACaptionAppearanceCopyWindowColor(kMACaptionAppearanceDomainUser, nullptr), 0);
    return a;
}

CaptionStyleFlags MapCaptionAppearance(const CaptionAppearance& a) {
    constexpr double kTransparent = 0.005;
    CaptionStyleFlags f;
    f.ignoreBackground = a.backgroundOpacity < kTransparent && a.windowOpacity < kTransparent;
    f.forceOutline = a.edgeStyle == kMACaptionAppearanceTextEdgeStyleUniform || a.edgeStyle == kMACaptionAppearanceTextEdgeStyleDropShadow;
    return f;
}

wvp::SubtitleRenderStyle ResolveMacSubtitleStyle(const wvp::SubtitleStyle& settings, const CaptionAppearance& a) {
    if (!settings.useSystemCaptionStyle) return wvp::ResolveSubtitleStyle(settings, {});
    wvp::SubtitleRenderStyle r;
    r.applyToAss = settings.applyToAss;
    r.font = a.font;
    r.fontScale = a.fontScale;
    r.primaryRgba = a.textRgba;
    // libass has one event box; use the text background, or the caption window when the text background is transparent.
    r.backRgba = a.backgroundOpacity > 0 ? a.backgroundRgb | Alpha(a.backgroundOpacity) : a.windowRgb | Alpha(a.windowOpacity);
    unsigned defined = wvp::kSubFont | wvp::kSubSize | wvp::kSubTextColor | wvp::kSubBackground;
    if (a.edgeStyle == kMACaptionAppearanceTextEdgeStyleNone || a.edgeStyle == kMACaptionAppearanceTextEdgeStyleUniform ||
        a.edgeStyle == kMACaptionAppearanceTextEdgeStyleDropShadow) {
        r.outlineWidth = a.edgeStyle == kMACaptionAppearanceTextEdgeStyleUniform ? wvp::kSystemCaptionOutlinePx : 0;
        r.shadow = a.edgeStyle == kMACaptionAppearanceTextEdgeStyleDropShadow ? wvp::kSystemCaptionShadowPx : 0;
        defined |= wvp::kSubBorder;
    }
    const unsigned switched = (settings.overrideFont ? wvp::kSubFont | wvp::kSubFontStyle : 0u) |
                              (settings.overrideSize ? wvp::kSubSize : 0u) |
                              (settings.overrideColors ? wvp::kSubTextColor | wvp::kSubOutlineColor | wvp::kSubBackground : 0u) |
                              (settings.overrideBorder ? wvp::kSubBorder : 0u);
    r.fallbackFlags = defined;
    r.overrideFlags = defined & switched;
    return r;
}

std::string DescribeCaptionAppearance(const CaptionAppearance& a) {
    static const char* edges[] = {"undefined", "none", "raised", "depressed", "uniform", "drop shadow"};
    char b[200];
    std::snprintf(b, sizeof b, "background %.2f%s, window %.2f%s, edge %s%s, display type %d", a.backgroundOpacity, a.backgroundUseValue ? "" : " (video may override)",
                  a.windowOpacity, a.windowUseValue ? "" : " (video may override)", a.edgeStyle >= 0 && a.edgeStyle <= 5 ? edges[a.edgeStyle] : "?",
                  a.edgeUseValue ? "" : " (video may override)", a.displayType);
    return b;
}

void SetCaptionAppearanceOverride(std::optional<CaptionAppearance> a) { g_override = std::move(a); }

void PostCaptionAppearanceChanged() {
    CFNotificationCenterPostNotification(CFNotificationCenterGetLocalCenter(), kMACaptionAppearanceSettingsChangedNotification, nullptr, nullptr, true);
}

void StartCaptionAppearanceWatch(std::function<void()> changed) {
    g_changed = std::move(changed);
    if (g_watching) return;
    g_watching = true;
    for (int i = 0; i < 3; i++)
        CFNotificationCenterAddObserver(Centers(i), &g_observerToken, Changed, kMACaptionAppearanceSettingsChangedNotification, nullptr,
                                        CFNotificationSuspensionBehaviorDeliverImmediately);
}

void StopCaptionAppearanceWatch() {
    if (!g_watching) return;
    g_watching = false;
    for (int i = 0; i < 3; i++) CFNotificationCenterRemoveEveryObserver(Centers(i), &g_observerToken);
    g_changed = nullptr;
}

CaptionRefreshRecord& LastCaptionRefresh() {
    static CaptionRefreshRecord r;
    return r;
}

}
