#pragma once
// Native Win32 popup menus (TrackPopupMenuEx, Windows 11 styled; dark through the uxtheme app mode). Every menu of the app goes
// through here: the descriptor trees (pui::MenuItemDesc, built by features/Menus.cpp, the playlist panel, the pages) are converted
// to an HMENU, tracked modally on the owner window (the main window, or the Settings window while it handles its input: OwnerScope)
// and the chosen item's action runs after the menu closed. A native menu is a real top-level window, so it is never clipped by a
// small window. Coordinates are client DIPs of the owner window.
//
// --logictest / --headless-check must never show anything (OsDialogsBlocked): there the call only records the descriptors
// (LastShown) and reports "cancelled"; tests invoke the recorded actions with InvokeItem.
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "pui/Geometry.h"
#include "pui/Menu.h"
#include "pui/Theme.h"

namespace app::nativemenu {

struct Callbacks {
    std::function<void()> onOpen, onClose;   // pressed look of the anchor: onOpen before tracking, onClose after (also when cancelled)
};

void SetOwner(HWND owner);
HWND Owner();
/// Menus opened while it lives are owned by (and positioned in client DIPs of) `owner`, e.g. the Settings window while it
/// dispatches its own input; restores the previous owner.
class OwnerScope {
public:
    explicit OwnerScope(HWND owner) : prev_(Owner()) { SetOwner(owner); }
    ~OwnerScope() { SetOwner(prev_); }
    OwnerScope(const OwnerScope&) = delete;
    OwnerScope& operator=(const OwnerScope&) = delete;
private:
    HWND prev_;
};
/// App theme -> menu theme: System follows the OS (AllowDark), Dark forces dark, Light forces light. Safe to call repeatedly.
void ApplyTheme(pui::ThemeMode mode);
/// A native menu is being tracked (the full screen edge overlays and auto-hide logic treat it like an open popup).
bool IsOpen();

/// Menu with its top-left at a client point (right click); flips at screen edges. True when an item was chosen.
bool ShowAtPoint(std::vector<pui::MenuItemDesc> items, pui::PointF clientDip, Callbacks cb = {});
/// Menu next to an anchor rectangle (a button / chip), below it or above it when `preferAbove`; the anchor stays uncovered.
bool ShowBelow(std::vector<pui::MenuItemDesc> items, const pui::RectF& anchorDip, bool preferAbove = false, Callbacks cb = {});

// Descriptors of the last call; recorded instead of shown when OS dialogs are blocked.
const std::vector<pui::MenuItemDesc>& LastShown();
void ClearLastShown();
/// Finds an item by its text (searching sub-menus) and runs it like a click (toggles checks); false when missing / disabled.
bool InvokeItem(std::vector<pui::MenuItemDesc>& items, const std::wstring& text);
const pui::MenuItemDesc* FindItem(const std::vector<pui::MenuItemDesc>& items, const std::wstring& text);

}
