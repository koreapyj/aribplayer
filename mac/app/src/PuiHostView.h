#pragma once
// A layer-backed, flipped NSView hosting one pui::UiHost: paints through CGRenderer in
// drawRect:, routes NSEvents to pui (mouse, wheel, keys mapped to VK values), implements NSTextInputClient for IME text
// entry. Used by the Settings window (pages/SettingsPage in its own window); the player window has
// ShellView (player-view rules on top of the same routing).
#import <AppKit/AppKit.h>

#include <functional>

#include "pui/Theme.h"

namespace pui { class UiHost; }
namespace puimac { class CGRenderer; }

@interface PuiHostView : NSView <NSTextInputClient>
- (instancetype)initWithFrame:(NSRect)frame host:(pui::UiHost*)host renderer:(puimac::CGRenderer*)renderer;
/// The theme for a backing scale (called on appearance / scale changes and by applyTheme).
- (void)setThemeMaker:(std::function<pui::Theme(float scale)>)maker;
/// Called with the view size (points) whenever it changes; the owner lays out its root there.
- (void)setOnResize:(std::function<void(NSSize)>)onResize;
/// Re-makes the theme for the current appearance / backing scale and repaints.
- (void)applyTheme;
/// Repaints when the host is dirty and keeps the 60 Hz tick alive while widgets animate.
- (void)afterEvent;
@end
