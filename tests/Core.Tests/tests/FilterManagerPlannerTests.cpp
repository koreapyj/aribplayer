#include "wvptest.h"

#include <string>
#include <vector>

#include "wvp/FilterManager.h"
#include "wvp/PlaybackController.h"

using namespace wvp;

namespace {

class Fake : public IMediaPlayer {
public:
    struct Opened { std::wstring url; OpenOptions options; };
    std::vector<Opened> opened;
    std::vector<std::wstring> vf, af;
    std::vector<std::vector<std::wstring>> commands;
    PlayerState state = PlayerState::Idle;
    double pos = 0;
    int volume = 100;
    bool muted = false;

    PlayerState State() const override { return state; }
    int Volume() const override { return volume; }
    void SetVolume(int v) override { volume = v; }
    bool IsMuted() const override { return muted; }
    void SetMuted(bool m) override { muted = m; }
    double Position() override { return pos; }
    double Duration() override { return 100; }
    int Open(const std::wstring& url, const OpenOptions& o) override { opened.push_back({ url, o }); state = PlayerState::Playing; return 0; }
    int Stop() override { state = PlayerState::Stopped; return 0; }
    int Play() override { state = PlayerState::Playing; return 0; }
    int Pause() override { state = PlayerState::Paused; return 0; }
    int TogglePause() override { return 0; }
    int Seek(double, bool) override { return 0; }
    int CycleStream(StreamType) override { return 0; }
    int SetVideoFilter(const std::wstring& v) override { vf.push_back(v); return 0; }
    int SetAudioFilter(const std::wstring& a) override { af.push_back(a); return 0; }
    int SendFilterCommand(const std::wstring& t, const std::wstring& c, const std::wstring& a) override { commands.push_back({ t, c, a }); return 0; }
    std::optional<MediaInfo> GetMediaInfo() override { return std::nullopt; }
    std::optional<BgraFrame> Snapshot() override { return std::nullopt; }
};

std::wstring Opt(const OpenOptions& o, const std::wstring& k, bool* found = nullptr) {
    for (auto& e : o) if (e.first == k) { if (found) *found = true; return e.second; }
    if (found) *found = false;
    return L"";
}
int Count(const OpenOptions& o, const std::wstring& k) {
    int n = 0;
    for (auto& e : o) if (e.first == k) n++;
    return n;
}

ContentInfo Mpeg2() { return { "mpeg2video", 8, 1920, 1080, false }; }
ContentInfo Hevc10() { return { "hevc", 10, 1920, 1080, false }; }
ContentInfo H264() { return { "h264", 8, 1920, 1080, false }; }

}

TEST_CASE(FMPlanner_legacy_unchanged_without_caps) {
    FilterManager fm;
    CHECK(!fm.PlannerActive());
    CHECK(fm.CurrentPlan() == nullptr);
    FilterSettings s;
    s.deinterlace = DeinterlaceMode::On;
    s.deinterlacePriority = DeinterlacePriority::Quality;
    s.rotation = Rotation::Cw90;
    s.brightness = 0.2;
    FilterChange c = fm.Apply(s);
    CHECK(c.videoRebuild && !c.reopen && !c.presenterParamsChanged);
    CHECK_EQ(fm.VideoFilter(), std::wstring(L"bwdif=mode=send_frame:parity=auto:deint=all,transpose=1,eq@color=brightness=0.2:contrast=1:saturation=1:gamma=1"));
    s.brightness = 0.3;
    c = fm.Apply(s);
    CHECK(!c.videoRebuild && !c.reopen && c.commands.size() == 1);
    for (FilterChange x : { fm.SetPlanPreferences(DecoderPref::Software, "", PresenterPref::Sdl), fm.SetContent(Mpeg2()), fm.Blacklist("hwaccel:cuda"), fm.ClearBlacklist() }) {
        CHECK(!x.videoRebuild && !x.audioRebuild && !x.reopen && !x.presenterParamsChanged && x.commands.empty());
        CHECK_EQ(x.videoFilter, fm.VideoFilter());
    }
    FilterProvider p = fm.Provider();
    CHECK(p.planOptions);
    CHECK(p.planOptions().empty());
    CHECK(!p.planActive());
    FilterManager lg;
    FilterSettings iv;
    iv.deinterlace = DeinterlaceMode::Ivtc;
    lg.Apply(iv);
    CHECK_EQ(lg.VideoFilter(), std::wstring(L"fieldmatch,bwdif=mode=send_frame:parity=auto:deint=interlaced,decimate"));
}

