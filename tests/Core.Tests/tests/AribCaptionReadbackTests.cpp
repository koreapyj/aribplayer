// Fixture-optional hidden-host D3D11 test: compare composed pixels with the ARIB caption layer hidden.
#include "wvptest.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "AribOutlineMeasure.h"
#include "ffplay.h"
#include "wvp/JsonLite.h"
#include "wvp/Utf.h"

namespace fs = std::filesystem;
using namespace wvp;

namespace {

constexpr wchar_t kReadbackHostClass[] = L"WvpAribReadbackTestHost";

struct EventState {
    std::atomic<int> prepared{ 0 };
    std::atomic<int> errors{ 0 };
};

void FFP_CALL OnFfplayEvent(void* opaque, int event, const char*) {
    auto* state = static_cast<EventState*>(opaque);
    if (event == FFP_EVENT_PREPARED) ++state->prepared;
    else if (event == FFP_EVENT_ERROR) ++state->errors;
}

LRESULT CALLBACK ReadbackHostProc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    return DefWindowProcW(hwnd, message, w, l);
}

void EnsureReadbackHostClass() {
    static const ATOM atom = [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = ReadbackHostProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kReadbackHostClass;
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

fs::path FixturePath() { return RepoRoot() / L"testmedia" / L"arib" / L"tbs-captions.ts"; }
fs::path FontPath() { return RepoRoot() / L"assets" / L"fonts" / L"WLCMARU2004ARIBU.TTF"; }

void PumpMessages() {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

template <class Predicate>
bool WaitFor(Predicate&& predicate, double seconds) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(static_cast<long long>(seconds * 1000.0));
    while (std::chrono::steady_clock::now() < deadline) {
        PumpMessages();
        if (predicate()) return true;
        Sleep(10);
    }
    PumpMessages();
    return predicate();
}

struct HiddenHost {
    HWND parent = nullptr;
    HWND video = nullptr;

    HiddenHost() {
        EnsureReadbackHostClass();
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        parent = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kReadbackHostClass,
            L"ARIB readback host", WS_POPUP, -32000, -32000, 640, 360,
            nullptr, nullptr, instance, nullptr);
        if (parent) {
            video = CreateWindowExW(0, L"STATIC", L"video", WS_CHILD | WS_VISIBLE,
                0, 0, 640, 360, parent, nullptr, instance, nullptr);
        }
    }

    ~HiddenHost() {
        if (parent) DestroyWindow(parent);
    }
};

struct RawPlayer {
    HiddenHost host;
    EventState events;
    FFPlayer* player = nullptr;

    RawPlayer() {
        REQUIRE_MSG(host.parent && host.video, "could not create hidden D3D11 test host");
        std::atomic<bool> createDone{ false };
        std::thread creator([&] {
            player = ffp_create(host.video, OnFfplayEvent, &events);
            createDone.store(true, std::memory_order_release);
        });
        while (!createDone.load(std::memory_order_acquire)) {
            PumpMessages();
            Sleep(10);
        }
        creator.join();
        REQUIRE_MSG(player != nullptr, "ffp_create failed for hidden D3D11 test host");
    }

    ~RawPlayer() {
        if (player) {
            (void)ffp_test_d3d11_readback_enable(player, 0);
            ffp_destroy(player);
        }
    }

    bool OpenFixture(const std::string& extraOptions = {}) {
        std::string options = "presenter=d3d11\nsuperimpose=0\narib_font=" + ToUtf8(FontPath().wstring()) + extraOptions;
        return ffp_open(player, ToUtf8(FixturePath().wstring()).c_str(), options.c_str()) == 0;
    }
};

const json::Value* Member(const json::Value* object, std::string_view key) {
    return object && object->kind == json::Value::Kind::Object ? object->Find(key) : nullptr;
}

std::optional<double> Number(const json::Value* object, std::string_view key) {
    const json::Value* value = Member(object, key);
    if (!value || value->kind != json::Value::Kind::Number) return std::nullopt;
    return value->n;
}

std::optional<json::Value> ReadDebug(FFPlayer* player) {
    char* text = ffp_get_debug_info(player);
    if (!text) return std::nullopt;
    std::string owned(text);
    ffp_free(text);
    return json::Parse(owned);
}

bool IsD3D11Presenter(const json::Value& debug) {
    const json::Value* presenter = Member(&debug, "presenter");
    return presenter && presenter->Str("name") == "d3d11";
}

// D3D-side facts from debug.presenter.overlay (the decoder-side subtitle_layers counts cannot prove a draw).
struct OverlayStats {
    bool present = false;
    uint64_t cueUploads = 0;
    uint64_t rectsUploaded = 0;
    uint64_t rectsDrawn = 0;
    uint64_t drawFailures = 0;
    uint64_t readbackFailures = 0;
    uint64_t readbackSeq = 0;    // successful readbacks; advances once per captured (redrawn) frame
    uint64_t readbackRects = 0;  // overlay rects drawn into the latest captured frame (same snapshot as readbackSeq)
};

OverlayStats ReadOverlayStats(const json::Value& debug) {
    OverlayStats stats;
    const json::Value* overlay = Member(Member(&debug, "presenter"), "overlay");
    if (!overlay || overlay->kind != json::Value::Kind::Object) return stats;
    const auto get = [&](std::string_view key) {
        return static_cast<uint64_t>(std::max(0.0, Number(overlay, key).value_or(0.0)));
    };
    stats.present = true;
    stats.cueUploads = get("cue_uploads");
    stats.rectsUploaded = get("rects_uploaded");
    stats.rectsDrawn = get("rects_drawn");
    stats.drawFailures = get("draw_failures");
    stats.readbackFailures = get("readback_failures");
    stats.readbackSeq = get("readback_seq");
    stats.readbackRects = get("readback_rects");
    return stats;
}

std::optional<OverlayStats> ReadOverlayStats(FFPlayer* player) {
    auto debug = ReadDebug(player);
    if (!debug) return std::nullopt;
    return ReadOverlayStats(*debug);
}

std::string Describe(const OverlayStats& s) {
    return "overlay{cue_uploads=" + std::to_string(s.cueUploads) + " rects_uploaded=" + std::to_string(s.rectsUploaded) +
           " rects_drawn=" + std::to_string(s.rectsDrawn) + " draw_failures=" + std::to_string(s.drawFailures) +
           " readback_failures=" + std::to_string(s.readbackFailures) + " readback_seq=" + std::to_string(s.readbackSeq) +
           " readback_rects=" + std::to_string(s.readbackRects) + "}";
}

struct Readback {
    int width = 0;
    int height = 0;
    int stride = 0;
    int videoX = 0;
    int videoY = 0;
    int videoWidth = 0;
    int videoHeight = 0;
    std::vector<uint8_t> pixels;
};

struct CaptureResult {
    int status = FFP_EAGAIN;
    std::optional<Readback> frame;
};

CaptureResult Capture(FFPlayer* player) {
    CaptureResult result;
    uint8_t* bytes = nullptr;
    Readback frame;
    result.status = ffp_test_d3d11_readback_last(player, &bytes, &frame.width, &frame.height, &frame.stride,
        &frame.videoX, &frame.videoY, &frame.videoWidth, &frame.videoHeight);
    if (result.status != 0) return result;
    if (!bytes || frame.width <= 0 || frame.height <= 0 || frame.width > std::numeric_limits<int>::max() / 4 ||
        frame.stride < frame.width * 4 || frame.stride <= 0 ||
        static_cast<size_t>(frame.stride) > std::numeric_limits<size_t>::max() / static_cast<size_t>(frame.height)) {
        ffp_free(bytes);
        result.status = FFP_EINVAL;
        return result;
    }
    const size_t length = static_cast<size_t>(frame.stride) * static_cast<size_t>(frame.height);
    frame.pixels.assign(bytes, bytes + length);
    ffp_free(bytes);
    result.frame = std::move(frame);
    return result;
}

struct SnapshotFrame {
    int width = 0;
    int height = 0;
    int stride = 0;
    std::vector<uint8_t> pixels;
};

std::optional<SnapshotFrame> Snapshot(FFPlayer* player, unsigned flags) {
    uint8_t* bytes = nullptr;
    SnapshotFrame frame;
    if (ffp_snapshot_ex(player, flags, &bytes, &frame.width, &frame.height, &frame.stride) != 0)
        return std::nullopt;
    if (!bytes || frame.width <= 0 || frame.height <= 0 || frame.width > std::numeric_limits<int>::max() / 4 ||
        frame.stride < frame.width * 4 || frame.stride <= 0 ||
        static_cast<size_t>(frame.stride) > std::numeric_limits<size_t>::max() / static_cast<size_t>(frame.height)) {
        ffp_free(bytes);
        return std::nullopt;
    }
    const size_t length = static_cast<size_t>(frame.stride) * static_cast<size_t>(frame.height);
    frame.pixels.assign(bytes, bytes + length);
    ffp_free(bytes);
    return frame;
}

bool ValidVideoRect(const Readback& frame) {
    return frame.videoX >= 0 && frame.videoY >= 0 && frame.videoWidth > 0 && frame.videoHeight > 0 &&
        static_cast<int64_t>(frame.videoX) + frame.videoWidth <= frame.width &&
        static_cast<int64_t>(frame.videoY) + frame.videoHeight <= frame.height;
}

struct PixelDiff {
    size_t inside = 0;
    size_t outside = 0;
};

PixelDiff ComparePixels(const Readback& a, const Readback& b) {
    PixelDiff result;
    if (a.width != b.width || a.height != b.height || a.stride != b.stride ||
        a.videoX != b.videoX || a.videoY != b.videoY ||
        a.videoWidth != b.videoWidth || a.videoHeight != b.videoHeight ||
        a.pixels.size() < static_cast<size_t>(a.stride) * a.height ||
        b.pixels.size() < static_cast<size_t>(b.stride) * b.height)
        return result;

    const int right = a.videoX + a.videoWidth;
    const int bottom = a.videoY + a.videoHeight;
    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
            const size_t offset = static_cast<size_t>(y) * a.stride + static_cast<size_t>(x) * 4;
            if (std::memcmp(a.pixels.data() + offset, b.pixels.data() + offset, 3) == 0) continue;
            if (x >= a.videoX && x < right && y >= a.videoY && y < bottom) ++result.inside;
            else ++result.outside;
        }
    }
    return result;
}

}

