#pragma once
// The pui layer of the main view: a flipped, layer-backed, transparent NSView above the Metal video view that paints the
// shell tree (title bar, playlist panel, player bar, popups, tooltip) through CGRenderer and routes NSEvents to pui as
// PuiHostView does, plus the player-view rules of the Windows app: the empty title strip and the video are
// window-drag areas, a double click on the video toggles play / pause, right click opens the main menu, the wheel over
// the video changes the volume, keys go to MainView::Key.
#import <AppKit/AppKit.h>

namespace mainview { class MainView; }
namespace puimac { class CGRenderer; }

@interface ShellView : NSView <NSTextInputClient>
- (instancetype)initWithFrame:(NSRect)frame mainView:(mainview::MainView*)mv renderer:(puimac::CGRenderer*)renderer;
/// Re-makes the Mac theme for the current appearance / accent / backing scale and repaints.
- (void)applyTheme;
/// Repaints when the pui host is dirty and keeps the 60 Hz tick alive while widgets animate.
- (void)afterEvent;
/// Recomputes the shell layout from the view size and backing scale.
- (void)relayout;
@end