TEST_CASE(FMPlanner_nvidia_ivtc_uses_ivtc_d3d11_and_flags_reopen) {
    FilterManager fm;
    FilterSettings off; off.deinterlace = DeinterlaceMode::Off;
    fm.Apply(off);
    fm.SetContent(Mpeg2());
    FilterChange c = fm.SetCapabilities(fakecaps::Nvidia());
    REQUIRE(fm.PlannerActive());
    CHECK(fm.CurrentPlan() != nullptr);
    CHECK_EQ(fm.VideoFilter(), std::wstring());
    FilterSettings s;
    s.deinterlace = DeinterlaceMode::Ivtc;
    c = fm.Apply(s);
    CHECK_EQ(fm.VideoFilter(), std::wstring(L"ivtc_d3d11"));
    CHECK_EQ(fm.CurrentPlan()->hwaccel, std::string("d3d11va"));
    CHECK(fm.CurrentPlan()->hwframesKeep);
    CHECK(!c.reopen && c.videoRebuild);   // d3d11va + keep + d3d11 already hold for the empty graph: only the vf changes
    CHECK_EQ(c.videoFilter, std::wstring(L"ivtc_d3d11"));
    c = fm.Apply(off);
    CHECK(!c.reopen && c.videoRebuild);
    CHECK_EQ(fm.VideoFilter(), std::wstring());
    FilterSettings sp;
    sp.speed = 2.0;
    c = fm.Apply(sp);
    CHECK(c.audioRebuild);
    CHECK_EQ(c.audioFilter, std::wstring(L"atempo=2"));
    CHECK_EQ(fm.AudioFilter(), std::wstring(L"atempo=2"));
}

TEST_CASE(FMPlanner_nvidia_ivtc_uses_ivtc_cuda_when_d3d11va_is_blacklisted) {
    FilterManager fm;
    fm.SetContent(Mpeg2());
    fm.SetCapabilities(fakecaps::Nvidia());
    FilterSettings s;
    s.deinterlace = DeinterlaceMode::Ivtc;
    fm.Apply(s);
    CHECK_EQ(fm.CurrentPlan()->hwaccel, std::string("d3d11va"));
    FilterChange c = fm.Blacklist("hwaccel:d3d11va");
    CHECK_EQ(fm.VideoFilter(), std::wstring(L"ivtc_cuda"));
    CHECK_EQ(fm.CurrentPlan()->hwaccel, std::string("cuda"));
    CHECK(fm.CurrentPlan()->hwframesKeep);
    CHECK(c.reopen && c.videoRebuild);
    CHECK_EQ(c.videoFilter, std::wstring(L"ivtc_cuda"));
}

TEST_CASE(FMPlanner_eq_slider_is_commands_only) {
    FilterManager fm;
    fm.SetContent(H264());
    fm.SetCapabilities(fakecaps::None());   // no presenter that absorbs eq: eq stays in the vf
    REQUIRE(fm.PlannerActive());
    FilterSettings s;
    s.brightness = 0.1;
    s.rotation = Rotation::Cw90;
    FilterChange c = fm.Apply(s);
    CHECK(c.videoRebuild);
    CHECK_EQ(fm.VideoFilter(), std::wstring(L"transpose=1,eq@color=brightness=0.1:contrast=1:saturation=1:gamma=1"));
    s.brightness = 0.4;
    c = fm.Apply(s);
    CHECK(!c.videoRebuild && !c.reopen && !c.presenterParamsChanged);
    REQUIRE(c.commands.size() == 1);
    CHECK_EQ(c.commands[0].target, std::wstring(L"eq@color"));
    CHECK_EQ(c.commands[0].command, std::wstring(L"brightness"));
    CHECK_EQ(c.commands[0].argument, std::wstring(L"0.4"));
    CHECK_EQ(c.videoFilter, fm.VideoFilter());
    c = fm.Apply(s);
    CHECK(!c.videoRebuild && !c.reopen && c.commands.empty() && !c.presenterParamsChanged);
}

