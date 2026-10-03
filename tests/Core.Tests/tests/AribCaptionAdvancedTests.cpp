// Fixture-optional hidden-host integration tests for ARIB cue timing and presenter behavior.
#include "wvptest.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

#include "wvp/FfPlayer.h"
#include "wvp/JsonLite.h"
#include "wvp/Utf.h"

using namespace wvp;
namespace fs = std::filesystem;

namespace {

constexpr UINT kNotifyMsg = WM_APP + 70;
constexpr wchar_t kHostClass[] = L"WvpAribAdvancedTestHost";

LRESULT CALLBACK HostProc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    if (message == kNotifyMsg) {
        FfPlayer::HandleNotify(w, l);
        return 0;
    }
    return DefWindowProcW(hwnd, message, w, l);
}

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
        wchar_t buf[32768]{};
        GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(_countof(buf)));
        fs::path dir = fs::path(buf).parent_path();
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

void RequireFixture() {
    if (!fs::exists(FixturePath()))
        throw wvptest::SkipFailure{ "gitignored ARIB fixture is not present" };
}

void PumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

template <class F>
bool WaitFor(F&& predicate, double seconds = 10.0) {
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

const json::Value* Member(const json::Value* object, std::string_view key) {
    return object && object->kind == json::Value::Kind::Object ? object->Find(key) : nullptr;
}

std::optional<double> Number(const json::Value* object, std::string_view key) {
    const json::Value* value = Member(object, key);
    if (!value || value->kind != json::Value::Kind::Number) return std::nullopt;
    return value->n;
}

std::optional<json::Value> ReadDebug(FfPlayer& player) {
    const auto text = player.GetDebugInfoJson();
    if (!text) return std::nullopt;
    return json::Parse(*text);
}

struct CaptionDebug {
    int rects = 0;
    int cuesShown = 0;
    int cuesCleared = 0;
    int cuesUploaded = 0;
    int rerenders = 0;
    int canvasWidth = 0;
    int canvasHeight = 0;
    std::optional<double> start;
    std::optional<double> end;
    bool indefinite = false;
};

std::optional<CaptionDebug> ReadCaption(const json::Value& root) {
    const json::Value* layers = Member(&root, "subtitle_layers");
    const json::Value* caption = Member(layers, "caption");
    if (!caption || caption->kind != json::Value::Kind::Object) return std::nullopt;
    const json::Value* cue = Member(caption, "active_cue");
    const json::Value* canvas = Member(caption, "canvas");
    const json::Value* counts = Member(caption, "counts");
    if (!cue || cue->kind != json::Value::Kind::Object) return std::nullopt;
    CaptionDebug result;
    if (const auto n = Number(cue, "rect_count")) result.rects = static_cast<int>(*n);
    if (const auto n = Number(cue, "start")) result.start = *n;
    if (const auto n = Number(cue, "end")) result.end = *n;
    const json::Value* end = Member(cue, "end");
    result.indefinite = result.rects > 0 && end && end->kind == json::Value::Kind::Null;
    if (const auto n = Number(counts, "cues_shown")) result.cuesShown = static_cast<int>(*n);
    if (const auto n = Number(counts, "cues_cleared")) result.cuesCleared = static_cast<int>(*n);
    if (const auto n = Number(counts, "cues_uploaded")) result.cuesUploaded = static_cast<int>(*n);
    if (const auto n = Number(counts, "rerenders")) result.rerenders = static_cast<int>(*n);
    if (const auto n = Number(canvas, "width")) result.canvasWidth = static_cast<int>(*n);
    if (const auto n = Number(canvas, "height")) result.canvasHeight = static_cast<int>(*n);
    return result;
}

std::optional<double> VideoClock(const json::Value& root) {
    return Number(Member(&root, "sync"), "video_clock");
}

double FrameDuration(const json::Value& root) {
    const auto fps = Number(Member(&root, "video"), "frame_rate");
    return fps && *fps > 0.0 ? 1.0 / *fps : 1.0 / 30.0;
}

std::string PresenterName(const json::Value& root) {
    const json::Value* presenter = Member(&root, "presenter");
    return presenter ? presenter->Str("name") : std::string{};
}

struct HiddenHost {
    HWND parent = nullptr;
    HWND video = nullptr;

    HiddenHost() {
        EnsureHostClass();
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        parent = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kHostClass, L"ARIB test host",
            WS_POPUP, -32000, -32000, 640, 360, nullptr, nullptr, instance, nullptr);
        if (parent)
            video = CreateWindowExW(0, L"STATIC", L"video", WS_CHILD | WS_VISIBLE,
                0, 0, 640, 360, parent, nullptr, instance, nullptr);
    }

    ~HiddenHost() {
        if (parent) DestroyWindow(parent);
    }
};

struct Session {
    HiddenHost host;
    std::unique_ptr<FfPlayer> player;
    int prepared = 0;
    int errors = 0;

    Session() {
        REQUIRE_MSG(host.parent && host.video, "could not create hidden ARIB host window");
        int error = 0;
        player = FfPlayer::CreateSync(host.video, host.parent, kNotifyMsg, &error);
        REQUIRE_MSG(player != nullptr, "FfPlayer::CreateSync failed: " + std::to_string(error));
        player->Prepared.add([this](const MediaPreparedInfo&) { ++prepared; });
        player->Error.add([this](const PlayerErrorInfo&) { ++errors; });
    }

    ~Session() { player.reset(); }

    void Open(const wchar_t* presenter, const wchar_t* hwaccel = nullptr, bool keepFrames = false) {
        REQUIRE_MSG(fs::exists(FontPath()), "bundled WLC Maru ARIB font is missing");
        const int before = prepared;
        OpenOptions options = {
            { L"presenter", presenter },
            { L"superimpose", L"0" },
            { L"arib_font", FontPath().wstring() },
        };
        if (hwaccel && *hwaccel) options.emplace_back(L"hwaccel", hwaccel);
        if (keepFrames) options.emplace_back(L"hwframes", L"keep");
        REQUIRE_MSG(player->Open(FixturePath().wstring(), options) == 0, "ARIB fixture open rejected");
        REQUIRE_MSG(WaitFor([&] { return prepared > before || errors > 0; }, 25.0),
                    "fixture produced no PREPARED event");
        REQUIRE_MSG(errors == 0, "fixture open raised a player error");
    }

    std::optional<BgraFrame> Snapshot(bool withSubtitles = false) {
        std::optional<BgraFrame> frame;
        const bool ready = WaitFor([&] {
            frame = player->Snapshot(withSubtitles);
            return frame.has_value();
        }, 10.0);
        if (!ready) return std::nullopt;
        return frame;
    }
};

std::string NumberText(std::optional<double> value) {
    if (!value) return "?";
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << *value;
    return out.str();
}

