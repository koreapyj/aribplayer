#import "ShellView.h"

#include <algorithm>
#include <string>
#include <vector>

#include "MainView.h"
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

@implementation ShellView {
    mainview::MainView* _mv;          // not owned
    puimac::CGRenderer* _renderer;    // not owned
    NSTimer* _tick;
    NSTrackingArea* _tracking;
    std::wstring _marked;
    size_t _markedStart;
}

- (instancetype)initWithFrame:(NSRect)frame mainView:(mainview::MainView*)mv renderer:(puimac::CGRenderer*)renderer {
    if ((self = [super initWithFrame:frame])) {
        _mv = mv;
        _renderer = renderer;
        _markedStart = 0;
        self.wantsLayer = YES;
        self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawOnSetNeedsDisplay;
        __weak ShellView* weakSelf = self;
        _mv->host.onInvalidate = [weakSelf] { [weakSelf setNeedsDisplay:YES]; };
        [self updateTracking];
        // App::RegisterDropTargets: files (and URLs) dropped on the video / bar / title play, on the panel append
        [self registerForDraggedTypes:@[NSPasteboardTypeFileURL, NSPasteboardTypeURL]];
    }
    return self;
}

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
    _mv->host.SetTheme(_mv->MakeTheme((float)[self scale]));   // appearance + the accent of the settings
    [self setNeedsDisplay:YES];
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    if (self.window) [self applyTheme];
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    [self applyTheme];
    [self relayout];
}

- (void)viewDidChangeEffectiveAppearance {
    [super viewDidChangeEffectiveAppearance];
    [self applyTheme];
}

- (void)relayout {
    const CGFloat s = [self scale];
    _mv->Resize((int)std::lround(self.bounds.size.width * s), (int)std::lround(self.bounds.size.height * s), (float)s);
    [self setNeedsDisplay:YES];
}

- (void)setFrameSize:(NSSize)sz {
    [super setFrameSize:sz];
    [self relayout];
    [self updateTracking];
}

- (void)updateTracking {
    if (_tracking) [self removeTrackingArea:_tracking];
    _tracking = [[NSTrackingArea alloc] initWithRect:self.bounds
                                             options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
                                               owner:self userInfo:nil];
    [self addTrackingArea:_tracking];
}

// --- painting --------------------------------------------------------------------------------------------------------

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    CGContextRef ctx = NSGraphicsContext.currentContext.CGContext;
    if (!ctx) return;
    _renderer->Begin(ctx, (float)[self scale]);
    _mv->host.Paint(*_renderer);
    _renderer->End();
    _mv->host.ClearDirty();
    [self ensureTicking];
}

- (void)ensureTicking {
    if (!_mv->host.NeedsTick() || _tick) return;
    __weak ShellView* weakSelf = self;
    _tick = [NSTimer scheduledTimerWithTimeInterval:1.0 / 60 repeats:YES block:^(NSTimer* t) {
        ShellView* s = weakSelf;
        if (!s) { [t invalidate]; return; }
        bool more = s->_mv->host.Tick();
        if (s->_mv->host.IsDirty()) [s setNeedsDisplay:YES];
        if (!more) { [t invalidate]; s->_tick = nil; }
    }];
}

- (void)afterEvent {
    if (_mv->host.IsDirty()) [self setNeedsDisplay:YES];
    [self ensureTicking];
}

// --- pointer ----------------------------------------------------------------------------------------------------------

/// The splitter's horizontal-resize cursor (App's WM_SETCURSOR over ShellLayout::splitterPx).
- (void)resetCursorRects {
    [super resetCursorRects];
    const app::RectPx& r = _mv->Layout().splitterPx;
    if (r.Empty() || _mv->Layout().panelOverlay || _mv->Mode() != app::ShellMode::NowPlaying) return;
    const CGFloat s = _mv->Scale();
    [self addCursorRect:NSMakeRect(r.l / s, r.t / s, (r.r - r.l) / s, (r.b - r.t) / s) cursor:NSCursor.resizeLeftRightCursor];
}

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

- (void)titleDoubleClick {
    NSString* action = [[NSUserDefaults standardUserDefaults] stringForKey:@"AppleActionOnDoubleClick"];
    if ([action isEqualToString:@"Minimize"]) [self.window performMiniaturize:nil];
    else if ([action isEqualToString:@"None"]) return;
    else [self.window performZoom:nil];
}

