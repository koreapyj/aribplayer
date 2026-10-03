// The planner's predicted copy counts against what libffplay does on this machine (hidden host window,
// SDL_AUDIODRIVER=dummy). Real caps (FfPlayer::ProbeCaps), FilterManager in planner mode (Cost policy, Auto decoder, Auto presenter),
// PlaybackController + FfPlayer. One printed "PIPEROW" per case (plan summary, planned vs measured copies, presenter counters, renderer, fps).
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
#include "wvp/Utf.h"
#include "wvptest.h"

using namespace wvp;
namespace fs = std::filesystem;

namespace {

constexpr UINT kRealNotifyMsg = WM_APP + 59;

LRESULT CALLBACK RealHostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == kRealNotifyMsg) { FfPlayer::HandleNotify(w, l); return 0; }
    return DefWindowProcW(h, m, w, l);
}

void EnsureRealClass() {
    static ATOM a = [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = RealHostProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"WvpPipeRealHost";
        return RegisterClassW(&wc);
    }();
    (void)a;
}

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

void Pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}

template <class F>
bool WaitFor(F cond, double seconds) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds((long long)(seconds * 1000));
    while (std::chrono::steady_clock::now() < end) {
        Pump();
        if (cond()) return true;
        Sleep(15);
    }
    Pump();
    return cond();
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

struct Host {
    HWND parent = nullptr, child = nullptr;
    Host() {
        EnsureRealClass();
        HINSTANCE inst = GetModuleHandleW(nullptr);
        parent = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"WvpPipeRealHost", L"host", WS_POPUP, -32000, -32000, 960, 540, nullptr, nullptr, inst, nullptr);
        child = CreateWindowExW(0, L"STATIC", L"video", WS_CHILD | WS_VISIBLE, 0, 0, 960, 540, parent, nullptr, inst, nullptr);
    }
    ~Host() { if (parent) DestroyWindow(parent); }
};

void EnsureDummyAudio() {
    if (!std::getenv("SDL_AUDIODRIVER")) _putenv_s("SDL_AUDIODRIVER", "dummy");
}

fs::path Media(const wchar_t* file) { return RepoRoot() / L"testmedia" / L"hw" / file; }

// Opens the file once with default options just to read the stream info (what PipelineSession::OnPrepared would learn).
ContentInfo LearnContent(const fs::path& path) {
    ContentInfo c;
    Host host;
    int err = 0;
    auto player = FfPlayer::CreateSync(host.child, host.parent, kRealNotifyMsg, &err);
    if (!player) return c;
    int prepared = 0, errors = 0;
    player->Prepared.add([&](const MediaPreparedInfo&) { prepared++; });
    player->Error.add([&](const PlayerErrorInfo&) { errors++; });
    if (player->Open(path.wstring(), {}) != 0) return c;
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
    std::wstring presenterErrorText;
    DebugInfo d;          // the last sample with enough frames
    OpenOptions options;  // what the controller really passed to Open
};

RunResult PlayPlanned(FilterManager& fm, const fs::path& path, double seconds) {
    RunResult res;
    Host host;
    int err = 0;
    auto player = FfPlayer::CreateSync(host.child, host.parent, kRealNotifyMsg, &err);
    if (!player) { CHECK_MSG(false, "CreateSync failed " + std::to_string(err)); return res; }
    int prepared = 0, errors = 0;
    player->Prepared.add([&](const MediaPreparedInfo&) { prepared++; });
    player->Error.add([&](const PlayerErrorInfo&) { errors++; });
    player->PresenterError.add([&](const PresenterErrorInfo& e) { res.presenterErrors++; res.presenterErrorText = e.Error; });
    {
        PlaybackController ctl(*player);
        ctl.SetFilters(fm.Provider());
        ctl.onOpening = [&](const std::wstring&, const OpenOptions& o) { res.options = o; };
        ctl.SetQueue({ MediaItem{ path.wstring(), path.filename().wstring() } }, 0, true);
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
                "presenter %s direct %lld uploaded %lld cuda_zero_copy %lld cuda_copied %lld | renderer %s | fps filter_out %s displayed %s | params %s | err %s\n",
                name, p.Summary().c_str(), p.cost.hostCopies, p.cost.deviceCopies, v.FramesHwDirect.value_or(-1), v.FramesHwDownload.value_or(-1),
                v.FramesSoftware.value_or(-1), v.FramesSinkDownload.value_or(-1), d.Presenter.Name.c_str(), d.Presenter.FramesDirect.value_or(-1),
                d.Presenter.FramesUploaded.value_or(-1), d.Presenter.FramesCudaZeroCopy.value_or(-1), d.Presenter.FramesCudaCopied.value_or(-1),
                d.Renderer.c_str(), Fps(d.Fps.FilterOut).c_str(), Fps(d.Fps.Displayed).c_str(),
                d.Presenter.Params.c_str(), d.Presenter.Error.empty() ? "-" : d.Presenter.Error.c_str());
    std::fflush(stdout);
}

