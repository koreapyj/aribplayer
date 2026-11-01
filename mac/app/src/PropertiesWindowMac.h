#pragma once
// The Properties window on macOS: the Mac counterpart of src/App/shell/PropertiesWindow, modeled on SettingsWindowMac (a separate,
// modeless, single-instance window with its own pui::UiHost, standard title bar, the same z-order rules through PlayerAttached).
// Content: features/PropertiesDialog's page (selectable values; Cmd+C copies through the focused SelectableText). Showing it again
// while open refreshes the rows for the current media and brings it to the front. Close button, Esc and the red button close it.
#include <memory>
#include <vector>

#include "features/FxProps.h"

namespace pui { class UiHost; class Widget; }

namespace mainview {

class MainView;

class PropertiesWindow {
public:
    explicit PropertiesWindow(MainView& mv);
    ~PropertiesWindow();
    PropertiesWindow(const PropertiesWindow&) = delete;
    PropertiesWindow& operator=(const PropertiesWindow&) = delete;

    void Show(std::vector<app::fx::PropSection> sections);
    void Close();
    /// Open, also while hidden for a minimized player.
    bool IsOpen() const;
    // Z-order: see SettingsWindow.
    void StartFocusCycle();
    void OnPlayerPressed();
    void KeepAbove();
    void OnPlayerMinimized(bool minimized);
    void ApplyTheme();
    pui::UiHost& Ui();
    pui::Widget* Page() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
