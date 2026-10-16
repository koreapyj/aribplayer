#pragma once
// Closes a window's pui popups where the Windows app's windows close theirs: App / SettingsWindow WM_ACTIVATE
// (WA_INACTIVE) and App WM_SIZE (SIZE_MINIMIZED) call UiHost::CloseAllPopups, and so does closing the window. On macOS:
// when the window stops being the key window (the app deactivates, another of its windows becomes key) unless one of
// the app's own modal dialogs took the keyboard (NSOpenPanel, the NSAlert prompts: the dialogs Windows owns by the
// window, which keep the popups there), when it is miniaturised and when it closes.
#include <functional>

namespace pui { class UiHost; class Widget; }

namespace popupdismiss {

/// Watches `window` (NSWindow*) for `host`. `keep` spares a popup and the popups below it (the Settings window's
/// caption style editors stand in for the dialogs Windows owns by that window); `after` runs after popups closed (the
/// view repaints).
void Watch(pui::UiHost& host, void* window, std::function<bool(const pui::Widget*)> keep = {}, std::function<void()> after = {});
void Unwatch(pui::UiHost& host);

/// Closes the popups of `host` as Watch does (above the topmost kept one).
void CloseFor(pui::UiHost& host);

}
