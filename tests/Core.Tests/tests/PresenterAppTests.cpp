// Real caps fixture: testdata\caps-rtx5070.json (ffp_probe_caps output for an RTX 5070).
// D3D11 presenter VP mirror=false, procamp brightness +-34.016, contrast 0.4..1.6, saturation 0.13..1.87.
// Found via __FILE__ (the repository path at build time) so the tests need no working-directory assumptions.
#include "wvptest.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "wvp/DebugInfo.h"
#include "wvp/FilterManager.h"
#include "wvp/PipelinePlanner.h"
#include "wvp/PipelineSession.h"
#include "wvp/PlaybackController.h"

using namespace wvp;

namespace {

class Fake : public IMediaPlayer {
public:
    struct Opened { std::wstring url; OpenOptions options; };
    std::vector<Opened> opened;
    std::vector<std::wstring> vf;
    std::vector<PresenterParams> presenterCalls;
    int presenterResult = 0;           // returned for non-identity params
    PlayerState state = PlayerState::Idle;

    PlayerState State() const override { return state; }
    int Volume() const override { return 100; }
    void SetVolume(int) override {}
    bool IsMuted() const override { return false; }
    void SetMuted(bool) override {}
    double Position() override { return 0; }
    double Duration() override { return 100; }
    int Open(const std::wstring& url, const OpenOptions& o) override { opened.push_back({ url, o }); state = PlayerState::Playing; return 0; }
    int Stop() override { state = PlayerState::Stopped; return 0; }
    int Play() override { return 0; }
    int Pause() override { return 0; }
    int TogglePause() override { return 0; }
    int Seek(double, bool) override { return 0; }
    int CycleStream(StreamType) override { return 0; }
    int SetVideoFilter(const std::wstring& v) override { vf.push_back(v); return 0; }
    int SetAudioFilter(const std::wstring&) override { return 0; }
    int SendFilterCommand(const std::wstring&, const std::wstring&, const std::wstring&) override { return 0; }
    std::optional<MediaInfo> GetMediaInfo() override { return std::nullopt; }
    std::optional<BgraFrame> Snapshot() override { return std::nullopt; }
    int SetPresenterParams(const PresenterParams& p) override {
        presenterCalls.push_back(p);
        return p.IsIdentity() ? 0 : presenterResult;
    }
};

std::wstring Opt(const OpenOptions& o, const std::wstring& k, bool* found = nullptr) {
    for (auto& e : o) if (e.first == k) { if (found) *found = true; return e.second; }
    if (found) *found = false;
    return L"";
}

CapabilitySet RealCaps() {
    std::filesystem::path p = std::filesystem::path(__FILE__).parent_path().parent_path() / "testdata" / "caps-rtx5070.json";
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    auto c = ParseCapabilities(ss.str());
    if (!c) { CHECK_MSG(false, "cannot load " + p.string()); return fakecaps::Nvidia(); }
    return *c;
}

ContentInfo Hevc10() { return { "hevc", 10, 1920, 1080, false }; }
ContentInfo H264() { return { "h264", 8, 1920, 1080, false }; }

Plan MakePlan(const CapabilitySet& caps, const FilterSettings& s, ContentInfo c = Hevc10(), std::set<std::string> black = {}) {
    PlanRequest r;
    r.settings = s;
    r.content = std::move(c);
    r.blacklist = std::move(black);
    return PipelinePlanner(caps).Make(r);
}

bool Contains(const std::wstring& s, const std::wstring& sub) { return s.find(sub) != std::wstring::npos; }

}

