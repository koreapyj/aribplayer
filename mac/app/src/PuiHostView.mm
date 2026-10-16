#import "PuiHostView.h"

#include <algorithm>
#include <string>

#include "pui/Widget.h"
#include "pui/TextBox.h"
#include "puimac/CGRenderer.h"
#include "puimac/KeyMap.h"
#include "puimac/MacTheme.h"

namespace {
std::wstring ToW(NSString* s) {
    if (!s) return {};
    NSData* d = [s dataUsingEncoding:NSUTF32LittleEndianStringEncoding];
    return std::wstring(static_cast<const wchar_t*>(d.bytes), d.length / sizeof(wchar_t));
}
}

@implementation PuiHostView {
    pui::UiHost* _host;               // not owned
    std::function<pui::Theme(float)> _themeMaker;
    std::function<void(NSSize)> _onResize;
    std::function<void()> _onEscape;
    puimac::CGRenderer* _renderer;    // not owned
    NSTimer* _tick;
    NSTrackingArea* _tracking;
    std::wstring _marked;             // IME composition currently shown inline in the focused TextBox
    size_t _markedStart;
}

- (instancetype)initWithFrame:(NSRect)frame host:(pui::UiHost*)host renderer:(puimac::CGRenderer*)renderer {
    if ((self = [super initWithFrame:frame])) {
        _host = host;
        _renderer = renderer;
        _markedStart = 0;
        self.wantsLayer = YES;
        self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawOnSetNeedsDisplay;
        __weak PuiHostView* weakSelf = self;
        _host->onInvalidate = [weakSelf] { [weakSelf setNeedsDisplay:YES]; };
        [self updateTracking];
    }
    return self;
}

- (void)setThemeMaker:(std::function<pui::Theme(float)>)maker { _themeMaker = std::move(maker); [self applyTheme]; }
- (void)setOnResize:(std::function<void(NSSize)>)onResize { _onResize = std::move(onResize); if (_onResize) _onResize(self.bounds.size); }
- (void)setOnEscape:(std::function<void()>)onEscape { _onEscape = std::move(onEscape); }
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)e { (void)e; return YES; }
- (BOOL)isOpaque { return NO; }
// The pui widgets decide: a non-opaque view answers YES by default, and in the transparent title-bar region AppKit (the
// window server) would then drag the window on a press there instead of delivering it (the title strip's buttons).
// The empty strip still drags through performWindowDragWithEvent: in mouseDown.
- (BOOL)mouseDownCanMoveWindow { return NO; }

- (CGFloat)scale { return self.window ? self.window.backingScaleFactor : 2.0; }

- (void)applyTheme {
    _renderer->SetScale((float)[self scale]);
    _host->SetTheme(_themeMaker ? _themeMaker((float)[self scale]) : puimac::MakeMacThemeForSystem((float)[self scale]));
    [self setNeedsDisplay:YES];
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    if (self.window) [self applyTheme];
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    [self applyTheme];
}

- (void)viewDidChangeEffectiveAppearance {
    [super viewDidChangeEffectiveAppearance];
    [self applyTheme];
}

- (void)setFrameSize:(NSSize)s {
    [super setFrameSize:s];
    if (_onResize) _onResize(s);
    [self updateTracking];
}

/// Hover (and so tooltips) whether or not this window is key or the app active, as Win32 windows get WM_MOUSEMOVE.
- (void)updateTracking {
    if (_tracking) [self removeTrackingArea:_tracking];
    _tracking = [[NSTrackingArea alloc] initWithRect:self.bounds
                                             options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways | NSTrackingInVisibleRect
                                               owner:self userInfo:nil];
    [self addTrackingArea:_tracking];
}

// --- painting --------------------------------------------------------------------------------------------------------

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    CGContextRef ctx = NSGraphicsContext.currentContext.CGContext;
    if (!ctx) return;
    _renderer->Begin(ctx, (float)[self scale]);
    _host->Paint(*_renderer);
    _renderer->End();
    _host->ClearDirty();
    [self ensureTicking];
}

- (void)ensureTicking {
    if (!_host->NeedsTick()) return;
    if (_tick) return;
    __weak PuiHostView* weakSelf = self;
    _tick = [NSTimer scheduledTimerWithTimeInterval:1.0 / 60 repeats:YES block:^(NSTimer* t) {
        PuiHostView* s = weakSelf;
        if (!s) { [t invalidate]; return; }
        bool more = s->_host->Tick();
        if (s->_host->IsDirty()) [s setNeedsDisplay:YES];
        if (!more) { [t invalidate]; s->_tick = nil; }
    }];
}

