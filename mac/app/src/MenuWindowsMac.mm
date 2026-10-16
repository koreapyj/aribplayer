#import "MenuWindowsMac.h"

#import <AppKit/AppKit.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

#include "pui/Widget.h"
#include "puimac/CGRenderer.h"
#include "puimac/KeyMap.h"
#include "puimac/MenuPopup.h"

@class MenuPanel;

/// The owner view's repaint entry point (ShellView, PuiHostView).
@protocol MenuOwnerView
- (void)afterEvent;
@end

namespace {

struct Attachment {
    pui::UiHost* host = nullptr;
    __weak NSView* view = nil;
    int listener = 0;
    std::function<void()> ownerInvalidate;   // the host's onInvalidate before Attach (repaints the owner view)
    NSMutableArray<MenuPanel*>* panels = [NSMutableArray array];   // bottom -> top, as the host's popups
    NSMutableArray* observers = [NSMutableArray array];
    id localMonitor = nil, globalMonitor = nil;
    puimac::CGRenderer renderer;
};
std::unique_ptr<Attachment> g;

NSRect ScreenRectOf(NSView* view, const pui::RectF& r) {
    const NSRect inWindow = [view convertRect:NSMakeRect(r.l, r.t, r.w(), r.h()) toView:nil];
    return [view.window convertRectToScreen:inWindow];
}

void CloseMenus();
void Refresh();

}

/// A menu's window: never key or main (the owner window keeps the keyboard and stays active-looking).
@interface MenuPanel : NSPanel
@property(nonatomic) puimac::MenuPopup* popup;  // owned by the host; cleared when the host closes it
@property(nonatomic) int paints;
@end

@implementation MenuPanel
- (BOOL)canBecomeKeyWindow { return NO; }
- (BOOL)canBecomeMainWindow { return NO; }
@end

/// Paints the panel's menu (translated to the window's origin) and forwards the pointer to the host in the owner view's
/// coordinates.
@interface MenuPanelView : NSView
@end

@implementation MenuPanelView {
    NSTrackingArea* _tracking;
}

- (BOOL)isFlipped { return YES; }
- (BOOL)isOpaque { return NO; }
- (BOOL)acceptsFirstMouse:(NSEvent*)e { (void)e; return YES; }
- (BOOL)acceptsFirstResponder { return NO; }
- (BOOL)mouseDownCanMoveWindow { return NO; }

- (puimac::MenuPopup*)popup { return [(MenuPanel*)self.window popup]; }

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_tracking) [self removeTrackingArea:_tracking];
    // The panel is never key: track whenever the app is active, and while it is not (a menu opened by a right click
    // on an inactive window).
    _tracking = [[NSTrackingArea alloc] initWithRect:self.bounds
                                             options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways | NSTrackingInVisibleRect
                                               owner:self userInfo:nil];
    [self addTrackingArea:_tracking];
}

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    puimac::MenuPopup* m = [self popup];
    CGContextRef ctx = NSGraphicsContext.currentContext.CGContext;
    if (!m || !ctx || !g) return;
    const pui::RectF b = m->Bounds();
    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx, -b.l, -b.t);
    g->renderer.Begin(ctx, (float)self.window.backingScaleFactor);
    m->PaintInWindow(g->renderer, g->host->GetTheme());
    g->renderer.End();
    CGContextRestoreGState(ctx);
    MenuPanel* panel = (MenuPanel*)self.window;
    if (panel.paints++ == 0) dispatch_async(dispatch_get_main_queue(), ^{ [panel invalidateShadow]; });   // the shadow follows the painted shape
}

- (pui::PointerEvent)pointerEvent:(NSEvent*)e button:(pui::PointerButton)b {
    pui::PointerEvent pe;
    NSView* owner = g ? g->view : nil;
    if (owner.window) {
        const NSPoint screen = [self.window convertPointToScreen:e.locationInWindow];
        const NSPoint p = [owner convertPoint:[owner.window convertPointFromScreen:screen] fromView:nil];
        pe.pt = {(float)p.x, (float)p.y};
    }
    pe.button = b;
    // clickCount is only valid for mouse-button events; AppKit throws for mouseMoved / mouseEntered.
    if (b != pui::PointerButton::None) pe.clickCount = (int)std::max<NSInteger>(1, e.clickCount);
    pe.mods = puimac::ModsFromFlags(e.modifierFlags);
    return pe;
}

- (bool)live { return g && g->view.window && [self popup]; }

- (void)mouseDown:(NSEvent*)e {
    if (![self live]) return;
    const bool ctrlClick = (e.modifierFlags & NSEventModifierFlagControl) != 0;
    g->host->PointerDown([self pointerEvent:e button:ctrlClick ? pui::PointerButton::Right : pui::PointerButton::Left]);
    Refresh();
}

- (void)mouseDragged:(NSEvent*)e {
    if (![self live]) return;
    g->host->PointerMove([self pointerEvent:e button:pui::PointerButton::Left]);
    Refresh();
}