TEST_CASE_INTEGRATION(Arib_d3d11_composed_readback_caption_pixels_stay_inside_video_rect) {
    if (!fs::exists(FixturePath()))
        throw wvptest::SkipFailure{ "gitignored ARIB fixture is not present" };
    REQUIRE_MSG(fs::exists(FontPath()), "bundled WLC Maru ARIB font is missing");

    RawPlayer session;
    REQUIRE_MSG(session.OpenFixture(), "D3D11 ARIB fixture open rejected");
    REQUIRE_MSG(WaitFor([&] {
        return session.events.prepared.load(std::memory_order_relaxed) > 0 ||
               session.events.errors.load(std::memory_order_relaxed) > 0;
    }, 25.0), "fixture produced no PREPARED event");
    REQUIRE_MSG(session.events.errors.load(std::memory_order_relaxed) == 0, "fixture open raised a player error");

    std::optional<json::Value> initialDebug;
    REQUIRE_MSG(WaitFor([&] {
        initialDebug = ReadDebug(session.player);
        return initialDebug.has_value();
    }, 5.0), "libffplay returned no debug info after PREPARED");
    if (!IsD3D11Presenter(*initialDebug))
        throw wvptest::SkipFailure{ "D3D11 presenter is unavailable on this host; libffplay stayed on SDL" };

    const int enableResult = ffp_test_d3d11_readback_enable(session.player, 1);
    if (enableResult == FFP_EAGAIN)
        throw wvptest::SkipFailure{ "D3D11 presenter became unavailable before readback could be enabled" };
    REQUIRE_MSG(enableResult == 0, "could not enable the test-only D3D11 composed-backbuffer readback");

    bool presenterFellBack = false;
    bool activeCaption = false;
    bool overlayDrawn = false;
    bool overlayReported = false;
    OverlayStats lastOverlay;
    REQUIRE_MSG(WaitFor([&] {
        auto debug = ReadDebug(session.player);
        if (!debug) return false;
        if (!IsD3D11Presenter(*debug)) {
            presenterFellBack = true;
            return true;
        }
        const json::Value* layers = Member(&*debug, "subtitle_layers");
        const json::Value* caption = Member(layers, "caption");
        const json::Value* cue = Member(caption, "active_cue");
        const json::Value* counts = Member(caption, "counts");
        activeCaption = Number(cue, "rect_count").value_or(0) > 0 &&
                        Number(counts, "cues_shown").value_or(0) > 0;
        // Decoder-side cues_shown only says a cue reached the presenter call; require a successful D3D draw.
        lastOverlay = ReadOverlayStats(*debug);
        overlayReported = lastOverlay.present;
        overlayDrawn = overlayReported && lastOverlay.rectsDrawn > 0 && lastOverlay.drawFailures == 0;
        return (activeCaption && overlayDrawn) || session.events.errors.load(std::memory_order_relaxed) > 0;
    }, 30.0), "fixture produced no active ARIB caption drawn by the D3D11 presenter; " + Describe(lastOverlay));
    if (presenterFellBack)
        throw wvptest::SkipFailure{ "D3D11 presenter failed during playback; composed readback path is unavailable" };
    REQUIRE_MSG(session.events.errors.load(std::memory_order_relaxed) == 0, "fixture raised a playback error before its caption cue");
    REQUIRE_MSG(activeCaption, "debug info did not report an active ARIB caption cue");
    REQUIRE_MSG(overlayReported, "debug info has no presenter.overlay section");
    REQUIRE_MSG(overlayDrawn, "D3D11 overlay drew no caption rects without failures; " + Describe(lastOverlay));

    REQUIRE_MSG(ffp_pause(session.player) == 0, "could not pause the fixture for a stable pixel comparison");
    bool pausedWithCaption = false;
    REQUIRE_MSG(WaitFor([&] {
        auto debug = ReadDebug(session.player);
        if (!debug) return false;
        if (!IsD3D11Presenter(*debug)) {
            presenterFellBack = true;
            return true;
        }
        const json::Value* layers = Member(&*debug, "subtitle_layers");
        const json::Value* caption = Member(layers, "caption");
        const json::Value* cue = Member(caption, "active_cue");
        pausedWithCaption = debug->Bool("paused") && Number(cue, "rect_count").value_or(0) > 0;
        return pausedWithCaption || session.events.errors.load(std::memory_order_relaxed) > 0;
    }, 8.0), "player did not pause with an active caption cue");
    if (presenterFellBack)
        throw wvptest::SkipFailure{ "D3D11 presenter failed before the readback comparison" };
    REQUIRE_MSG(session.events.errors.load(std::memory_order_relaxed) == 0, "fixture raised a playback error while pausing");

    // Stats are sampled before and after each capture; an equal readback_seq means the copied frame is the
    // one those stats describe (the presenter updates seq/rects and the pixels under one lock).
    std::optional<Readback> visible;
    OverlayStats visibleStats;
    OverlayStats latestStats;
    bool visibleHasOverlay = false;
    int captureStatus = FFP_EAGAIN;
    const bool haveVisible = WaitFor([&] {
        const auto before = ReadOverlayStats(session.player);
        CaptureResult capture = Capture(session.player);
        captureStatus = capture.status;
        const auto after = ReadOverlayStats(session.player);
        if (after) latestStats = *after;
        if (capture.frame) {
            visible = std::move(capture.frame);
            if (before && after && before->readbackSeq == after->readbackSeq && after->readbackSeq > 0) {
                visibleStats = *after;
                visibleHasOverlay = after->readbackRects > 0;
            } else {
                visibleHasOverlay = false;
            }
            return visibleHasOverlay;
        }
        return captureStatus != FFP_EAGAIN;
    }, 6.0);
    if (!visible)
        throw wvptest::SkipFailure{ "D3D11 composed-backbuffer readback is unavailable on this GPU/driver" };
    REQUIRE_MSG(captureStatus == 0, "D3D11 readback returned an unexpected status");
    REQUIRE_MSG(haveVisible && visibleHasOverlay,
                "the paused pre-hide readback frame had no overlay rects drawn into it; " + Describe(latestStats));
    REQUIRE_MSG(ValidVideoRect(*visible), "readback returned an invalid video rectangle");

    // Frames with readback_seq above this were composed after the hide request was issued.
    const auto atHide = ReadOverlayStats(session.player);
    REQUIRE_MSG(atHide.has_value(), "no debug info right before hiding the caption");
    const uint64_t hideSeq = std::max(atHide->readbackSeq, visibleStats.readbackSeq);
    REQUIRE_MSG(ffp_set_subtitle_visible(session.player, FFP_SUBTITLE_CAPTION, 0) == 0,
                "could not hide the caption layer for the pixel comparison");
    std::optional<Readback> hidden;
    PixelDiff diff;
    OverlayStats hiddenStats;
    OverlayStats latestHide = *atHide;
    bool sawRedraw = false;          // a frame composed after the hide request was captured
    bool geometryMatched = false;    // ... and had the same geometry as the visible frame
    const bool hiddenCaptured = WaitFor([&] {
        const auto before = ReadOverlayStats(session.player);
        CaptureResult capture = Capture(session.player);
        captureStatus = capture.status;
        const auto after = ReadOverlayStats(session.player);
        if (after) latestHide = *after;
        if (!capture.frame)
            return captureStatus != FFP_EAGAIN;
        if (!before || !after || before->readbackSeq != after->readbackSeq || after->readbackSeq <= hideSeq)
            return false;            // not provably a post-hide frame (yet)
        sawRedraw = true;
        Readback candidate = std::move(*capture.frame);
        if (candidate.width != visible->width || candidate.height != visible->height ||
            candidate.videoX != visible->videoX || candidate.videoY != visible->videoY ||
            candidate.videoWidth != visible->videoWidth || candidate.videoHeight != visible->videoHeight)
            return false;
        geometryMatched = true;
        diff = ComparePixels(*visible, candidate);
        if (diff.inside == 0 && diff.outside == 0) return false;
        hiddenStats = *after;
        hidden = std::move(candidate);
        return true;
    }, 8.0);
    REQUIRE_MSG(captureStatus == 0, "D3D11 readback failed while waiting for the hidden-caption frame; " + Describe(latestHide));
    REQUIRE_MSG(sawRedraw, "no D3D11 redraw was captured after hiding the caption (readback_seq stayed at " +
                std::to_string(hideSeq) + "); " + Describe(latestHide));
    REQUIRE_MSG(geometryMatched, "post-hide D3D11 redraws had different geometry than the visible frame; " + Describe(latestHide));
    REQUIRE_MSG(hiddenCaptured && hidden.has_value(),
                "D3D11 redrew after hiding the caption but its pixels never differed from the visible frame (visible frame had " +
                std::to_string(visibleStats.readbackRects) + " overlay rects, latest redraw had " +
                std::to_string(latestHide.readbackRects) + "); " + Describe(latestHide));
    CHECK_MSG(hiddenStats.readbackRects == 0, "post-hide frame still had overlay rects drawn into it; " + Describe(hiddenStats));
    CHECK_MSG(diff.inside > 0, "caption visibility changed no pixels inside the D3D11 video rectangle");
    CHECK_MSG(diff.outside == 0, "caption visibility changed pixels outside the D3D11 video rectangle");
}

