// Real caps fixture: testdata\caps-rtx5070.json (probe output for an RTX 5070 with its D3D11 presenter; the build's
// CUDA filters are listed, the player has no CUDA device).
// Found via __FILE__ (the repository path at build time) so the tests need no working-directory assumptions.
#include "wvptest.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include "wvp/DebugInfo.h"
#include "wvp/PipelinePlanner.h"

using namespace wvp;

namespace {

CapabilitySet RealCaps() {
    std::filesystem::path p = std::filesystem::path(__FILE__).parent_path().parent_path() / "testdata" / "caps-rtx5070.json";
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    auto c = ParseCapabilities(ss.str());
    if (!c) { CHECK_MSG(false, "cannot load " + p.string()); return fakecaps::Nvidia(); }
    return *c;
}

Plan MakePlan(const CapabilitySet& caps, const FilterSettings& s, ContentInfo c, std::set<std::string> black = {}) {
    PlanRequest r;
    r.blacklist = std::move(black);
    r.settings = s;
    r.content = std::move(c);
    return PipelinePlanner(caps).Make(r);
}

}

TEST_CASE(PresP2_real_caps_load_the_platform_presenter) {
    CapabilitySet c = RealCaps();
    REQUIRE(c.presenters.size() == 1);
    const PresenterCap* d = c.Presenter("d3d11");
    REQUIRE(d != nullptr);
    CHECK(d->ok);
    CHECK(d->shader);
    CHECK(d->inputs == (std::vector<FrameDomain>{ FrameDomain::D3D11, FrameDomain::System }));
    CHECK(!c.DeviceOk("cuda"));
    Plan p = MakePlan(c, FilterSettings{}, { "hevc", 10, 1920, 1080, false });
    REQUIRE(p.ok);
    CHECK_EQ(p.steps.back().name, std::string("present:d3d11"));
    CHECK_EQ(p.cost.hostCopies, 0);
    CHECK_EQ(p.cost.deviceCopies, 0);
}

TEST_CASE(PresP2_debug_info_parses_the_presenter_object) {
    DebugInfo d = ParseDebugInfo(R"({"presenter":{"name":"d3d11","error":"","frames_direct":119,"frames_uploaded":3,
        "stats":{"frames_shader":42,"frames_d3d11_copied":7,"route":"vp"}}})");
    REQUIRE(d.Valid);
    CHECK(d.Presenter.Present);
    CHECK_EQ(d.Presenter.Name, std::string("d3d11"));
    CHECK_EQ(d.Presenter.Error, std::string(""));
    CHECK(d.Presenter.FramesDirect.has_value() && *d.Presenter.FramesDirect == 119);
    CHECK(d.Presenter.FramesUploaded.has_value() && *d.Presenter.FramesUploaded == 3);
    CHECK_EQ(d.Presenter.FramesShader.value_or(-1), 42LL);
    CHECK_EQ(d.Presenter.FramesD3d11Copied.value_or(-1), 7LL);
    CHECK_EQ(d.Presenter.Route, std::string("vp"));
    DebugInfo e = ParseDebugInfo(R"({"presenter":{"name":"d3d11","error":"forced failure","stats":{}}})");
    CHECK(e.Presenter.Present);
    CHECK_EQ(e.Presenter.Error, std::string("forced failure"));
    CHECK(!e.Presenter.FramesDirect.has_value());
    CHECK(!e.Presenter.FramesShader.has_value() && !e.Presenter.FramesD3d11Copied.has_value());
    DebugInfo old = ParseDebugInfo(R"({"paused":false})");
    CHECK(old.Valid);
    CHECK(!old.Presenter.Present);
    CHECK(old.Presenter.Name.empty() && !old.Presenter.FramesUploaded.has_value());
}