std::string CompactValue(const json::Value* value, int depth = 0) {
    if (!value) return "not-reported";
    switch (value->kind) {
    case json::Value::Kind::Null:
        return "null";
    case json::Value::Kind::Bool:
        return value->b ? "true" : "false";
    case json::Value::Kind::Number:
        return NumberText(value->n);
    case json::Value::Kind::String: {
        std::string text = value->s;
        for (char& c : text)
            if (c == '\\r' || c == '\\n' || c == '\\t') c = ' ';
        if (text.size() > 72) text = text.substr(0, 69) + "...";
        return text.empty() ? "\"\"" : text;
    }
    case json::Value::Kind::Array:
        return "array[" + std::to_string(value->arr.size()) + "]";
    case json::Value::Kind::Object: {
        if (depth >= 2) return "object";
        std::ostringstream out;
        out << '{';
        const size_t count = std::min<size_t>(value->keys.size(), 6);
        for (size_t i = 0; i < count; ++i) {
            if (i) out << ',';
            out << value->keys[i] << ':' << CompactValue(&value->vals[i], depth + 1);
        }
        if (value->keys.size() > count) out << ",...";
        out << '}';
        return out.str();
    }
    }
    return "?";
}

void AppendDebugValue(std::ostringstream& out, std::string_view name, const json::Value* value) {
    if (value) out << ' ' << name << '=' << CompactValue(value);
}

void AppendHostClientSize(std::ostringstream& out, HWND videoHwnd) {
    RECT client{};
    if (videoHwnd && GetClientRect(videoHwnd, &client)) {
        out << " host_client=" << (client.right - client.left) << 'x' << (client.bottom - client.top);
    } else {
        out << " host_client=unavailable";
    }
}

std::string DiagnosticState(Session& session, const json::Value* snapshot = nullptr) {
    std::optional<json::Value> live;
    if (!snapshot) {
        live = ReadDebug(*session.player);
        if (live) snapshot = &*live;
    }
    if (!snapshot) {
        std::ostringstream out;
        out << " [diag debug=unavailable player_errors=" << session.errors;
        AppendHostClientSize(out, session.host.video);
        out << ']';
        return out.str();
    }

    std::ostringstream out;
    out << " [diag pos=" << CompactValue(Member(snapshot, "position"))
        << '/' << CompactValue(Member(snapshot, "duration"))
        << " paused=" << CompactValue(Member(snapshot, "paused"))
        << " video_clock=" << CompactValue(Member(Member(snapshot, "sync"), "video_clock"))
        << " clock_master=" << CompactValue(Member(Member(snapshot, "sync"), "master"))
        << " presenter=" << CompactValue(Member(Member(snapshot, "presenter"), "name"))
        << " player_errors=" << session.errors;
    AppendDebugValue(out, "presenter_requested", Member(Member(snapshot, "presenter"), "requested"));
    AppendDebugValue(out, "renderer", Member(snapshot, "renderer"));

    const json::Value* video = Member(snapshot, "video");
    if (video && video->kind == json::Value::Kind::Null) {
        out << " video=null";
    } else {
        AppendDebugValue(out, "video_stream", Member(video, "stream_index"));
        AppendDebugValue(out, "video_codec", Member(video, "codec"));
    }
    const json::Value* fps = Member(snapshot, "fps");
    AppendDebugValue(out, "frames_decoded", Member(fps, "frames_decoded"));
    AppendDebugValue(out, "frames_displayed", Member(fps, "frames_displayed"));
    AppendDebugValue(out, "video_queue", Member(Member(snapshot, "queues"), "video"));
    AppendHostClientSize(out, session.host.video);

    const auto caption = ReadCaption(*snapshot);
    if (caption) {
        out << " caption={rects=" << caption->rects
            << ",start=" << NumberText(caption->start)
            << ",end=" << NumberText(caption->end)
            << ",indefinite=" << (caption->indefinite ? "true" : "false")
            << ",shown=" << caption->cuesShown
            << ",cleared=" << caption->cuesCleared
            << ",uploaded=" << caption->cuesUploaded
            << ",rerenders=" << caption->rerenders
            << ",canvas=" << caption->canvasWidth << 'x' << caption->canvasHeight << '}';
    } else {
        out << " caption=unavailable";
    }

    const json::Value* layers = Member(snapshot, "subtitle_layers");
    const json::Value* layer = Member(layers, "caption");
    AppendDebugValue(out, "caption_stream", Member(layer, "stream_index"));
    AppendDebugValue(out, "caption_decoder", Member(layer, "decoder"));
    AppendDebugValue(out, "caption_options", Member(layer, "options"));
    AppendDebugValue(out, "caption_decoder_options", Member(layer, "decoder_options"));
    AppendDebugValue(out, "caption_font", Member(layer, "resolved_font"));
    AppendDebugValue(out, "caption_error", Member(layer, "error"));
    AppendDebugValue(out, "subtitle_stream", Member(Member(snapshot, "subtitle"), "stream_index"));
    AppendDebugValue(out, "subtitle_codec", Member(Member(snapshot, "subtitle"), "codec"));
    AppendDebugValue(out, "subtitle_queue", Member(Member(snapshot, "queues"), "subtitle"));
    AppendDebugValue(out, "caption_queue", Member(layer, "queue"));
    AppendDebugValue(out, "caption_packet_queue", Member(layer, "packet_queue"));
    AppendDebugValue(out, "caption_frame_queue", Member(layer, "frame_queue"));
    AppendDebugValue(out, "caption_presenter_error", Member(Member(snapshot, "presenter"), "error"));
    out << ']';
    return out.str();
}

void FlushPreThrowDiagnostic(const std::string& message) {
    constexpr size_t kMaxMessageChars = 4000;
    const size_t chars = std::min(message.size(), kMaxMessageChars);
    std::fprintf(stderr, "[ARIB SDL pre-throw] %.*s%s\n", static_cast<int>(chars), message.c_str(),
                 message.size() > chars ? " [truncated]" : "");
    std::fflush(stderr);
}

std::optional<json::Value> WaitForActive(Session& session, double seconds = 30.0,
                                         std::optional<json::Value>* lastObserved = nullptr) {
    std::optional<json::Value> found;
    const bool ready = WaitFor([&] {
        auto debug = ReadDebug(*session.player);
        if (!debug) return false;
        if (lastObserved) *lastObserved = *debug;
        const auto caption = ReadCaption(*debug);
        if (!caption || caption->rects <= 0 || !caption->start || caption->cuesShown <= 0) return false;
        found = std::move(debug);
        return true;
    }, seconds);
    if (!ready) {
        FlushPreThrowDiagnostic("active ARIB cue wait timed out" +
            DiagnosticState(session, lastObserved && lastObserved->has_value() ? &lastObserved->value() : nullptr));
    }
    return ready ? found : std::nullopt;
}

bool SamePixels(const BgraFrame& a, const BgraFrame& b) {
    if (a.Width != b.Width || a.Height != b.Height || a.Stride < a.Width * 4 || b.Stride < b.Width * 4 ||
        a.Data.size() < static_cast<size_t>(a.Stride) * a.Height ||
        b.Data.size() < static_cast<size_t>(b.Stride) * b.Height) return false;
    for (int y = 0; y < a.Height; ++y) {
        if (std::memcmp(a.Data.data() + static_cast<size_t>(y) * a.Stride,
                        b.Data.data() + static_cast<size_t>(y) * b.Stride,
                        static_cast<size_t>(a.Width) * 4) != 0) return false;
    }
    return true;
}

