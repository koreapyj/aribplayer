// The Acrylic backdrop of the D3D11 presenter (D3D11Presenter::SetBackdropRegions) on a hidden host.
//
// The acrylic pass samples the presenter's composed back buffer, so it is independent of how the frame reached the GPU.
// The test requests hwaccel=d3d11va and reports video.frame_path; "hw_direct" is the zero-copy path (decoder texture
// sampled directly). GPUs/drivers without D3D11VA zero-copy are skipped.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "PresenterTestKit.h"

using namespace ptk;
using wvp::win32::BackdropRegion;
using wvp::win32::ComposedFrame;

namespace {

fs::path FixturePath() {
    const fs::path a = RepoRoot() / L"testmedia" / L"testsrc2-720p.mp4";
    if (fs::exists(a)) return a;
    return RepoRoot() / L"testmedia" / L"testsrc2-360p-short.mp4";
}

const uint8_t* At(const ComposedFrame& f, int x, int y) {
    return f.bgra.data() + static_cast<size_t>(y) * f.stride + static_cast<size_t>(x) * 4;
}

// Waits for the presenter to redraw (readback_seq advances) after `afterSeq`, then captures that frame.
std::optional<ComposedFrame> CaptureAfterRedraw(Session& s, uint64_t afterSeq, double seconds) {
    std::optional<ComposedFrame> out;
    WaitFor([&] {
        if (ReadbackSeq(s) <= afterSeq) return false;
        out = s.d3d11->ComposedReadback();
        return out.has_value();
    }, seconds);
    return out;
}

// Mean absolute neighbour difference (horizontal + vertical, RGB) over a rect: a high-frequency energy proxy.
double Energy(const ComposedFrame& f, int x0, int y0, int x1, int y1) {
    double sum = 0;
    size_t n = 0;
    for (int y = y0; y < y1 - 1; ++y)
        for (int x = x0; x < x1 - 1; ++x) {
            const uint8_t* a = At(f, x, y);
            const uint8_t* r = At(f, x + 1, y);
            const uint8_t* d = At(f, x, y + 1);
            for (int c = 0; c < 3; ++c) {
                sum += std::abs(int(a[c]) - int(r[c])) + std::abs(int(a[c]) - int(d[c]));
                ++n;
            }
        }
    return n ? sum / double(n) : 0.0;
}

void MeanColor(const ComposedFrame& f, int x0, int y0, int x1, int y1, double out[3]) {
    double s[3]{};
    size_t n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const uint8_t* p = At(f, x, y);
            for (int c = 0; c < 3; ++c) s[c] += p[c];
            ++n;
        }
    for (int c = 0; c < 3; ++c) out[c] = n ? s[c] / double(n) : 0.0;
}

bool SameRGB(const ComposedFrame& a, const ComposedFrame& b, int x, int y) { return std::memcmp(At(a, x, y), At(b, x, y), 3) == 0; }

struct Snap { int w = 0, h = 0, stride = 0; std::vector<uint8_t> px; };
std::optional<Snap> TakeSnapshot(FFPlayer* p) {
    uint8_t* bytes = nullptr;
    Snap s;
    if (ffp_snapshot_ex(p, 0, &bytes, &s.w, &s.h, &s.stride) != 0 || !bytes || s.w <= 0 || s.h <= 0 || s.stride < s.w * 4) {
        ffp_free(bytes);
        return std::nullopt;
    }
    s.px.assign(bytes, bytes + static_cast<size_t>(s.stride) * s.h);
    ffp_free(bytes);
    return s;
}

}

