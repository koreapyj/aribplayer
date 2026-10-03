#pragma once
// Root widget "Shell": TitleBar, PlaylistPanel, PlayerBar (children in tree order: title -> panel -> bar; the player view has no keyboard focus)
// plus a paint sentinel. The shell paints layers separately (see App layer split):
//   title layer = TitleBar,  bar layer = PlayerBar,
//   popup layer   = UiHost::Paint with SetPopupOnly(true): the root and its children are painted with drawing suppressed
//                   (ISuppressible), the sentinel re-enables it, so only popups + tooltip reach the surface.
#include "ShellLogic.h"
#include "features/MiniBar.h"
#include "pui/Widget.h"
#include "shell/PlayerBar.h"
#include "shell/PlaylistPanel.h"
#include "shell/TitleBar.h"

namespace app {

class ShellRoot : public pui::Widget {
public:
    explicit ShellRoot(AppContext& ctx);
    TitleBar* Title() const { return title_; }
    PlaylistPanel* Panel() const { return panel_; }
    PlayerBar* Bar() const { return bar_; }
    MiniBar* Mini() const { return mini_; }

    void SetPopupOnly(bool on) { popupOnly_ = on; }
    /// Positions the children for a mode (title/bar rects from ShellLayout, DIPs) and shows/hides them.
    void ApplyLayout(const ShellLayout& l, ShellMode mode);
    /// Detached UI popup, painted above the video, title and controls by UiHost's popup layer.
    void Paint(pui::IRenderer& r, const pui::Theme& t) override;

private:
    TitleBar* title_;
    PlaylistPanel* panel_;
    PlayerBar* bar_;
    MiniBar* mini_;
    pui::Widget* sentinel_;
    bool popupOnly_ = false;
};

}