size_t DifferentPixels(const BgraFrame& a, const BgraFrame& b) {
    if (a.Width != b.Width || a.Height != b.Height || a.Stride < a.Width * 4 || b.Stride < b.Width * 4 ||
        a.Data.size() < static_cast<size_t>(a.Stride) * a.Height ||
        b.Data.size() < static_cast<size_t>(b.Stride) * b.Height) return 0;
    size_t different = 0;
    for (int y = 0; y < a.Height; ++y) {
        for (int x = 0; x < a.Width; ++x) {
            const size_t ao = static_cast<size_t>(y) * a.Stride + static_cast<size_t>(x) * 4;
            const size_t bo = static_cast<size_t>(y) * b.Stride + static_cast<size_t>(x) * 4;
            if (std::memcmp(a.Data.data() + ao, b.Data.data() + bo, 4) != 0) ++different;
        }
    }
    return different;
}

bool ResizeVideoWindow(HWND hwnd, int width, int height) {
    return SetWindowPos(hwnd, nullptr, 0, 0, width, height,
                        SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER) != FALSE;
}

bool GetClientSize(HWND hwnd, int& width, int& height) {
    RECT r{};
    if (!GetClientRect(hwnd, &r)) return false;
    width = r.right - r.left;
    height = r.bottom - r.top;
    return width > 0 && height > 0;
}

bool HasCaptionStream(const MediaInfo& info) {
    for (const auto& stream : info.Streams)
        if (stream.Type == StreamType::Subtitle && stream.AribType == L"caption") return true;
    return false;
}

}

TEST_CASE_INTEGRATION(AribAdvanced_sdl_caption_transition_tracks_video_clock) {
    RequireFixture();
    Session s;
    s.Open(L"sdl");
    const auto info = s.player->GetMediaInfo();
    REQUIRE(info.has_value());
    REQUIRE_MSG(HasCaptionStream(*info), "fixture has no classified ARIB caption stream");

    const auto initial = ReadDebug(*s.player);
    if (!initial) {
        const std::string reason = "libffplay returned no initial caption debug snapshot" + DiagnosticState(s);
        FlushPreThrowDiagnostic(reason);
        REQUIRE_MSG(initial.has_value(), reason);
    }
    REQUIRE_MSG(initial.has_value(), "libffplay returned no initial caption debug snapshot");
    const auto initialCaption = ReadCaption(*initial);
    const int initialShown = initialCaption ? initialCaption->cuesShown : 0;
    const auto initialStart = initialCaption ? initialCaption->start : std::nullopt;

    std::optional<json::Value> transition;
    std::optional<json::Value> lastObserved;
    const bool sawTransition = WaitFor([&] {
        auto debug = ReadDebug(*s.player);
        if (!debug) return false;
        lastObserved = *debug;
        const auto caption = ReadCaption(*debug);
        if (!caption || caption->rects <= 0 || caption->cuesShown <= initialShown || !caption->start ||
            (initialStart && *caption->start <= *initialStart + 0.001) || !VideoClock(*debug)) return false;
        transition = std::move(debug);
        return true;
    }, 30.0);
    if (!sawTransition) {
        const std::string reason =
            "fixture supplied no newly observed caption transition for a frame-timing sample; baseline{shown=" +
            std::to_string(initialShown) + ",start=" + NumberText(initialStart) + "}" +
            DiagnosticState(s, &*initial) + " last_observed" +
            DiagnosticState(s, lastObserved ? &*lastObserved : nullptr);
        FlushPreThrowDiagnostic(reason);
        throw wvptest::SkipFailure{ reason };
    }

    const auto caption = ReadCaption(*transition);
    REQUIRE(caption.has_value());
    const auto clock = VideoClock(*transition);
    REQUIRE(clock.has_value());
    const double frame = FrameDuration(*transition);
    const double timingError = *clock - *caption->start;
    const double oneFramePlusClockGranularity = frame + 0.025;
    CHECK_MSG(timingError >= -oneFramePlusClockGranularity,
              "caption became active before its start PTS (clock-start=" + std::to_string(timingError) + "s)");
    CHECK_MSG(timingError <= oneFramePlusClockGranularity,
              "new caption transition was not presented within one frame of its start PTS (clock-start=" +
              std::to_string(timingError) + "s)");
    CHECK_EQ(PresenterName(*transition), std::string("sdl"));
}

TEST_CASE_INTEGRATION(AribAdvanced_sdl_indefinite_caption_survives_hide_show) {
    RequireFixture();
    Session s;
    s.Open(L"sdl");

    std::optional<json::Value> activeWaitState;
    auto active = WaitForActive(s, 30.0, &activeWaitState);
    REQUIRE_MSG(active.has_value(), "fixture produced no active ARIB caption cue" +
                DiagnosticState(s, activeWaitState ? &*activeWaitState : nullptr));
    auto activeCaption = ReadCaption(*active);
    REQUIRE(activeCaption.has_value());
    if (!activeCaption->indefinite) {
        const std::string reason = "fixture did not expose an active caption with an indefinite end" +
                                   DiagnosticState(s, &*active);
        FlushPreThrowDiagnostic(reason);
        throw wvptest::SkipFailure{ reason };
    }

    CHECK_EQ(s.player->Pause(), 0);
    const bool pauseCompleted = WaitFor([&] { return s.player->State() == PlayerState::Paused; }, 6.0);
    if (!pauseCompleted) {
        const std::string reason = "player did not pause with an active caption" + DiagnosticState(s);
        FlushPreThrowDiagnostic(reason);
        REQUIRE_MSG(pauseCompleted, reason);
    }
    REQUIRE_MSG(pauseCompleted, "player did not pause with an active caption");
    auto paused = ReadDebug(*s.player);
    REQUIRE(paused.has_value());
    activeCaption = ReadCaption(*paused);
    REQUIRE(activeCaption.has_value());
    if (!activeCaption->indefinite || activeCaption->rects <= 0 || !activeCaption->start) {
        const std::string reason = "the indefinite cue ended before the hidden host reached the paused state" +
                                   DiagnosticState(s, &*paused);
        FlushPreThrowDiagnostic(reason);
        throw wvptest::SkipFailure{ reason };
    }
    const double cueStart = *activeCaption->start;

    auto videoOnly = s.Snapshot(false);
    auto visible = s.Snapshot(true);
    REQUIRE(videoOnly.has_value() && visible.has_value());
    CHECK_MSG(DifferentPixels(*videoOnly, *visible) > 0,
              "active indefinite caption did not affect the with-subtitles snapshot" + DiagnosticState(s));

    CHECK_EQ(s.player->SetSubtitleVisible(SubtitleLayer::Caption, false), 0);
    bool hidden = false;
    const bool hideObserved = WaitFor([&] {
        auto a = s.player->Snapshot(false);
        auto b = s.player->Snapshot(true);
        hidden = a && b && SamePixels(*a, *b);
        return hidden;
    }, 8.0);
    if (!hideObserved) {
        const std::string reason = "caption visibility off did not remove the caption from the snapshot" + DiagnosticState(s);
        FlushPreThrowDiagnostic(reason);
        REQUIRE_MSG(hideObserved, reason);
    }
    REQUIRE_MSG(hideObserved, "caption visibility off did not remove the caption from the snapshot");
    auto hiddenDebug = ReadDebug(*s.player);
    REQUIRE(hiddenDebug.has_value());
    auto stillActive = ReadCaption(*hiddenDebug);
    REQUIRE(stillActive.has_value());
    CHECK_MSG(stillActive->rects > 0 && stillActive->start && std::abs(*stillActive->start - cueStart) < 0.01,
              "hiding the layer discarded its active indefinite cue" + DiagnosticState(s, &*hiddenDebug));

    CHECK_EQ(s.player->SetSubtitleVisible(SubtitleLayer::Caption, true), 0);
    bool visibleAgain = false;
    const bool showObserved = WaitFor([&] {
        auto a = s.player->Snapshot(false);
        auto b = s.player->Snapshot(true);
        visibleAgain = a && b && DifferentPixels(*a, *b) > 0;
        return visibleAgain;
    }, 8.0);
    if (!showObserved) {
        const std::string reason = "active indefinite caption did not reappear after show" + DiagnosticState(s);
        FlushPreThrowDiagnostic(reason);
        REQUIRE_MSG(showObserved, reason);
    }
    REQUIRE_MSG(showObserved, "active indefinite caption did not reappear after show");
}