void CheckMeasuredAgainstPlan(const Plan& p, const RunResult& r) {
    REQUIRE_MSG(r.opened, "open failed");
    const DebugInfo& d = r.d;
    REQUIRE_MSG(d.Valid && d.HasVideo, "no debug info with decoded frames");
    const DebugVideo& v = d.Video;
    CHECK_EQ(r.presenterErrors, 0);
    CHECK_MSG(d.Presenter.Error.empty(), d.Presenter.Error);
    if (p.presenter == "d3d11") CHECK_MSG(d.Renderer.rfind("d3d11", 0) == 0, "renderer " + d.Renderer);
    else CHECK_MSG(d.Renderer.rfind("sdl", 0) == 0, "renderer " + d.Renderer);
    if (d.Presenter.Present) CHECK_EQ(d.Presenter.Name, p.presenter);
    if (p.cost.hostCopies == 0) {
        CHECK_EQ(v.FramesHwDownload.value_or(0), 0LL);
        CHECK_EQ(v.FramesSinkDownload.value_or(0), 0LL);
        CHECK_EQ(d.Presenter.FramesUploaded.value_or(0), 0LL);
        CHECK(v.FramesHwDirect.value_or(0) > 0);
    } else {
        CHECK(v.FramesHwDownload.value_or(0) + v.FramesSinkDownload.value_or(0) + d.Presenter.FramesUploaded.value_or(0) + v.FramesSoftware.value_or(0) > 0);
    }
    for (const auto& step : p.steps) {
        if (step.kind != PlanStep::Kind::Present || step.name != "present:d3d11" || step.in != FrameDomain::Cuda) continue;
        if (step.copy == CopyKind::None) {
            CHECK_EQ(d.Presenter.FramesCudaCopied.value_or(-1), 0LL);
            CHECK(d.Presenter.FramesCudaZeroCopy.value_or(0) > 0);
        } else if (step.copy == CopyKind::Device) {
            CHECK(d.Presenter.FramesCudaCopied.value_or(0) > 0);
        }
    }
}

bool HasStep(const Plan& p, const std::string& name) {
    for (const auto& s : p.steps) if (s.name == name) return true;
    return false;
}

}