- (void)mouseDown:(NSEvent*)e {
    [self.window makeFirstResponder:self];
    bool ctrlClick = (e.modifierFlags & NSEventModifierFlagControl) != 0;
    pui::PointerEvent pe = [self pointerEvent:e button:ctrlClick ? pui::PointerButton::Right : pui::PointerButton::Left];
    pui::UiHost& host = _mv->host;
    // App's WM_LBUTTONDOWN: the splitter between the video and the docked panel is neither video nor panel.
    if (!ctrlClick && _mv->BeginSplitterDrag(pe.pt, e.clickCount == 2)) {
        if (_mv->SplitterDragging()) [[NSCursor resizeLeftRightCursor] push];
        [self afterEvent];
        return;
    }
    pui::Widget* hit = host.HitTest(pe.pt);
    const bool empty = hit == nullptr || hit == _mv->shell;   // no widget: the title strip's drag zone or the video
    if (!ctrlClick && !_mv->UserPopupOpen() && empty) {
        _mv->PressAt(nullptr);
        if (_mv->IsTitleStripPoint(pe.pt)) {   // HTCAPTION
            if (e.clickCount == 2) { [self titleDoubleClick]; return; }
            host.CancelCapture();
            [self.window performWindowDragWithEvent:e];
            return;
        }
        if (_mv->IsVideoPoint(pe.pt)) {   // App::OnVideoInput: double click = play / pause, drag moves the window
            if (e.clickCount == 2) { _mv->VideoDoubleClick(); [self afterEvent]; return; }
            if (!_mv->Modes().fullscreen) { host.CancelCapture(); [self.window performWindowDragWithEvent:e]; }
            return;
        }
    }
    _mv->PressAt(hit);
    host.PointerDown(pe);
    [self afterEvent];
}

- (void)mouseDragged:(NSEvent*)e {
    if (_mv->SplitterDragging()) {   // live: relayout per move, the video column follows
        _mv->DragSplitter([self pointerEvent:e button:pui::PointerButton::Left].pt);
        [self afterEvent];
        return;
    }
    _mv->host.PointerMove([self pointerEvent:e button:pui::PointerButton::Left]);
    [self afterEvent];
}

- (void)mouseUp:(NSEvent*)e {
    if (_mv->EndSplitterDrag()) {   // release persists playlistPanelWidthDip
        [NSCursor pop];
        [self.window invalidateCursorRectsForView:self];
        [self afterEvent];
        return;
    }
    bool ctrlClick = (e.modifierFlags & NSEventModifierFlagControl) != 0;
    _mv->host.PointerUp([self pointerEvent:e button:ctrlClick ? pui::PointerButton::Right : pui::PointerButton::Left]);
    [self afterEvent];
}

- (void)rightMouseDown:(NSEvent*)e {
    [self.window makeFirstResponder:self];
    pui::PointerEvent pe = [self pointerEvent:e button:pui::PointerButton::Right];
    pui::Widget* hit = _mv->host.HitTest(pe.pt);
    if (!_mv->UserPopupOpen() && (hit == nullptr || hit == _mv->shell) && _mv->IsVideoPoint(pe.pt)) {
        _mv->ShowMainMenu(pe.pt);   // the main menu is the video's context menu
        [self afterEvent];
        return;
    }
    _mv->host.PointerDown(pe);
    [self afterEvent];
}

- (void)rightMouseDragged:(NSEvent*)e {
    _mv->host.PointerMove([self pointerEvent:e button:pui::PointerButton::Right]);
    [self afterEvent];
}

- (void)rightMouseUp:(NSEvent*)e {
    _mv->host.PointerUp([self pointerEvent:e button:pui::PointerButton::Right]);
    [self afterEvent];
}

- (void)mouseMoved:(NSEvent*)e {
    pui::PointerEvent pe = [self pointerEvent:e button:pui::PointerButton::None];
    _mv->host.PointerMove(pe);
    _mv->PointerAt(pe.pt);
    [self afterEvent];
}

- (void)mouseExited:(NSEvent*)e {
    (void)e;
    _mv->host.PointerLeave();
    _mv->PointerAt(std::nullopt);
    [self afterEvent];
}