TEST_CASE_INTEGRATION(AribAdvanced_sdl_indefinite_caption_clear_event_is_observable) {
    RequireFixture();
    Session s;
    s.Open(L"sdl");
    std::optional<json::Value> activeWaitState;
    auto active = WaitForActive(s, 30.0, &activeWaitState);
    REQUIRE_MSG(active.has_value(), "fixture produced no active ARIB caption cue" +
                DiagnosticState(s, activeWaitState ? &*activeWaitState : nullptr));
    auto caption = ReadCaption(*active);
    REQUIRE(caption.has_value());
    if (!caption->indefinite) {
        const std::string reason = "fixture did not expose an indefinite ARIB caption cue" +
                                   DiagnosticState(s, &*active);
        FlushPreThrowDiagnostic(reason);
        throw wvptest::SkipFailure{ reason };
    }

    double trackedStart = *caption->start;
    int trackedClears = caption->cuesCleared;
    bool trackingIndefinite = true;
    bool clearObserved = false;
    std::optional<json::Value> clearState;
    std::optional<json::Value> lastClearState;
    const bool ended = WaitFor([&] {
        auto debug = ReadDebug(*s.player);
        if (!debug) return false;
        lastClearState = *debug;
        const auto now = ReadCaption(*debug);
        if (!now) return false;
        if (trackingIndefinite && now->rects == 0 && now->cuesCleared > trackedClears) {
            clearObserved = true;
            clearState = std::move(debug);
            return true;
        }
        if (now->rects > 0 && now->start && std::abs(*now->start - trackedStart) > 0.01) {
            if (now->indefinite) {
                trackedStart = *now->start;
                trackedClears = now->cuesCleared;
                trackingIndefinite = true;
            } else {
                trackingIndefinite = false; // a replacement cue is not evidence of an indefinite clear
            }
        }
        return false;
    }, 35.0);
    if (!ended || !clearObserved) {
        const std::string reason =
            "fixture did not show an indefinite cue followed by a visible clear event" +
            DiagnosticState(s, lastClearState ? &*lastClearState : nullptr);
        FlushPreThrowDiagnostic(reason);
        throw wvptest::SkipFailure{ reason };
    }

    REQUIRE(clearState.has_value());
    caption = ReadCaption(*clearState);
    REQUIRE(caption.has_value());
    CHECK_EQ(caption->rects, 0);
    CHECK_MSG(caption->cuesCleared > trackedClears,
              "clear counter did not advance for the empty cue event" + DiagnosticState(s, &*clearState));
}

TEST_CASE_INTEGRATION(AribAdvanced_sdl_seek_flushes_active_caption_layer) {
    RequireFixture();
    Session s;
    s.Open(L"sdl");
    const auto info = s.player->GetMediaInfo();
    REQUIRE(info.has_value());
    if (!info->Seekable || info->Duration <= 0.0)
        throw wvptest::SkipFailure{ "ARIB fixture is not seekable" };
    std::optional<json::Value> activeWaitState;
    auto active = WaitForActive(s, 30.0, &activeWaitState);
    REQUIRE_MSG(active.has_value(), "fixture produced no active ARIB caption cue to flush" +
                DiagnosticState(s, activeWaitState ? &*activeWaitState : nullptr));
    const auto beforeCaption = ReadCaption(*active);
    REQUIRE(beforeCaption.has_value());

    CHECK_EQ(s.player->Pause(), 0);
    const bool pausedForSeek = WaitFor([&] { return s.player->State() == PlayerState::Paused; }, 6.0);
    if (!pausedForSeek) {
        const std::string reason = "player did not pause with an active caption for seek" + DiagnosticState(s);
        FlushPreThrowDiagnostic(reason);
        REQUIRE_MSG(pausedForSeek, reason);
    }
    REQUIRE_MSG(pausedForSeek, "player did not pause with an active caption for seek");
    const auto pausedDebug = ReadDebug(*s.player);
    REQUIRE(pausedDebug.has_value());
    const auto pausedCaption = ReadCaption(*pausedDebug);
    if (!pausedCaption || pausedCaption->rects <= 0) {
        const std::string reason = "caption cue ended before the seek-flush baseline was captured" +
                                   DiagnosticState(s, &*pausedDebug);
        FlushPreThrowDiagnostic(reason);
        throw wvptest::SkipFailure{ reason };
    }
    const double beforePosition = s.player->Position();
    const double target = beforePosition < info->Duration * 0.55
        ? std::min(info->Duration - 1.0, beforePosition + 15.0)
        : std::max(0.0, beforePosition - 15.0);
    if (std::abs(target - beforePosition) < 2.0) {
        const std::string reason = "fixture is too short to select a distinct seek target" +
                                   DiagnosticState(s, &*pausedDebug);
        FlushPreThrowDiagnostic(reason);
        throw wvptest::SkipFailure{ reason };
    }

    const int clearedBefore = pausedCaption->cuesCleared;
    CHECK_EQ(s.player->Seek(target), 0);
    std::optional<json::Value> seekState;
    const bool seekCompleted = WaitFor([&] {
        auto debug = ReadDebug(*s.player);
        if (!debug) return false;
        seekState = *debug;
        const auto now = ReadCaption(*debug);
        return now && now->cuesCleared > clearedBefore && std::abs(s.player->Position() - target) < 2.0;
    }, 12.0);
    if (!seekCompleted) {
        const std::string reason = "seek did not flush the active caption serial or reach its target" +
                                   DiagnosticState(s, seekState ? &*seekState : nullptr);
        FlushPreThrowDiagnostic(reason);
        REQUIRE_MSG(seekCompleted, reason);
    }
    REQUIRE_MSG(seekCompleted, "seek did not flush the active caption serial or reach its target");
}

