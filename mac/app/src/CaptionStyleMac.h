#pragma once
// The macOS system caption style (System Settings > Accessibility > Captions, MediaAccessibility's MACaptionAppearance)
// as the two libaribcaption overrides the Windows app derives from Windows' Closed Caption settings
// (features/SettingsService.cpp AribCaptionStyle). It is also the text-subtitle style source (ResolveMacSubtitleStyle).
//
//   Windows                                                  macOS (MapCaptionAppearance)
//   ignoreBackground = text BackgroundOpacity == 0 %          the style draws no box behind the text: background
//     (ARIB draws one box per character cell = the text        opacity 0 AND window (region) opacity 0. macOS draws its
//     background; the caption window has no ARIB              default box with the WINDOW (opacity 0.5, text background
//     counterpart; "Default" = the content decides)            0), so the default style keeps ARIB's boxes, as Windows'
//                                                              Default does
//   forceOutline = FontEffect Uniform or DropShadow          edge style Uniform or Drop Shadow
//
// The "Allow video to override" behaviour (kMACaptionAppearanceBehaviorUseContentIfAvailable) is not consulted: every
// built-in profile sets it on the edge style (Outline Text = Uniform, overridable), so honouring it would make the
// profile the user picked do nothing. Windows has no such flag; its Default edge is "None" here too (the Default
// profile's edge is None). Changes arrive as kMACaptionAppearanceSettingsChangedNotification (observed on the local,
// distributed and Darwin notification centers; debounced 250 ms like the Windows registry watch).
#include <functional>
#include <optional>
#include <string>

#include "wvp/SubtitleStyle.h"

namespace mainview {

struct CaptionAppearance {
    double backgroundOpacity = 0, windowOpacity = 0.5;
    bool backgroundUseValue = true, windowUseValue = true;
    int edgeStyle = 1;              // MACaptionAppearanceTextEdgeStyle (1 None, 2 Raised, 3 Depressed, 4 Uniform, 5 Drop Shadow)
    bool edgeUseValue = false;
    int displayType = 0;            // MACaptionAppearanceDisplayType (0 forced only, 1 automatic, 2 always on): logged only
    std::string font = WVP_DEFAULT_SUBTITLE_FONT;
    double fontScale = 1.0;
    std::uint32_t textRgba = 0xFFFFFFFFu, backgroundRgb = 0, windowRgb = 0;   // RGB fields have zero alpha
    bool operator==(const CaptionAppearance&) const = default;
};

struct CaptionStyleFlags {
    bool ignoreBackground = false;
    bool forceOutline = false;
    bool operator==(const CaptionStyleFlags&) const = default;
};

/// The user's caption appearance (kMACaptionAppearanceDomainUser), or the test override when one is set.
CaptionAppearance ReadCaptionAppearance();
/// Pure mapping (see the header comment).
CaptionStyleFlags MapCaptionAppearance(const CaptionAppearance& a);
/// Text subtitle style from the selected source, with per-property fallback and override flags.
wvp::SubtitleRenderStyle ResolveMacSubtitleStyle(const wvp::SubtitleStyle& settings, const CaptionAppearance& appearance);
std::string DescribeCaptionAppearance(const CaptionAppearance& a);
/// Self-test: replaces what MediaAccessibility reports (nullopt = the real settings again).
void SetCaptionAppearanceOverride(std::optional<CaptionAppearance> a);
/// Posts kMACaptionAppearanceSettingsChangedNotification on the local center (what MediaAccessibility does in-process).
void PostCaptionAppearanceChanged();

/// Calls `changed` on the main queue 250 ms after the last settings-changed notification. Idempotent.
void StartCaptionAppearanceWatch(std::function<void()> changed);
void StopCaptionAppearanceWatch();

/// What the last caption-style refresh did (OnSystemCaptionSettingsChanged -> FfPlayer::RefreshCaptionStyle); tests.
struct CaptionRefreshRecord {
    int count = 0;
    CaptionStyleFlags flags;
    int rc = 0;
};
CaptionRefreshRecord& LastCaptionRefresh();

/// System Settings > Accessibility > Captions.
constexpr const char* kCaptionSettingsUrl = "x-apple.systempreferences:com.apple.preference.universalaccess?Captioning";

}
