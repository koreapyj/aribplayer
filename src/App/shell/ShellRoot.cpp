#include "shell/ShellRoot.h"

#include "LayerRenderer.h"

#include <algorithm>

namespace app {

namespace {
class Sentinel : public pui::Widget {
public:
    Sentinel() { SetHitTestVisible(false); }
    void Paint(pui::IRenderer& r, const pui::Theme&) override {
        if (auto* s = dynamic_cast<ISuppressible*>(&r)) s->SetDrawEnabled(true);
    }
};

}

ShellRoot::ShellRoot(AppContext& ctx) : pui::Widget(pui::Role::Window) {
    SetName(L"ARIB Player");
    SetAutomationId(L"Shell");
    title_ = Add<TitleBar>();
    panel_ = Add<PlaylistPanel>(ctx);
    panel_->SetVisible(false);
    bar_ = Add<PlayerBar>();
    mini_ = Add<MiniBar>();
    mini_->SetVisible(false);
    sentinel_ = Add<Sentinel>();
}

void ShellRoot::ApplyLayout(const ShellLayout& l, ShellMode mode) {
    bool mini = mode == ShellMode::MiniPlayer;
    title_->SetFullscreenMode(mode == ShellMode::FullScreen);
    title_->SetVisible(!mini && !l.title.Empty());
    {   // playlist panel: docked beside the video in the windowed player view only
        const bool showPanel = mode == ShellMode::NowPlaying && !l.panel.Empty();
        const bool was = panel_->Visible();
        panel_->SetVisible(showPanel);
        if (!showPanel) panel_->SetActive(false);   // a hidden panel is no longer the key target
        if (showPanel) panel_->SetBounds(l.panel);
        if (showPanel && (!was || panel_->Stale())) panel_->Refresh();
    }
    bar_->SetVisible(!mini);
    mini_->SetVisible(mini);
    if (!l.title.Empty()) title_->SetBounds(l.title);
    if (mini) mini_->SetBounds(l.bar); else bar_->SetBounds(l.bar);
}

void ShellRoot::Paint(pui::IRenderer& r, const pui::Theme&) {
    if (popupOnly_)
        if (auto* s = dynamic_cast<ISuppressible*>(&r)) s->SetDrawEnabled(false);
}

}