TEST_CASE_INTEGRATION(AribAdvanced_sdl_resize_rerenders_without_losing_active_cue) {
    RequireFixture();
    Session s;
    s.Open(L"sdl");
    const auto info = s.player->GetMediaInfo();
    REQUIRE(info.has_value());
    const auto videoStreams = info->Of(StreamType::Video);
    REQUIRE(!videoStreams.empty());
    std::optional<json::Value> activeWaitState;
    auto active = WaitForActive(s, 30.0, &activeWaitState);
    REQUIRE_MSG(active.has_value(), "fixture produced no active ARIB caption cue" +
                DiagnosticState(s, activeWaitState ? &*activeWaitState : nullptr));
    auto before = ReadCaption(*active);
    REQUIRE(before.has_value());

    CHECK_EQ(s.player->Pause(), 0);
    const bool pausedForResize = WaitFor([&] { return s.player->State() == PlayerState::Paused; }, 6.0);
    if (!pausedForResize) {
        const std::string reason = "player did not pause with an active caption for resize" + DiagnosticState(s);
        FlushPreThrowDiagnostic(reason);
        REQUIRE_MSG(pausedForResize, reason);
    }
    REQUIRE_MSG(pausedForResize, "player did not pause with an active caption for resize");
    const auto paused = ReadDebug(*s.player);
    REQUIRE(paused.has_value());
    before = ReadCaption(*paused);
    REQUIRE(before.has_value());
    if (before->rects <= 0 || !before->start) {
        const std::string reason = "caption cue ended while pausing before the resize check" +
                                   DiagnosticState(s, &*paused);
        FlushPreThrowDiagnostic(reason);
        throw wvptest::SkipFailure{ reason };
    }
    const double oldStart = *before->start;
    const bool oldEndIsIndefinite = before->indefinite;
    const std::optional<double> oldEnd = before->end;
    const int oldRenders = before->rerenders;

    int clientWidth = 0, clientHeight = 0;
    REQUIRE_MSG(GetClientSize(s.host.video, clientWidth, clientHeight), "could not query the initial hidden host size");
    double displayAspect = videoStreams.front().Height > 0
        ? static_cast<double>(videoStreams.front().Width) / videoStreams.front().Height : 16.0 / 9.0;
    if (videoStreams.front().SarNum > 0 && videoStreams.front().SarDen > 0)
        displayAspect *= static_cast<double>(videoStreams.front().SarNum) / videoStreams.front().SarDen;
    if (displayAspect <= 0.0) displayAspect = 16.0 / 9.0;
    const int targetWidth = std::max(960, clientWidth * 3 / 2);
    const int targetHeight = std::max(1, static_cast<int>(std::lround(targetWidth / displayAspect)));
    REQUIRE_MSG(ResizeVideoWindow(s.host.video, targetWidth, targetHeight), "SetWindowPos failed for the resize test");
    int expectedWidth = 0, expectedHeight = 0;
    REQUIRE_MSG(GetClientSize(s.host.video, expectedWidth, expectedHeight), "could not query the resized hidden host size");

    bool observedBlank = false;
    std::optional<CaptionDebug> after;
    std::optional<json::Value> lastResizeState;
    const bool resized = WaitFor([&] {
        auto debug = ReadDebug(*s.player);
        if (!debug) return false;
        lastResizeState = *debug;
        const auto now = ReadCaption(*debug);
        if (!now) return false;
        if (now->rects == 0) observedBlank = true;
        if (now->canvasWidth != before->canvasWidth || now->canvasHeight != before->canvasHeight) {
            if (now->rerenders > oldRenders && now->canvasWidth > 0 && now->canvasHeight > 0) {
                after = *now;
                return true;
            }
        }
        return false;
    }, 12.0);
    if (!resized) {
        const std::string reason = "ARIB canvas did not update after resizing the hidden host" +
                                   DiagnosticState(s, lastResizeState ? &*lastResizeState : nullptr);
        FlushPreThrowDiagnostic(reason);
        REQUIRE_MSG(resized, reason);
    }
    REQUIRE_MSG(resized, "ARIB canvas did not update after resizing the hidden host");
    CHECK_MSG(!observedBlank, "active caption layer became blank while the resize render was pending" +
              DiagnosticState(s, lastResizeState ? &*lastResizeState : nullptr));
    REQUIRE(after.has_value());
    CHECK_MSG(std::abs(after->canvasWidth - expectedWidth) <= 2 && std::abs(after->canvasHeight - expectedHeight) <= 2,
              "ARIB canvas does not match the resized video rect" +
              DiagnosticState(s, lastResizeState ? &*lastResizeState : nullptr));
    CHECK_MSG(after->rects > 0 && after->start && std::abs(*after->start - oldStart) < 0.01,
              "resize did not preserve the active cue and its start time" +
              DiagnosticState(s, lastResizeState ? &*lastResizeState : nullptr));
    CHECK_EQ(after->indefinite, oldEndIsIndefinite);
    if (oldEnd && after->end)
        CHECK_MSG(std::abs(*after->end - *oldEnd) < 0.01,
                  "resize changed the active cue end time" +
                  DiagnosticState(s, lastResizeState ? &*lastResizeState : nullptr));

    auto videoOnly = s.Snapshot(false);
    auto withCaption = s.Snapshot(true);
    REQUIRE(videoOnly.has_value() && withCaption.has_value());
    CHECK_MSG(DifferentPixels(*videoOnly, *withCaption) > 0,
              "resized active cue is absent from the with-subtitles snapshot" +
              DiagnosticState(s, lastResizeState ? &*lastResizeState : nullptr));
}

// A with-captions snapshot is re-rendered at the snapshot's own (window independent) size. The same paused cue
// snapshotted under two different window sizes must therefore be byte-identical; a scaled copy of the live
// bitmap would differ between the two sizes.
TEST_CASE_INTEGRATION(AribAdvanced_sdl_snapshot_exact_rerender_is_window_size_independent) {
    RequireFixture();
    Session s;
    s.Open(L"sdl");
    auto active = WaitForActive(s, 30.0);
    REQUIRE_MSG(active.has_value(), "fixture produced no active ARIB caption cue" + DiagnosticState(s));
    CHECK_EQ(s.player->Pause(), 0);
    REQUIRE_MSG(WaitFor([&] { return s.player->State() == PlayerState::Paused; }, 6.0),
                "player did not pause with an active caption" + DiagnosticState(s));

    auto readLive = [&]() -> std::optional<CaptionDebug> {
        const auto debug = ReadDebug(*s.player);
        return debug ? ReadCaption(*debug) : std::nullopt;
    };
    const auto start = readLive();
    REQUIRE(start.has_value());
    if (start->rects <= 0 || !start->start) {
        const std::string reason = "caption cue ended while pausing before the exact rerender check" + DiagnosticState(s);
        FlushPreThrowDiagnostic(reason);
        throw wvptest::SkipFailure{ reason };
    }
    const double cueStart = *start->start;

    auto videoA = s.Snapshot(false);
    auto capA = s.Snapshot(true);
    REQUIRE(videoA.has_value() && capA.has_value());
    const auto liveA = readLive();
    REQUIRE(liveA.has_value());
    CHECK_MSG(liveA->canvasWidth == start->canvasWidth && liveA->canvasHeight == start->canvasHeight,
              "snapshot re-render did not restore the live caption canvas" + DiagnosticState(s));

    int w0 = 0, h0 = 0;
    REQUIRE(GetClientSize(s.host.video, w0, h0));
    const int w1 = std::max(960, w0 * 3 / 2), h1 = std::max(1, h0 * w1 / w0);
    REQUIRE(ResizeVideoWindow(s.host.video, w1, h1));
    const bool settled = WaitFor([&] {
        const auto c = readLive();
        return c && c->rects > 0 && c->canvasWidth != start->canvasWidth && c->canvasHeight != start->canvasHeight;
    }, 12.0);
    if (!settled) {
        const std::string reason = "live ARIB canvas did not settle after the resize" + DiagnosticState(s);
        FlushPreThrowDiagnostic(reason);
        throw wvptest::SkipFailure{ reason };
    }
    Sleep(300);
    PumpMessages();

    auto videoB = s.Snapshot(false);
    auto capB = s.Snapshot(true);
    REQUIRE(videoB.has_value() && capB.has_value());
    const auto liveB = readLive();
    REQUIRE(liveB.has_value());
    if (liveB->rects <= 0 || !liveB->start || std::abs(*liveB->start - cueStart) > 0.01) {
        const std::string reason = "caption cue changed during the exact rerender check" + DiagnosticState(s);
        FlushPreThrowDiagnostic(reason);
        throw wvptest::SkipFailure{ reason };
    }

    REQUIRE_MSG(capA->Width == capB->Width && capA->Height == capB->Height,
                "snapshot size depends on the window size");
    if (!SamePixels(*videoA, *videoB)) {
        const std::string reason = "paused video frame changed between snapshots; cannot isolate the caption" +
                                   DiagnosticState(s);
        FlushPreThrowDiagnostic(reason);
        throw wvptest::SkipFailure{ reason };
    }
    CHECK_MSG(DifferentPixels(*videoA, *capA) > 0 && DifferentPixels(*videoB, *capB) > 0,
              "with-captions snapshot contains no caption pixels" + DiagnosticState(s));
    CHECK_MSG(SamePixels(*capA, *capB),
              "with-captions snapshots differ between window sizes: snapshot is not an exact rerender at its own size" +
              DiagnosticState(s));
}

