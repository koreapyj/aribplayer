#include "wvptest.h"

#include <algorithm>
#include <string>
#include <vector>

#include "wvp/FilterManager.h"
#include "wvp/PlaybackController.h"

using namespace wvp;

namespace {

class Fake : public IMediaPlayer {
public:
    struct Opened { std::string url; OpenOptions options; };
    std::vector<Opened> opened;
    std::vector<std::string> vf, af;
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
    int Open(const std::string& url, const OpenOptions& o) override { opened.push_back({ url, o }); state = PlayerState::Playing; return 0; }
    int Stop() override { state = PlayerState::Stopped; return 0; }
    int Play() override { state = PlayerState::Playing; return 0; }
    int Pause() override { state = PlayerState::Paused; return 0; }
    int TogglePause() override { return 0; }
    int Seek(double, bool) override { return 0; }
    int CycleStream(StreamType) override { return 0; }
    int SetVideoFilter(const std::string& v) override { vf.push_back(v); return 0; }
    int SetAudioFilter(const std::string& a) override { af.push_back(a); return 0; }
    int SendFilterCommand(const std::string&, const std::string&, const std::string&) override { return 0; }
    std::optional<MediaInfo> GetMediaInfo() override { return std::nullopt; }
    std::optional<BgraFrame> Snapshot() override { return std::nullopt; }
};

std::string Opt(const OpenOptions& o, const std::string& k, bool* found = nullptr) {
    for (auto& e : o) if (e.first == k) { if (found) *found = true; return e.second; }
    if (found) *found = false;
    return "";
}
int Count(const OpenOptions& o, const std::string& k) {
    int n = 0;
    for (auto& e : o) if (e.first == k) n++;
    return n;
}

ContentInfo Mpeg2() { return { "mpeg2video", 8, 1920, 1080, false }; }
ContentInfo Hevc10() { return { "hevc", 10, 1920, 1080, false }; }
ContentInfo H264() { return { "h264", 8, 1920, 1080, false }; }

// A plan without options: software decoding, no filters.
FilterProvider Plain() {
    FilterProvider fp;
    fp.planOptions = [] { return std::optional<OpenOptions>(OpenOptions{}); };
    return fp;
}

}

TEST_CASE(FMPlanner_no_plan_without_caps) {
    FilterManager fm;
    CHECK(!fm.PlannerActive() && !fm.HasCapabilities());
    CHECK(fm.CurrentPlan() == nullptr);
    CHECK(fm.PlanError().empty());
    FilterSettings s;
    s.deinterlace = DeinterlaceMode::On;
    s.speed = 2;
    FilterChange c = fm.Apply(s);
    CHECK(!c.videoRebuild && !c.reopen && c.audioRebuild);
    CHECK_EQ(fm.VideoFilter(), std::string());
    CHECK_EQ(fm.AudioFilter(), std::string("atempo=2"));
    for (FilterChange x : { fm.SetPlanPreferences(DecoderPref::Software, ""), fm.SetContent(Mpeg2()), fm.Blacklist("hwaccel:d3d11va"), fm.ClearBlacklist() })
        CHECK(!x.videoRebuild && !x.audioRebuild && !x.reopen);
    FilterProvider p = fm.Provider();
    REQUIRE(p.planOptions);
    CHECK(!p.planOptions().has_value());
    c = fm.SetCapabilities(fakecaps::None());
    CHECK(c.reopen && c.videoRebuild);   // the first plan: the waiting open runs with it
    CHECK_EQ(fm.VideoFilter(), std::string("yadif=mode=send_frame:parity=auto:deint=all,setpts=PTS/2"));
    REQUIRE(p.planOptions().has_value());
}

