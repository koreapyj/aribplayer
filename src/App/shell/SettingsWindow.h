#pragma once
// The Settings window: a HostWindow (separate, modeless, single-instance top-level window; see HostWindow.h for the window model,
// z-order rules and rendering) hosting the SettingsPage. Standard resizable frame with min / max / close; position / size /
// maximized are remembered in AppSettings::settingsWindow.
#include "shell/HostWindow.h"

namespace app {

class SettingsWindow final : public HostWindow {
public:
    static constexpr float kDefaultWidthDip = 900, kDefaultHeightDip = 700, kMinWidthDip = 480, kMinHeightDip = 360;

    SettingsWindow(AppContext& ctx, Gfx& gfx, IDWriteFactory* dw);
    ~SettingsWindow() override;

    /// Creates the window (remembered placement, else centred over the main window) or brings the open one to the front.
    /// hidden (--headless-check): created, laid out and rendered but never shown or activated. attachDevice: the shared UI
    /// device is usable (false while a failed rebind left none: the next successful rebind attaches it).
    bool Open(bool hidden, bool attachDevice);
    pui::Widget* Page() const { return page_; }

private:
    void BeforeDestroy() override;
    void EditFilters();

    pui::Widget* page_ = nullptr;
};

}