TEST_CASE_INTEGRATION(AribAdvanced_sdl_new_caption_wins_observed_resize_race) {
    // Opportunistic probe: it needs a new cue to arrive while a resize re-render is pending, and no public API can
    // force that. It skips with a fixed reason unless
    // WVP_RACE_TESTS=1 (then it makes the timing-dependent attempts below). Real coverage needs a libffplay debug hook
    // that holds the resize re-render.
    {
        char* race = nullptr;
        size_t raceLen = 0;
        const bool enabled = _dupenv_s(&race, &raceLen, "WVP_RACE_TESTS") == 0 && race && race[0] == '1';
        std::free(race);
        if (!enabled) throw wvptest::SkipFailure{"opportunistic race probe; set WVP_RACE_TESTS=1 to attempt it"};
    }
    RequireFixture();
    Session s;
    s.Open(L"sdl");
    std::optional<json::Value> activeWaitState;
    auto active = WaitForActive(s, 30.0, &activeWaitState);
    REQUIRE_MSG(active.has_value(), "fixture produced no active ARIB caption cue" +
                DiagnosticState(s, activeWaitState ? &*activeWaitState : nullptr));
    auto first = ReadCaption(*active);
    if (!first.has_value() || !first->start) {
        const std::string reason = "active caption debug state did not contain cue timing" +
                                   DiagnosticState(s, &*active);
        FlushPreThrowDiagnostic(reason);
        REQUIRE_MSG(first.has_value() && first->start, reason);
    }
    REQUIRE_MSG(first.has_value() && first->start,
                "active caption debug state did not contain cue timing");

    double lastStart = *first->start;
    bool overlapObserved = false;
    double newerStart = lastStart;
    int overlapRenders = 0;
    const auto totalDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(18);
    std::optional<json::Value> lastRaceState;
    int resizeAttempt = 0;
    while (std::chrono::steady_clock::now() < totalDeadline && !overlapObserved) {
        auto baseDebug = ReadDebug(*s.player);
        if (baseDebug) lastRaceState = *baseDebug;
        const auto base = baseDebug ? ReadCaption(*baseDebug) : std::nullopt;
        if (!base) break;
        const int baseRenders = base->rerenders;
        const int width = resizeAttempt++ % 2 ? 1040 : 960;
        const int height = static_cast<int>(std::lround(width * 9.0 / 16.0));
        if (!ResizeVideoWindow(s.host.video, width, height)) break;
        const auto resizeStarted = std::chrono::steady_clock::now();
        auto resizeBaseDebug = ReadDebug(*s.player);
        if (resizeBaseDebug) lastRaceState = *resizeBaseDebug;
        const auto resizeBaseCaption = resizeBaseDebug ? ReadCaption(*resizeBaseDebug) : std::nullopt;
        const double attemptBaseStart = resizeBaseCaption && resizeBaseCaption->start
            ? *resizeBaseCaption->start : lastStart;
        const int attemptBaseShown = resizeBaseCaption ? resizeBaseCaption->cuesShown : base->cuesShown;
        lastStart = std::max(lastStart, attemptBaseStart);
        const auto attemptDeadline = resizeStarted + std::chrono::milliseconds(1500);
        while (std::chrono::steady_clock::now() < attemptDeadline) {
            auto debug = ReadDebug(*s.player);
            if (debug) lastRaceState = *debug;
            const auto now = debug ? ReadCaption(*debug) : std::nullopt;
            if (now && now->rects > 0 && now->start) {
                if (now->start.value() > attemptBaseStart + 0.01 && now->cuesShown > attemptBaseShown) {
                    const bool debounceElapsed = std::chrono::steady_clock::now() - resizeStarted >=
                        std::chrono::milliseconds(120);
                    if (now->indefinite && now->rerenders == baseRenders && debounceElapsed) {
                        overlapObserved = true;
                        newerStart = *now->start;
                        overlapRenders = baseRenders;
                        break;
                    }
                    lastStart = *now->start;
                }
                if (now->rerenders > baseRenders) break;
            }
            Sleep(10);
        }
        if (!overlapObserved) {
            (void)WaitFor([&] {
                auto debug = ReadDebug(*s.player);
                const auto now = debug ? ReadCaption(*debug) : std::nullopt;
                return now && now->rerenders > baseRenders;
            }, 2.0);
            if (auto debug = ReadDebug(*s.player)) {
                lastRaceState = *debug;
                if (const auto now = ReadCaption(*debug); now && now->start) lastStart = std::max(lastStart, *now->start);
            }
        }
    }
    if (!overlapObserved) {
        const std::string reason =
            "fixture produced no indefinite new cue during a pending resize; no deterministic public race trigger exists" +
            DiagnosticState(s, lastRaceState ? &*lastRaceState : nullptr);
        FlushPreThrowDiagnostic(reason);
        throw wvptest::SkipFailure{ reason };
    }

    bool regressed = false;
    const bool completed = WaitFor([&] {
        auto debug = ReadDebug(*s.player);
        if (debug) lastRaceState = *debug;
        const auto now = debug ? ReadCaption(*debug) : std::nullopt;
        if (!now) return false;
        if (now->start && *now->start + 0.01 < newerStart) regressed = true;
        return now->rerenders > overlapRenders;
    }, 8.0);
    if (!completed) {
        const std::string reason = "resize re-render did not complete after the newer cue arrived" +
                                   DiagnosticState(s, lastRaceState ? &*lastRaceState : nullptr);
        FlushPreThrowDiagnostic(reason);
        REQUIRE_MSG(completed, reason);
    }
    REQUIRE_MSG(completed, "resize re-render did not complete after the newer cue arrived");
    CHECK_MSG(!regressed, "stale resize re-render replaced the newer active cue" +
              DiagnosticState(s, lastRaceState ? &*lastRaceState : nullptr));
    const auto latest = ReadDebug(*s.player);
    REQUIRE(latest.has_value());
    const auto latestCaption = ReadCaption(*latest);
    REQUIRE(latestCaption.has_value());
    CHECK_MSG(!latestCaption->start || *latestCaption->start + 0.01 >= newerStart,
              "new cue generation lost to an older resize result" + DiagnosticState(s, &*latest));
}