TEST_CASE_INTEGRATION(Backdrop_acrylic_region_blurs_tints_and_clears) {
    if (!fs::exists(FixturePath())) throw wvptest::SkipFailure{ "testsrc2 fixture is not present" };

    Session s;
    REQUIRE_MSG(s.Open(FixturePath(), "hwaccel=d3d11va"), "testsrc2 open failed");
    s.d3d11->EnableComposedReadback(true);

    WaitFor([&] { return ReadbackSeq(s) >= 3; }, 15.0);
    {
        auto d = s.Debug();
        REQUIRE_MSG(d.has_value(), "no debug info after PREPARED");
        if (PresenterFailed(*d)) throw wvptest::SkipFailure{ "the D3D11 presenter failed during playback" };
        if (ReadbackSeq(s) == 0) throw wvptest::SkipFailure{ "D3D11 composed readback produced no frames on this GPU/driver" };
        const Value* video = Member(&*d, "video");
        const std::string path = video ? video->Str("frame_path") : std::string();
        if (path != "hw_direct")
            throw wvptest::SkipFailure{ "D3D11VA zero-copy path unavailable; video.frame_path='" + path + "'" };
        std::printf("[backdrop] zero-copy proven: video.frame_path=hw_direct\n");
    }
    REQUIRE_MSG(ffp_pause(s.player) == 0, "could not pause");
    REQUIRE_MSG(WaitFor([&] { auto d = s.Debug(); return d && d->Bool("paused"); }, 5.0), "player did not report paused");
    Sleep(300);   // let any in-flight frame finish

    // Baseline: force a fresh redraw with an empty region set, so baseline and later frames come from the same path.
    uint64_t seq = ReadbackSeq(s);
    REQUIRE_MSG(s.d3d11->SetBackdropRegions({}), "clearing backdrop regions failed");
    auto base = CaptureAfterRedraw(s, seq, 3.0);
    if (!base) base = s.d3d11->ComposedReadback();   // a no-op clear may not redraw; use the last composed frame
    if (!base) throw wvptest::SkipFailure{ "D3D11 composed readback unavailable on this GPU/driver" };
    seq = ReadbackSeq(s);
    auto snapBefore = TakeSnapshot(s.player);
    REQUIRE_MSG(snapBefore.has_value(), "video-only snapshot unavailable before enabling Acrylic");

    const int W = base->width, H = base->height;
    REQUIRE_MSG(W >= 320 && H >= 180, "readback target too small");
    struct { int x, y, w, h; float radius; } r{ W / 4, H / 4, W / 2, H / 2, 40.0f };

    BackdropRegion reg;
    reg.x = r.x; reg.y = r.y; reg.width = r.w; reg.height = r.h;
    reg.cornerRadius = r.radius;
    reg.tintColor = 0xFF2040FFu; reg.tintOpacity = 0.6f;
    reg.luminosityColor = 0xFF202020u; reg.luminosityOpacity = 0.3f;
    std::string error;
    REQUIRE_MSG(s.d3d11->SetBackdropRegions({ reg }, &error), "SetBackdropRegions rejected a valid region: " + error);

    auto fx = CaptureAfterRedraw(s, seq, 5.0);
    REQUIRE_MSG(fx.has_value(), "paused presenter did not redraw after backdrop regions changed");
    REQUIRE(fx->width == base->width && fx->height == base->height);
    REQUIRE_MSG(WaitFor([&] {
        auto d = s.Debug();
        const Value* ac = Member(d ? Stats(*d) : nullptr, "acrylic");
        return Number(ac, "regions").value_or(0) == 1 && Number(ac, "blur_ms").value_or(0) > 0.0;
    }, 5.0), "presenter stats did not report one Acrylic region and non-zero GPU blur timing");

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
            const double distance = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) + std::min(std::max(qx, qy), 0.0) - radius;
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

    seq = ReadbackSeq(s);
    REQUIRE_MSG(s.d3d11->SetBackdropRegions({}), "clearing backdrop regions failed");
    auto cleared = CaptureAfterRedraw(s, seq, 5.0);
    REQUIRE_MSG(cleared.has_value(), "paused presenter did not redraw after clearing backdrop regions");
    size_t restoredDiff = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (!SameRGB(*base, *cleared, x, y)) ++restoredDiff;
    CHECK_MSG(restoredDiff == 0, std::to_string(restoredDiff) + " pixels differ from baseline after clearing regions");
}
