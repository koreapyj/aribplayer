#pragma once
// UI Automation provider layer for PlayerUi. Wraps pui::Widget objects in IRawElementProviderFragment objects
// (one COM provider per widget, cached in Widget::uiaSlot) and forwards UiHost UI events to UIA clients.
// The app must link uiautomationcore.lib.
// Shut the bridge down before the window and the UiHost are destroyed.
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <uiautomation.h>

#include <functional>
#include <memory>
#include <string>

#include "Widget.h"

namespace pui {

struct UiaBridgeState;

int UiaControlTypeFor(Role r);
std::wstring UiaClassNameFor(Role r);

class UiaBridge {
public:
    /// hwnd may be nullptr (tests): no host provider, screen coordinates = client coordinates + SetClientOrigin().
    UiaBridge(HWND hwnd, UiHost& host, std::function<float()> scale);
    ~UiaBridge();
    UiaBridge(const UiaBridge&) = delete;
    UiaBridge& operator=(const UiaBridge&) = delete;

    /// WM_GETOBJECT handler. Returns true (and *result) when lParam == UiaRootObjectId.
    bool HandleGetObject(WPARAM wParam, LPARAM lParam, LRESULT* result);

    /// Fragment root (the window). Borrowed pointer (no AddRef); valid until Shutdown/destruction.
    IRawElementProviderFragmentRoot* Root();
    /// Provider for a widget (creates and caches it in Widget::uiaSlot). Borrowed (no AddRef). The host's root widget
    /// maps to the fragment root. nullptr for nullptr / after Shutdown.
    IRawElementProviderSimple* ProviderFor(Widget* w);

    /// Disconnect all providers, remove the host listener, detach from the window. Idempotent.
    void Shutdown();
    bool IsShutdown() const;

    /// App-defined extra off-screen rule: a widget (or any ancestor) for which `pred` returns true reports IsOffscreen=true
    /// and is skipped by ElementProviderFromPoint (e.g. the auto-hidden player bar / title strip). Call NotifyOffscreenChanged
    /// for the affected widgets when the answer changes.
    void SetOffscreenPredicate(std::function<bool(Widget*)> pred);
    void NotifyOffscreenChanged(Widget* w);                 // raises Prop:IsOffscreen for w with its current value

    void SetClientOrigin(int x, int y);                     // screen position of the client origin when hwnd == nullptr
    void SetAlwaysRaise(bool on);                           // raise UIA events even when no client is listening
    /// Called for each UIA event the bridge raises: (description such as "Event:Invoke" / "Prop:ToggleState", widget).
    void SetRaisedHook(std::function<void(const wchar_t*, Widget*)> h);
    unsigned EventsHandled() const;                         // host events received while subscribed
    unsigned EventsRaised() const;                          // UIA events actually raised
    int ListenerToken() const;                              // 0 after Shutdown
    size_t CachedProviderCount() const;
    static long LiveProviderCount();                        // all provider objects alive in the process (leak check)

private:
    std::shared_ptr<UiaBridgeState> s_;
};

/// Text dump of a UIA tree: one line per element, indented 2 spaces per level: `Button "Play" [Invoke,Toggle]`.
std::wstring DumpUiaTree(IRawElementProviderFragment* root, int maxDepth = 32);

}
