#pragma once
// Pure settings logic (no windows, no AppContext): settings <-> page-state mapping, theme mapping, accent palette,
// window placement validation and the Settings page row/page layout.
// Covered by LOGIC_TESTs in SettingsLogic.cpp.
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "pui/Geometry.h"
#include "pui/Theme.h"
#include "wvp/AppSettings.h"

namespace app::sl {

pui::ThemeMode ToThemeMode(wvp::ThemeSetting s);
wvp::ThemeSetting ToThemeSetting(pui::ThemeMode m);
/// RadioGroup index: 0 System, 1 Light, 2 Dark (out of range -> System).
int ThemeIndex(wvp::ThemeSetting s);
wvp::ThemeSetting ThemeFromIndex(int i);

struct AccentChoice { const wchar_t* name; std::uint32_t argb; };
inline constexpr int kAccentCount = 12;
const std::array<AccentChoice, kAccentCount>& AccentPalette();
/// Index of the palette entry equal to argb (alpha ignored), -1 when custom or 0 (system).
int AccentIndexFor(std::uint32_t argb);

inline constexpr int kAlwaysOnTopCount = 4;   // Disable, Enable, WhenPlaying, WhenFullscreen
int AlwaysOnTopIndex(wvp::AlwaysOnTop m);
wvp::AlwaysOnTop AlwaysOnTopFromIndex(int index);   // out of range -> Disable
/// Whether the main window should currently be topmost for `mode`.
bool ShouldBeTopmost(wvp::AlwaysOnTop mode, bool playing, bool fullscreen);
wvp::AlwaysOnTop NextAlwaysOnTop(wvp::AlwaysOnTop mode);

enum class TimeDisplayMode { Elapsed, Remaining };
TimeDisplayMode ToggleTimeDisplay(TimeDisplayMode m);
/// "01:23" / "1:02:03" for Elapsed, "-00:40" for Remaining (duration <= 0 or unknown -> "-00:00").
std::wstring FormatTimeDisplay(TimeDisplayMode m, double position, double duration);

struct PageState {
    int themeIndex = 0;
    bool useSystemAccent = true;
    int accentIndex = -1;                // palette index shown selected when !useSystemAccent, -1 = custom colour
    int alwaysOnTopIndex = 0;            // 0 Disable, 1 Enable, 2 WhenPlaying, 3 WhenFullscreen
    bool debugLogging = false;
    bool operator==(const PageState&) const = default;
};
PageState StateFromSettings(const wvp::AppSettings& s);
/// Writes the page-owned fields back. A custom accent (accentIndex -1, !useSystemAccent) keeps the existing colour.
void ApplyState(wvp::AppSettings& s, const PageState& st);

struct MonitorRect { int l = 0, t = 0, r = 0, b = 0; };
inline constexpr int kMinWindowW = 400, kMinWindowH = 300;     // device pixels
/// The saved placement when it is valid and its caption area is reachable on one of the monitors; nullopt = use the
/// default. Sizes are clamped to at least the minimum and at most the widest/tallest monitor.
std::optional<wvp::WindowPlacement> ValidatePlacement(const wvp::WindowPlacement& p, const std::vector<MonitorRect>& monitors);

/// Size (device px) saved at `p.dpi` converted to `targetDpi` (the monitor the window will be shown on). x/y are screen
/// coordinates and stay; the result's dpi is `targetDpi`. No-op for equal or invalid DPIs.
wvp::WindowPlacement RescalePlacement(const wvp::WindowPlacement& p, int targetDpi);

/// Settings > Subtitles outline widths (the page offers these values; a stored value outside them shows the nearest one).
inline constexpr std::array<double, 7> kSubtitleOutlinePresets = {0.0, 1.0, 1.5, 2.0, 3.0, 4.0, 6.0};
/// Index of the preset whose value is closest to `value`.
template <size_t N> int NearestPreset(const std::array<double, N>& presets, double value) {
    int best = 0;
    for (size_t i = 1; i < N; i++)
        if (std::abs(presets[i] - value) < std::abs(presets[(size_t)best] - value)) best = (int)i;
    return best;
}
std::wstring PixelLabel(double px);        // 0 -> "None", 1.5 -> "1.5 px"
/// "Malgun Gothic, Bold Italic, 45 pt" (weight names as the Windows font dialog shows them).
std::wstring SubtitleFontSummary(const std::string& family, int weight, bool italic, double points);
std::wstring RgbaHex(std::uint32_t rgba);  // 0x11AAFFFF -> "#11AAFF"

inline constexpr float kPageMarginH = 36, kHeaderTop = 20, kHeaderH = 44, kMaxContentW = 1000;
struct PageLayout {
    pui::RectF header;
    pui::RectF scroll;      // ScrollViewer
    float contentPadH = 0;  // horizontal padding inside the scrolled column so it is at most kMaxContentW wide
};
PageLayout ComputePageLayout(float pageW, float pageH);

struct RowInput {
    float width = 0;
    float controlW = 0, controlH = 0;
    float titleH = 20, descH = 0;        // descH 0 = no description
    bool hasGlyph = true;
    bool stacked = false;                // control below the header, aligned with the text column
};
struct RowLayoutResult {
    float height = 0;
    pui::RectF glyph, title, desc, control;   // relative to the row origin
    float textLeft = 0, textRight = 0;
};
inline constexpr float kRowPad = 16, kGlyphCol = 36, kRowMinH = 56, kRowMinHDesc = 64;
RowLayoutResult ComputeRowLayout(const RowInput& in);

/// Swatch grid: cell origins (relative to the grid) of `count` swatches of `size` with `gap`, wrapping at `maxWidth`.
struct SwatchCell { float x = 0, y = 0; };
std::vector<SwatchCell> ComputeSwatchGrid(int count, float size, float gap, float maxWidth);
pui::SizeF SwatchGridSize(int count, float size, float gap, float maxWidth);

}
