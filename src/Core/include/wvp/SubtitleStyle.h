#pragma once
// Text subtitle style (every libass-rendered codec: SRT, SAMI, WebVTT, ASS/SSA ...). Colours are 0xRRGGBBAA.
//
// One style source per playback: the Windows caption settings when SubtitleStyle::useSystemCaptionStyle is on, else the
// app's own values. For each property the track's own style wins where the track defines it; the source fills in where
// it does not. Plain-text codecs (everything but ASS/SSA) decode with a default style that defines nothing, so they take
// every property the source defines. A property's override switch makes the source win over the track as well (on
// ASS/SSA only with applyToAss). A source that does not define a property (a Windows "Default") leaves the track's value.
#include <cstdint>
#include <optional>
#include <string>

namespace wvp {

/// Settings > Subtitles.
struct SubtitleStyle {
    bool useSystemCaptionStyle = true;    // the Windows caption settings are the source; the values below are unused
    std::string font = "Malgun Gothic";
    int fontWeight = 400;                 // 100 .. 900
    bool italic = false;
    double fontSizePt = 45;               // points at 1080 video lines, scaled with the video (45 = the decoders' default)
    std::uint32_t primaryRgba = 0xFFFFFFFFu;
    std::uint32_t outlineRgba = 0x000000FFu;
    double outlineWidth = 2.0;            // px at 1080 video lines
    bool overrideFont = false, overrideSize = false, overrideColors = false, overrideBorder = false;
    bool applyToAss = false;              // the override switches also apply to ASS/SSA tracks
    bool operator==(const SubtitleStyle&) const = default;
};

/// Style properties; the values equal libffplay's FFP_SUBSTYLE_* flags.
enum SubtitleProperty : unsigned {
    kSubFont = 1u << 0,
    kSubSize = 1u << 1,
    kSubTextColor = 1u << 2,
    kSubBorder = 1u << 3,          // outline width and shadow
    kSubOutlineColor = 1u << 4,
    kSubFontStyle = 1u << 5,       // weight and italic
    kSubBackground = 1u << 6,      // a box behind each subtitle event
};

/// What the renderer gets (libffplay FFPSubtitleStyle).
struct SubtitleRenderStyle {
    unsigned overrideFlags = 0;   // replace the track's value on plain-text tracks, and on ASS/SSA tracks with applyToAss
    unsigned fallbackFlags = 0;   // replace it on plain-text tracks only
    bool applyToAss = false;
    std::string font = "Malgun Gothic";   // also the renderer's default family
    int fontWeight = 400;
    bool italic = false;
    double fontSizePt = 0;        // 0 = the track's size
    double fontScale = 1.0;       // times the (track's or fontSizePt) size
    std::uint32_t primaryRgba = 0xFFFFFFFFu;
    std::uint32_t outlineRgba = 0x000000FFu;
    std::uint32_t backRgba = 0x000000FFu;
    double outlineWidth = 0, shadow = 0;   // px at 1080 video lines
    bool operator==(const SubtitleRenderStyle&) const = default;
};

/// Windows.Media.ClosedCaptioning.ClosedCaptionProperties as the enum values of ClosedCaptionStyle, ClosedCaptionSize,
/// ClosedCaptionColor, ClosedCaptionOpacity and ClosedCaptionEdgeEffect; 0 is "Default" in each.
struct WindowsCaptionProperties {
    int fontStyle = 0, fontSize = 0, fontColor = 0, fontOpacity = 0, fontEffect = 0;
    int backgroundColor = 0, backgroundOpacity = 0;
    bool operator==(const WindowsCaptionProperties&) const = default;
};

enum class CaptionEdge { None, Outline, DropShadow };

/// The Windows caption settings as style values; nullopt = not defined ("Default", or no counterpart in the renderer).
struct SystemCaptionStyle {
    std::optional<std::string> font;
    std::optional<double> fontScale;
    std::optional<std::uint32_t> textRgba;
    std::optional<CaptionEdge> edge;
    std::optional<std::uint32_t> backRgba;
};

/// Edge widths used for the Windows edge effects, px at 1080 video lines.
inline constexpr double kSystemCaptionOutlinePx = 3.0, kSystemCaptionShadowPx = 3.0;

SystemCaptionStyle MapWindowsCaptionProperties(const WindowsCaptionProperties& p);
SubtitleRenderStyle ResolveSubtitleStyle(const SubtitleStyle& settings, const WindowsCaptionProperties& windows);

}