// MPEG-2 1080i telecined + Deinterlace=Ivtc
TEST_CASE_INTEGRATION(PipelineReal_a_mpeg2_1080i_ivtc) {
    EnsureDummyAudio();
    Watchdog wd("PipelineReal_a");
    fs::path path = Media(L"mpeg2-telecine-1080i.ts");
    REQUIRE_MSG(fs::exists(path), "missing testmedia\\hw\\mpeg2-telecine-1080i.ts");
    auto caps = FfPlayer::ProbeCaps();
    REQUIRE_MSG(caps.has_value(), "no caps (libffplay API < 3)");
    ContentInfo c = LearnContent(path);
    REQUIRE_MSG(c.Known(), "cannot read the stream info");
    FilterManager fm;
    fm.SetPlanPreferences(DecoderPref::Auto, "", PresenterPref::Auto);
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
    EnsureDummyAudio();
    Watchdog wd(row);
    fs::path path = Media(L"h264-1920x1080i-2997.ts");
    REQUIRE_MSG(fs::exists(path), "missing testmedia\\hw\\h264-1920x1080i-2997.ts");
    auto caps = FfPlayer::ProbeCaps();
    REQUIRE_MSG(caps.has_value(), "no caps (libffplay API < 3)");
    ContentInfo c = LearnContent(path);
    REQUIRE_MSG(c.Known(), "cannot read the stream info");
    FilterManager fm;
    fm.SetPlanPreferences(DecoderPref::Auto, "", PresenterPref::Auto);
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
    EnsureDummyAudio();
    Watchdog wd("InterlacedFps_2997i_bwdif_send_frame_and_send_field", 90);
    const fs::path path = Media(L"h264-1920x1080i-2997.ts");
    REQUIRE_MSG(fs::exists(path), "missing 29.97i interlaced fixture");
    for (bool hw : {false, true}) {
        for (bool field : {false, true}) {
            Host host;
            int err = 0;
            auto player = FfPlayer::CreateSync(host.child, host.parent, kRealNotifyMsg, &err);
            REQUIRE_MSG(player != nullptr, "CreateSync failed: " + std::to_string(err));
            int prepared = 0, errors = 0;
            player->Prepared.add([&](const MediaPreparedInfo&) { ++prepared; });
            player->Error.add([&](const PlayerErrorInfo&) { ++errors; });
            OpenOptions opts = {
                { L"presenter", L"d3d11" },
                { L"vf", field ? L"bwdif=mode=send_field:parity=auto:deint=all"
                                 : L"bwdif=mode=send_frame:parity=auto:deint=all" },
            };
            if (hw) opts.emplace_back(L"hwaccel", L"d3d11va");
            REQUIRE(player->Open(path.wstring(), opts) == 0);
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

// CUDA deinterlacing of the same broadcast 1920x1080i fixture: both output
// cadences stay on-device through the D3D11 CUDA-pool import.
TEST_CASE_INTEGRATION(InterlacedFps_2997i_bwdif_cuda_send_frame_and_send_field) {
    EnsureDummyAudio();
    Watchdog wd("InterlacedFps_2997i_bwdif_cuda_send_frame_and_send_field", 90);
    const fs::path path = Media(L"h264-1920x1080i-2997.ts");
    REQUIRE_MSG(fs::exists(path), "missing 29.97i interlaced fixture");
    auto caps = FfPlayer::ProbeCaps();
    REQUIRE_MSG(caps.has_value(), "cannot probe CUDA caps");
    const auto* dec = caps->Decoder("h264", "cuda");
    const auto* present = caps->Presenter("d3d11");
    if (!caps->DeviceOk("cuda") || !dec || !dec->ok || !caps->Filter("bwdif_cuda") ||
        !caps->HasFeature("hwframes_keep") || !present || !present->ok ||
        !present->importCuda || !present->importCudaZeroCopy) {
        std::printf("INTERLACEDFPS | cuda SKIP: caps lack CUDA decode/bwdif_cuda/hwframes_keep/D3D11 zero-copy import\n");
        std::fflush(stdout);
        return;
    }
    for (bool field : {false, true}) {
        Host host;
        int err = 0;
        auto player = FfPlayer::CreateSync(host.child, host.parent, kRealNotifyMsg, &err);
        REQUIRE_MSG(player != nullptr, "CreateSync failed: " + std::to_string(err));
        int prepared = 0, errors = 0;
        player->Prepared.add([&](const MediaPreparedInfo&) { ++prepared; });
        player->Error.add([&](const PlayerErrorInfo&) { ++errors; });
        OpenOptions opts = {
            { L"hwaccel", L"cuda" }, { L"hwframes", L"keep" }, { L"presenter", L"d3d11" },
            { L"vf", field ? L"bwdif_cuda=mode=send_field:parity=auto:deint=all"
                            : L"bwdif_cuda=mode=send_frame:parity=auto:deint=all" },
        };
        REQUIRE(player->Open(path.wstring(), opts) == 0);
        REQUIRE_MSG(WaitFor([&] { return prepared || errors; }, 12) && !errors,
                    "29.97i CUDA playback did not prepare");
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
        }, 4.3), "insufficient CUDA displayed frames for 29.97i rate measurement");
        std::printf("INTERLACEDFPS | cuda %s | filter %s displayed %s | hw_download %lld sink_download %lld uploaded %lld cuda_zero_copy %lld cuda_copied %lld\n",
                    field ? "send_field" : "send_frame", Fps(d.Fps.FilterOut).c_str(), Fps(d.Fps.Displayed).c_str(),
                    d.Video.FramesHwDownload.value_or(-1), d.Video.FramesSinkDownload.value_or(-1),
                    d.Presenter.FramesUploaded.value_or(-1), d.Presenter.FramesCudaZeroCopy.value_or(-1),
                    d.Presenter.FramesCudaCopied.value_or(-1));
        std::fflush(stdout);
        REQUIRE(d.Fps.FilterOut.has_value());
        REQUIRE(d.Fps.Displayed.has_value());
        CHECK_MSG(std::fabs(*d.Fps.FilterOut - expected) <= expected * 0.10,
                  "filter_out " + Fps(d.Fps.FilterOut));
        CHECK_MSG(std::fabs(*d.Fps.Displayed - expected) <= expected * 0.10,
                  "displayed " + Fps(d.Fps.Displayed));
        CHECK_EQ(d.Video.HwaccelDevice, std::string("cuda"));
        CHECK_EQ(d.Video.FramesHwDownload.value_or(-1), 0LL);
        CHECK_EQ(d.Video.FramesSinkDownload.value_or(-1), 0LL);
        CHECK_EQ(d.Presenter.FramesUploaded.value_or(-1), 0LL);
        CHECK_EQ(d.Presenter.FramesCudaCopied.value_or(-1), 0LL);
        CHECK(d.Presenter.FramesCudaZeroCopy.value_or(0) > 0);
        player->Stop();
        player.reset();
    }
}

