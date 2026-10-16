#pragma once
// The pui menus (puimac::MenuPopup: the nativemenu menus and their submenus) of a host, each in a window of its own, as
// the native Win32 menus of the Windows app are top-level popup windows: a borderless, non-activating panel at the
// pop-up menu level, a child of the owner window (so it joins the owner's space, full screen included), with a shadow,
// the owner's appearance and its own backing scale. A small owner window (mini player, a small main window) no longer
// clips a menu; menus are kept inside the visible frame of the screen (puimac::SetMenuArea) and flip left / above at
// its edges.
//
// The menus stay popups of the owner's UiHost (focus, keys, light dismiss, the submenu chain and the shell's popup
// rules are unchanged); the host's Paint skips them (MenuPopup::SetOwnWindow) and each window paints its menu and
// forwards the pointer to the host in the owner view's coordinates. Keys keep going to the owner window, which stays
// key. Besides the host's own light dismiss (a press outside the menus in the owner view, Escape) and PopupDismissMac's
// (the owner losing key, miniaturised, closed), the menus close when the app is deactivated, the owner window starts
// moving / resizing / changes full screen or screen, a press lands in another window or in another app, or on the
// owner window's frame (traffic lights).
namespace pui { class UiHost; class Widget; }

namespace menuwindows {

/// Shows the menus of `host` in their own windows above the window of `ownerView` (the NSView painting the host; it
/// answers -afterEvent, as ShellView and PuiHostView do). One host at a time; attaching again replaces it.
void Attach(pui::UiHost& host, void* ownerView);
void Detach();

// self-test hooks
/// The NSPanel showing `menu`, nullptr when it has none.
void* WindowOf(const pui::Widget* menu);
/// Visible menu windows.
int OpenWindows();
/// How often the window of `menu` painted it.
int PaintCount(const pui::Widget* menu);

}
