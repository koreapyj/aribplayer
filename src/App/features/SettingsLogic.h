#pragma once
// Pure settings logic (no windows, no AppContext): settings <-> page-state mapping, theme mapping, accent palette,
// window placement validation and the Settings page row/page layout.
// Covered by LOGIC_TESTs in SettingsLogic.cpp.
#include <array>
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

struct HwDecoderChoice { const wchar_t* value; const wchar_t* label; };   // value = AppSettings::hwDecoder / hwaccel option
inline constexpr int kHwDecoderCount = 7;   // Auto, Prefer GPU, D3D11VA, D3D12VA, CUDA, DXVA2, Off
const std::array<HwDecoderChoice, kHwDecoderCount>& HwDecoderChoices();
/// Index of `value` in HwDecoderChoices(); unknown values -> 0 (Auto).
int HwDecoderIndex(const std::wstring& value);
std::wstring HwDecoderValue(int index);   // out of range -> "auto"

struct VideoOutputChoice { const wchar_t* value; const wchar_t* label; };   // value = AppSettings::presenter
inline constexpr int kVideoOutputCount = 3;   // Auto, Direct3D 11, SDL (fallback)
const std::array<VideoOutputChoice, kVideoOutputCount>& VideoOutputChoices();
/// Index of `value` in VideoOutputChoices(); unknown values -> 0 (Auto).
int VideoOutputIndex(const std::wstring& value);
std::wstring VideoOutputValue(int index);   // out of range -> "auto"

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
    int hwDecoderIndex = 0;              // RadioGroup index into HwDecoderChoices(): Auto, Prefer GPU, D3D11VA, D3D12VA, CUDA, DXVA2, Off
    int videoOutputIndex = 0;            // RadioGroup index into VideoOutputChoices(): Auto, Direct3D 11, SDL
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
