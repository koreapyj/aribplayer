#pragma once
// Title strip (kStripDip = 44, full client width): [Menu - hamburger] file name ... [Pin] [Min]
// [Max / Restore] [Close]. Full screen drops Pin and Max and shows "exit full screen" in the Max slot. The strip draws
// its own caption buttons (the window has no WS_CAPTION): the maximize button is HTMAXBUTTON for the snap-layouts flyout (its
// hover / press arrive through the WM_NC* messages, see App::Proc), the rest of the strip is the drag area (HTCAPTION).
// TitleLayout (ShellLogic.h) is the single source of the rects, shared with App::NcHitTest.
#include <functional>
#include <string>
#include <utility>

#include "ShellLogic.h"
#include "pui/Widget.h"
#include "shell/ShellControls.h"
#include "wvp/AppSettings.h"

namespace app {

class TitleBar : public pui::Widget {
public:
    TitleBar();

    GlyphButton* MainMenuButton() const { return menu_; }
    GlyphButton* PinButton() const { return pin_; }
    GlyphButton* MinimizeButton() const { return min_; }
    GlyphButton* MaximizeButton() const { return max_; }
    GlyphButton* ExitFullscreenButton() const { return exitFs_; }
    GlyphButton* CloseButton() const { return close_; }

    /// File name shown in the player views (empty = nothing open: no name is drawn).
    void SetTitle(std::wstring title);
    const std::wstring& Title() const { return title_; }
    void SetFullscreenMode(bool fullscreen);
    bool FullscreenMode() const { return fullscreen_; }
    /// The window is maximized: the Max button shows Restore.
    void SetMaximized(bool maximized);
    /// The persisted always-on-top mode: Pin / Pinned glyph and the tooltip.
    void SetTopmostMode(wvp::AlwaysOnTop mode);
    void SetScale(float scale);

    std::function<void()> onMainMenu, onPin, onMinimize, onMaximize, onExitFullscreen, onClose;

    TitleLayout Layout() const;
    TitleZone Zone(pui::PointF p) const { return Layout().Classify(p); }
    /// Tooltip of the Pin button for a mode ("Cycle always on top (Always on top)" ...).
    static std::wstring PinToolTip(wvp::AlwaysOnTop mode);

    void OnLayout() override;
    void Paint(pui::IRenderer& r, const pui::Theme& t) override;
    pui::Widget* HitTest(pui::PointF p) override;

private:
    GlyphButton* menu_ = nullptr;
    GlyphButton* pin_ = nullptr;
    GlyphButton* min_ = nullptr;
    GlyphButton* max_ = nullptr;
    GlyphButton* exitFs_ = nullptr;
    GlyphButton* close_ = nullptr;
    std::wstring title_;
    float scale_ = 1.f;
    bool fullscreen_ = false, maximized_ = false;
    wvp::AlwaysOnTop topmost_ = wvp::AlwaysOnTop::Disable;
};

}
