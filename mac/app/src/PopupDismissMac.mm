#import "PopupDismissMac.h"

#import <AppKit/AppKit.h>

#include <map>
#include <vector>

#include "pui/Widget.h"

namespace popupdismiss {

namespace {
struct Watcher {
    __weak NSWindow* window = nil;
    std::function<bool(const pui::Widget*)> keep;
    std::function<void()> after;
    NSMutableArray* observers = [NSMutableArray array];
};
std::map<pui::UiHost*, Watcher>& Watchers() {
    static std::map<pui::UiHost*, Watcher> w;
    return w;
}
}

void CloseFor(pui::UiHost& host) {
    auto it = Watchers().find(&host);
    if (it == Watchers().end() || host.PopupCount() == 0) return;
    const std::vector<pui::Widget*> popups = host.Popups();   // bottom -> top
    size_t first = 0;
    if (it->second.keep)
        for (size_t i = popups.size(); i-- > 0;)
            if (it->second.keep(popups[i])) { first = i + 1; break; }
    if (first < popups.size()) host.ClosePopup(popups[first]);   // and every popup above it
    if (it->second.after) it->second.after();
}

void Watch(pui::UiHost& host, void* window, std::function<bool(const pui::Widget*)> keep, std::function<void()> after) {
    Unwatch(host);
    NSWindow* w = (__bridge NSWindow*)window;
    Watcher& watcher = Watchers()[&host];
    watcher.window = w;
    watcher.keep = std::move(keep);
    watcher.after = std::move(after);
    pui::UiHost* h = &host;
    NSNotificationCenter* nc = NSNotificationCenter.defaultCenter;
    // WA_INACTIVE: decided once AppKit has settled the new key window (during a modal session NSApp.modalWindow is set).
    [watcher.observers addObject:[nc addObserverForName:NSWindowDidResignKeyNotification object:w queue:nil usingBlock:^(NSNotification* n) {
        (void)n;
        dispatch_async(dispatch_get_main_queue(), ^{
            auto it = Watchers().find(h);
            if (it == Watchers().end()) return;
            NSWindow* win = it->second.window;
            if (!win || win.keyWindow || NSApp.modalWindow) return;   // key again, or one of the app's own modal dialogs
            CloseFor(*h);
        });
    }]];
    for (NSNotificationName name : {NSWindowWillMiniaturizeNotification, NSWindowWillCloseNotification})
        [watcher.observers addObject:[nc addObserverForName:name object:w queue:nil usingBlock:^(NSNotification* n) { (void)n; CloseFor(*h); }]];
}

void Unwatch(pui::UiHost& host) {
    auto it = Watchers().find(&host);
    if (it == Watchers().end()) return;
    for (id o in it->second.observers) [NSNotificationCenter.defaultCenter removeObserver:o];
    Watchers().erase(it);
}

}
