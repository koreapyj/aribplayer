// The D3D11 presenter's routes and diagnostics on a hidden host: zero-copy D3D11VA, the forced shader route, the readback
// against the composed frame, injected failures and the device recovery at the next open.
#include <cstdio>
#include <cstdlib>

#include "PresenterTestKit.h"

using namespace ptk;
using wvp::win32::PresenterOptions;

namespace {

fs::path Testsrc() {
    const fs::path a = RepoRoot() / L"testmedia" / L"testsrc2-720p.mp4";
    if (fs::exists(a)) return a;
    return RepoRoot() / L"testmedia" / L"testsrc2-360p-short.mp4";
}

struct Presented {
    uint64_t direct = 0, uploaded = 0;      // engine: presenter.frames_direct / frames_uploaded
    uint64_t shader = 0, d3d11Copied = 0;
    std::string route, renderer, error;
    std::vector<std::string> fallbacks;
};

Presented ReadPresented(const Value& debug) {
    Presented p;
    const Value* pres = Member(&debug, "presenter");
    const Value* stats = Stats(debug);
    p.direct = Count(pres, "frames_direct");
    p.uploaded = Count(pres, "frames_uploaded");
    p.error = pres ? pres->Str("error") : std::string();
    p.shader = Count(stats, "frames_shader");
    p.d3d11Copied = Count(stats, "frames_d3d11_copied");
    p.route = stats ? stats->Str("route") : std::string();
    p.renderer = stats ? stats->Str("renderer") : std::string();
    if (const Value* f = Member(stats, "fallbacks"); f && f->IsArray())
        for (const Value& v : f->arr) p.fallbacks.push_back(v.s);
    return p;
}

void Print(const char* name, const Presented& p) {
    std::printf("PRESENTER | %-12s | renderer %s route %s | direct %llu uploaded %llu | shader %llu d3d11_copied %llu | error %s\n",
                name, p.renderer.c_str(), p.route.c_str(), (unsigned long long)p.direct, (unsigned long long)p.uploaded,
                (unsigned long long)p.shader, (unsigned long long)p.d3d11Copied, p.error.empty() ? "-" : p.error.c_str());
    std::fflush(stdout);
}

// Plays until the presenter showed `frames` pictures; the last debug sample.
std::optional<Value> PlayFrames(Session& s, uint64_t frames, double seconds) {
    std::optional<Value> last;
    WaitFor([&] {
        last = s.Debug();
        if (!last) return false;
        const Presented p = ReadPresented(*last);
        return p.direct + p.uploaded >= frames || !p.error.empty();
    }, seconds);
    return last;
}

bool Has(const std::vector<std::string>& v, const std::string& s) {
    for (const auto& x : v)
        if (x == s) return true;
    return false;
}

}

TEST_CASE_INTEGRATION(Presenter_d3d11va_frames_are_shown_without_a_copy) {
    if (!fs::exists(Testsrc())) throw wvptest::SkipFailure{ "testsrc2 fixture is not present" };
    Session s;
    REQUIRE_MSG(s.Open(Testsrc(), "hwaccel=d3d11va"), "open failed");
    auto d = PlayFrames(s, 30, 15.0);
    REQUIRE_MSG(d.has_value(), "no debug info");
    const Presented p = ReadPresented(*d);
    Print("d3d11va", p);
    REQUIRE_MSG(p.error.empty(), p.error);
    const Value* video = Member(&*d, "video");
    if (!video || video->Str("hwaccel") != "d3d11va")
        throw wvptest::SkipFailure{ "no D3D11VA decoding of the fixture on this GPU" };
    CHECK(p.direct >= 30);
    CHECK_EQ(p.uploaded, 0ULL);
    CHECK_EQ(p.d3d11Copied, 0ULL);
    CHECK_EQ(Count(Member(video, "frame_path_counts"), "hw_download"), 0ULL);
}

TEST_CASE_INTEGRATION(Presenter_software_frames_are_uploaded_once) {
    if (!fs::exists(Testsrc())) throw wvptest::SkipFailure{ "testsrc2 fixture is not present" };
    Session s;
    REQUIRE_MSG(s.Open(Testsrc(), ""), "open failed");
    auto d = PlayFrames(s, 30, 15.0);
    REQUIRE_MSG(d.has_value(), "no debug info");
    const Presented p = ReadPresented(*d);
    Print("software", p);
    REQUIRE_MSG(p.error.empty(), p.error);
    CHECK(p.uploaded >= 30);
    CHECK_EQ(p.direct, 0ULL);
    CHECK_EQ(p.route, std::string("vp"));
}

TEST_CASE_INTEGRATION(Presenter_forced_shader_route_is_used_and_reported) {
    if (!fs::exists(Testsrc())) throw wvptest::SkipFailure{ "testsrc2 fixture is not present" };
    PresenterOptions o;
    o.route = "shader";
    Session s(o);
    REQUIRE_MSG(s.Open(Testsrc(), "hwaccel=d3d11va"), "open failed");
    auto d = PlayFrames(s, 30, 15.0);
    REQUIRE_MSG(d.has_value(), "no debug info");
    const Presented p = ReadPresented(*d);
    Print("shader", p);
    REQUIRE_MSG(p.error.empty(), p.error);
    CHECK_EQ(p.route, std::string("shader"));
    CHECK(p.shader >= 30);
    CHECK_MSG(Has(p.fallbacks, "route_forced:shader"), "fallbacks lack route_forced:shader");
}