// HEVC 10-bit + eq (brightness 0.1, contrast 1.2, gamma 1) + rotation 90 absorbed by the presenter
TEST_CASE_INTEGRATION(PipelineReal_c_hevc10_eq_rotate_absorbed) {
    EnsureDummyAudio();
    Watchdog wd("PipelineReal_c");
    fs::path path = Media(L"hevc-10bit-1080p.mp4");
    REQUIRE_MSG(fs::exists(path), "missing testmedia\\hw\\hevc-10bit-1080p.mp4");
    auto caps = FfPlayer::ProbeCaps();
    REQUIRE_MSG(caps.has_value(), "no caps (libffplay API < 3)");
    ContentInfo c = LearnContent(path);
    REQUIRE_MSG(c.Known(), "cannot read the stream info");
    FilterManager fm;
    fm.SetPlanPreferences(DecoderPref::Auto, "", PresenterPref::Auto);
    fm.SetCapabilities(*caps);
    fm.SetContent(c);
    FilterSettings s;
    s.brightness = 0.1; s.contrast = 1.2; s.gamma = 1.0;
    s.rotation = Rotation::Cw90;
    fm.Apply(s);
    REQUIRE(fm.PlannerActive());
    Plan p = *fm.CurrentPlan();
    REQUIRE_MSG(p.ok, p.error);
    CHECK_EQ(p.hwaccel, std::string("d3d11va"));
    CHECK(p.hwframesKeep);
    CHECK(HasStep(p, "absorbed:transpose"));
    CHECK(HasStep(p, "absorbed:eq"));
    CHECK(HasStep(p, "present:d3d11"));
    CHECK_EQ(p.cost.hostCopies, 0);
    CHECK_EQ(p.cost.deviceCopies, 0);
    RunResult r = PlayPlanned(fm, path, 4.0);
    PrintRow("c-eq-rot", p, r);
    CheckMeasuredAgainstPlan(p, r);
    // the presenter really got the parameters: from the open option (first frame already has them)
    CHECK_MSG(r.d.Presenter.Params.find("\"rotation\":90") != std::string::npos, r.d.Presenter.Params);
    CHECK_MSG(r.d.Presenter.Params.find("\"eq\":{") != std::string::npos, r.d.Presenter.Params);
}