TEST_CASE(FMPlanner_nvidia_ivtc_uses_ivtc_d3d11_and_flags_reopen) {
    FilterManager fm;
    FilterSettings off; off.deinterlace = DeinterlaceMode::Off;
    fm.Apply(off);
    fm.SetContent(Mpeg2());
    FilterChange c = fm.SetCapabilities(fakecaps::Nvidia());
    REQUIRE(fm.PlannerActive());
    CHECK(fm.CurrentPlan() != nullptr);
    CHECK_EQ(fm.VideoFilter(), std::string());
    FilterSettings s;
    s.deinterlace = DeinterlaceMode::Ivtc;
    c = fm.Apply(s);
    CHECK_EQ(fm.VideoFilter(), std::string("ivtc_d3d11"));
    CHECK_EQ(fm.CurrentPlan()->hwaccel, std::string("d3d11va"));
    CHECK(fm.CurrentPlan()->hwframesKeep);
    CHECK(!c.reopen && c.videoRebuild);   // d3d11va + keep + d3d11 already hold for the empty graph: only the vf changes
    CHECK_EQ(c.videoFilter, std::string("ivtc_d3d11"));
    c = fm.Apply(off);
    CHECK(!c.reopen && c.videoRebuild);
    CHECK_EQ(fm.VideoFilter(), std::string());
    FilterSettings sp;
    sp.speed = 2.0;
    c = fm.Apply(sp);
    CHECK(c.audioRebuild);
    CHECK_EQ(c.audioFilter, std::string("atempo=2"));
    CHECK_EQ(fm.AudioFilter(), std::string("atempo=2"));
}

TEST_CASE(FMPlanner_nvidia_ivtc_without_d3d11va_uploads_software_frames) {
    FilterManager fm;
    fm.SetContent(Mpeg2());
    fm.SetCapabilities(fakecaps::Nvidia());
    FilterSettings s;
    s.deinterlace = DeinterlaceMode::Ivtc;
    fm.Apply(s);
    CHECK_EQ(fm.CurrentPlan()->hwaccel, std::string("d3d11va"));
    FilterChange c = fm.Blacklist("hwaccel:d3d11va");
    CHECK_EQ(fm.VideoFilter(), std::string("format=nv12,hwupload,ivtc_d3d11"));
    CHECK_EQ(fm.CurrentPlan()->hwaccel, std::string());
    CHECK(c.reopen && c.videoRebuild);
}

TEST_CASE(FMPlanner_blacklist_replans_and_clear_restores) {
    FilterManager fm;
    fm.SetContent(Mpeg2());
    fm.SetCapabilities(fakecaps::Nvidia());
    FilterSettings s;
    s.deinterlace = DeinterlaceMode::Ivtc;
    fm.Apply(s);
    CHECK_EQ(fm.VideoFilter(), std::string("ivtc_d3d11"));
    FilterChange c = fm.Blacklist("filter:ivtc_d3d11");
    CHECK_EQ(fm.VideoFilter(), std::string("fieldmatch,bwdif=mode=send_frame:parity=auto:deint=interlaced,decimate"));
    CHECK_EQ(fm.CurrentPlan()->hwaccel, std::string());
    CHECK(c.reopen);
    CHECK_EQ(c.videoFilter, std::string("fieldmatch,bwdif=mode=send_frame:parity=auto:deint=interlaced,decimate"));
    c = fm.ClearBlacklist();
    CHECK(c.reopen);
    CHECK_EQ(fm.VideoFilter(), std::string("ivtc_d3d11"));
    fm.Blacklist("hwaccel:d3d11va");
    CHECK(fm.CurrentPlan()->hwaccel != "d3d11va");
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
    FilterChange c = fm.SetPlanPreferences(DecoderPref::Software, "");
    CHECK(c.reopen && c.videoRebuild);
    CHECK_EQ(fm.CurrentPlan()->hwaccel, std::string());
    c = fm.SetPlanPreferences(DecoderPref::Forced, "dxva2");
    CHECK(c.reopen);
    CHECK_EQ(fm.CurrentPlan()->hwaccel, std::string("dxva2"));
    c = fm.SetPlanPreferences(DecoderPref::Forced, "d3d11va");
    CHECK(c.reopen);
    CHECK_EQ(fm.CurrentPlan()->hwaccel, std::string("d3d11va"));
    c = fm.SetPlanPreferences(DecoderPref::Forced, "d3d11va");
    CHECK(!c.reopen && !c.videoRebuild);
    c = fm.SetContent(H264());
    CHECK(!c.reopen);
}