TEST_CASE_INTEGRATION(AribAdvanced_d3d11_overlay_counters_preserve_direct_video_path) {
    RequireFixture();
    Session s;
    s.Open(L"d3d11", L"d3d11va", true);
    const auto firstDebug = ReadDebug(*s.player);
    const auto firstCaption = firstDebug ? ReadCaption(*firstDebug) : std::nullopt;
    const int initialShown = firstCaption ? firstCaption->cuesShown : 0;
    const int initialUploaded = firstCaption ? firstCaption->cuesUploaded : 0;

    auto directPath = WaitFor([&] {
        auto debug = ReadDebug(*s.player);
        if (!debug || PresenterName(*debug) != "d3d11") return false;
        const json::Value* video = Member(&*debug, "video");
        const json::Value* counts = Member(video, "frame_path_counts");
        const auto direct = Number(counts, "hw_direct");
        const auto downloads = Number(counts, "hw_download");
        const auto sinkDownloads = Number(counts, "sink_download");
        const auto device = Member(video, "hwaccel_device");
        return device && device->kind == json::Value::Kind::String && device->s == "d3d11va" &&
               direct && *direct > 0 && downloads && *downloads == 0 && sinkDownloads && *sinkDownloads == 0;
    }, 12.0);
    if (!directPath)
        throw wvptest::SkipFailure{ "D3D11 presenter plus direct D3D11VA decode is unavailable on this host" };

    auto baseline = ReadDebug(*s.player);
    REQUIRE(baseline.has_value());
    const json::Value* baseVideo = Member(&*baseline, "video");
    const json::Value* basePaths = Member(baseVideo, "frame_path_counts");
    const int initialDownloads = static_cast<int>(Number(basePaths, "hw_download").value_or(0));
    const int initialSinkDownloads = static_cast<int>(Number(basePaths, "sink_download").value_or(0));
    const json::Value* basePresenter = Member(&*baseline, "presenter");
    const int initialDeviceCopies = static_cast<int>(Number(basePresenter, "frames_d3d11_copied").value_or(0));
    if (initialDeviceCopies != 0)
        throw wvptest::SkipFailure{ "D3D11 direct decode path requires device-local copies on this host" };

    std::optional<json::Value> displayed;
    const bool captionShown = WaitFor([&] {
        auto debug = ReadDebug(*s.player);
        if (!debug) return false;
        const auto caption = ReadCaption(*debug);
        if (!caption || caption->cuesShown <= initialShown || caption->cuesUploaded <= initialUploaded) return false;
        displayed = std::move(debug);
        return true;
    }, 30.0);
    REQUIRE_MSG(captionShown, "D3D11 debug counters did not report a shown and uploaded caption overlay");
    CHECK_EQ(PresenterName(*displayed), std::string("d3d11"));

    const json::Value* video = Member(&*displayed, "video");
    const json::Value* paths = Member(video, "frame_path_counts");
    const json::Value* presenter = Member(&*displayed, "presenter");
    CHECK_MSG(Number(paths, "hw_direct").value_or(0) > 0, "video stopped using the direct D3D11 frame path");
    CHECK_EQ(static_cast<int>(Number(paths, "hw_download").value_or(0)), initialDownloads);
    CHECK_EQ(static_cast<int>(Number(paths, "sink_download").value_or(0)), initialSinkDownloads);
    CHECK_MSG(Number(presenter, "frames_direct").value_or(0) > 0, "D3D11 presenter did not report direct video frames");
    CHECK_EQ(static_cast<int>(Number(presenter, "frames_d3d11_copied").value_or(-1)), initialDeviceCopies);
}

namespace {

// tbs-captions.ts cue timeline (stream timestamps, s): indefinite cues at 12.455, 19.129, 21.965, ..., 42.052; one with
// a finite duration at 15.292 (to 17.992); the next cue starts at 19.129. The history window covers all of it.
constexpr double kCueA = 19.129;       // indefinite, replaced by the cue at 21.965
constexpr double kInsideA = 20.5;
constexpr double kCueB = 12.455;       // indefinite, replaced by the cue at 15.292
constexpr double kInsideB = 13.9;
constexpr double kAfterFiniteCue = 18.5;   // the cue at 15.292 ended at 17.992, the next one starts at 19.129
constexpr double kUnplayed = 43.8;     // inside the cue at 42.052, far beyond what the first seconds decode
constexpr int64_t kHistoryBudget = 5 * 1024 * 1024;

struct HistoryDebug {
    int64_t events = 0;
    int64_t bytes = 0;
    int64_t appended = 0;
    int landed = 0;
};

std::optional<HistoryDebug> ReadHistory(const json::Value& root) {
    const json::Value* history = Member(Member(Member(&root, "subtitle_layers"), "caption"), "history");
    if (!history || history->kind != json::Value::Kind::Object) return std::nullopt;
    HistoryDebug result;
    result.events = static_cast<int64_t>(Number(history, "events").value_or(0));
    result.bytes = static_cast<int64_t>(Number(history, "bytes").value_or(0));
    result.appended = static_cast<int64_t>(Number(history, "appended_bytes").value_or(0));
    result.landed = static_cast<int>(Number(history, "landed").value_or(0));
    return result;
}

std::optional<double> ClockOf(Session& s) {
    auto debug = ReadDebug(*s.player);
    return debug ? VideoClock(*debug) : std::nullopt;
}

// Stream timestamp minus player position: converts a cue time into a Seek / ss argument.
double StreamOffset(Session& s) {
    const auto clock = ClockOf(s);
    return clock ? *clock - s.player->Position() : 0.0;
}

bool CueShown(const json::Value& debug, double start) {
    const auto caption = ReadCaption(debug);
    return caption && caption->rects > 0 && caption->start && std::abs(*caption->start - start) < 0.02;
}

// A seek or reopen has landed: the video clock is within a keyframe's reach of the target.
bool Landed(const json::Value& debug, double target) {
    const auto clock = VideoClock(debug);
    return clock && std::abs(*clock - target) < 0.9;
}

// A hard link to the fixture under a path no other test uses: the decoder keeps caption histories per file path,
// so a link starts with an empty one however much the other tests played the fixture.
struct FixtureLink {
    fs::path path;
    explicit FixtureLink(const wchar_t* tag) {
        path = FixturePath().parent_path() / (std::wstring(L"history-") + tag + L"-" + std::to_wstring(GetTickCount64()) + L".ts");
        std::error_code ec;
        fs::create_hard_link(FixturePath(), path, ec);
        if (ec) throw wvptest::SkipFailure{ "could not link the fixture for an isolated caption history" };
    }
    ~FixtureLink() {
        std::error_code ec;
        fs::remove(path, ec);
    }
};

void OpenFixtureAs(Session& s, const fs::path& file, double ss, bool paused) {
    const int before = s.prepared;
    s.player->Stop();
    OpenOptions options = {
        { L"presenter", L"sdl" },
        { L"superimpose", L"0" },
        { L"arib_font", FontPath().wstring() },
    };
    if (ss > 0.0) options.emplace_back(L"ss", std::to_wstring(ss));
    if (paused) options.emplace_back(L"start_paused", L"1");
    REQUIRE_MSG(s.player->Open(file.wstring(), options) == 0, "ARIB fixture reopen rejected");
    REQUIRE_MSG(WaitFor([&] { return s.prepared > before || s.errors > 0; }, 25.0), "reopen produced no PREPARED event");
    REQUIRE_MSG(s.errors == 0, "reopen raised a player error");
}

// Plays until the video clock passes `until` and a cue has been shown; returns the stream offset.
double PlayPast(Session& s, double until) {
    const bool played = WaitFor([&] {
        auto debug = ReadDebug(*s.player);
        if (!debug) return false;
        const auto clock = VideoClock(*debug);
        const auto caption = ReadCaption(*debug);
        return clock && *clock >= until && caption && caption->cuesShown > 0;
    }, 45.0);
    REQUIRE_MSG(played, "fixture did not play to " + std::to_string(until) + " s" + DiagnosticState(s));
    return StreamOffset(s);
}

}

