// Fixture-optional hidden-host D3D11 test for libffplay's Acrylic backdrop regions (ffp_set_backdrop_regions).
//
// Platform note: the acrylic pass samples the presenter's composed backbuffer, so it is independent of how the frame
// reached the GPU. The test requests hwaccel=d3d11va and reports video.frame_path; "hw_direct" is the zero-copy path
// (decoder texture sampled directly). GPUs/drivers without D3D11VA zero-copy are skipped.
#include "wvptest.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "ffplay.h"
#include "wvp/JsonLite.h"
#include "wvp/PlayerTypes.h"
#include "wvp/Utf.h"

namespace fs = std::filesystem;
using namespace wvp;

namespace {

constexpr wchar_t kHostClass[] = L"WvpBackdropAcrylicTestHost";

struct EventState {
    std::atomic<int> prepared{ 0 };
    std::atomic<int> errors{ 0 };
};

void FFP_CALL OnEvent(void* opaque, int event, const char*) {
    auto* s = static_cast<EventState*>(opaque);
    if (event == FFP_EVENT_PREPARED) ++s->prepared;
    else if (event == FFP_EVENT_ERROR) ++s->errors;
}

LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

void EnsureHostClass() {
    static const ATOM atom = [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = HostProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kHostClass;
        return RegisterClassW(&wc);
    }();
    (void)atom;
}

fs::path RepoRoot() {
    static const fs::path root = [] {
        wchar_t buffer[32768]{};
        GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(_countof(buffer)));
        fs::path dir = fs::path(buffer).parent_path();
        while (!dir.empty()) {
            if (fs::exists(dir / L"CMakePresets.json")) return dir;
            const fs::path parent = dir.parent_path();
            if (parent == dir) break;
            dir = parent;
        }
        return fs::current_path();
    }();
    return root;
}

fs::path FixturePath() {
    const fs::path a = RepoRoot() / L"testmedia" / L"testsrc2-720p.mp4";
    if (fs::exists(a)) return a;
    return RepoRoot() / L"testmedia" / L"testsrc2-360p-short.mp4";
}

void PumpMessages() {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
}

template <class P>
bool WaitFor(P&& pred, double seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<long long>(seconds * 1000.0));
    while (std::chrono::steady_clock::now() < deadline) {
        PumpMessages();
        if (pred()) return true;
        Sleep(10);
    }
    PumpMessages();
    return pred();
}

struct HiddenHost {
    HWND parent = nullptr, video = nullptr;
    HiddenHost() {
        EnsureHostClass();
        const HINSTANCE inst = GetModuleHandleW(nullptr);
        parent = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kHostClass, L"Backdrop host", WS_POPUP,
            -32000, -32000, 640, 360, nullptr, nullptr, inst, nullptr);
        if (parent) video = CreateWindowExW(0, L"STATIC", L"video", WS_CHILD | WS_VISIBLE, 0, 0, 640, 360, parent, nullptr, inst, nullptr);
    }
    ~HiddenHost() { if (parent) DestroyWindow(parent); }
};

struct RawPlayer {
    HiddenHost host;
    EventState events;
    FFPlayer* player = nullptr;

    RawPlayer() {
        REQUIRE_MSG(host.parent && host.video, "could not create hidden D3D11 test host");
        std::atomic<bool> done{ false };
        std::thread creator([&] {
            player = ffp_create(host.video, OnEvent, &events);
            done.store(true, std::memory_order_release);
        });
        while (!done.load(std::memory_order_acquire)) { PumpMessages(); Sleep(10); }
        creator.join();
        REQUIRE_MSG(player != nullptr, "ffp_create failed for hidden D3D11 test host");
    }
    ~RawPlayer() {
        if (player) {
            (void)ffp_test_d3d11_readback_enable(player, 0);
            ffp_destroy(player);
        }
    }
};

const json::Value* Member(const json::Value* o, std::string_view k) {
    return o && o->kind == json::Value::Kind::Object ? o->Find(k) : nullptr;
}

std::optional<json::Value> ReadDebug(FFPlayer* p) {
    char* t = ffp_get_debug_info(p);
    if (!t) return std::nullopt;
    std::string s(t);
    ffp_free(t);
    return json::Parse(s);
}