TEST_CASE(PresP2_open_options_carry_presenter_params_when_not_identity) {
    FilterSettings s;
    s.rotation = Rotation::Cw90;
    s.brightness = 0.1; s.contrast = 1.2;
    Plan p = MakePlan(fakecaps::Nvidia(), s);
    REQUIRE(p.ok);
    CHECK_EQ(p.presenter, std::string("d3d11"));
    CHECK(!p.presenterParams.IsIdentity());
    bool found = false;
    OpenOptions o = PipelinePlanner::OpenOptionsFor(p, PlanPolicy::Cost);
    std::wstring j = Opt(o, L"presenter_params", &found);
    CHECK(found);
    CHECK_EQ(j, std::wstring(L"{\"rotation\":90,\"flip_h\":false,\"flip_v\":false,\"eq\":{\"brightness\":0.1,\"contrast\":1.2,\"saturation\":1,\"gamma\":1}}"));
    size_t ip = 0, ipp = 0;
    for (size_t i = 0; i < o.size(); i++) { if (o[i].first == L"presenter") ip = i; if (o[i].first == L"presenter_params") ipp = i; }
    CHECK(ip < ipp);
    Plan id = MakePlan(fakecaps::Nvidia(), FilterSettings{});
    Opt(PipelinePlanner::OpenOptionsFor(id, PlanPolicy::Cost), L"presenter_params", &found);
    CHECK(!found);
    Opt(PipelinePlanner::OpenOptionsFor(p, PlanPolicy::Legacy), L"presenter_params", &found);
    CHECK(!found);
    // sdl presenter: never (params would be refused)
    PlanRequest r;
    r.settings = s;
    r.content = Hevc10();
    r.presenter = PresenterPref::Sdl;
    Plan sdl = PipelinePlanner(fakecaps::Nvidia()).Make(r);
    CHECK(sdl.presenterParams.IsIdentity());
    Opt(PipelinePlanner::OpenOptionsFor(sdl, PlanPolicy::Cost), L"presenter_params", &found);
    CHECK(!found);
}

TEST_CASE(PresP2_params_json_flips_and_eq_null) {
    PresenterParams p;
    p.rotation = 270; p.flipH = true;
    CHECK_EQ(PresenterParamsJson(p), std::wstring(L"{\"rotation\":270,\"flip_h\":true,\"flip_v\":false,\"eq\":null}"));
    CHECK_EQ(PresenterParamsJson(PresenterParams{}), std::wstring(L"{\"rotation\":0,\"flip_h\":false,\"flip_v\":false,\"eq\":null}"));
}

TEST_CASE(PresP3_fake_cuda_import_zero_copy_vs_per_plane) {
    FilterSettings s;
    s.rotation = Rotation::Cw90;
    s.flipHorizontal = true;
    s.gamma = 1.5;
    auto caps = fakecaps::Nvidia(true, true, true);
    auto& d3 = caps.presenters.back();
    d3.shader = false; d3.vpMirror = false; d3.vpProcamp = false;
    Plan z = MakePlan(caps, s);
    REQUIRE(z.ok);
    CHECK_EQ(z.hwaccel, std::string("cuda"));
    CHECK_EQ(z.cost.hostCopies, 0);
    CHECK_EQ(z.cost.deviceCopies, 0);
    CHECK(z.videoFilter.empty());
    CHECK(z.presenterParams.flipH && z.presenterParams.eq && z.presenterParams.rotation == 90);
    caps.presenters.back().importCudaZeroCopy = false;
    Plan c = MakePlan(caps, s);
    REQUIRE(c.ok);
    CHECK_EQ(c.hwaccel, std::string("cuda"));
    CHECK_EQ(c.cost.hostCopies, 0);
    CHECK_EQ(c.cost.deviceCopies, 1);
    CHECK(c.videoFilter.empty());
    CHECK_EQ(c.steps.back().reason, std::string("per-plane import"));
}

TEST_CASE(PresP2_real_caps_load_and_have_the_expected_limits) {
    CapabilitySet c = RealCaps();
    const PresenterCap* d = c.Presenter("d3d11");
    REQUIRE(d != nullptr);
    CHECK(d->ok);
    CHECK(d->vpRotate);
    CHECK(!d->vpMirror);
    CHECK(d->vpProcamp);
    CHECK(d->shader);
    CHECK(d->vpBrightnessMax > 34.0 && d->vpBrightnessMax < 34.1);
}

TEST_CASE(PresP2_real_caps_rotation_and_eq_are_absorbed_within_the_ranges) {
    CapabilitySet c = RealCaps();
    FilterSettings s;
    s.rotation = Rotation::Cw90;
    s.brightness = 0.1; s.contrast = 1.2;
    Plan p = MakePlan(c, s);
    REQUIRE(p.ok);
    CHECK_EQ(p.presenter, std::string("d3d11"));
    CHECK_EQ(p.presenterParams.rotation, 90);
    CHECK(p.presenterParams.eq);
    CHECK(p.videoFilter.empty());
    CHECK_EQ(p.cost.hostCopies, 0);
    CHECK_EQ(p.cost.deviceCopies, 0);
}

