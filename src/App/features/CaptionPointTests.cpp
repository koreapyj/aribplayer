// Headless: no window.
#include <cmath>

#include "LogicTest.h"
#include "features/CaptionPoint.h"

namespace app {

LOGIC_TEST(caption_point_letterbox_and_zoom_fill) {
    auto near = [](double a, double b) { return std::fabs(a - b) < 1e-9; };
    // 4:3 picture in a 1600x900 area: pillarbox bars of 200 px each side
    auto c = CaptionQueryPoint(800, 450, 1600, 900, 4.0 / 3.0, false);
    LT_CHECK(c && near(c->x, 0.5) && near(c->y, 0.5));
    LT_CHECK(!CaptionQueryPoint(150, 450, 1600, 900, 4.0 / 3.0, false));
    auto e = CaptionQueryPoint(200, 0, 1600, 900, 4.0 / 3.0, false);
    LT_CHECK(e && near(e->x, 0) && near(e->y, 0));
    // 2.4:1 picture in a 1600x900 area: letterbox bars above and below
    LT_CHECK(!CaptionQueryPoint(800, 100, 1600, 900, 2.4, false));
    auto l = CaptionQueryPoint(1200, 450, 1600, 900, 2.4, false);
    LT_CHECK(l && near(l->x, 0.75) && near(l->y, 0.5));
    // zoom to fill: the picture is the whole area
    auto z = CaptionQueryPoint(150, 100, 1600, 900, 4.0 / 3.0, true);
    LT_CHECK(z && near(z->x, 150.0 / 1600) && near(z->y, 100.0 / 900));
    LT_CHECK(!CaptionQueryPoint(1600, 10, 1600, 900, 0, true));
    LT_CHECK(!CaptionQueryPoint(10, 10, 0, 900, 1.5, false));
}

}
