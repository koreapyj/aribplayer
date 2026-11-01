#pragma once
// The Properties window: a HostWindow (see HostWindow.h) showing the current media's properties (features/PropertiesDialog).
// Single instance; showing it again while open refreshes the content for the current media and brings it to the front.
// Size and position are not remembered: it opens centred over the player.
#include <vector>

#include "features/FxProps.h"
#include "shell/HostWindow.h"

namespace app {

class PropertiesWindow final : public HostWindow {
public:
    static constexpr float kDefaultWidthDip = 560, kDefaultHeightDip = 560, kMinWidthDip = 320, kMinHeightDip = 240;

    PropertiesWindow(AppContext& ctx, Gfx& gfx, IDWriteFactory* dw);
    ~PropertiesWindow() override;

    /// hidden (--headless-check) / attachDevice: as SettingsWindow::Open.
    bool Show(std::vector<fx::PropSection> sections, bool hidden, bool attachDevice);
    pui::Widget* Page() const { return page_; }

private:
    void BeforeDestroy() override { page_ = nullptr; }
    void SetContent(std::vector<fx::PropSection> sections);

    pui::Widget* page_ = nullptr;
};

}