TEST_CASE(PresP2_real_caps_flips_use_shader_even_when_vp_mirror_is_false) {
    CapabilitySet c = RealCaps();
    for (auto& p : c.presenters) if (p.name == "d3d11") p.importCuda = false;
    FilterSettings s;
    s.flipHorizontal = true;
    Plan p = MakePlan(c, s);
    REQUIRE(p.ok);
    CHECK(p.presenterParams.flipH);
    CHECK(p.videoFilter.empty());
    FilterSettings s2;
    s2.flipVertical = true; s2.rotation = Rotation::Cw90;
    Plan q = MakePlan(c, s2);
    REQUIRE(q.ok);
    CHECK(q.presenterParams.flipV);
    CHECK(q.videoFilter.empty());
    CHECK_EQ(q.presenterParams.rotation, 90);
}

TEST_CASE(PresP2_real_caps_eq_outside_vp_ranges_is_absorbed_by_shader) {
    CapabilitySet c = RealCaps();
    for (auto& p : c.presenters) if (p.name == "d3d11") p.importCuda = false; // system/D3D11 shader, not CUDA
    auto eqIn = [&](double b, double ct, double sat, double g) {
        FilterSettings s;
        s.brightness = b; s.contrast = ct; s.saturation = sat; s.gamma = g;
        Plan p = MakePlan(c, s);
        CHECK(p.ok);
        return std::make_pair(p.presenterParams.eq, Contains(p.videoFilter, L"eq@color"));
    };
    CHECK(eqIn(0.1, 1.2, 1.0, 1.0) == std::make_pair(true, false));
    CHECK(eqIn(0.34, 1.6, 1.87, 1.0) == std::make_pair(true, false));
    CHECK(eqIn(0.35, 1.0, 1.0, 1.0) == std::make_pair(true, false));      // brightness above 34.016/100
    CHECK(eqIn(0.0, 1.7, 1.0, 1.0) == std::make_pair(true, false));       // contrast above 1.6
    CHECK(eqIn(0.0, 0.3, 1.0, 1.0) == std::make_pair(true, false));       // contrast below 0.4
    CHECK(eqIn(0.0, 1.0, 1.9, 1.0) == std::make_pair(true, false));       // saturation above 1.87
    CHECK(eqIn(0.0, 1.0, 0.1, 1.0) == std::make_pair(true, false));       // saturation below 0.13
    CHECK(eqIn(0.1, 1.0, 1.0, 1.5) == std::make_pair(true, false));       // gamma != 1: no eq absorption without a shader
}

TEST_CASE(PresP2_absorb_blacklist_keys_move_the_operation_into_the_graph) {
    FilterSettings s;
    s.rotation = Rotation::Cw90;
    s.brightness = 0.1;
    CapabilitySet nv = fakecaps::Nvidia();
    Plan base = MakePlan(nv, s);
    CHECK(base.presenterParams.eq && base.presenterParams.rotation == 90 && base.videoFilter.empty());
    Plan e = MakePlan(nv, s, Hevc10(), { "absorb:eq" });
    CHECK(!e.presenterParams.eq);
    CHECK_EQ(e.presenterParams.rotation, 90);
    CHECK(Contains(e.videoFilter, L"eq@color"));
    Plan r = MakePlan(nv, s, Hevc10(), { "absorb:rotate" });
    CHECK(r.presenterParams.eq);
    CHECK_EQ(r.presenterParams.rotation, 0);
    CHECK(Contains(r.videoFilter, L"transpose"));
    FilterSettings f;
    f.flipHorizontal = true;
    CHECK(MakePlan(nv, f).presenterParams.flipH);
    Plan fl = MakePlan(nv, f, Hevc10(), { "absorb:flip" });
    CHECK(!fl.presenterParams.flipH);
    CHECK(Contains(fl.videoFilter, L"hflip"));
    // the structure differs, so Diff says the graph changes (decoder download or rebuild), not a presenter param update
    CHECK(PipelinePlanner::Diff(base, e).kind >= PlanDeltaKind::GraphRebuild);
}