- (void)scrollWheel:(NSEvent*)e {
    pui::PointerEvent pe = [self pointerEvent:e button:pui::PointerButton::None];
    CGFloat dy = e.scrollingDeltaY;
    pe.wheelDelta = (float)(e.hasPreciseScrollingDeltas ? dy / 48.0 : dy);
    if (pe.wheelDelta == 0) return;
    if (!_mv->host.PointerWheel(pe) && _mv->IsVideoPoint(pe.pt)) _mv->VideoWheel(pe.wheelDelta);   // wheel over the video = volume
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
    bool typing = pui::TextBox::IsTextInputFocused(_mv->host);
    bool command = (e.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) != 0;
    bool handled = vk != 0 && _mv->Key(vk, mods, e.isARepeat);
    if (!handled && !command) {
        if (typing) [self.inputContext handleEvent:e];   // the search box: characters through the IME path (insertText: / setMarkedText:)
        else if (!e.isARepeat) [super keyDown:e];
    }
    [self afterEvent];
}

- (void)keyUp:(NSEvent*)e {
    int vk = [self vkFor:e];
    if (vk) { _mv->KeyUp(vk); _mv->host.KeyUp(vk, puimac::ModsFromFlags(e.modifierFlags)); }
    [self afterEvent];
}

// --- drag and drop (App's IDropTarget: RouteDropAtClient + HandleDropAtClient) -------------------------------------------

- (std::vector<std::wstring>)droppedPaths:(id<NSDraggingInfo>)info {
    std::vector<std::wstring> out;
    NSArray<NSURL*>* urls = [info.draggingPasteboard readObjectsForClasses:@[[NSURL class]] options:@{}];
    for (NSURL* u in urls) {
        NSString* s = u.isFileURL ? u.URLByResolvingSymlinksInPath.path : u.absoluteString;
        if (s.length) out.push_back(ToW(s));
    }
    return out;
}

- (pui::PointF)dropPoint:(id<NSDraggingInfo>)info {
    NSPoint p = [self convertPoint:info.draggingLocation fromView:nil];
    return {(float)p.x, (float)p.y};
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info { return [self draggingUpdated:info]; }

- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)info {
    if ([self droppedPaths:info].empty()) return NSDragOperationNone;
    return _mv->AcceptsDropAt([self dropPoint:info]) ? NSDragOperationCopy : NSDragOperationNone;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)info {
    const std::vector<std::wstring> paths = [self droppedPaths:info];
    const bool ok = _mv->DropAt([self dropPoint:info], paths);
    if (ok) [NSApp activateIgnoringOtherApps:YES];
    [self afterEvent];
    return ok;
}

- (void)cut:(id)s { (void)s; _mv->host.KeyDown(0x58, pui::ModCtrl); [self afterEvent]; }
// Cmd+C: the focused search box copies text; otherwise the frame with captions (the key table's Ctrl+C on Windows).
- (void)copy:(id)s {
    (void)s;
    if (pui::TextBox::IsTextInputFocused(_mv->host)) _mv->host.KeyDown(0x43, pui::ModCtrl);
    else _mv->Execute(app::CommandId::CaptureFrameWithCaptions);
    [self afterEvent];
}
- (void)paste:(id)s { (void)s; _mv->host.KeyDown(0x56, pui::ModCtrl); [self afterEvent]; }
- (void)selectAll:(id)s { (void)s; _mv->host.KeyDown(pui::key::A, pui::ModCtrl); [self afterEvent]; }

// --- NSTextInputClient (as PuiHostView: the composition is shown inline as plain characters) -------------------------------

- (pui::TextBox*)focusedTextBox { return dynamic_cast<pui::TextBox*>(_mv->host.Focused()); }

- (void)removeMarked {
    if (_marked.empty()) return;
    if (pui::TextBox* tb = [self focusedTextBox]) {
        tb->SetSelection(_markedStart, _markedStart + _marked.size());
        _mv->host.KeyDown(pui::key::Back, 0);
    }
    _marked.clear();
}

- (void)insertChars:(const std::wstring&)s { for (wchar_t c : s) _mv->host.Char(c); }

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
    st.size = _mv->host.GetTheme().fontBody;
    st.ellipsis = false;
    size_t idx = std::min<size_t>(range.location == NSNotFound ? tb->SelectionStart() : range.location, tb->Text().size());
    float x = b.l + 32 + _renderer->MeasureText(std::wstring_view(tb->Text()).substr(0, idx), st, 0).w;
    NSRect r = NSMakeRect(x, b.t, 1, b.h());
    return [self.window convertRectToScreen:[self convertRect:r toView:nil]];
}
- (void)doCommandBySelector:(SEL)sel { (void)sel; }

@end
