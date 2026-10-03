#include "LayerOrder.h"
#include "features/AcrylicLogic.h"
#include "LogicTest.h"
#include "ShellLogic.h"

using namespace app;

LOGIC_TEST(layer_stack_popup_is_topmost) {
    LT_CHECK_EQ(LayerZ(Layer::Popup), (int)Layer::Count - 1);
    for (Layer l : kLayerStack)
        if (l != Layer::Popup) LT_CHECK(LayerZ(l) < LayerZ(Layer::Popup));
}

LOGIC_TEST(layer_stack_relative_order) {
    LT_CHECK(LayerZ(Layer::DebugOverlay) > LayerZ(Layer::Content));
    LT_CHECK(LayerZ(Layer::DebugOverlay) < LayerZ(Layer::Title));
    LT_CHECK(LayerZ(Layer::DebugOverlay) < LayerZ(Layer::Bar));
    LT_CHECK(LayerZ(Layer::Title) < LayerZ(Layer::Popup));
    LT_CHECK(LayerZ(Layer::Bar) < LayerZ(Layer::Popup));
    for (int i = 0; i < (int)Layer::Count; i++) {
        LT_CHECK(LayerZ((Layer)i) >= 0);
        for (int j = i + 1; j < (int)Layer::Count; j++) LT_CHECK(LayerZ((Layer)i) != LayerZ((Layer)j));
    }
}

LOGIC_TEST(acrylic_popup_over_visible_chrome_falls_back) {
    const pui::RectF bar{0, 634, 1280, 720}, title{0, 0, 1280, kStripDip};
    // a menu over the visible bar falls back to solid; over a hidden bar it is fine
    const pui::RectF menu{900, 500, 1100, 680};
    LT_CHECK(app::acrylic::OverlapsVisibleChrome(menu, bar, true, title, true));
    LT_CHECK(!app::acrylic::OverlapsVisibleChrome(menu, bar, false, title, true));
    // a menu reaching into the always-visible title strip falls back too
    LT_CHECK(app::acrylic::OverlapsVisibleChrome(pui::RectF{900, 10, 1100, 300}, bar, false, title, true));
    // the full-screen title strip is sliding chrome: Acrylic may overlap it only after the strip is fully hidden
    const pui::RectF fullscreenTitle{0, 0, 1920, kStripDip};
    const pui::RectF fullscreenTopMenu{900, 8, 1100, 200};
    LT_CHECK(app::acrylic::OverlapsVisibleChrome(fullscreenTopMenu, pui::RectF{}, false, fullscreenTitle, true));
    LT_CHECK(!app::acrylic::OverlapsVisibleChrome(fullscreenTopMenu, pui::RectF{}, false, fullscreenTitle, false));
}