namespace {
struct Rig {
    Fake player;
    FilterManager fm;
    std::unique_ptr<PlaybackController> ctl;
    Rig() {
        fm.SetContent(H264());
        fm.SetCapabilities(fakecaps::Nvidia());
        ctl = std::make_unique<PlaybackController>(player);
        ctl->SetFilters(fm.Provider());
        ctl->onPresenterParamsRejected = [this](int, const PresenterParams& rejected) {
            ctl->ApplyFilterChange(fm.PresenterParamsRejected(rejected));
        };
        ctl->SetQueue({ MediaItem{ L"clip.mp4", L"clip" } }, 0, true);
    }
};
}

TEST_CASE(PresP2_presenter_param_change_calls_the_player_not_the_graph) {
    Rig rig;
    REQUIRE(rig.fm.PlannerActive());
    size_t opens = rig.player.opened.size();
    // a plan-mode open without vf clears the instance's stale vf once (SetVideoFilter("")): only count later calls
    const size_t vfCalls = rig.player.vf.size();
    FilterSettings s = rig.fm.Settings();
    s.rotation = Rotation::Cw90;
    s.brightness = 0.1;
    FilterChange c = rig.fm.Apply(s);
    CHECK(c.presenterParamsChanged && !c.reopen && !c.videoRebuild);
    CHECK_EQ(c.presenterParams.rotation, 90);
    rig.ctl->ApplyFilterChange(c);
    REQUIRE(rig.player.presenterCalls.size() == 1);
    CHECK_EQ(rig.player.presenterCalls[0].rotation, 90);
    CHECK(rig.player.presenterCalls[0].eq);
    CHECK_EQ(rig.player.vf.size(), vfCalls);
    CHECK_EQ(rig.player.opened.size(), opens);
    s.brightness = 0.2;
    rig.ctl->ApplyFilterChange(rig.fm.Apply(s));
    REQUIRE(rig.player.presenterCalls.size() == 2);
    CHECK_EQ(rig.player.presenterCalls[1].brightness, 0.2);
    CHECK_EQ(rig.player.vf.size(), vfCalls);
}

TEST_CASE(PresP2_player_refuses_params_absorb_is_blacklisted_and_replanned) {
    Rig rig;
    rig.player.presenterResult = -22;
    FilterSettings s = rig.fm.Settings();
    s.brightness = 0.1;
    FilterChange c = rig.fm.Apply(s);
    CHECK(c.presenterParamsChanged);
    rig.ctl->ApplyFilterChange(c);
    REQUIRE(rig.player.presenterCalls.size() >= 1);
    CHECK(rig.player.presenterCalls[0].eq);
    const Plan* pl = rig.fm.CurrentPlan();
    REQUIRE(pl != nullptr);
    CHECK(!pl->presenterParams.eq);
    CHECK(Contains(pl->videoFilter, L"eq@color"));
    bool reopened = false;
    if (rig.player.opened.size() > 1) reopened = Contains(Opt(rig.player.opened.back().options, L"vf"), L"eq@color");
    CHECK(reopened || (!rig.player.vf.empty() && Contains(rig.player.vf.back(), L"eq@color")));
    CHECK(rig.player.presenterCalls.size() <= 3);   // nothing loops
    FilterSettings s2 = rig.fm.Settings();
    s2.rotation = Rotation::Cw90;
    rig.ctl->ApplyFilterChange(rig.fm.Apply(s2));
    CHECK(Contains(rig.fm.CurrentPlan()->videoFilter, L"transpose"));
}

TEST_CASE(PresP2_rejected_without_anything_left_to_exclude_is_a_no_op) {
    FilterManager fm;
    fm.SetContent(H264());
    fm.SetCapabilities(fakecaps::Nvidia());
    FilterChange c = fm.PresenterParamsRejected(PresenterParams{});
    CHECK(!c.videoRebuild && !c.reopen && !c.presenterParamsChanged && c.commands.empty());
    PresenterParams eq; eq.eq = true;
    fm.PresenterParamsRejected(eq);
    FilterChange again = fm.PresenterParamsRejected(eq);
    CHECK(!again.videoRebuild && !again.reopen);
}

TEST_CASE(PresP2_params_with_nothing_open_ride_on_the_next_open) {
    Fake player;
    PlaybackController ctl(player);
    FilterChange c;
    c.presenterParamsChanged = true;
    c.presenterParams.rotation = 90;
    ctl.ApplyFilterChange(c);
    CHECK(player.presenterCalls.empty());          // idle: non-identity is not sent (the SDL presenter would refuse it)
    c.presenterParams = PresenterParams{};
    ctl.ApplyFilterChange(c);
    CHECK_EQ(player.presenterCalls.size(), (size_t)1);   // identity clears stale params
}