bool IsD3D11(const json::Value& d) {
    const json::Value* pr = Member(&d, "presenter");
    return pr && pr->Str("name") == "d3d11";
}

uint64_t ReadbackSeq(FFPlayer* p) {
    auto d = ReadDebug(p);
    const json::Value* ov = Member(Member(d ? &*d : nullptr, "presenter"), "overlay");
    const json::Value* v = Member(ov, "readback_seq");
    return v && v->kind == json::Value::Kind::Number ? static_cast<uint64_t>(std::max(0.0, v->n)) : 0;
}

struct Readback {
    int width = 0, height = 0, stride = 0, vx = 0, vy = 0, vw = 0, vh = 0;
    std::vector<uint8_t> px;   // BGRA
    const uint8_t* At(int x, int y) const { return px.data() + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4; }
};

std::optional<Readback> Capture(FFPlayer* p) {
    uint8_t* bytes = nullptr;
    Readback f;
    int rc = ffp_test_d3d11_readback_last(p, &bytes, &f.width, &f.height, &f.stride, &f.vx, &f.vy, &f.vw, &f.vh);
    if (rc != 0) return std::nullopt;
    if (!bytes || f.width <= 0 || f.height <= 0 || f.stride < f.width * 4) { ffp_free(bytes); return std::nullopt; }
    f.px.assign(bytes, bytes + static_cast<size_t>(f.stride) * f.height);
    ffp_free(bytes);
    return f;
}

// Waits for the presenter to redraw (readback_seq advances) after `afterSeq`, then captures that frame.
std::optional<Readback> CaptureAfterRedraw(FFPlayer* p, uint64_t afterSeq, double seconds) {
    std::optional<Readback> out;
    WaitFor([&] {
        if (ReadbackSeq(p) <= afterSeq) return false;
        out = Capture(p);
        return out.has_value();
    }, seconds);
    return out;
}

struct Region { int x, y, w, h; float radius; };

// Mean absolute neighbour difference (horizontal + vertical, RGB) over a rect: a high-frequency energy proxy.
double Energy(const Readback& f, int x0, int y0, int x1, int y1) {
    double sum = 0; size_t n = 0;
    for (int y = y0; y < y1 - 1; ++y)
        for (int x = x0; x < x1 - 1; ++x) {
            const uint8_t* a = f.At(x, y); const uint8_t* r = f.At(x + 1, y); const uint8_t* d = f.At(x, y + 1);
            for (int c = 0; c < 3; ++c) { sum += std::abs(int(a[c]) - int(r[c])) + std::abs(int(a[c]) - int(d[c])); ++n; }
        }
    return n ? sum / double(n) : 0.0;
}

void MeanColor(const Readback& f, int x0, int y0, int x1, int y1, double out[3]) {
    double s[3]{}; size_t n = 0;
    for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) { const uint8_t* p = f.At(x, y); for (int c = 0; c < 3; ++c) s[c] += p[c]; ++n; }
    for (int c = 0; c < 3; ++c) out[c] = n ? s[c] / double(n) : 0.0;
}

bool SameRGB(const Readback& a, const Readback& b, int x, int y) { return std::memcmp(a.At(x, y), b.At(x, y), 3) == 0; }

struct Snap { int w = 0, h = 0, stride = 0; std::vector<uint8_t> px; };
std::optional<Snap> TakeSnapshot(FFPlayer* p) {
    uint8_t* bytes = nullptr; Snap s;
    if (ffp_snapshot_ex(p, 0, &bytes, &s.w, &s.h, &s.stride) != 0 || !bytes || s.w <= 0 || s.h <= 0 || s.stride < s.w * 4) {
        ffp_free(bytes);
        return std::nullopt;
    }
    s.px.assign(bytes, bytes + static_cast<size_t>(s.stride) * s.h);
    ffp_free(bytes);
    return s;
}

}

