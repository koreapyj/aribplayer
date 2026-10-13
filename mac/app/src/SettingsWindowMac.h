#pragma once
// The Settings window on macOS: the Mac counterpart of src/App/shell/SettingsWindow (a separate, modeless,
// single-instance window with its own pui::UiHost) hosting src/App's pages/SettingsPage (Appearance, Playback,
// Captions, About) through PuiHostView. Every change goes through OnSettingsChanged (saved + applied live), as on
// Windows. Its placement is AppSettings::settingsWindow (points, see SettingsServiceMac.mm). Cmd+, / F5 / the main menu.
//
// Mac-side adjustments of the page: the Video GPU row is hidden and disabled; caption settings and log paths use
// Mac wording. The caption link opens System Settings > Accessibility > Captions. Subtitle font/colour buttons open
// asynchronous pui editors (no native dialogs), preserving opacity and saving only on Apply. The shared page owns
// subtitle style source, override switches, outline width, summaries and swatches.
#include <memory>
#include <string>
#include <vector>

namespace pui { class UiHost; class Widget; }

namespace mainview {

class MainView;

class SettingsWindow {
public:
    explicit SettingsWindow(MainView& mv);
    ~SettingsWindow();
    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;

    /// Shows the window (a fresh page over the current settings) or brings the open one to the front.
    void Open();
    /// Saves the placement and hides the window.
    void Close();
    bool IsOpen() const;
    /// The app theme / accent changed.
    void ApplyTheme();
    pui::UiHost& Ui();
    pui::Widget* Page() const;
    void* Window() const;   // NSWindow*

    /// URIs the page launched (after the Mac mapping), in order; the self-test sets suppressLaunch to only record them.
    std::vector<std::string> launched;
    bool suppressLaunch = false;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