// The composed D3D11 outline must keep the thickness libaribcaption draws,
// stroke = outline_width(1.5) * canvas_width / caption_plane_width(960). Catches presenter-side thinning.
TEST_CASE_INTEGRATION(Arib_d3d11_caption_outline_thickness_matches_canvas_scale) {
    if (!fs::exists(FixturePath()))
        throw wvptest::SkipFailure{ "gitignored ARIB fixture is not present" };
    REQUIRE_MSG(fs::exists(FontPath()), "bundled WLC Maru ARIB font is missing");

    RawPlayer session;
    // No caption background box, as with a transparent Windows caption window: the outline borders the video.
    REQUIRE_MSG(session.OpenFixture("\narib_ignore_background=1"), "D3D11 ARIB fixture open rejected");
    REQUIRE_MSG(WaitFor([&] {
        return session.events.prepared.load(std::memory_order_relaxed) > 0 ||
               session.events.errors.load(std::memory_order_relaxed) > 0;
    }, 25.0), "fixture produced no PREPARED event");
    REQUIRE_MSG(session.events.errors.load(std::memory_order_relaxed) == 0, "fixture open raised a player error");

    std::optional<json::Value> initialDebug;
    REQUIRE_MSG(WaitFor([&] {
        initialDebug = ReadDebug(session.player);
        return initialDebug.has_value();
    }, 5.0), "libffplay returned no debug info after PREPARED");
    if (!IsD3D11Presenter(*initialDebug))
        throw wvptest::SkipFailure{ "D3D11 presenter is unavailable on this host; libffplay stayed on SDL" };
    const int enableResult = ffp_test_d3d11_readback_enable(session.player, 1);
    if (enableResult == FFP_EAGAIN)
        throw wvptest::SkipFailure{ "D3D11 presenter became unavailable before readback could be enabled" };
    REQUIRE_MSG(enableResult == 0, "could not enable the test-only D3D11 composed-backbuffer readback");

    constexpr const char* kFlatGrey = "{\"eq\":{\"brightness\":0,\"contrast\":0,\"saturation\":1,\"gamma\":1}}";
    bool presenterFellBack = false;
    bool ready = false;
    double canvasWidth = 0.0;
    OverlayStats lastOverlay;
    const bool found = WaitFor([&] {
        auto debug = ReadDebug(session.player);
        if (!debug) return false;
        if (!IsD3D11Presenter(*debug)) { presenterFellBack = true; return true; }
        const json::Value* caption = Member(Member(&*debug, "subtitle_layers"), "caption");
        const bool active = Number(Member(caption, "active_cue"), "rect_count").value_or(0) > 0 &&
                            Number(Member(caption, "counts"), "cues_shown").value_or(0) > 0;
        lastOverlay = ReadOverlayStats(*debug);
        ready = active && lastOverlay.rectsDrawn > 0 && lastOverlay.drawFailures == 0;
        if (ready) canvasWidth = Number(Member(caption, "canvas"), "width").value_or(0.0);
        return ready || session.events.errors.load(std::memory_order_relaxed) > 0;
    }, 30.0);
    if (presenterFellBack) throw wvptest::SkipFailure{ "D3D11 presenter failed during playback" };
    if (!found || !ready) throw wvptest::SkipFailure{ "no active ARIB caption cue drawn by D3D11; " + Describe(lastOverlay) };

    // A black outline cannot be told apart from dark video, so flatten the video to uniform mid grey
    // (eq contrast 0 runs before the caption overlay) and enlarge the host so the outline spans ~3 px.
    REQUIRE_MSG(ffp_set_presenter_params(session.player, kFlatGrey) == 0,
                "D3D11 presenter rejected the flat-grey eq used as a measurement background");
    REQUIRE_MSG(SetWindowPos(session.host.parent, nullptr, 0, 0, 1920, 1080,
                             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) &&
                SetWindowPos(session.host.video, nullptr, 0, 0, 1920, 1080,
                             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE),
                "could not resize the hidden D3D11 test host");
    // The displayed cue must be re-rendered for the enlarged canvas, not shown as the old bitmap scaled up
    // (a stretched 1-px outline reads as a thin, soft edge).
    bool resized = false;
    bool cueActive = false;
    double bitmapWidth = 0.0;
    WaitFor([&] {
        auto debug = ReadDebug(session.player);
        if (!debug) return false;
        const json::Value* caption = Member(Member(&*debug, "subtitle_layers"), "caption");
        const json::Value* cue = Member(caption, "active_cue");
        canvasWidth = Number(Member(caption, "canvas"), "width").value_or(0.0);
        bitmapWidth = Number(cue, "bitmap_width").value_or(0.0);
        cueActive = Number(cue, "rect_count").value_or(0) > 0;
        resized = cueActive && canvasWidth >= 1800.0 && bitmapWidth == canvasWidth;
        return resized;
    }, 8.0);
    if (!resized && !cueActive) throw wvptest::SkipFailure{ "caption cue ended before the enlarged host was re-rendered" };
    REQUIRE_MSG(resized, "displayed caption stayed a scaled bitmap after the resize: bitmap width " +
                std::to_string(bitmapWidth) + " vs canvas " + std::to_string(canvasWidth));

    REQUIRE_MSG(ffp_pause(session.player) == 0, "could not pause the fixture");
    bool paused = false;
    WaitFor([&] {
        auto debug = ReadDebug(session.player);
        if (!debug) return false;
        const json::Value* cue = Member(Member(Member(&*debug, "subtitle_layers"), "caption"), "active_cue");
        paused = debug->Bool("paused") && Number(cue, "rect_count").value_or(0) > 0;
        return paused;
    }, 8.0);
    if (!paused) throw wvptest::SkipFailure{ "caption cue ended before the player paused" };

    // Redraw the paused frame with the eq in effect so the visible and hidden captures share one background.
    const auto atParams = ReadOverlayStats(session.player);
    const uint64_t paramsSeq = atParams ? atParams->readbackSeq : 0;
    REQUIRE_MSG(ffp_set_presenter_params(session.player, kFlatGrey) == 0, "could not re-apply the flat-grey eq");
    std::optional<Readback> visible;
    uint64_t visibleSeq = 0;
    int status = FFP_EAGAIN;
    WaitFor([&] {
        const auto before = ReadOverlayStats(session.player);
        CaptureResult capture = Capture(session.player);
        status = capture.status;
        const auto after = ReadOverlayStats(session.player);
        if (capture.frame && before && after && before->readbackSeq == after->readbackSeq &&
            after->readbackSeq > paramsSeq && after->readbackRects > 0) {
            visible = std::move(capture.frame);
            visibleSeq = after->readbackSeq;
            return true;
        }
        return !capture.frame && status != FFP_EAGAIN;
    }, 6.0);
    if (!visible) throw wvptest::SkipFailure{ "D3D11 composed-backbuffer readback unavailable or showed no caption" };
    REQUIRE_MSG(ValidVideoRect(*visible), "readback returned an invalid video rectangle");

    REQUIRE_MSG(ffp_set_subtitle_visible(session.player, FFP_SUBTITLE_CAPTION, 0) == 0, "could not hide the caption layer");
    std::optional<Readback> hidden;
    WaitFor([&] {
        const auto before = ReadOverlayStats(session.player);
        CaptureResult capture = Capture(session.player);
        const auto after = ReadOverlayStats(session.player);
        if (!capture.frame || !before || !after || before->readbackSeq != after->readbackSeq ||
            after->readbackSeq <= visibleSeq || after->readbackRects != 0)
            return false;
        const Readback& c = *capture.frame;
        if (c.width != visible->width || c.height != visible->height || c.stride != visible->stride ||
            c.videoX != visible->videoX || c.videoY != visible->videoY ||
            c.videoWidth != visible->videoWidth || c.videoHeight != visible->videoHeight)
            return false;
        hidden = std::move(capture.frame);
        return true;
    }, 8.0);
    REQUIRE_MSG(hidden.has_value(), "no post-hide D3D11 frame with matching geometry was captured");

    // libaribcaption draws a 1.5 caption-plane-pixel outline (960-wide plane); judge it at the displayed size.
    const double expected = 1.5 * static_cast<double>(visible->videoWidth) / 960.0;
    const aribmeasure::OutlineStats stats = aribmeasure::MeasureOutline(
        visible->pixels.data(), visible->stride, hidden->pixels.data(), hidden->stride,
        visible->videoX, visible->videoY, visible->videoWidth, visible->videoHeight);
    std::printf("[arib-outline-d3d11] median=%.3fpx expected=%.3fpx ratio=%.3f samples=%zu canvas_width=%.0f video_width=%d\n",
                stats.median, expected, expected > 0 ? stats.median / expected : 0.0, stats.samples,
                canvasWidth, visible->videoWidth);
    std::fflush(stdout);
    char dumpDir[MAX_PATH] = {};
    if (GetEnvironmentVariableA("WVP_ARIB_OUTLINE_DUMP", dumpDir, MAX_PATH) > 0) {
        const std::string dir(dumpDir);
        aribmeasure::WriteBmp((dir + "\\d3d-visible.bmp").c_str(), visible->pixels.data(), visible->stride,
                              visible->width, visible->height);
        aribmeasure::WriteBmp((dir + "\\d3d-hidden.bmp").c_str(), hidden->pixels.data(), hidden->stride,
                              hidden->width, hidden->height);
        std::printf("[arib-outline-d3d11] dumped readbacks to %s (video rect %d,%d %dx%d)\n", dumpDir,
                    visible->videoX, visible->videoY, visible->videoWidth, visible->videoHeight);
        std::fflush(stdout);
    }
    if (stats.samples < 20)
        throw wvptest::SkipFailure{ "too few outline samples (" + std::to_string(stats.samples) +
                                    ") to measure the caption outline" };
    CHECK_MSG(stats.median >= 0.75 * expected, "composed caption outline is too thin: median " +
              std::to_string(stats.median) + "px vs expected " + std::to_string(expected) + "px");
    CHECK_MSG(stats.median <= 1.4 * expected, "composed caption outline is too thick: median " +
              std::to_string(stats.median) + "px vs expected " + std::to_string(expected) + "px");
}