// --- pointer ----------------------------------------------------------------------------------------------------------

- (pui::PointerEvent)pointerEvent:(NSEvent*)e button:(pui::PointerButton)b {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    pui::PointerEvent pe;
    pe.pt = {(float)p.x, (float)p.y};
    pe.button = b;
    // clickCount is only valid for mouse-button events; AppKit throws for scrollWheel / mouseMoved.
    if (b != pui::PointerButton::None) pe.clickCount = (int)std::max<NSInteger>(1, e.clickCount);
    pe.mods = puimac::ModsFromFlags(e.modifierFlags);
    return pe;
}

- (void)afterEvent {
    if (_host->IsDirty()) [self setNeedsDisplay:YES];
    [self ensureTicking];
}

- (void)dealloc { [_tick invalidate]; }

- (void)titleDoubleClick {
    NSString* action = [[NSUserDefaults standardUserDefaults] stringForKey:@"AppleActionOnDoubleClick"];
    if ([action isEqualToString:@"Minimize"]) [self.window performMiniaturize:nil];
    else if ([action isEqualToString:@"None"]) return;
    else [self.window performZoom:nil];   // "Maximize" / "Fill" / unset
}

- (void)mouseDown:(NSEvent*)e {
    [self.window makeFirstResponder:self];
    bool ctrlClick = (e.modifierFlags & NSEventModifierFlagControl) != 0;
    pui::PointerEvent pe = [self pointerEvent:e button:ctrlClick ? pui::PointerButton::Right : pui::PointerButton::Left];
    pui::UiHost& host = *_host;
    if (!ctrlClick && host.PopupCount() == 0 && pe.pt.y < 28 && (host.HitTest(pe.pt) == nullptr || host.HitTest(pe.pt) == host.Root())) {
        // Empty area under the (transparent) title bar: the HTCAPTION equivalent.
        if (e.clickCount == 2) { [self titleDoubleClick]; return; }
        host.CancelCapture();
        [self.window performWindowDragWithEvent:e];
        return;
    }
    host.PointerDown(pe);
    [self afterEvent];
}

- (void)mouseDragged:(NSEvent*)e {
    _host->PointerMove([self pointerEvent:e button:pui::PointerButton::Left]);
    [self afterEvent];
}

- (void)mouseUp:(NSEvent*)e {
    bool ctrlClick = (e.modifierFlags & NSEventModifierFlagControl) != 0;
    _host->PointerUp([self pointerEvent:e button:ctrlClick ? pui::PointerButton::Right : pui::PointerButton::Left]);
    [self afterEvent];
}

- (void)rightMouseDown:(NSEvent*)e {
    [self.window makeFirstResponder:self];
    _host->PointerDown([self pointerEvent:e button:pui::PointerButton::Right]);
    [self afterEvent];
}

- (void)rightMouseDragged:(NSEvent*)e {
    _host->PointerMove([self pointerEvent:e button:pui::PointerButton::Right]);
    [self afterEvent];
}

- (void)rightMouseUp:(NSEvent*)e {
    _host->PointerUp([self pointerEvent:e button:pui::PointerButton::Right]);
    [self afterEvent];
}

- (void)mouseMoved:(NSEvent*)e {
    _host->PointerMove([self pointerEvent:e button:pui::PointerButton::None]);
    [self afterEvent];
}

- (void)mouseExited:(NSEvent*)e {
    (void)e;
    _host->PointerLeave();
    [self afterEvent];
}

- (void)scrollWheel:(NSEvent*)e {
    pui::PointerEvent pe = [self pointerEvent:e button:pui::PointerButton::None];
    // pui: +1 per wheel notch (= one wheelStep of 48 DIP). Trackpads report precise point deltas.
    CGFloat dy = e.scrollingDeltaY;
    pe.wheelDelta = (float)(e.hasPreciseScrollingDeltas ? dy / 48.0 : dy);
    if (pe.wheelDelta != 0) _host->PointerWheel(pe);
    [self afterEvent];
}

// --- keyboard -----------------------------------------------------------------------------------------------------------

- (int)vkFor:(NSEvent*)e {
    std::wstring chars = ToW(e.charactersIgnoringModifiers);
    unsigned int first = chars.empty() ? 0u : (unsigned int)chars[0];
    return puimac::VkFromMacKey(e.keyCode, first);
}