TEST_CASE(FMPlanner_first_caps_bring_the_plan_or_its_error) {
    FilterSettings off; off.deinterlace = DeinterlaceMode::Off;
    FilterManager fm;
    fm.Apply(off);
    fm.SetContent(H264());
    FilterChange c = fm.SetCapabilities(fakecaps::Nvidia());
    CHECK(fm.PlannerActive());
    CHECK(c.reopen);
    FilterManager bad;
    FilterSettings cuda = off;
    cuda.customVideoFilter = "bwdif_cuda";   // a filter of the build the player cannot reach
    bad.Apply(cuda);
    c = bad.SetCapabilities(fakecaps::Nvidia());
    CHECK(!bad.PlannerActive() && bad.HasCapabilities());
    CHECK(!c.reopen && !c.videoRebuild);
    CHECK_EQ(bad.PlanError(), std::string("no valid pipeline for this request"));
    c = bad.Apply(off);
    CHECK(c.reopen && bad.PlannerActive() && bad.PlanError().empty());
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
    c.SetFilters(fm.Provider());
    std::vector<OpenOptions> seen;
    c.onOpening = [&](const std::string&, const OpenOptions& o) { seen.push_back(o); };
    c.SetQueue({ MediaItem("a.ts") }, 0, true);
    REQUIRE(p.opened.size() == 1);
    const OpenOptions& o = p.opened[0].options;
    CHECK_EQ(Opt(o, "hwaccel"), std::string("d3d11va"));
    CHECK_EQ(Count(o, "hwaccel"), 1);
    CHECK_EQ(Opt(o, "hwframes"), std::string("keep"));
    CHECK(Opt(o, "vcodec").empty());
    CHECK_EQ(Opt(o, "vf"), std::string("ivtc_d3d11"));
    CHECK(Opt(o, "filter_hw_device").empty());
    REQUIRE(seen.size() == 1);
    CHECK(seen[0] == o);
}

TEST_CASE(PCPlan_plan_options_override_base_options_and_audio_filter_is_kept) {
    FilterManager fm;
    fm.SetContent(Mpeg2());
    fm.SetCapabilities(fakecaps::Nvidia());
    FilterSettings s;
    s.deinterlace = DeinterlaceMode::Ivtc;
    s.speed = 1.5;
    fm.Apply(s);
    Fake p;
    PlaybackController c(p);
    c.SetBaseOption("hwaccel", "dxva2");
    c.SetBaseOption("user_agent", "x");
    c.SetFilters(fm.Provider());
    c.SetQueue({ MediaItem("a.ts") }, 0, true);
    REQUIRE(p.opened.size() == 1);
    const OpenOptions& o = p.opened[0].options;
    CHECK_EQ(Opt(o, "hwaccel"), std::string("d3d11va"));
    CHECK_EQ(Count(o, "hwaccel"), 1);
    CHECK_EQ(Opt(o, "user_agent"), std::string("x"));
    CHECK_EQ(Opt(o, "hwframes"), std::string("keep"));
    CHECK_EQ(Opt(o, "vf"), std::string("ivtc_d3d11,setpts=PTS/1.5"));
    CHECK_EQ(Opt(o, "af"), std::string("atempo=1.5"));
}

TEST_CASE(PCPlan_software_plan_adds_no_hwaccel_and_filter_hw_device_comes_from_the_plan) {
    FilterManager fm;
    fm.SetContent(H264());
    fm.SetCapabilities(fakecaps::Nvidia());
    fm.SetPlanPreferences(DecoderPref::Software, "");
    REQUIRE(fm.PlannerActive());
    Fake p;
    PlaybackController c(p);
    c.SetFilters(fm.Provider());
    c.SetQueue({ MediaItem("a.ts") }, 0, true);
    REQUIRE(p.opened.size() == 1);
    bool found = true;
    Opt(p.opened[0].options, "hwaccel", &found);
    CHECK(!found);
    FilterProvider fp;
    fp.planOptions = [] { return std::optional<OpenOptions>(OpenOptions{ { "vf", "x" }, { "filter_hw_device", "d3d11va" } }); };
    c.SetFilters(fp);
    p.state = PlayerState::Stopped;
    c.Play();
    REQUIRE(p.opened.size() == 2);
    CHECK_EQ(Opt(p.opened[1].options, "filter_hw_device"), std::string("d3d11va"));
    CHECK_EQ(Opt(p.opened[1].options, "vf"), std::string("x"));
}

