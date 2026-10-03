#include "LogicTest.h"
#include "features/LoggingFormat.h"

using namespace app;

namespace {
bool Has(const std::wstring& s, const std::wstring& part) { return s.find(part) != std::wstring::npos; }
}

LOGIC_TEST(effective_options_live_vf_replaces_open_vf_and_keeps_hwaccel) {
    wvp::OpenOptions open{{L"hwaccel", L"d3d11va"}, {L"vf", L"yadif"}, {L"af", L"atempo=1.5"}};
    wvp::OpenOptions eff = log::EffectiveOptions(open, L"yadif,eq=brightness=0.2", L"atempo=1.5");
    LT_CHECK_EQ(eff.size(), (size_t)3);
    LT_CHECK(eff[0] == std::make_pair(std::wstring(L"hwaccel"), std::wstring(L"d3d11va")));
    LT_CHECK(eff[1] == std::make_pair(std::wstring(L"vf"), std::wstring(L"yadif,eq=brightness=0.2")));
    std::wstring line = log::FormatOptionsLine(eff);
    LT_CHECK(Has(line, L"vf=yadif,eq=brightness=0.2"));
    std::wstring cmd = log::FfplayCommandLine(L"C:\a.mp4", eff);
    LT_CHECK(Has(cmd, L"-hwaccel d3d11va"));
    LT_CHECK(Has(cmd, L"-vf \"yadif,eq=brightness=0.2\""));
    LT_CHECK(Has(cmd, L"-af \"atempo=1.5\""));
    LT_CHECK(!Has(cmd, L"-vf yadif "));
}

LOGIC_TEST(effective_options_live_vf_added_removed) {
    wvp::OpenOptions open{{L"hwaccel", L"d3d11va"}};
    wvp::OpenOptions added = log::EffectiveOptions(open, L"hue=s=0", L"");
    LT_CHECK(Has(log::FfplayCommandLine(L"u", added), L"-vf \"hue=s=0\""));
    LT_CHECK(!Has(log::FfplayCommandLine(L"u", added), L"-af"));
    wvp::OpenOptions withVf{{L"vf", L"hue=s=0"}, {L"hwaccel", L"d3d11va"}};
    wvp::OpenOptions removed = log::EffectiveOptions(withVf, L"", L"");
    LT_CHECK_EQ(removed.size(), (size_t)1);
    LT_CHECK(!Has(log::FfplayCommandLine(L"u", removed), L"-vf"));
    LT_CHECK(Has(log::FfplayCommandLine(L"u", removed), L"-hwaccel d3d11va"));
}
