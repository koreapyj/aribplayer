#import "PropertiesWindowMac.h"

#import <AppKit/AppKit.h>

#include <algorithm>

#include "Localize.h"
#include "MainView.h"
#include "PlayerAttachedMac.h"
#include "PopupDismissMac.h"
#include "PuiHostView.h"
#include "features/PropertiesDialog.h"
#include "pui/ToolTipStyle.h"
#include "pui/Widget.h"
#include "puimac/CGRenderer.h"
#include "wvp/win32/Utf.h"

@interface PropertiesWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) mainview::PropertiesWindow* owner;
@end

@implementation PropertiesWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)w { (void)w; if (_owner) _owner->Close(); return NO; }
@end

namespace mainview {

namespace {
constexpr CGFloat kDefaultW = 560, kDefaultH = 560, kMinW = 320, kMinH = 240;   // PropertiesWindow::kDefault* / kMin* (DIP)

/// The window's root: fills the client with the layer colour and hosts the page (HostRoot).
class Root final : public pui::Widget {
public:
    Root() : pui::Widget(pui::Role::Window) {
        SetName(app::Tr(L"Properties", L"재생 정보"));
        SetAutomationId(L"PropertiesWindow");
    }
    pui::Widget* page = nullptr;
    void OnLayout() override { if (page) page->SetBounds(Bounds()); }
    void Paint(pui::IRenderer& r, const pui::Theme& t) override { r.FillRect(Bounds(), t.layerFill); }
};
}

struct PropertiesWindow::Impl : PlayerAttached {
    pui::UiHost host;
    puimac::CGRenderer renderer;
    PuiHostView* view = nil;
    PropertiesWindowDelegate* delegate = nil;
    Root* root = nullptr;
    pui::Widget* page = nullptr;
    explicit Impl(MainView& m) : PlayerAttached(m) {}
};

PropertiesWindow::PropertiesWindow(MainView& mv) : impl_(std::make_unique<Impl>(mv)) {
    Impl& d = *impl_;
    pui::InstallToolTipStyle(d.host);
    NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable;   // no miniaturize: as on Windows
    d.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, kDefaultW, kDefaultH) styleMask:style backing:NSBackingStoreBuffered defer:YES];
    d.window.title = [NSString stringWithUTF8String:wvp::ToUtf8(app::Tr(L"Properties", L"재생 정보")).c_str()];
    d.window.minSize = NSMakeSize(kMinW, kMinH);
    d.window.releasedWhenClosed = NO;
    d.window.tabbingMode = NSWindowTabbingModeDisallowed;
    d.window.collectionBehavior |= NSWindowCollectionBehaviorFullScreenAuxiliary | NSWindowCollectionBehaviorManaged;
    d.delegate = [[PropertiesWindowDelegate alloc] init];
    d.delegate.owner = this;
    d.window.delegate = d.delegate;

    NSVisualEffectView* fx = [[NSVisualEffectView alloc] initWithFrame:NSMakeRect(0, 0, kDefaultW, kDefaultH)];
    fx.material = NSVisualEffectMaterialWindowBackground;
    fx.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    fx.state = NSVisualEffectStateFollowsWindowActiveState;
    d.window.contentView = fx;
    d.view = [[PuiHostView alloc] initWithFrame:fx.bounds host:&d.host renderer:&d.renderer];
    d.view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [fx addSubview:d.view];
    MainView* m = &mv;
    [d.view setThemeMaker:[m](float scale) { return m->MakeTheme(scale); }];
    pui::UiHost* host = &d.host;
    [d.view setOnResize:[host](NSSize s) { host->SetSize({(float)s.width, (float)s.height}); }];
    PropertiesWindow* self = this;
    [d.view setOnEscape:[self] { self->Close(); }];
    PuiHostView* view = d.view;
    popupdismiss::Watch(d.host, (__bridge void*)d.window, {}, [view] { [view afterEvent]; });
}

PropertiesWindow::~PropertiesWindow() {
    Impl& d = *impl_;
    popupdismiss::Unwatch(d.host);
    d.host.onInvalidate = nullptr;
    d.delegate.owner = nullptr;
    [d.window orderOut:nil];
    [d.view removeFromSuperview];
}

void PropertiesWindow::Show(std::vector<app::fx::PropSection> sections) {
    Impl& d = *impl_;
    const bool wasOpen = IsOpen();
    d.host.CloseAllPopups();
    // Closing from the button runs inside the page that closing tears down: posted.
    PropertiesWindow* self = this;
    MainView* mv = &d.mv;
    app::PropertiesPage p = app::CreatePropertiesPage(std::move(sections), &mv->Measurer(), [self, mv] {
        mv->PostToUi([self] { self->Close(); });
    });
    if (!d.root) {
        auto root = std::make_unique<Root>();
        d.root = root.get();
        d.host.SetRoot(std::move(root));
        d.host.SetSize({(float)NSWidth(d.view.bounds), (float)NSHeight(d.view.bounds)});
        [d.view applyTheme];
    }
    if (d.page) d.root->RemoveChild(d.page);
    d.page = d.root->AddChild(std::move(p.root));
    d.root->page = d.page;
    d.root->OnLayout();
    d.host.SetFocus(p.initialFocus);

    if (!wasOpen) {   // centred over the player window, inside its screen
        NSWindow* main = d.Player();
        NSRect f = d.window.frame;
        if (main) {
            const NSRect m = main.frame;
            f.origin = NSMakePoint(NSMidX(m) - NSWidth(f) / 2, NSMidY(m) - NSHeight(f) / 2);
            if (NSScreen* s = main.screen) {
                const NSRect vis = s.visibleFrame;
                f.origin.x = std::clamp(f.origin.x, NSMinX(vis), std::max(NSMinX(vis), NSMaxX(vis) - NSWidth(f)));
                f.origin.y = std::clamp(f.origin.y, NSMinY(vis), std::max(NSMinY(vis), NSMaxY(vis) - NSHeight(f)));
            }
            [d.window setFrame:f display:NO];
        } else {
            [d.window center];
        }
    }
    d.hiddenForPlayer = false;
    [d.window makeKeyAndOrderFront:nil];
    [d.window makeFirstResponder:d.view];
    StartFocusCycle();
    [d.view afterEvent];
}

void PropertiesWindow::Close() {
    Impl& d = *impl_;
    if (!IsOpen()) return;
    d.host.CloseAllPopups();
    d.hiddenForPlayer = false;
    [d.window orderOut:nil];
}

bool PropertiesWindow::IsOpen() const { return impl_->window.visible || impl_->hiddenForPlayer; }
void PropertiesWindow::StartFocusCycle() { impl_->StartFocusCycle(); }
void PropertiesWindow::OnPlayerPressed() { impl_->OnPlayerPressed(); }
void PropertiesWindow::KeepAbove() { impl_->KeepAbove(); }
void PropertiesWindow::OnPlayerMinimized(bool minimized) { impl_->OnPlayerMinimized(impl_->host, minimized); }
void PropertiesWindow::ApplyTheme() { [impl_->view applyTheme]; }
pui::UiHost& PropertiesWindow::Ui() { return impl_->host; }
pui::Widget* PropertiesWindow::Page() const { return impl_->page; }

}