TEST_CASE(PCPlan_open_waits_for_a_plan) {
    Fake p;
    PlaybackController c(p);
    bool planned = false;
    FilterProvider fp;
    fp.planOptions = [&]() -> std::optional<OpenOptions> {
        if (!planned) return std::nullopt;
        return OpenOptions{ { "hwaccel", "d3d11va" } };
    };
    c.SetFilters(fp);
    int resumes = 0, hooks = 0;
    c.resolveResumePosition = [&](const MediaItem&, double) { resumes++; return 12.5; };
    c.beforeOpen = [&](const std::string&) { hooks++; };
    c.SetQueue({ MediaItem("a.ts"), MediaItem("b.ts") }, 0, true);
    CHECK(p.opened.empty() && c.HasPendingOpen());
    CHECK(!c.OpenPending());                   // still no plan
    CHECK(!c.ReopenCurrentHere());             // a reopen of the waiting item is the waiting open
    CHECK(p.opened.empty());
    planned = true;
    c.ApplyFilterChange(FilterChange{});       // a filter change with a plan runs the waiting open
    REQUIRE(p.opened.size() == 1);
    CHECK_EQ(p.opened[0].url, std::string("a.ts"));
    CHECK_EQ(Opt(p.opened[0].options, "hwaccel"), std::string("d3d11va"));
    CHECK(Opt(p.opened[0].options, "ss").rfind("12.5", 0) == 0);
    CHECK(resumes == 1 && hooks == 1);
    CHECK(!c.HasPendingOpen() && !c.OpenPending());
    // a stop or a later open cancels a waiting open
    planned = false;
    c.Next();
    CHECK(c.HasPendingOpen());
    c.Stop();
    CHECK(!c.HasPendingOpen());
    planned = true;
    CHECK(!c.OpenPending());
    CHECK_EQ(p.opened.size(), (size_t)1);
    // no provider: never a plan
    Fake q;
    PlaybackController none(q);
    none.SetQueue({ MediaItem("a.ts") }, 0, true);
    CHECK(q.opened.empty() && none.HasPendingOpen());
}

TEST_CASE(PCPlan_beforeOpen_runs_before_the_options_are_built) {
    FilterManager fm;
    fm.SetCapabilities(fakecaps::Nvidia());
    fm.SetContent(H264());
    Fake p;
    PlaybackController c(p);
    c.SetFilters(fm.Provider());
    std::vector<std::string> urls;
    size_t openedAtHook = 99;
    c.beforeOpen = [&](const std::string& url) {
        urls.push_back(url);
        openedAtHook = p.opened.size();
        fm.SetContent(url == "a.ts" ? Mpeg2() : Hevc10());
    };
    c.SetQueue({ MediaItem("a.ts"), MediaItem("b.ts") }, 0, true);
    CHECK(urls.size() == 1 && p.opened.size() == 1 && openedAtHook == 0);
    c.Next();
    CHECK(urls.size() == 2 && p.opened.size() == 2 && openedAtHook == 1);
    CHECK_EQ(urls[1], std::string("b.ts"));
    CHECK(fm.Content().codec == "hevc");
}

TEST_CASE(PCPlan_reopen_change_reopens_instead_of_live_commands) {
    Fake p;
    PlaybackController c(p);
    c.SetFilters(Plain());
    c.SetQueue({ MediaItem("a.ts") }, 0, true);
    p.pos = 33.5;
    p.state = PlayerState::Paused;
    FilterChange fc;
    fc.reopen = true;
    fc.videoRebuild = true;
    fc.videoFilter = "ivtc_d3d11";
    c.ApplyFilterChange(fc);
    CHECK_EQ(p.opened.size(), (size_t)2);
    CHECK(std::find(p.vf.begin(), p.vf.end(), "ivtc_d3d11") == p.vf.end());   // reopened, no live graph change
    CHECK(Opt(p.opened[1].options, "ss").rfind("33.5", 0) == 0);
    p.Prepared.raise(MediaPreparedInfo{});
    CHECK(p.state == PlayerState::Paused);
    Fake q;
    PlaybackController c2(q);
    c2.SetFilters(Plain());
    c2.SetQueue({ MediaItem("a.ts") }, 0, false);
    c2.ApplyFilterChange(fc);
    CHECK(q.opened.empty());
    FilterChange live;
    live.videoRebuild = true;
    live.videoFilter = "hflip";
    p.state = PlayerState::Playing;
    c.ApplyFilterChange(live);
    CHECK(!p.vf.empty() && p.vf.back() == "hflip");
    CHECK_EQ(p.opened.size(), (size_t)2);
}
