#pragma once
// Mac implementation of features/NativeMenu.h (app::nativemenu): the pui::MenuItemDesc trees the shell builds are shown
// as pui-drawn puimac::MenuPopup popups in the shell's UiHost instead of Win32 TrackPopupMenuEx. The host registers the
// UiHost + measurer once; HWND owner functions are accepted and ignored.
#include <string>
#include <vector>

#include "Commands.h"
#include "features/NativeMenu.h"

namespace pui { class UiHost; class IRenderer; }

namespace app::nativemenu {
void SetMacHost(pui::UiHost* host, pui::IRenderer* measurer);
/// Commands.cpp shortcut text in the Mac notation ("Ctrl+Shift+Left" -> "⇧⌘←"); the menus show it this way.
std::wstring MacAccelerator(const std::wstring& windowsText);

/// The Mac shortcut of a command where the Mac binds another key than Commands.cpp's (the menu bar's key equivalents:
/// ⌘, Settings, ⌘Q Quit, ⇧⌘O Open folder, ...). One table for both menus: main.mm's menu bar takes its key equivalents
/// from it and the pui menus show it instead of the Windows key, so a shortcut is shown the same way in both and the
/// shown key works (the menu bar's key equivalent runs the command).
enum : unsigned { kMacCmd = 1, kMacShift = 2, kMacOpt = 4, kMacCtrl = 8 };
struct MacShortcut {
    CommandId command;
    wchar_t key;     // NSMenuItem.keyEquivalent: a lower-case character or an NS*FunctionKey code (0xF700 + ...)
    unsigned mods;   // kMac*
};
const std::vector<MacShortcut>& MacShortcuts();
const MacShortcut* MacShortcutFor(CommandId c);
std::wstring MacShortcutText(const MacShortcut& s);   // "⇧⌘O", "⌘,", "F4", "⌥⌘←"
/// What a pui menu item shows: the Mac shortcut of its command (found by Commands.cpp's text for it, or by the
/// Menus.cpp label for the items Windows leaves without a key: Exit, Previous / Next file, Mini player), else
/// MacAccelerator of its text.
std::wstring MacMenuAccelerator(const std::wstring& label, const std::wstring& windowsText);
}