// The snapshot (presenter readback, the frame as shown) matches the video rectangle of the composed back buffer once the
// host is sized to the snapshot, so both are rendered at the same size.
TEST_CASE_INTEGRATION(Presenter_readback_matches_the_composed_frame) {
    if (!fs::exists(Testsrc())) throw wvptest::SkipFailure{ "testsrc2 fixture is not present" };
    Session s;
    REQUIRE_MSG(s.Open(Testsrc(), "hwaccel=d3d11va"), "open failed");
    s.d3d11->EnableComposedReadback(true);
    auto d = PlayFrames(s, 10, 15.0);
    REQUIRE_MSG(d.has_value() && ReadPresented(*d).error.empty(), "presenter failed");
    REQUIRE(ffp_pause(s.player) == 0);
    REQUIRE(WaitFor([&] { auto x = s.Debug(); return x && x->Bool("paused"); }, 5.0));
    uint8_t* bgra = nullptr;
    int w = 0, h = 0, stride = 0;
    REQUIRE_MSG(ffp_snapshot(s.player, &bgra, &w, &h, &stride) == 0 && bgra, "snapshot failed");
    std::vector<uint8_t> snap(bgra, bgra + static_cast<size_t>(stride) * h);
    ffp_free(bgra);
    const size_t reports = s.watch->Reports();
    REQUIRE(s.host.Resize(w, h));
    // the presenter redraws the held picture at the new size, captures it, then reports the size through presented()
    REQUIRE_MSG(s.watch->WaitForSize(reports, w, h, std::chrono::seconds(30)), "no redraw at the snapshot size after the host resize");
    const std::optional<wvp::win32::ComposedFrame> f = s.d3d11->ComposedReadback();
    REQUIRE_MSG(f && f->videoWidth == w && f->videoHeight == h, "the composed frame is not the redraw at the snapshot size");
    double diff = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            for (int c = 0; c < 3; c++) {
                const int a = snap[static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4 + c];
                const int b = f->bgra[static_cast<size_t>(y + f->videoY) * f->stride + static_cast<size_t>(x + f->videoX) * 4 + c];
                diff += std::abs(a - b);
            }
    diff /= double(w) * h * 3;
    std::printf("PRESENTER | readback     | %dx%d mean abs diff %.3f\n", w, h, diff);
    CHECK_MSG(diff < 2.0, "snapshot and composed frame differ (mean abs diff " + std::to_string(diff) + ")");
}

TEST_CASE_INTEGRATION(Presenter_injected_failure_raises_presenter_error_and_playback_goes_on) {
    if (!fs::exists(Testsrc())) throw wvptest::SkipFailure{ "testsrc2 fixture is not present" };
    PresenterOptions o;
    o.fail = "display";
    Session s(o);
    REQUIRE_MSG(s.Open(Testsrc(), "hwaccel=d3d11va"), "open failed");
    REQUIRE_MSG(WaitFor([&] { return s.events.presenterErrors.load() > 0; }, 10.0), "no FFP_EVENT_PRESENTER_ERROR");
    std::string json;
    {
        std::lock_guard<std::mutex> g(s.events.mu);
        json = s.events.lastPresenterError;
    }
    auto e = wvp::json::Parse(json);
    REQUIRE_MSG(e.has_value(), "unparsable presenter error " + json);
    CHECK_EQ(e->Str("presenter"), std::string("d3d11"));
    CHECK(!e->Str("error").empty());
    CHECK(!e->Bool("device_lost"));
    // pictures are still timed and dropped: the position moves on
    const double start = ffp_get_position(s.player);
    CHECK_MSG(WaitFor([&] { return ffp_get_position(s.player) > start + 0.5; }, 5.0), "playback stopped with the presenter");
}

// A (simulated) device loss is reported as such; the next open recovers on a new device and shows pictures again.
TEST_CASE_INTEGRATION(Presenter_device_loss_is_recovered_at_the_next_open) {
    if (!fs::exists(Testsrc())) throw wvptest::SkipFailure{ "testsrc2 fixture is not present" };
    PresenterOptions o;
    o.fail = "device_lost";
    Session s(o);
    REQUIRE_MSG(s.Open(Testsrc(), "hwaccel=d3d11va"), "open failed");
    REQUIRE_MSG(WaitFor([&] { return s.events.presenterErrors.load() > 0; }, 10.0), "no FFP_EVENT_PRESENTER_ERROR");
    std::string json;
    {
        std::lock_guard<std::mutex> g(s.events.mu);
        json = s.events.lastPresenterError;
    }
    auto e = wvp::json::Parse(json);
    REQUIRE_MSG(e.has_value(), "unparsable presenter error " + json);
    CHECK(e->Bool("device_lost"));
    CHECK(!e->Str("removed_reason").empty());
    char reason[32] = "";
    CHECK(s.presenter->Native()->device_lost(s.presenter->Native()->opaque, reason, sizeof reason) == 1);

    s.events.prepared = 0;
    REQUIRE_MSG(s.Open(Testsrc(), "hwaccel=d3d11va"), "reopen failed");
    auto d = PlayFrames(s, 20, 15.0);
    REQUIRE_MSG(d.has_value(), "no debug info after the reopen");
    const Presented p = ReadPresented(*d);
    Print("recovered", p);
    CHECK_MSG(p.error.empty(), p.error);
    CHECK(p.direct + p.uploaded >= 20);
    CHECK(s.presenter->Native()->device_lost(s.presenter->Native()->opaque, reason, sizeof reason) == 0);
}