- (void)mouseUp:(NSEvent*)e {
    if (!g) return;
    const bool ctrlClick = (e.modifierFlags & NSEventModifierFlagControl) != 0;
    g->host->PointerUp([self pointerEvent:e button:ctrlClick ? pui::PointerButton::Right : pui::PointerButton::Left]);
    Refresh();
}

- (void)rightMouseDown:(NSEvent*)e {
    if (![self live]) return;
    g->host->PointerDown([self pointerEvent:e button:pui::PointerButton::Right]);
    Refresh();
}

- (void)rightMouseDragged:(NSEvent*)e {
    if (![self live]) return;
    g->host->PointerMove([self pointerEvent:e button:pui::PointerButton::Right]);
    Refresh();
}

- (void)rightMouseUp:(NSEvent*)e {
    if (!g) return;
    g->host->PointerUp([self pointerEvent:e button:pui::PointerButton::Right]);
    Refresh();
}

- (void)mouseMoved:(NSEvent*)e {
    if (![self live]) return;
    g->host->PointerMove([self pointerEvent:e button:pui::PointerButton::None]);
    Refresh();
}

- (void)mouseEntered:(NSEvent*)e { [self mouseMoved:e]; }

- (void)mouseExited:(NSEvent*)e {
    (void)e;
    if (![self live]) return;
    // Into another menu of the chain or back into the owner view: those report the pointer themselves.
    const NSPoint at = NSEvent.mouseLocation;
    for (MenuPanel* p in g->panels)
        if (p != self.window && p.visible && NSPointInRect(at, p.frame)) return;
    NSView* owner = g->view;
    if (NSPointInRect(at, ScreenRectOf(owner, {0, 0, (float)owner.bounds.size.width, (float)owner.bounds.size.height}))) return;
    g->host->PointerLeave();
    Refresh();
}

- (void)scrollWheel:(NSEvent*)e { (void)e; }   // menus do not scroll; the wheel must not reach the video (volume)

@end

namespace {

MenuPanel* PanelOf(const pui::Widget* menu) {
    if (!g || !menu) return nil;
    for (MenuPanel* p in g->panels)
        if (p.popup == menu) return p;
    return nil;
}

/// Repaints the menu windows, and the owner view through -afterEvent (it repaints a dirty host and runs the tick that
/// opens submenus after the hover delay).
void Refresh() {
    if (!g) return;
    for (MenuPanel* p in g->panels) [p.contentView setNeedsDisplay:YES];
    NSView* owner = g->view;
    if ([owner respondsToSelector:@selector(afterEvent)]) [(id<MenuOwnerView>)owner afterEvent];
}

/// Closes the bottom-most menu of the host and so every menu above it (other popups below it stay, e.g. the full-screen
/// playlist overlay a context menu was opened from).
void CloseMenus() {
    if (!g || g->panels.count == 0) return;
    for (pui::Widget* w : g->host->Popups())
        if (dynamic_cast<puimac::MenuPopup*>(w)) { g->host->ClosePopup(w); break; }
    Refresh();
}

/// The menu windows follow the owner view (host coordinates are the view's).
void SyncFrames() {
    if (!g || !g->view.window) return;
    for (MenuPanel* p in g->panels)
        if (p.popup) [p setFrame:ScreenRectOf(g->view, p.popup->Bounds()) display:NO];
}

void OpenWindow(puimac::MenuPopup* m) {
    NSView* owner = g->view;
    NSWindow* ownerWindow = owner.window;
    if (!ownerWindow || PanelOf(m)) return;
    m->SetOwnWindow(true);
    MenuPanel* p = [[MenuPanel alloc] initWithContentRect:ScreenRectOf(owner, m->Bounds())
                                                styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                                                  backing:NSBackingStoreBuffered defer:NO];
    p.popup = m;
    p.opaque = NO;
    p.backgroundColor = NSColor.clearColor;
    p.hasShadow = YES;
    p.hidesOnDeactivate = NO;   // deactivation closes the menus (CloseMenus), not just hides them
    p.releasedWhenClosed = NO;
    p.animationBehavior = NSWindowAnimationBehaviorNone;
    p.collectionBehavior = NSWindowCollectionBehaviorTransient | NSWindowCollectionBehaviorIgnoresCycle | NSWindowCollectionBehaviorFullScreenAuxiliary;
    p.appearance = ownerWindow.effectiveAppearance;
    p.acceptsMouseMovedEvents = YES;
    MenuPanelView* v = [[MenuPanelView alloc] initWithFrame:NSMakeRect(0, 0, p.frame.size.width, p.frame.size.height)];
    v.wantsLayer = YES;
    v.layerContentsRedrawPolicy = NSViewLayerContentsRedrawOnSetNeedsDisplay;
    p.contentView = v;
    [g->panels addObject:p];
    [ownerWindow addChildWindow:p ordered:NSWindowAbove];   // moves with the owner, joins its space
    p.level = std::max<NSWindowLevel>(NSPopUpMenuWindowLevel, ownerWindow.level + 1);   // above a floating mini player
    [p orderFront:nil];
}

void CloseWindow(const pui::Widget* w) {
    MenuPanel* p = PanelOf(w);
    if (!p) return;
    p.popup = nullptr;
    [p.parentWindow removeChildWindow:p];
    [p orderOut:nil];
    [g->panels removeObject:p];
}

/// The visible frame of the screen at a host point, in host coordinates (the menus' area).
pui::RectF ScreenAreaAt(pui::PointF near) {
    NSView* owner = g ? g->view : nil;
    NSWindow* w = owner.window;
    if (!w) return {0, 0, 0, 0};
    const NSPoint at = [w convertPointToScreen:[owner convertPoint:NSMakePoint(near.x, near.y) toView:nil]];
    NSScreen* screen = w.screen ?: NSScreen.mainScreen;
    for (NSScreen* s in NSScreen.screens)
        if (NSPointInRect(at, s.frame)) { screen = s; break; }
    const NSRect r = [owner convertRect:[w convertRectFromScreen:screen.visibleFrame] fromView:nil];   // flipped: top-left origin
    return {(float)NSMinX(r), (float)NSMinY(r), (float)NSMaxX(r), (float)NSMaxY(r)};
}

void Observe(NSNotificationName name, id object, void (^block)(void)) {
    [g->observers addObject:[NSNotificationCenter.defaultCenter addObserverForName:name object:object queue:nil
                                                                          usingBlock:^(NSNotification* n) { (void)n; block(); }]];
}

}