TEST_CASE(PresP2_presenter_error_falls_back_once_per_generation) {
    PipelineSession ps;
    PlanRequest r;
    r.content = H264();
    Plan p = PipelinePlanner(fakecaps::Nvidia()).Make(r);
    ps.BeforeOpen(L"a.mp4");
    PresenterErrorInfo e;
    e.Presenter = L"d3d11"; e.Error = L"Present failed 0x887A0005"; e.Fallback = L"sdl";
    auto fb = ps.OnPresenterError(p, e);
    REQUIRE(fb.has_value());
    CHECK_EQ(fb->key, std::string("presenter:d3d11"));
    CHECK_EQ(fb->reason, std::string("Present failed 0x887A0005"));
    CHECK(!ps.OnPresenterError(p, e).has_value());        // once
    CHECK(ps.Blacklist().count("presenter:d3d11") == 1);
    ps.BeforeOpen(L"a.mp4");
    CHECK(!ps.OnPresenterError(p, e).has_value());        // known key cannot be the cause again
    FilterSettings s;
    s.rotation = Rotation::Cw90;
    Plan q = MakePlan(fakecaps::Nvidia(), s, H264(), ps.Blacklist());
    CHECK_EQ(q.presenter, std::string("sdl"));
    CHECK(q.presenterParams.IsIdentity());
    CHECK(Contains(q.videoFilter, L"transpose"));
    // an empty presenter name is read as d3d11
    PipelineSession ps2;
    ps2.BeforeOpen(L"b.mp4");
    auto fb2 = ps2.OnPresenterError(p, PresenterErrorInfo{});
    REQUIRE(fb2.has_value());
    CHECK_EQ(fb2->key, std::string("presenter:d3d11"));
}

TEST_CASE(PresP2_debug_info_parses_the_presenter_object) {
    DebugInfo d = ParseDebugInfo(R"({"renderer":"d3d11:videoprocessor","presenter":{"name":"d3d11","requested":"d3d11",
        "params":{"rotation":90,"flip_h":false,"flip_v":false,"eq":{"brightness":0.1,"contrast":1.2,"saturation":1,"gamma":1}},
        "error":"","frames_direct":119,"frames_uploaded":3,"frames_cuda_zero_copy":42,"frames_cuda_copied":7}})");
    REQUIRE(d.Valid);
    CHECK(d.Presenter.Present);
    CHECK_EQ(d.Presenter.Name, std::string("d3d11"));
    CHECK_EQ(d.Presenter.Requested, std::string("d3d11"));
    CHECK_EQ(d.Presenter.Error, std::string(""));
    CHECK(d.Presenter.FramesDirect.has_value() && *d.Presenter.FramesDirect == 119);
    CHECK(d.Presenter.FramesUploaded.has_value() && *d.Presenter.FramesUploaded == 3);
    CHECK_EQ(d.Presenter.FramesCudaZeroCopy.value_or(-1), 42LL);
    CHECK_EQ(d.Presenter.FramesCudaCopied.value_or(-1), 7LL);
    CHECK(d.Presenter.Params.find("\"rotation\":90") != std::string::npos);
    CHECK(d.Presenter.Params.find("\"eq\":{") != std::string::npos);
    CHECK(d.Presenter.Params.find("\"brightness\":0.1") != std::string::npos);
    DebugInfo e = ParseDebugInfo(R"({"presenter":{"name":"sdl","requested":"d3d11","params":{},"error":"forced failure"}})");
    CHECK(e.Presenter.Present);
    CHECK_EQ(e.Presenter.Name, std::string("sdl"));
    CHECK_EQ(e.Presenter.Error, std::string("forced failure"));
    CHECK_EQ(e.Presenter.Params, std::string("{}"));
    CHECK(!e.Presenter.FramesDirect.has_value());
    CHECK(!e.Presenter.FramesCudaZeroCopy.has_value() && !e.Presenter.FramesCudaCopied.has_value());
    DebugInfo old = ParseDebugInfo(R"({"renderer":"sdl:direct3d11","paused":false})");
    CHECK(old.Valid);
    CHECK(!old.Presenter.Present);
    CHECK(old.Presenter.Name.empty() && old.Presenter.Params.empty() && !old.Presenter.FramesUploaded.has_value());
}