- (void)keyDown:(NSEvent*)e {
    int vk = [self vkFor:e];
    unsigned mods = puimac::ModsFromFlags(e.modifierFlags);
    bool handled = vk != 0 && _host->KeyDown(vk, mods);   // Esc closes an open flyout / dialog first
    bool command = (e.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) != 0;
    if (!handled && vk == pui::key::Escape && mods == 0 && _marked.empty() && _onEscape) { _onEscape(); return; }   // (an IME composition takes Esc itself)
    if (!handled && !command) [self.inputContext handleEvent:e];   // -> insertText: / setMarkedText: (IME)
    else if (!handled && !e.isARepeat) [super keyDown:e];
    [self afterEvent];
}

- (void)keyUp:(NSEvent*)e {
    int vk = [self vkFor:e];
    if (vk) _host->KeyUp(vk, puimac::ModsFromFlags(e.modifierFlags));
    [self afterEvent];
}

// Edit menu (key equivalents are consumed by the menu bar before keyDown:); pui uses Ctrl+X/C/V/A.
- (void)cut:(id)s { (void)s; _host->KeyDown(0x58, pui::ModCtrl); [self afterEvent]; }
- (void)copy:(id)s { (void)s; _host->KeyDown(0x43, pui::ModCtrl); [self afterEvent]; }
- (void)paste:(id)s { (void)s; _host->KeyDown(0x56, pui::ModCtrl); [self afterEvent]; }
- (void)selectAll:(id)s { (void)s; _host->KeyDown(pui::key::A, pui::ModCtrl); [self afterEvent]; }

// --- NSTextInputClient ------------------------------------------------------------------------------------------------
// TextBox has no composition API (its header: "no IME composition UI"), so the composition is shown inline by inserting
// the marked text as ordinary characters and replacing it on every update; the committed text replaces it for good.

- (pui::TextBox*)focusedTextBox { return dynamic_cast<pui::TextBox*>(_host->Focused()); }

- (void)removeMarked {
    if (_marked.empty()) return;
    if (pui::TextBox* tb = [self focusedTextBox]) {
        tb->SetSelection(_markedStart, _markedStart + _marked.size());
        _host->KeyDown(pui::key::Back, 0);
    }
    _marked.clear();
}

- (void)insertChars:(const std::wstring&)s {
    for (wchar_t c : s) _host->Char(c);
}

- (void)insertText:(id)string replacementRange:(NSRange)rr {
    (void)rr;
    NSString* s = [string isKindOfClass:[NSAttributedString class]] ? [(NSAttributedString*)string string] : (NSString*)string;
    [self removeMarked];
    [self insertChars:ToW(s)];
    [self afterEvent];
}

- (void)setMarkedText:(id)string selectedRange:(NSRange)sel replacementRange:(NSRange)rr {
    (void)sel; (void)rr;
    NSString* s = [string isKindOfClass:[NSAttributedString class]] ? [(NSAttributedString*)string string] : (NSString*)string;
    [self removeMarked];
    pui::TextBox* tb = [self focusedTextBox];
    if (!tb) return;
    _markedStart = tb->SelectionStart();
    _marked = ToW(s);
    [self insertChars:_marked];
    [self afterEvent];
}

- (void)unmarkText { _marked.clear(); }
- (BOOL)hasMarkedText { return !_marked.empty(); }
- (NSRange)markedRange { return _marked.empty() ? NSMakeRange(NSNotFound, 0) : NSMakeRange(_markedStart, _marked.size()); }
- (NSRange)selectedRange {
    pui::TextBox* tb = [self focusedTextBox];
    if (!tb) return NSMakeRange(NSNotFound, 0);
    return NSMakeRange(tb->SelectionStart(), tb->SelectionEnd() - tb->SelectionStart());
}
- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText { return @[]; }
- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)r actualRange:(NSRangePointer)a { (void)r; (void)a; return nil; }
- (NSUInteger)characterIndexForPoint:(NSPoint)p { (void)p; return NSNotFound; }

- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actual {
    (void)actual;
    pui::TextBox* tb = [self focusedTextBox];
    if (!tb) return NSZeroRect;
    pui::RectF b = tb->Bounds();
    pui::TextStyle st;
    st.size = _host->GetTheme().fontBody;
    st.ellipsis = false;
    size_t idx = std::min<size_t>(range.location == NSNotFound ? tb->SelectionStart() : range.location, tb->Text().size());
    float x = b.l + 32 + _renderer->MeasureText(std::wstring_view(tb->Text()).substr(0, idx), st, 0).w;   // 10 pad + 14 glyph + 8 gap
    NSRect r = NSMakeRect(x, b.t, 1, b.h());
    return [self.window convertRectToScreen:[self convertRect:r toView:nil]];
}

- (void)doCommandBySelector:(SEL)sel {
    (void)sel;   // Enter/Tab/arrows already went through UiHost::KeyDown; nothing to do here
}

@end