namespace menuwindows {

void Attach(pui::UiHost& host, void* ownerView) {
    Detach();
    NSView* view = (__bridge NSView*)ownerView;
    g = std::make_unique<Attachment>();
    g->host = &host;
    g->view = view;
    g->ownerInvalidate = host.onInvalidate;
    host.onInvalidate = [] {
        if (!g) return;
        if (g->ownerInvalidate) g->ownerInvalidate();
        for (MenuPanel* p in g->panels) [p.contentView setNeedsDisplay:YES];
    };
    g->listener = host.AddListener([](pui::Widget* w, pui::UiEvent ev) {
        if (!g) return;
        if (ev == pui::UiEvent::PopupOpened) {
            if (auto* m = dynamic_cast<puimac::MenuPopup*>(w)) OpenWindow(m);
        } else if (ev == pui::UiEvent::PopupClosed) {
            CloseWindow(w);
        }
    });
    puimac::SetMenuArea(&host, [](pui::PointF near) { return ScreenAreaAt(near); });

    // Light dismiss beyond the host's own and PopupDismissMac's (the owner losing key, miniaturised, closed): what
    // closes a native menu.
    NSWindow* ownerWindow = view.window;
    Observe(NSApplicationDidResignActiveNotification, nil, ^{ CloseMenus(); });   // also when the owner was not key
    for (NSNotificationName n : {NSWindowWillMoveNotification, NSWindowWillStartLiveResizeNotification, NSWindowWillEnterFullScreenNotification,
                                 NSWindowWillExitFullScreenNotification, NSWindowDidChangeScreenNotification})
        Observe(n, ownerWindow, ^{ CloseMenus(); });
    for (NSNotificationName n : {NSWindowDidMoveNotification, NSWindowDidResizeNotification})
        Observe(n, ownerWindow, ^{ SyncFrames(); });
    const NSEventMask presses = NSEventMaskLeftMouseDown | NSEventMaskRightMouseDown | NSEventMaskOtherMouseDown;
    g->localMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:presses handler:^NSEvent*(NSEvent* e) {
        if (!g || g->panels.count == 0) return e;
        NSView* owner = g->view;
        if ([e.window isKindOfClass:[MenuPanel class]]) return e;
        if (e.window && e.window == owner.window) {
            // In the owner view the host decides (a press outside the menus closes them there); on the window's
            // frame (traffic lights, the title bar outside the view) it never sees the press.
            NSView* hit = [e.window.contentView.superview hitTest:e.locationInWindow];
            if (hit && (hit == owner || [hit isDescendantOf:owner])) return e;
        }
        CloseMenus();
        return e;
    }];
    g->globalMonitor = [NSEvent addGlobalMonitorForEventsMatchingMask:presses handler:^(NSEvent* e) { (void)e; CloseMenus(); }];
}

void Detach() {
    if (!g) return;
    for (MenuPanel* p in [g->panels copy]) {
        if (p.popup) p.popup->SetOwnWindow(false);
        CloseWindow(p.popup);
    }
    for (id o in g->observers) [NSNotificationCenter.defaultCenter removeObserver:o];
    if (g->localMonitor) [NSEvent removeMonitor:g->localMonitor];
    if (g->globalMonitor) [NSEvent removeMonitor:g->globalMonitor];
    g->host->RemoveListener(g->listener);
    g->host->onInvalidate = g->ownerInvalidate;
    puimac::SetMenuArea(g->host, nullptr);
    g.reset();
}

void* WindowOf(const pui::Widget* menu) { return (__bridge void*)PanelOf(menu); }

int OpenWindows() {
    int n = 0;
    if (g)
        for (MenuPanel* p in g->panels) n += p.visible ? 1 : 0;
    return n;
}

int PaintCount(const pui::Widget* menu) {
    MenuPanel* p = PanelOf(menu);
    return p ? p.paints : 0;
}

}