TEST_CASE(FMPlanner_rotation_with_d3d11_presenter_changes_only_presenter_params) {
    FilterManager fm;
    fm.SetContent(Hevc10());
    fm.SetCapabilities(fakecaps::Nvidia());
    FilterSettings s;
    s.rotation = Rotation::Cw90;
    s.brightness = 0.1;
    FilterChange c = fm.Apply(s);
    REQUIRE(fm.PlannerActive());
    CHECK_EQ(fm.CurrentPlan()->presenter, std::string("d3d11"));
    CHECK_EQ(fm.CurrentPlan()->presenterParams.rotation, 90);
    CHECK_EQ(fm.VideoFilter(), std::wstring());
    s.rotation = Rotation::Ccw90;
    c = fm.Apply(s);
    CHECK(c.presenterParamsChanged);
    CHECK(!c.reopen && !c.videoRebuild && !c.audioRebuild && c.commands.empty());
    CHECK_EQ(fm.CurrentPlan()->presenterParams.rotation, 270);
    s.contrast = 1.5;
    c = fm.Apply(s);
    CHECK(c.presenterParamsChanged && !c.reopen && !c.videoRebuild && c.commands.empty());
}

TEST_CASE(FMPlanner_blacklist_replans_and_clear_restores) {
    FilterManager fm;
    fm.SetContent(Mpeg2());
    fm.SetCapabilities(fakecaps::Nvidia());
    FilterSettings s;
    s.deinterlace = DeinterlaceMode::Ivtc;
    fm.Apply(s);
    CHECK_EQ(fm.VideoFilter(), std::wstring(L"ivtc_d3d11"));
    FilterChange c = fm.Blacklist("filter:ivtc_d3d11");
    CHECK_EQ(fm.VideoFilter(), std::wstring(L"ivtc_cuda"));
    CHECK_EQ(fm.CurrentPlan()->hwaccel, std::string("cuda"));
    CHECK(c.reopen);
    c = fm.Blacklist("filter:ivtc_cuda");
    CHECK_EQ(fm.VideoFilter(), std::wstring(L"fieldmatch,bwdif=mode=send_frame:parity=auto:deint=interlaced,decimate"));
    CHECK(c.reopen || c.videoRebuild);
    CHECK_EQ(c.videoFilter, std::wstring(L"fieldmatch,bwdif=mode=send_frame:parity=auto:deint=interlaced,decimate"));
    c = fm.ClearBlacklist();
    CHECK_EQ(fm.VideoFilter(), std::wstring(L"ivtc_d3d11"));
    fm.Blacklist("hwaccel:d3d11va");
    CHECK_EQ(fm.CurrentPlan()->hwaccel, std::string("cuda"));
    fm.ClearBlacklist();
    fm.Blacklist("hwaccel:d3d11va");
    fm.Blacklist("hwaccel:cuda");
    CHECK(fm.CurrentPlan()->hwaccel != "cuda" && fm.CurrentPlan()->hwaccel != "d3d11va");
    fm.ClearBlacklist();
    fm.Blacklist("hwframes_keep");
    CHECK(!fm.CurrentPlan()->hwframesKeep);
}

TEST_CASE(FMPlanner_preferences_and_content_replan) {
    FilterManager fm;
    FilterSettings off; off.deinterlace = DeinterlaceMode::Off;
    fm.Apply(off);
    fm.SetContent(H264());
    fm.SetCapabilities(fakecaps::Nvidia());
    REQUIRE(fm.PlannerActive());
    FilterChange c = fm.SetPlanPreferences(DecoderPref::Software, "", PresenterPref::Auto);
    CHECK(c.reopen && c.videoRebuild);
    CHECK_EQ(fm.CurrentPlan()->hwaccel, std::string());
    c = fm.SetPlanPreferences(DecoderPref::Forced, "d3d11va", PresenterPref::Auto);
    CHECK(c.reopen);
    CHECK_EQ(fm.CurrentPlan()->hwaccel, std::string("d3d11va"));
    c = fm.SetPlanPreferences(DecoderPref::Forced, "d3d11va", PresenterPref::Sdl);
    CHECK(c.reopen);
    CHECK_EQ(fm.CurrentPlan()->presenter, std::string("sdl"));
    c = fm.SetPlanPreferences(DecoderPref::Forced, "d3d11va", PresenterPref::Sdl);
    CHECK(!c.reopen && !c.videoRebuild && c.commands.empty());
    c = fm.SetContent(H264());
    CHECK(!c.reopen);
    c = fm.SetCapabilities(std::nullopt);
    CHECK(!fm.PlannerActive());
    CHECK(c.reopen);
    CHECK(fm.CurrentPlan() == nullptr);
}

