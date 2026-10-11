// The planner's plans against what libffplay does on this machine (recording presenter, dummy audio driver). Caps: libffplay's
// probe of its default devices with the build tables and the recording presenter (RealCaps), FilterManager with the Auto
// decoder preference, PlaybackController + FfPlayer. One printed "PIPEROW" per case (plan summary, planned copies,
// measured frame paths, fps).
// Media: testmedia\hw (mpeg2-telecine-1080i.ts is a 1920x1080 telecined MPEG-2 clip made from testsrc2 with telecine=pattern=23).
// Every case runs with a watchdog: a hang of more than 90 s ends the process with exit code 3 and a message instead of blocking the runner.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "wvp/DebugInfo.h"
#include "wvp/FfPlayer.h"
#include "wvp/FilterManager.h"
#include "wvp/PipelineSession.h"
#include "wvp/PipelineTable.h"
#include "wvp/PlaybackController.h"
#include "wvp/win32/Utf.h"
#include "TestPlayer.h"
#include "wvptest.h"

using namespace wvp;
namespace fs = std::filesystem;

namespace {

fs::path RepoRoot() {
    wchar_t buf[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    fs::path d = fs::path(buf).parent_path();
    while (!d.empty()) {
        if (fs::exists(d / L"CMakePresets.json")) return d;
        auto parent = d.parent_path();
        if (parent == d) break;
        d = parent;
    }
    return fs::current_path();
}

using wvptest::Pump;

template <class F>
bool WaitFor(F cond, double seconds) { return wvptest::WaitFor(cond, seconds); }

// A player on the recording presenter; hwaccel opens create default devices (the presenter brings none).
std::unique_ptr<FfPlayer> RealPlayer() {
    int err = 0;
    auto p = wvptest::CreateTestPlayer(nullptr, &err, false);
    REQUIRE_MSG(p != nullptr, "FfPlayer::Create failed: " + std::to_string(err));
    return p;
}

// What the planner sees here: libffplay's probe of its default devices, the build tables, and the recording presenter
// (system frames only).
std::optional<CapabilitySet> RealCaps() {
    auto engine = FfPlayer::ProbeEngine(nullptr, 0);
    if (!engine) return std::nullopt;
    CapabilitySet caps = MergeBuildCapabilities(*engine, BuildCapabilities());
    PresenterCap recording;
    recording.name = "recording";
    recording.ok = true;
    recording.inputs = { FrameDomain::System };
    caps.presenters = { recording };
    return caps;
}

struct Watchdog {
    std::shared_ptr<std::atomic<bool>> done = std::make_shared<std::atomic<bool>>(false);
    explicit Watchdog(std::string what, int seconds = 90) {
        std::thread([d = done, what = std::move(what), seconds] {
            for (int i = 0; i < seconds * 10; i++) {
                if (d->load()) return;
                Sleep(100);
            }
            if (d->load()) return;
            std::printf("PIPEROW | WATCHDOG: %s hung for %d s, terminating the process\n", what.c_str(), seconds);
            std::fflush(stdout);
            std::_Exit(3);
        }).detach();
    }
    ~Watchdog() { done->store(true); }
};

fs::path Media(const wchar_t* file) { return RepoRoot() / L"testmedia" / L"hw" / file; }

// Opens the file once with default options just to read the stream info (what PipelineSession::OnPrepared would learn).
ContentInfo LearnContent(const fs::path& path) {
    ContentInfo c;
    auto player = RealPlayer();
    int prepared = 0, errors = 0;
    player->Prepared.add([&](const MediaPreparedInfo&) { prepared++; });
    player->Error.add([&](const PlayerErrorInfo&) { errors++; });
    if (player->Open(wvp::ToUtf8(path.wstring()), {}) != 0) return c;
    if (WaitFor([&] { return prepared > 0 || errors > 0; }, 15) && errors == 0) {
        if (auto mi = player->GetMediaInfo()) c = PipelineSession::ContentOf(*mi);
    }
    player->Stop();
    player.reset();
    return c;
}

struct RunResult {
    bool opened = false;
    int presenterErrors = 0;
    std::string presenterErrorText;
    DebugInfo d;          // the last sample with enough frames
    OpenOptions options;  // what the controller really passed to Open
};

RunResult PlayPlanned(FilterManager& fm, const fs::path& path, double seconds) {
    RunResult res;
    auto player = RealPlayer();
    int prepared = 0, errors = 0;
    player->Prepared.add([&](const MediaPreparedInfo&) { prepared++; });
    player->Error.add([&](const PlayerErrorInfo&) { errors++; });
    player->PresenterError.add([&](const PresenterErrorInfo& e) { res.presenterErrors++; res.presenterErrorText = e.Error; });
    {
        PlaybackController ctl(*player);
        ctl.SetFilters(fm.Provider());
        ctl.onOpening = [&](const std::string&, const OpenOptions& o) { res.options = o; };
        ctl.SetQueue({ MediaItem{ wvp::ToUtf8(path.wstring()), wvp::ToUtf8(path.filename().wstring()) } }, 0, true);
        res.opened = WaitFor([&] { return prepared > 0 || errors > 0; }, 15) && errors == 0;
        if (res.opened) {
            auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds((long long)(seconds * 1000));
            while (std::chrono::steady_clock::now() < end) {
                Pump();
                auto js = player->GetDebugInfoJson();
                if (js) {
                    DebugInfo d = ParseDebugInfo(*js);
                    long long n = d.Video.FramesHwDirect.value_or(0) + d.Video.FramesHwDownload.value_or(0) + d.Video.FramesSoftware.value_or(0);
                    if (d.Valid && d.HasVideo && n >= 10) res.d = std::move(d);
                }
                Sleep(100);
            }
        }
        ctl.Stop();
        ctl.Dispose();
    }
    player.reset();
    return res;
}

std::string Fps(const std::optional<double>& v) {
    char b[32];
    if (!v) return "?";
    std::snprintf(b, sizeof b, "%.3f", *v);
    return b;
}

void PrintRow(const char* name, const Plan& p, const RunResult& r) {
    const DebugInfo& d = r.d;
    const DebugVideo& v = d.Video;
    std::printf("PIPEROW | %-10s | plan: %s | planned host %d device %d | measured hw_direct %lld hw_download %lld software %lld sink_download %lld | "
                "presenter %s direct %lld uploaded %lld | fps filter_out %s displayed %s | err %s\n",
                name, p.Summary().c_str(), p.cost.hostCopies, p.cost.deviceCopies, v.FramesHwDirect.value_or(-1), v.FramesHwDownload.value_or(-1),
                v.FramesSoftware.value_or(-1), v.FramesSinkDownload.value_or(-1), d.Presenter.Name.c_str(), d.Presenter.FramesDirect.value_or(-1),
                d.Presenter.FramesUploaded.value_or(-1), Fps(d.Fps.FilterOut).c_str(), Fps(d.Fps.Displayed).c_str(),
                d.Presenter.Error.empty() ? "-" : d.Presenter.Error.c_str());
    std::fflush(stdout);
}

bool HasStep(const Plan& p, const std::string& name) {
    for (const auto& s : p.steps) if (s.name == name) return true;
    return false;
}

void CheckMeasuredAgainstPlan(const Plan& p, const RunResult& r) {
    REQUIRE_MSG(r.opened, "open failed");
    const DebugInfo& d = r.d;
    REQUIRE_MSG(d.Valid && d.HasVideo, "no debug info with decoded frames");
    const DebugVideo& v = d.Video;
    CHECK_EQ(r.presenterErrors, 0);
    CHECK_MSG(d.Presenter.Error.empty(), d.Presenter.Error);
    CHECK_EQ(d.Presenter.Name, std::string("recording"));
    CHECK(HasStep(p, "present:recording"));
    if (!p.hwaccel.empty()) CHECK_EQ(v.HwaccelDevice, p.hwaccel);
    // the recording presenter takes system frames only: every plan reaches it
    CHECK(d.Presenter.FramesDirect.value_or(0) > 0);
}

}

// MPEG-2 1080i telecined + Deinterlace=Ivtc
TEST_CASE_INTEGRATION(PipelineReal_a_mpeg2_1080i_ivtc) {
    Watchdog wd("PipelineReal_a");
    fs::path path = Media(L"mpeg2-telecine-1080i.ts");
    REQUIRE_MSG(fs::exists(path), "missing testmedia\\hw\\mpeg2-telecine-1080i.ts");
    auto caps = RealCaps();
    REQUIRE_MSG(caps.has_value(), "no caps");
    ContentInfo c = LearnContent(path);
    REQUIRE_MSG(c.Known(), "cannot read the stream info");
    FilterManager fm;
    fm.SetPlanPreferences(DecoderPref::Auto, "");
    fm.SetCapabilities(*caps);
    fm.SetContent(c);
    FilterSettings s;
    s.deinterlace = DeinterlaceMode::Ivtc;
    fm.Apply(s);
    REQUIRE(fm.PlannerActive());
    Plan p = *fm.CurrentPlan();
    REQUIRE_MSG(p.ok, p.error);
    RunResult r = PlayPlanned(fm, path, 4.0);
    PrintRow("a-ivtc", p, r);
    CheckMeasuredAgainstPlan(p, r);
    // an inverse telecine of 29.97i telecined from 23.976p comes out at ~23.976
    REQUIRE(r.d.Fps.FilterOut.has_value());
    CHECK_MSG(std::fabs(*r.d.Fps.FilterOut - 23.976) < 0.5, "fps filter_out " + Fps(r.d.Fps.FilterOut));
}

// Broadcast H.264 1920x1080i (30000/1001 interlaced pictures/s, MBAFF) + Bwdif: send_frame ~29.97 fps
static void RunBwdifAutoCase(const char* row) {
    Watchdog wd(row);
    fs::path path = Media(L"h264-1920x1080i-2997.ts");
    REQUIRE_MSG(fs::exists(path), "missing testmedia\\hw\\h264-1920x1080i-2997.ts");
    auto caps = RealCaps();
    REQUIRE_MSG(caps.has_value(), "no caps");
    ContentInfo c = LearnContent(path);
    REQUIRE_MSG(c.Known(), "cannot read the stream info");
    FilterManager fm;
    fm.SetPlanPreferences(DecoderPref::Auto, "");
    fm.SetCapabilities(*caps);
    fm.SetContent(c);
    FilterSettings s;
    s.deinterlace = DeinterlaceMode::On;
    s.deinterlacePriority = DeinterlacePriority::Quality;
    fm.Apply(s);
    REQUIRE(fm.PlannerActive());
    Plan p = *fm.CurrentPlan();
    REQUIRE_MSG(p.ok, p.error);
    RunResult r = PlayPlanned(fm, path, 4.0);
    PrintRow(row, p, r);
    CheckMeasuredAgainstPlan(p, r);
    REQUIRE(r.d.Fps.FilterOut.has_value());
    constexpr double expected = 30000.0 / 1001.0;
    CHECK_MSG(std::fabs(*r.d.Fps.FilterOut - expected) <= expected * 0.10, "fps filter_out " + Fps(r.d.Fps.FilterOut));
}
TEST_CASE_INTEGRATION(PipelineReal_b_h264_1080i_bwdif) { RunBwdifAutoCase("b-bwdif"); }
TEST_CASE_INTEGRATION(PipelineReal_b2_h264_1080i_bwdif_auto) { RunBwdifAutoCase("b2-auto"); }

// Broadcast 29.97i: 59.94p source interleaved into 29.97 interlaced pictures/s.
// libx264 produces MBAFF (field_pic_flag=0), not genuine PAFF. Verify both the
// declared filter output rate and the actually displayed cadence.
TEST_CASE_INTEGRATION(InterlacedFps_2997i_bwdif_send_frame_and_send_field) {
    Watchdog wd("InterlacedFps_2997i_bwdif_send_frame_and_send_field", 90);
    const fs::path path = Media(L"h264-1920x1080i-2997.ts");
    REQUIRE_MSG(fs::exists(path), "missing 29.97i interlaced fixture");
    for (bool hw : {false, true}) {
        for (bool field : {false, true}) {
            auto player = RealPlayer();
            int prepared = 0, errors = 0;
            player->Prepared.add([&](const MediaPreparedInfo&) { ++prepared; });
            player->Error.add([&](const PlayerErrorInfo&) { ++errors; });
            OpenOptions opts = {
                { "vf", field ? "bwdif=mode=send_field:parity=auto:deint=all"
                                 : "bwdif=mode=send_frame:parity=auto:deint=all" },
            };
            if (hw) opts.emplace_back("hwaccel", "d3d11va");
            REQUIRE(player->Open(wvp::ToUtf8(path.wstring()), opts) == 0);
            REQUIRE_MSG(WaitFor([&] { return prepared || errors; }, 12) && !errors,
                        "29.97i playback did not prepare");
            // Sample before its 5 s end and after the ~1.5 s FPS sliding window fills.
            DebugInfo d;
            const double expected = field ? 60000.0 / 1001.0 : 30000.0 / 1001.0;
            REQUIRE_MSG(WaitFor([&] {
                auto json = player->GetDebugInfoJson();
                if (json) {
                    auto current = ParseDebugInfo(*json);
                    if (current.Valid && current.Fps.FramesDisplayed.value_or(0) >= (field ? 72 : 40))
                        d = std::move(current);
                }
                return d.Valid && d.Fps.Displayed &&
                       d.Fps.FramesDisplayed.value_or(0) >= (field ? 105 : 55);
            }, 4.3), "insufficient displayed frames for 29.97i rate measurement");
            std::printf("INTERLACEDFPS | %s %s | stream %s filter %s decoded %s filtered %s displayed %s | frames %lld/%lld/%lld\n",
                        hw ? "d3d11va" : "software", field ? "send_field" : "send_frame",
                        Fps(d.Fps.StreamAvg).c_str(), Fps(d.Fps.FilterOut).c_str(),
                        Fps(d.Fps.Decoded).c_str(), Fps(d.Fps.Filtered).c_str(), Fps(d.Fps.Displayed).c_str(),
                        (long long)d.Fps.FramesDecoded.value_or(-1),
                        (long long)d.Fps.FramesFiltered.value_or(-1),
                        (long long)d.Fps.FramesDisplayed.value_or(-1));
            std::fflush(stdout);
            REQUIRE(d.Fps.FilterOut.has_value());
            REQUIRE(d.Fps.Displayed.has_value());
            CHECK_MSG(std::fabs(*d.Fps.FilterOut - expected) <= expected * 0.10,
                      "filter_out " + Fps(d.Fps.FilterOut));
            CHECK_MSG(std::fabs(*d.Fps.Displayed - expected) <= expected * 0.10,
                      "displayed " + Fps(d.Fps.Displayed));
            CHECK_EQ(d.Video.HwaccelDevice, hw ? std::string("d3d11va") : std::string("none"));
            player->Stop();
            player.reset();
        }
    }
}

TEST_CASE_INTEGRATION(PipelineReal_presenter_error_event_and_reopen) {
    Watchdog wd("PipelineReal_presenter_error_event_and_reopen");
    fs::path path = Media(L"hevc-10bit-1080p.mp4");
    REQUIRE_MSG(fs::exists(path), "missing sample");
    wvptest::RecordingPresenter* presenter = nullptr;
    int err = 0;
    auto player = wvptest::CreateTestPlayer(&presenter, &err);
    REQUIRE(player != nullptr);
    std::vector<PresenterErrorInfo> errs;
    int prepared = 0;
    player->Prepared.add([&](const MediaPreparedInfo&) { prepared++; });
    player->PresenterError.add([&](const PresenterErrorInfo& e) { errs.push_back(e); });
    REQUIRE(player->Open(wvp::ToUtf8(path.wstring()), {}) == 0);
    REQUIRE(WaitFor([&] { return prepared > 0 && presenter->Presents() > 0; }, 15));
    presenter->FailNextPresent(false);
    REQUIRE(WaitFor([&] { return !errs.empty(); }, 10));
    CHECK_EQ(errs[0].Presenter, std::string("recording"));
    CHECK(!errs[0].DeviceLost);
    CHECK(!errs[0].Error.empty());
    auto js = player->GetDebugInfoJson();
    REQUIRE(js.has_value());
    CHECK(!ParseDebugInfo(*js).Presenter.Error.empty());
    const int before = presenter->Presents();
    REQUIRE(player->Open(wvp::ToUtf8(path.wstring()), {}) == 0);
    CHECK(WaitFor([&] { return presenter->Presents() > before; }, 15));
    player->Stop();
    player.reset();
}

// Software decoding + Deinterlace=Ivtc to a presenter that takes system frames, on the telecined MPEG-2 clip and the 1080i
// broadcast fixture: the CPU recipe (every GPU ivtc blacklisted) and the cheapest plan both display ~23.976 fps without a
// download.
TEST_CASE_INTEGRATION(PipelineReal_d_software_ivtc) {
    Watchdog wd("PipelineReal_d", 150);
    auto caps = RealCaps();
    REQUIRE_MSG(caps.has_value(), "no caps");
    const fs::path files[] = { Media(L"mpeg2-telecine-1080i.ts"), RepoRoot() / L"testmedia" / L"arib" / L"tbs-captions.ts" };
    REQUIRE_MSG(fs::exists(files[0]), "missing testmedia\\hw\\mpeg2-telecine-1080i.ts");
    for (const fs::path& path : files) {
        if (!fs::exists(path)) {   // the broadcast fixture is local only (docs/debugging.md)
            std::printf("PIPEROW | d-ivtc     | SKIP: missing %ls\n", path.c_str());
            std::fflush(stdout);
            continue;
        }
        ContentInfo c = LearnContent(path);
        REQUIRE_MSG(c.Known(), "cannot read the stream info");
        for (bool cpu : { true, false }) {
            FilterManager fm;
            fm.SetPlanPreferences(DecoderPref::Software, "");
            fm.SetCapabilities(*caps);
            fm.SetContent(c);
            if (cpu)
                for (const FilterVariant& v : PipelineTable::Discover(*caps).VariantsFor(PipelineOp::Ivtc))
                    if (v.domain != FrameDomain::System) fm.Blacklist("filter:" + v.filter);
            FilterSettings s;
            s.deinterlace = DeinterlaceMode::Ivtc;
            fm.Apply(s);
            REQUIRE(fm.PlannerActive());
            Plan p = *fm.CurrentPlan();
            REQUIRE_MSG(p.ok, p.error);
            CHECK(p.hwaccel.empty());
            if (cpu) CHECK_MSG(HasStep(p, "fieldmatch,bwdif,decimate"), p.Summary());
            RunResult r = PlayPlanned(fm, path, 4.0);
            PrintRow(cpu ? "d-cpu-ivtc" : "d-ivtc", p, r);
            CheckMeasuredAgainstPlan(p, r);
            const DebugInfo& d = r.d;
            REQUIRE(d.Fps.FilterOut.has_value() && d.Fps.Displayed.has_value());
            CHECK_MSG(std::fabs(*d.Fps.FilterOut - 23.976) < 0.5, "fps filter_out " + Fps(d.Fps.FilterOut));
            CHECK_MSG(std::fabs(*d.Fps.Displayed - 23.976) < 23.976 * 0.10, "fps displayed " + Fps(d.Fps.Displayed));
            CHECK_MSG(d.Fps.FramesDisplayed.value_or(0) >= 40, "displayed frames " + std::to_string(d.Fps.FramesDisplayed.value_or(-1)));
            CHECK_EQ(d.Video.FramesHwDownload.value_or(-1), 0LL);
            CHECK_EQ(d.Video.FramesSinkDownload.value_or(-1), 0LL);
            CHECK_EQ(p.cost.hostCopies, 1);   // the presenter's upload of each displayed frame
        }
    }
}