TEST_CASE_INTEGRATION(Backdrop_acrylic_region_blurs_tints_and_clears_on_d3d11_presenter) {
    if (!fs::exists(FixturePath())) throw wvptest::SkipFailure{ "testsrc2 fixture is not present" };

    RawPlayer s;
    const std::string options = "presenter=d3d11\nhwaccel=d3d11va";
    REQUIRE_MSG(ffp_open(s.player, ToUtf8(FixturePath().wstring()).c_str(), options.c_str()) == 0, "testsrc2 open rejected");
    REQUIRE_MSG(WaitFor([&] { return s.events.prepared.load() > 0 || s.events.errors.load() > 0; }, 25.0), "no PREPARED event");
    REQUIRE_MSG(s.events.errors.load() == 0, "open raised a player error");

    std::optional<json::Value> dbg;
    REQUIRE_MSG(WaitFor([&] { dbg = ReadDebug(s.player); return dbg.has_value(); }, 5.0), "no debug info after PREPARED");
    if (!IsD3D11(*dbg)) throw wvptest::SkipFailure{ "D3D11 presenter unavailable on this host; libffplay stayed on SDL" };

    const int en = ffp_test_d3d11_readback_enable(s.player, 1);
    if (en == FFP_EAGAIN) throw wvptest::SkipFailure{ "D3D11 presenter went away before readback could be enabled" };
    REQUIRE_MSG(en == 0, "could not enable D3D11 composed readback");

    WaitFor([&] { return ReadbackSeq(s.player) >= 3; }, 15.0);
    {
        auto d = ReadDebug(s.player);
        if (!d || !IsD3D11(*d)) throw wvptest::SkipFailure{ "D3D11 presenter failed during playback" };
        if (ReadbackSeq(s.player) == 0) throw wvptest::SkipFailure{ "D3D11 composed readback produced no frames on this GPU/driver" };
        const json::Value* video = Member(&*d, "video");
        const std::string path = video ? video->Str("frame_path") : std::string();
        if (path != "hw_direct")
            throw wvptest::SkipFailure{ "D3D11VA zero-copy path unavailable; video.frame_path='" + path + "'" };
        std::printf("[backdrop] zero-copy proven: video.frame_path=hw_direct\n");
    }
    REQUIRE_MSG(ffp_pause(s.player) == 0, "could not pause");
    REQUIRE_MSG(WaitFor([&] { auto d = ReadDebug(s.player); return d && d->Bool("paused"); }, 5.0), "player did not report paused");
    Sleep(300);   // let any in-flight frame finish

    // Baseline: force a fresh redraw with an empty region set, so baseline and later frames come from the same path.
    uint64_t seq = ReadbackSeq(s.player);
    REQUIRE_MSG(ffp_set_backdrop_regions(s.player, nullptr, 0) == 0, "clearing backdrop regions failed");
    auto base = CaptureAfterRedraw(s.player, seq, 3.0);
    if (!base) base = Capture(s.player);   // a no-op clear may legitimately not redraw; use the last composed frame
    if (!base) throw wvptest::SkipFailure{ "D3D11 composed readback unavailable on this GPU/driver" };
    seq = ReadbackSeq(s.player);
    auto snapBefore = TakeSnapshot(s.player);
    REQUIRE_MSG(snapBefore.has_value(), "video-only snapshot unavailable before enabling Acrylic");

    const int W = base->width, H = base->height;
    REQUIRE_MSG(W >= 320 && H >= 180, "readback target too small");
    const Region r{ W / 4, H / 4, W / 2, H / 2, 40.0f };

    FFPBackdropRegion reg{};
    reg.x = r.x; reg.y = r.y; reg.width = r.w; reg.height = r.h;
    reg.corner_radius = r.radius;
    reg.tint_color = 0xFF2040FFu; reg.tint_opacity = 0.6f;
    reg.luminosity_color = 0xFF202020u; reg.luminosity_opacity = 0.3f;
    reg.noise = 0;
    reg.z = 0;
    REQUIRE_MSG(ffp_set_backdrop_regions(s.player, &reg, 1) == 0, "ffp_set_backdrop_regions rejected a valid region");

    auto fx = CaptureAfterRedraw(s.player, seq, 5.0);
    REQUIRE_MSG(fx.has_value(), "paused presenter did not redraw after backdrop regions changed");
    REQUIRE(fx->width == base->width && fx->height == base->height);
    REQUIRE_MSG(WaitFor([&] {
        auto d = ReadDebug(s.player);
        const json::Value* ac = Member(Member(d ? &*d : nullptr, "presenter"), "acrylic");
        const json::Value* regions = Member(ac, "regions");
        const json::Value* blur = Member(ac, "blur_ms");
        return regions && regions->kind == json::Value::Kind::Number && regions->n == 1 &&
               blur && blur->kind == json::Value::Kind::Number && blur->n > 0.0;
    }, 5.0), "debug JSON did not report one Acrylic region and non-zero GPU blur timing");

    // Interior inset past the corner radius so the rounded corners cannot skew the statistics.
    const int inset = static_cast<int>(r.radius) + 4;
    const int ix0 = r.x + inset, iy0 = r.y + inset, ix1 = r.x + r.w - inset, iy1 = r.y + r.h - inset;
    const double eBase = Energy(*base, ix0, iy0, ix1, iy1);
    const double eFx = Energy(*fx, ix0, iy0, ix1, iy1);
    CHECK_MSG(eBase > 1.0, "testsrc2 baseline has no high-frequency content in the region");
    CHECK_MSG(eFx < eBase * 0.7, "region high-frequency energy did not drop (base=" + std::to_string(eBase) + " fx=" + std::to_string(eFx) + ")");

    double mb[3], mf[3];
    MeanColor(*base, ix0, iy0, ix1, iy1, mb);
    MeanColor(*fx, ix0, iy0, ix1, iy1, mf);
    const double shift = std::abs(mb[0] - mf[0]) + std::abs(mb[1] - mf[1]) + std::abs(mb[2] - mf[2]);
    CHECK_MSG(shift > 12.0, "region mean colour did not change under Acrylic (shift=" + std::to_string(shift) + ")");
    CHECK_MSG(mf[0] - mb[0] > 10.0 && mf[0] - mb[0] > mf[1] - mb[1],
              "region mean did not shift toward the blue tint (base BGR=" + std::to_string(mb[0]) + "," +
              std::to_string(mb[1]) + "," + std::to_string(mb[2]) + " fx BGR=" + std::to_string(mf[0]) + "," +
              std::to_string(mf[1]) + "," + std::to_string(mf[2]) + ")");

    size_t outsideDiff = 0, outsideTotal = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const double hx = r.w * 0.5, hy = r.h * 0.5;
            const double radius = std::min<double>(r.radius, std::min(hx, hy));
            const double qx = std::abs((x + 0.5) - (r.x + hx)) - (hx - radius);
            const double qy = std::abs((y + 0.5) - (r.y + hy)) - (hy - radius);
            const double distance = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) +
                std::min(std::max(qx, qy), 0.0) - radius;
            if (distance <= 0.25) continue;
            ++outsideTotal;
            if (!SameRGB(*base, *fx, x, y)) ++outsideDiff;
        }
    CHECK_MSG(outsideDiff == 0, std::to_string(outsideDiff) + " of " + std::to_string(outsideTotal) + " pixels outside the rounded region changed");

    // Snapshot captures the video only, never the backdrop.
    auto snapWith = TakeSnapshot(s.player);
    REQUIRE_MSG(snapWith.has_value(), "video-only snapshot unavailable with Acrylic enabled");
    CHECK(snapBefore->w == snapWith->w && snapBefore->h == snapWith->h && snapBefore->stride == snapWith->stride);
    CHECK(snapBefore->px == snapWith->px);

    seq = ReadbackSeq(s.player);
    REQUIRE_MSG(ffp_set_backdrop_regions(s.player, nullptr, 0) == 0, "clearing backdrop regions failed");
    auto cleared = CaptureAfterRedraw(s.player, seq, 5.0);
    REQUIRE_MSG(cleared.has_value(), "paused presenter did not redraw after clearing backdrop regions");
    size_t restoredDiff = 0;
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) if (!SameRGB(*base, *cleared, x, y)) ++restoredDiff;
    CHECK_MSG(restoredDiff == 0, std::to_string(restoredDiff) + " pixels differ from baseline after clearing regions");
}
