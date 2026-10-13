#pragma once
// NOT Win32: ShellExecuteW for shell/PlaylistPanel.cpp ("Open playlists folder") and pages/SettingsPage.cpp (its default
// URI launcher; the Mac settings window passes its own). See windows.h.
#include "windows.h"

#define SW_SHOWNORMAL 1

/// operation "open" on a local path (NSWorkspace: a folder opens in Finder) or on a URL with a scheme (http(s):,
/// x-apple.systempreferences:, ...). Other operations are ignored. Returns nullptr (the callers ignore the result).
void* ShellExecuteW(HWND owner, LPCWSTR operation, LPCWSTR file, LPCWSTR parameters, LPCWSTR directory, int showCmd);