TEST_CASE(FMPlanner_first_caps_reopens_only_when_the_open_options_differ) {
    // Legacy open used: user decoder (forced d3d11va here), download frames, sdl presenter, legacy vf.
    FilterSettings off; off.deinterlace = DeinterlaceMode::Off;
    FilterManager fm;
    fm.Apply(off);
    fm.SetContent(H264());
    fm.SetPlanPreferences(DecoderPref::Forced, "d3d11va", PresenterPref::Sdl);
    FilterChange c = fm.SetCapabilities(fakecaps::Nvidia());
    CHECK(fm.PlannerActive());
    CHECK(!c.reopen);
    FilterManager fm2;
    fm2.Apply(off);
    fm2.SetContent(H264());
    fm2.SetPlanPreferences(DecoderPref::Software, "", PresenterPref::Auto);
    c = fm2.SetCapabilities(fakecaps::Nvidia());
    CHECK(c.reopen);
}

TEST_CASE(PCPlan_options_come_from_the_plan) {
    FilterManager fm;
    fm.SetContent(Mpeg2());
    fm.SetCapabilities(fakecaps::Nvidia());
    FilterSettings s;
    s.deinterlace = DeinterlaceMode::Ivtc;
    fm.Apply(s);
    Fake p;
    PlaybackController c(p);
    c.SetHwDecoder(L"d3d12va");                 // must NOT leak into a plan-mode open
    c.SetFilters(fm.Provider());
    std::vector<OpenOptions> seen;
    c.onOpening = [&](const std::wstring&, const OpenOptions& o) { seen.push_back(o); };
    c.SetQueue({ MediaItem(L"a.ts") }, 0, true);
    REQUIRE(p.opened.size() == 1);
    const OpenOptions& o = p.opened[0].options;
    CHECK_EQ(Opt(o, L"hwaccel"), std::wstring(L"d3d11va"));
    CHECK_EQ(Count(o, L"hwaccel"), 1);
    CHECK_EQ(Opt(o, L"hwframes"), std::wstring(L"keep"));
    CHECK_EQ(Opt(o, L"presenter"), std::wstring(L"d3d11"));
    CHECK_EQ(Opt(o, L"vf"), std::wstring(L"ivtc_d3d11"));
    CHECK(Opt(o, L"filter_hw_device").empty());
    REQUIRE(seen.size() == 1);
    CHECK(seen[0] == o);
}

TEST_CASE(PCPlan_explicit_base_hwaccel_wins_and_audio_filter_is_kept) {
    FilterManager fm;
    fm.SetContent(Mpeg2());
    fm.SetCapabilities(fakecaps::Nvidia());
    FilterSettings s;
    s.deinterlace = DeinterlaceMode::Ivtc;
    s.speed = 1.5;
    fm.Apply(s);
    Fake p;
    PlaybackController c(p);
    c.SetBaseOption(L"hwaccel", L"dxva2");
    c.SetFilters(fm.Provider());
    c.SetQueue({ MediaItem(L"a.ts") }, 0, true);
    REQUIRE(p.opened.size() == 1);
    const OpenOptions& o = p.opened[0].options;
    CHECK_EQ(Opt(o, L"hwaccel"), std::wstring(L"dxva2"));
    CHECK_EQ(Count(o, L"hwaccel"), 1);
    CHECK_EQ(Opt(o, L"hwframes"), std::wstring(L"keep"));
    CHECK_EQ(Opt(o, L"vf"), std::wstring(L"ivtc_d3d11,setpts=PTS/1.5"));
    CHECK_EQ(Opt(o, L"af"), std::wstring(L"atempo=1.5"));
}