TEST_CASE_INTEGRATION(AribAdvanced_sdl_seek_back_shows_active_caption_from_history) {
    RequireFixture();
    FixtureLink link(L"seek");
    Session s;
    OpenFixtureAs(s, link.path, 0.0, false);
    const auto info = s.player->GetMediaInfo();
    REQUIRE(info.has_value());
    if (!info->Seekable) throw wvptest::SkipFailure{ "ARIB fixture is not seekable" };
    const double offset = PlayPast(s, 24.0);

    // The history holds the played cues, within its budget.
    {
        const auto debug = ReadDebug(*s.player);
        REQUIRE(debug.has_value());
        const auto history = ReadHistory(*debug);
        REQUIRE_MSG(history.has_value(), "debug info has no caption history block");
        CHECK_MSG(history->events >= 4, "caption history holds fewer than the played cues");
        CHECK(history->bytes > 0);
        CHECK_MSG(history->bytes <= kHistoryBudget, "caption history exceeds its 5 MiB budget");
        std::fprintf(stderr, "[ARIB history] fixture: %lld events, %lld bytes held, %lld appended\n",
                     static_cast<long long>(history->events), static_cast<long long>(history->bytes),
                     static_cast<long long>(history->appended));
    }

    // Playing: a backward seek into an active cue whose packet is not read again.
    const int landedBefore = ReadHistory(*ReadDebug(*s.player)).value_or(HistoryDebug{}).landed;
    CHECK_EQ(s.player->Seek(kInsideA - offset), 0);
    std::optional<json::Value> state;
    const bool playingShown = WaitFor([&] {
        auto debug = ReadDebug(*s.player);
        if (!debug) return false;
        state = *debug;
        return Landed(*debug, kInsideA) && CueShown(*debug, kCueA);
    }, 12.0);
    CHECK_MSG(playingShown, "backward seek into an active cue did not show it" + DiagnosticState(s, state ? &*state : nullptr));
    CHECK_MSG(ReadHistory(*state).value_or(HistoryDebug{}).landed > landedBefore,
              "the cue did not come from the caption history");
    CHECK_EQ(s.player->State(), PlayerState::Playing);

    // Paused: the landing frame carries the cue too.
    CHECK_EQ(s.player->Pause(), 0);
    REQUIRE(WaitFor([&] { return s.player->State() == PlayerState::Paused; }, 6.0));
    CHECK_EQ(s.player->Seek(kInsideB - offset), 0);
    const bool pausedShown = WaitFor([&] {
        auto debug = ReadDebug(*s.player);
        if (!debug) return false;
        state = *debug;
        return Landed(*debug, kInsideB) && CueShown(*debug, kCueB);
    }, 12.0);
    CHECK_MSG(pausedShown, "paused backward seek into an active cue did not show it" + DiagnosticState(s, state ? &*state : nullptr));
    CHECK_EQ(s.player->State(), PlayerState::Paused);

    // A cue that ended before the landing point is not shown.
    CHECK_EQ(s.player->Seek(kAfterFiniteCue - offset), 0);
    REQUIRE(WaitFor([&] {
        auto debug = ReadDebug(*s.player);
        return debug && Landed(*debug, kAfterFiniteCue);
    }, 12.0));
    WaitFor([] { return false; }, 0.8);
    {
        const auto debug = ReadDebug(*s.player);
        REQUIRE(debug.has_value());
        const auto caption = ReadCaption(*debug);
        REQUIRE(caption.has_value());
        CHECK_MSG(caption->rects == 0, "a finished cue was shown at the landing point" + DiagnosticState(s, &*debug));
    }

    // Not played yet this session: no history covers it, so nothing shows until the next cue.
    const int landedUncovered = ReadHistory(*ReadDebug(*s.player)).value_or(HistoryDebug{}).landed;
    CHECK_EQ(s.player->Seek(kUnplayed - offset), 0);
    REQUIRE(WaitFor([&] {
        auto debug = ReadDebug(*s.player);
        return debug && Landed(*debug, kUnplayed);
    }, 12.0));
    WaitFor([] { return false; }, 0.8);
    {
        const auto debug = ReadDebug(*s.player);
        REQUIRE(debug.has_value());
        const auto caption = ReadCaption(*debug);
        REQUIRE(caption.has_value());
        CHECK_MSG(caption->rects == 0, "an unplayed part showed a cue that was never decoded" + DiagnosticState(s, &*debug));
        CHECK_EQ(ReadHistory(*debug).value_or(HistoryDebug{}).landed, landedUncovered);
    }
}

TEST_CASE_INTEGRATION(AribAdvanced_sdl_same_file_reopen_continues_caption_history) {
    RequireFixture();
    FixtureLink link(L"same");
    FixtureLink other(L"other");
    Session s;
    OpenFixtureAs(s, link.path, 0.0, false);
    const auto info = s.player->GetMediaInfo();
    REQUIRE(info.has_value());
    if (!info->Seekable) throw wvptest::SkipFailure{ "ARIB fixture is not seekable" };
    const double offset = PlayPast(s, 24.0);

    // The same file reopened paused inside an active cue (a device-loss or settings reopen): a new decoder, the
    // same history.
    OpenFixtureAs(s, link.path, kInsideA - offset, true);
    std::optional<json::Value> state;
    const bool sameFile = WaitFor([&] {
        auto debug = ReadDebug(*s.player);
        if (!debug) return false;
        state = *debug;
        return s.player->State() == PlayerState::Paused && Landed(*debug, kInsideA) && CueShown(*debug, kCueA);
    }, 12.0);
    CHECK_MSG(sameFile, "reopen of the same file did not show the active cue on the paused frame" +
                        DiagnosticState(s, state ? &*state : nullptr));
    CHECK_MSG(ReadHistory(*state).value_or(HistoryDebug{}).landed >= 1, "the reopened cue did not come from the history");

    // A different file (another path) starts with an empty history, even with the same bytes.
    OpenFixtureAs(s, other.path, kInsideA - offset, true);
    REQUIRE(WaitFor([&] {
        auto debug = ReadDebug(*s.player);
        return debug && s.player->State() == PlayerState::Paused && Landed(*debug, kInsideA);
    }, 12.0));
    WaitFor([] { return false; }, 1.0);
    const auto debug = ReadDebug(*s.player);
    REQUIRE(debug.has_value());
    const auto caption = ReadCaption(*debug);
    REQUIRE(caption.has_value());
    CHECK_MSG(caption->rects == 0, "a different file inherited the caption history" + DiagnosticState(s, &*debug));
    CHECK_EQ(ReadHistory(*debug).value_or(HistoryDebug{}).landed, 0);
}
