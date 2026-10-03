#pragma once
// The Settings content (Appearance, Playback, Captions, About). Hosted by the separate Settings window
// (shell/SettingsWindow), which owns its own UiHost; every change is saved and applied live through OnSettingsChanged.
#include <windows.h>

#include <functional>
#include <memory>

#include "pui/Widget.h"

namespace app {
class AppContext;

/// Opens an ms-settings: URI. The default launcher uses ShellExecuteW; tests inject a recorder.
using UriLauncher = std::function<void(const wchar_t* uri)>;

struct SettingsPageEnv {
    pui::UiHost* host = nullptr;          // the UiHost the page lives in (focus -> scroll into view); nullptr = ctx.Ui()
    HWND owner = nullptr;                 // owner of the caption font picker; nullptr = ctx.MainHwnd()
    std::function<void()> editFilters;    // "Edit filters..."; empty = the filters panel in the main window
    UriLauncher launchUri;                // empty = ShellExecuteW
};

std::unique_ptr<pui::Widget> CreateSettingsPage(AppContext& ctx, SettingsPageEnv env = {});
}