TEST_CASE_INTEGRATION(PipelineReal_set_presenter_params_binding) {
    EnsureDummyAudio();
    Watchdog wd("PipelineReal_set_presenter_params_binding");
    fs::path path = Media(L"hevc-10bit-1080p.mp4");
    REQUIRE_MSG(fs::exists(path), "missing sample");
    auto caps = FfPlayer::ProbeCaps();
    REQUIRE_MSG(caps.has_value(), "no caps (libffplay API < 3)");
    const PresenterCap* d3 = caps->Presenter("d3d11");
    REQUIRE_MSG(d3 && d3->ok, "d3d11 presenter unavailable");
    Host host;
    int err = 0;
    auto player = FfPlayer::CreateSync(host.child, host.parent, kRealNotifyMsg, &err);
    REQUIRE(player != nullptr);
    PresenterParams rot;
    rot.rotation = 90;
    // before the first open the SDL presenter is in effect: identity is accepted, anything else refused (state unchanged)
    CHECK_EQ(player->SetPresenterParams(PresenterParams{}), 0);
    CHECK_EQ(player->SetPresenterParams(rot), -22);
    int prepared = 0;
    player->Prepared.add([&](const MediaPreparedInfo&) { prepared++; });
    REQUIRE(player->Open(path.wstring(), { { L"hwaccel", L"d3d11va" }, { L"hwframes", L"keep" }, { L"presenter", L"d3d11" } }) == 0);
    REQUIRE(WaitFor([&] { return prepared > 0; }, 15));
    CHECK_EQ(player->SetPresenterParams(rot), 0);
    PresenterParams eq;
    eq.eq = true; eq.brightness = 0.1; eq.contrast = 1.2; eq.saturation = 1; eq.gamma = 1;
    CHECK_EQ(player->SetPresenterParams(eq), 0);
    // With the shader route (caps shader:true) gamma, eq beyond the VP ranges and flips are accepted (route "shader");
    // without it they are refused and the state is unchanged.
    const int beyondVp = d3->shader ? 0 : -22;
    PresenterParams gamma = eq;
    gamma.gamma = 1.5;
    CHECK_EQ(player->SetPresenterParams(gamma), beyondVp);
    PresenterParams wide = eq;
    wide.brightness = 0.5;
    CHECK_EQ(player->SetPresenterParams(wide), beyondVp);
    PresenterParams flip;
    flip.flipH = true;
    const bool flipOk = d3->vpMirror || d3->shader;
    CHECK_EQ(player->SetPresenterParams(flip), flipOk ? 0 : -22);
    PresenterParams bad = eq;
    bad.rotation = 45;
    CHECK_EQ(player->SetPresenterParams(bad), -22);
    WaitFor([&] { return false; }, 0.5);
    auto js = player->GetDebugInfoJson();
    REQUIRE(js.has_value());
    DebugInfo di = ParseDebugInfo(*js);
    // the last accepted call is in effect: the flip, or (no shader / mirror) the eq call
    if (flipOk) CHECK_MSG(di.Presenter.Params.find("\"flip_h\":true") != std::string::npos, di.Presenter.Params);
    else CHECK_MSG(di.Presenter.Params.find("\"brightness\":0.1") != std::string::npos, di.Presenter.Params);
    CHECK_MSG(di.Presenter.Params.find("\"rotation\":0") != std::string::npos, di.Presenter.Params);
    CHECK_EQ(player->SetPresenterParams(PresenterParams{}), 0);
    player->Stop();
    player.reset();
}

// A forced presenter failure raises PresenterError, falls back to SDL and keeps playing (libffplay test hook).
TEST_CASE_INTEGRATION(PipelineReal_presenter_error_event_and_fallback) {
    EnsureDummyAudio();
    Watchdog wd("PipelineReal_presenter_error_event_and_fallback");
    fs::path path = Media(L"hevc-10bit-1080p.mp4");
    REQUIRE_MSG(fs::exists(path), "missing sample");
    auto caps = FfPlayer::ProbeCaps();
    REQUIRE_MSG(caps.has_value(), "no caps (libffplay API < 3)");
    const PresenterCap* d3 = caps->Presenter("d3d11");
    REQUIRE_MSG(d3 && d3->ok, "d3d11 presenter unavailable");
    Host host;
    int err = 0;
    auto player = FfPlayer::CreateSync(host.child, host.parent, kRealNotifyMsg, &err);
    REQUIRE(player != nullptr);
    std::vector<PresenterErrorInfo> errs;
    int prepared = 0;
    player->Prepared.add([&](const MediaPreparedInfo&) { prepared++; });
    player->PresenterError.add([&](const PresenterErrorInfo& e) { errs.push_back(e); });
    REQUIRE(player->Open(path.wstring(), { { L"hwaccel", L"d3d11va" }, { L"hwframes", L"keep" }, { L"presenter", L"d3d11" }, { L"presenter_debug_fail", L"display" } }) == 0);
    REQUIRE(WaitFor([&] { return prepared > 0; }, 15));
    REQUIRE(WaitFor([&] { return !errs.empty(); }, 10));
    CHECK_EQ(errs[0].Presenter, std::wstring(L"d3d11"));
    CHECK_EQ(errs[0].Fallback, std::wstring(L"sdl"));
    CHECK(!errs[0].Error.empty());
    WaitFor([&] { return false; }, 1.0);
    auto js = player->GetDebugInfoJson();
    REQUIRE(js.has_value());
    DebugInfo di = ParseDebugInfo(*js);
    CHECK(!di.Presenter.Error.empty());
    CHECK_EQ(di.Presenter.Name, std::string("sdl"));
    CHECK_MSG(di.Renderer.rfind("sdl", 0) == 0, di.Renderer);
    player->Stop();
    player.reset();
}

