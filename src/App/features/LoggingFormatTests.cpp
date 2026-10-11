#include "LogicTest.h"
#include "features/LoggingFormat.h"

using namespace app;

namespace {
bool Has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }
}

LOGIC_TEST(effective_options_live_vf_replaces_open_vf_and_keeps_hwaccel) {
    wvp::OpenOptions open{{"hwaccel", "d3d11va"}, {"vf", "yadif"}, {"af", "atempo=1.5"}};
    wvp::OpenOptions eff = log::EffectiveOptions(open, "yadif,eq=brightness=0.2", "atempo=1.5");
    LT_CHECK_EQ(eff.size(), (size_t)3);
    LT_CHECK(eff[0] == std::make_pair(std::string("hwaccel"), std::string("d3d11va")));
    LT_CHECK(eff[1] == std::make_pair(std::string("vf"), std::string("yadif,eq=brightness=0.2")));
    std::string line = log::FormatOptionsLine(eff);
    LT_CHECK(Has(line, "vf=yadif,eq=brightness=0.2"));
}

LOGIC_TEST(effective_options_live_vf_added_removed) {
    wvp::OpenOptions open{{"hwaccel", "d3d11va"}};
    wvp::OpenOptions added = log::EffectiveOptions(open, "hue=s=0", "");
    LT_CHECK(added == (wvp::OpenOptions{{"hwaccel", "d3d11va"}, {"vf", "hue=s=0"}}));
    wvp::OpenOptions withVf{{"vf", "hue=s=0"}, {"hwaccel", "d3d11va"}};
    wvp::OpenOptions removed = log::EffectiveOptions(withVf, "", "");
    LT_CHECK_EQ(removed.size(), (size_t)1);
    LT_CHECK(removed[0] == std::make_pair(std::string("hwaccel"), std::string("d3d11va")));
}