// The public snapshot hook exposes the active bitmap after PAL8 conversion and composition. A flat-grey
// presenter background lets this test infer each black outline pixel's alpha from the captured pair.
TEST_CASE_INTEGRATION(Arib_d3d11_force_outline_is_opaque_with_smooth_alpha_ramp) {
    if (!fs::exists(FixturePath()))
        throw wvptest::SkipFailure{ "gitignored ARIB fixture is not present" };
    REQUIRE_MSG(fs::exists(FontPath()), "bundled WLC Maru ARIB font is missing");

    RawPlayer session;
    REQUIRE_MSG(session.OpenFixture("\narib_force_outline=1\narib_ignore_background=1"),
                "D3D11 ARIB fixture open rejected");
    REQUIRE_MSG(WaitFor([&] {
        return session.events.prepared.load(std::memory_order_relaxed) > 0 ||
               session.events.errors.load(std::memory_order_relaxed) > 0;
    }, 25.0), "fixture produced no PREPARED event");
    REQUIRE_MSG(session.events.errors.load(std::memory_order_relaxed) == 0,
                "fixture open raised a player error");

    auto initial = ReadDebug(session.player);
    REQUIRE_MSG(initial.has_value(), "libffplay returned no debug info after PREPARED");
    if (!IsD3D11Presenter(*initial))
        throw wvptest::SkipFailure{ "D3D11 presenter is unavailable on this host; libffplay stayed on SDL" };

    REQUIRE_MSG(ffp_refresh_caption_style(session.player, 1, 1) == 0,
                "could not queue the ARIB style refresh");
    bool presenterFellBack = false;
    bool activeCaption = false;
    REQUIRE_MSG(WaitFor([&] {
        auto debug = ReadDebug(session.player);
        if (!debug) return false;
        if (!IsD3D11Presenter(*debug)) {
            presenterFellBack = true;
            return true;
        }
        const json::Value* layers = Member(&*debug, "subtitle_layers");
        const json::Value* caption = Member(layers, "caption");
        const json::Value* cue = Member(caption, "active_cue");
        const json::Value* counts = Member(caption, "counts");
        activeCaption = Number(cue, "rect_count").value_or(0) > 0 &&
                        Number(counts, "cues_shown").value_or(0) > 0;
        return activeCaption || session.events.errors.load(std::memory_order_relaxed) > 0;
    }, 30.0), "style-refreshed fixture produced no active caption");
    if (presenterFellBack)
        throw wvptest::SkipFailure{ "D3D11 presenter failed during playback; snapshot test is unavailable" };
    REQUIRE_MSG(session.events.errors.load(std::memory_order_relaxed) == 0,
                "fixture raised a playback error before its caption cue");
    REQUIRE_MSG(activeCaption, "debug info did not report an active ARIB caption cue");

    REQUIRE_MSG(ffp_pause(session.player) == 0, "could not pause the fixture for stable snapshots");
    REQUIRE_MSG(WaitFor([&] {
        auto debug = ReadDebug(session.player);
        return debug && debug->Bool("paused");
    }, 8.0), "player did not pause with an active caption");
    constexpr const char* kFlatGrey =
        "{\"eq\":{\"brightness\":0,\"contrast\":0,\"saturation\":1,\"gamma\":1}}";
    REQUIRE_MSG(ffp_set_presenter_params(session.player, kFlatGrey) == 0,
                "D3D11 presenter rejected the flat-grey measurement background");

    std::optional<SnapshotFrame> hidden;
    std::optional<SnapshotFrame> visible;
    REQUIRE_MSG(WaitFor([&] {
        hidden = Snapshot(session.player, 0);
        visible = Snapshot(session.player, FFP_SNAPSHOT_SUBTITLES);
        return hidden && visible;
    }, 8.0), "snapshot hook did not return both video and caption frames");
    REQUIRE_MSG(hidden->width == visible->width && hidden->height == visible->height &&
                hidden->stride == visible->stride,
                "video-only and caption snapshots have different geometry");

    std::set<int> intermediateAlpha;
    size_t outlineSamples = 0;
    size_t opaqueOutlinePixels = 0;
    const int width = hidden->width;
    const int height = hidden->height;
    for (int y = 0; y < height; ++y) {
        const uint8_t* base = hidden->pixels.data() + static_cast<size_t>(y) * hidden->stride;
        const uint8_t* image = visible->pixels.data() + static_cast<size_t>(y) * visible->stride;
        std::vector<aribmeasure::detail::Px> line(static_cast<size_t>(width));
        for (int x = 0; x < width; ++x)
            line[x] = aribmeasure::detail::Classify(image + static_cast<size_t>(x) * 4,
                                                     base + static_cast<size_t>(x) * 4);

        for (int x = 0; x < width;) {
            if (!line[x].fill) {
                ++x;
                continue;
            }
            int end = x;
            while (end + 1 < width && line[end + 1].fill)
                ++end;

            for (int direction : { -1, 1 }) {
                int edge = direction < 0 ? x - 1 : end + 1;
                bool skippedTransition = false;
                for (int count = 0; edge >= 0 && edge < width && count < 24; edge += direction, ++count) {
                    const auto& px = line[edge];
                    if (px.fill)
                        break;
                    if (px.coverage <= 0.05) {
                        if (!skippedTransition) {
                            skippedTransition = true;
                            continue;
                        }
                        break;
                    }
                    ++outlineSamples;
                    const int alpha = static_cast<int>(std::lround(px.coverage * 255.0));
                    if (alpha > 0 && alpha < 255)
                        intermediateAlpha.insert(alpha);
                    const uint8_t* p = image + static_cast<size_t>(edge) * 4;
                    if (alpha >= 250 && p[0] <= 8 && p[1] <= 8 && p[2] <= 8)
                        ++opaqueOutlinePixels;
                }
            }
            x = end + 1;
        }
    }

    std::printf("[arib-outline-aa] intermediate_alpha_levels=%zu opaque_outline_pixels=%zu outline_samples=%zu\n",
                intermediateAlpha.size(), opaqueOutlinePixels, outlineSamples);
    std::printf("[arib-outline-aa] inferred_alpha_values=");
    for (int alpha : intermediateAlpha)
        std::printf("%s%d", alpha == *intermediateAlpha.begin() ? "" : ",", alpha);
    std::printf("\n");
    std::fflush(stdout);
    CHECK_MSG(intermediateAlpha.size() > 4,
              "caption outline exposed no more than four intermediate alpha levels");
    CHECK_MSG(opaqueOutlinePixels > 0,
              "no fully opaque black outline pixel was found beside a glyph fill pixel");
}