// Software decoding + Deinterlace=Ivtc + Direct3D 11 output, on the telecined MPEG-2 clip and the 1080i broadcast fixture:
// the CPU recipe (every GPU ivtc blacklisted) and the cheapest plan (`sw > hwupload > ivtc_<hw> > present:d3d11`) both
// display ~23.976 fps on the d3d11 presenter, and the copy counters are the planned ones.
TEST_CASE_INTEGRATION(PipelineReal_d_software_ivtc_d3d11) {
    EnsureDummyAudio();
    Watchdog wd("PipelineReal_d", 150);
    auto caps = FfPlayer::ProbeCaps();
    REQUIRE_MSG(caps.has_value(), "no caps (libffplay API < 3)");
    const PresenterCap* d3 = caps->Presenter("d3d11");
    REQUIRE_MSG(d3 && d3->ok, "d3d11 presenter unavailable");
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
            fm.SetPlanPreferences(DecoderPref::Software, "", PresenterPref::D3D11);
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
            CHECK_EQ(p.presenter, std::string("d3d11"));
            if (cpu) CHECK_MSG(HasStep(p, "fieldmatch,bwdif,decimate"), p.Summary());
            RunResult r = PlayPlanned(fm, path, 4.0);
            PrintRow(cpu ? "d-cpu-ivtc" : "d-ivtc", p, r);
            CheckMeasuredAgainstPlan(p, r);
            const DebugInfo& d = r.d;
            REQUIRE(d.Fps.FilterOut.has_value() && d.Fps.Displayed.has_value());
            CHECK_MSG(std::fabs(*d.Fps.FilterOut - 23.976) < 0.5, "fps filter_out " + Fps(d.Fps.FilterOut));
            CHECK_MSG(std::fabs(*d.Fps.Displayed - 23.976) < 23.976 * 0.10, "fps displayed " + Fps(d.Fps.Displayed));
            CHECK_MSG(d.Fps.FramesDisplayed.value_or(0) >= 40, "displayed frames " + std::to_string(d.Fps.FramesDisplayed.value_or(-1)));
            CHECK_EQ(d.Presenter.Name, std::string("d3d11"));
            CHECK_EQ(d.Video.FramesHwDownload.value_or(-1), 0LL);
            CHECK_EQ(d.Video.FramesSinkDownload.value_or(-1), 0LL);
            CHECK_EQ(p.cost.hostCopies, 1);
            const PlanStep& present = p.steps.back();
            REQUIRE(present.kind == PlanStep::Kind::Present);
            if (present.in == FrameDomain::System) {
                // the one planned host copy is the presenter's upload of each displayed frame
                CHECK(d.Presenter.FramesUploaded.value_or(0) > 0);
                CHECK(d.Presenter.FramesUploaded.value_or(0) + 2 >= d.Fps.FramesDisplayed.value_or(0));
                CHECK_EQ(d.Presenter.FramesCudaZeroCopy.value_or(0), 0LL);
            } else {
                // the one planned host copy is the hwupload in the graph; the presenter shows the hw frames as they are
                CHECK_EQ(d.Presenter.FramesUploaded.value_or(-1), 0LL);
                if (present.in == FrameDomain::Cuda && present.copy == CopyKind::None)
                    CHECK(d.Presenter.FramesCudaZeroCopy.value_or(0) > 0);
            }
        }
    }
}
