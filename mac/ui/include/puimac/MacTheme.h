#pragma once
// pui::Theme filled with macOS design-language values (system label colours, control accent, HIG type sizes, 6 pt control
// radius). MakeMacTheme is the Mac counterpart of Theme::Make.
#include <functional>

#include "pui/Theme.h"

namespace puimac {

/// `accent` = 0 derives a hue-preserving light/dark shade of NSColor.controlAccentColor for the appearance.
/// Nonzero accents are already resolved overrides, as in Theme::Make; `scale` is the backing scale (hairline = 1 / scale).
pui::Theme MakeMacTheme(bool dark, pui::Color accent = 0, float scale = 2);

/// NSApp.effectiveAppearance is dark (falls back to NSAppearance.currentDrawingAppearance / AppleInterfaceStyle).
bool SystemIsDark();
/// NSColor.controlAccentColor resolved for the light or dark appearance, as straight ARGB.
pui::Color SystemAccentColor(bool dark);
pui::Theme MakeMacThemeForSystem(float scale);

/// Calls `onChange` (main thread) when the effective appearance (light/dark) or the accent colour changes.
class ThemeObserver {
public:
    explicit ThemeObserver(std::function<void()> onChange);
    ~ThemeObserver();
    ThemeObserver(const ThemeObserver&) = delete;
    ThemeObserver& operator=(const ThemeObserver&) = delete;
private:
    void* impl_;
};

}