TEST_CASE(PCPlan_software_plan_adds_no_hwaccel_and_filter_hw_device_comes_from_the_plan) {
    FilterManager fm;
    fm.SetContent(H264());
    fm.SetCapabilities(fakecaps::Nvidia());
    fm.SetPlanPreferences(DecoderPref::Software, "", PresenterPref::Sdl);
    REQUIRE(fm.PlannerActive());
    Fake p;
    PlaybackController c(p);                   // hwDecoder_ is "auto": would add hwaccel=auto in legacy mode
    c.SetFilters(fm.Provider());
    c.SetQueue({ MediaItem(L"a.ts") }, 0, true);
    REQUIRE(p.opened.size() == 1);
    bool found = true;
    Opt(p.opened[0].options, L"hwaccel", &found);
    CHECK(!found);
    FilterProvider fp;
    fp.planOptions = [] { return OpenOptions{ { L"vf", L"x" }, { L"filter_hw_device", L"cuda" } }; };
    c.SetFilters(fp);
    p.state = PlayerState::Stopped;
    c.Play();
    REQUIRE(p.opened.size() == 2);
    CHECK_EQ(Opt(p.opened[1].options, L"filter_hw_device"), std::wstring(L"cuda"));
    CHECK_EQ(Opt(p.opened[1].options, L"vf"), std::wstring(L"x"));
}

TEST_CASE(PCPlan_legacy_unchanged_when_the_plan_options_are_empty) {
    Fake p;
    PlaybackController c(p);
    FilterProvider fp;
    fp.videoFilter = [] { return std::wstring(L"yadif"); };
    fp.planOptions = [] { return OpenOptions{}; };   // empty = legacy
    c.SetFilters(fp);
    c.SetQueue({ MediaItem(L"a.ts") }, 0, true);
    REQUIRE(p.opened.size() == 1);
    CHECK_EQ(Opt(p.opened[0].options, L"hwaccel"), std::wstring(L"auto"));
    CHECK_EQ(Opt(p.opened[0].options, L"vf"), std::wstring(L"yadif"));
    CHECK_EQ(Opt(p.opened[0].options, L"filter_hw_device"), std::wstring());
}

TEST_CASE(PCPlan_beforeOpen_runs_before_the_options_are_built) {
    FilterManager fm;
    fm.SetCapabilities(fakecaps::Nvidia());
    fm.SetContent(H264());
    Fake p;
    PlaybackController c(p);
    c.SetFilters(fm.Provider());
    std::vector<std::wstring> urls;
    size_t openedAtHook = 99;
    c.beforeOpen = [&](const std::wstring& url) {
        urls.push_back(url);
        openedAtHook = p.opened.size();
        fm.SetContent(url == L"a.ts" ? Mpeg2() : Hevc10());
    };
    c.SetQueue({ MediaItem(L"a.ts"), MediaItem(L"b.ts") }, 0, true);
    CHECK(urls.size() == 1 && p.opened.size() == 1 && openedAtHook == 0);
    c.Next();
    CHECK(urls.size() == 2 && p.opened.size() == 2 && openedAtHook == 1);
    CHECK_EQ(urls[1], std::wstring(L"b.ts"));
    CHECK(fm.Content().codec == "hevc");
}

TEST_CASE(PCPlan_reopen_change_reopens_instead_of_live_commands) {
    Fake p;
    PlaybackController c(p);
    c.SetQueue({ MediaItem(L"a.ts") }, 0, true);
    p.pos = 33.5;
    p.state = PlayerState::Paused;
    FilterChange fc;
    fc.reopen = true;
    fc.videoRebuild = true;
    fc.videoFilter = L"ivtc_cuda";
    fc.commands.push_back({ L"eq@color", L"brightness", L"0.1" });
    c.ApplyFilterChange(fc);
    CHECK_EQ(p.opened.size(), (size_t)2);
    CHECK(p.vf.empty() && p.commands.empty());
    CHECK(Opt(p.opened[1].options, L"ss").rfind(L"33.5", 0) == 0);
    p.Prepared.raise(MediaPreparedInfo{});
    CHECK(p.state == PlayerState::Paused);
    Fake q;
    PlaybackController c2(q);
    c2.SetQueue({ MediaItem(L"a.ts") }, 0, false);
    c2.ApplyFilterChange(fc);
    CHECK(q.opened.empty());
    FilterChange live;
    live.videoRebuild = true;
    live.videoFilter = L"hflip";
    p.state = PlayerState::Playing;
    c.ApplyFilterChange(live);
    CHECK_EQ(p.vf.size(), (size_t)1);
    CHECK_EQ(p.opened.size(), (size_t)2);
}
