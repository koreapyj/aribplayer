// Headless libffplay integration tests.
// Hidden host window (an off-screen popup that is never shown, with a child), SDL_AUDIODRIVER=dummy (set in main),
// and a message pump while waiting so events are dispatched exactly like the UI does.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <numeric>
#include <string>
#include <vector>
#include <windows.h>

#include "ffplay.h"
#include "wvp/FfPlayer.h"
#include "wvp/FrameStepLogic.h"
#include "wvp/JsonLite.h"
#include "wvp/Utf.h"
#include "wvptest.h"

using namespace wvp;
namespace fs = std::filesystem;

namespace {

constexpr UINT kNotifyMsg = WM_APP + 41;

LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == kNotifyMsg) { FfPlayer::HandleNotify(w, l); return 0; }
    return DefWindowProcW(h, m, w, l);
}

ATOM EnsureClass() {
    static ATOM a = [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = HostProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"WvpTestHost";
        return RegisterClassW(&wc);
    }();
    return a;
}

fs::path RepoRoot() {
    static fs::path root = [] {
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
    }();
    return root;
}

// Returns the full path of a test media file, generating the set with scripts\make-testmedia.ps1 if missing.
std::wstring Media(const wchar_t* name) {
    fs::path p = RepoRoot() / L"testmedia" / name;
    if (!fs::exists(p)) {
        std::wstring cmd = L"powershell -NoProfile -ExecutionPolicy Bypass -File \"" +
                           (RepoRoot() / L"scripts" / L"make-testmedia.ps1").wstring() + L"\"";
        _wsystem(cmd.c_str());
    }
    return p.wstring();
}

void Pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

template <class F>
bool WaitFor(F cond, double seconds = 10) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds((long long)(seconds * 1000));
    while (std::chrono::steady_clock::now() < end) {
        Pump();
        if (cond()) return true;
        Sleep(15);
    }
    Pump();
    return cond();
}

void Delay(int ms) { WaitFor([] { return false; }, ms / 1000.0); }

struct HostWindow {
    HWND parent = nullptr, child = nullptr;
    HostWindow(int w = 640, int h = 360) {
        EnsureClass();
        HINSTANCE inst = GetModuleHandleW(nullptr);
        parent = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"WvpTestHost", L"host", WS_POPUP, -32000, -32000, w, h,
                                 nullptr, nullptr, inst, nullptr);
        child = CreateWindowExW(0, L"STATIC", L"video", WS_CHILD | WS_VISIBLE, 0, 0, w, h, parent, nullptr, inst, nullptr);
    }
    ~HostWindow() { if (parent) DestroyWindow(parent); }
};

struct Session {
    HostWindow host;
    std::unique_ptr<FfPlayer> player;
    std::vector<std::wstring> log;
    std::vector<MediaPreparedInfo> prepared;
    std::vector<PlayerErrorInfo> errors;
    std::vector<FilterErrorInfo> filterErrors;
    std::vector<PlayerState> states;
    int positions = 0, ended = 0, stepDones = 0;
    double lastPosition = -1;
    std::function<void(int, bool)> onStepDone;
    struct KeyDone { int dir = 0; bool moved = false; double position = NAN, key = NAN; };
    int keyDones = 0;
    KeyDone lastKey;
    std::function<void(const KeyDone&)> onKeyDone;

    Session() {
        int err = 0;
        player = FfPlayer::CreateSync(host.child, host.parent, kNotifyMsg, &err);
        REQUIRE_MSG(player != nullptr, "CreateSync failed: " + std::to_string(err));
        player->Prepared.add([this](const MediaPreparedInfo& p) { prepared.push_back(p); log.push_back(L"prepared"); });
        player->StateChanged.add([this](PlayerState s) { states.push_back(s); log.push_back(L"state " + std::to_wstring((int)s)); });
        player->PositionChanged.add([this](double pos, double) { lastPosition = pos; positions++; });
        player->Ended.add([this] { ended++; });
        player->FrameStepDone.add([this](int dir, bool moved) { stepDones++; if (onStepDone) onStepDone(dir, moved); });
        player->KeyframeSeekDone.add([this](int dir, bool moved, double position, double key) {
            lastKey = {dir, moved, position, key};
            keyDones++;
            if (onKeyDone) onKeyDone(lastKey);
        });
        player->Error.add([this](const PlayerErrorInfo& e) { errors.push_back(e); log.push_back(L"error " + e.Message); });
        player->FilterError.add([this](const FilterErrorInfo& e) { filterErrors.push_back(e); log.push_back(L"filtererror " + e.Type); });
    }
    ~Session() { player.reset(); }

    bool HasState(PlayerState s) const { for (auto x : states) if (x == s) return true; return false; }
    std::string LogText() const {
        std::string s; for (auto& l : log) s += ToUtf8(l) + " | "; return s;
    }

    MediaPreparedInfo OpenAndPrepare(const std::wstring& file, const OpenOptions& opts = {}) {
        size_t before = prepared.size();
        REQUIRE_MSG(player->Open(file, opts) == 0, "Open failed");
        bool ok = WaitFor([&] { return prepared.size() > before || !errors.empty(); });
        REQUIRE_MSG(ok, "no Prepared event; log: " + LogText());
        REQUIRE_MSG(errors.empty(), "open error: " + LogText());
        return prepared.back();
    }

    BgraFrame WaitSnapshot(double seconds = 10) {
        std::optional<BgraFrame> f;
        bool ok = WaitFor([&] { f = player->Snapshot(); return f.has_value(); }, seconds);
        REQUIRE_MSG(ok, "no frame displayed; log: " + LogText());
        return std::move(*f);
    }
};

}

TEST_CASE(JsonLite_parses_media_info) {
    const char* js = R"({"url":"aéb","title":"T","format":"mov,mp4","duration":10.5,"seekable":true,"bit_rate":1234567,
        "streams":[{"index":0,"type":"video","codec":"h264","selected":true,"width":1280,"height":720,"sar_num":1,"sar_den":1,"fps":30.0,"pix_fmt":"yuv420p"},
                   {"index":1,"type":"audio","codec":"aac","selected":true,"sample_rate":48000,"channels":2,"language":"eng"}]})";
    auto mi = MediaInfo::Parse(js);
    REQUIRE(mi.has_value());
    CHECK_EQ(mi->Url, std::wstring(L"aéb"));
    CHECK_EQ(mi->Streams.size(), (size_t)2);
    CHECK_EQ(mi->Of(StreamType::Video).size(), (size_t)1);
    CHECK_EQ(mi->Streams[0].Width, 1280);
    CHECK_EQ(mi->Streams[1].SampleRate, 48000);
    CHECK_EQ(mi->BitRate, (std::int64_t)1234567);
    CHECK(mi->Seekable);
    CHECK(!MediaInfo::Parse("{bad").has_value());
}

TEST_CASE(PreparedInfo_display_size_applies_sar) {
    MediaPreparedInfo i; i.Width = 720; i.Height = 576; i.SarNum = 16; i.SarDen = 15; i.Duration = 10;
    CHECK_EQ(i.DisplayWidth(), 768);
    CHECK_EQ(i.DisplayHeight(), 576);
}

TEST_CASE(Event_multicast_add_remove) {
    Event<int> e;
    int a = 0, b = 0;
    auto ta = e.add([&](int v) { a += v; });
    e.add([&](int v) { b += v; });
    e.raise(2);
    e.remove(ta);
    e.raise(3);
    CHECK_EQ(a, 2);
    CHECK_EQ(b, 5);
}

TEST_CASE_INTEGRATION(FfPlayer_reports_ffmpeg_version) {
    CHECK(!FfPlayer::Version().empty());
}

TEST_CASE_INTEGRATION(FfPlayer_open_raises_prepared_with_media_properties) {
    Session s;
    auto info = s.OpenAndPrepare(Media(L"testsrc2-720p.mp4"));
    CHECK_EQ(info.Width, 1280);
    CHECK_EQ(info.Height, 720);
    CHECK(info.Duration > 9 && info.Duration < 11);
    CHECK(info.HasVideo);
    CHECK(info.HasAudio);
    CHECK(info.Seekable);
    CHECK(WaitFor([&] { return s.HasState(PlayerState::Playing); }));
    CHECK(s.player->Duration() > 9 && s.player->Duration() < 11);
    auto mi = s.player->GetMediaInfo();
    REQUIRE(mi.has_value());
    bool v = false, a = false;
    for (auto& st : mi->Streams) {
        if (st.Type == StreamType::Video && st.Codec == L"h264" && st.Width == 1280) v = true;
        if (st.Type == StreamType::Audio && st.SampleRate == 48000) a = true;
    }
    CHECK(v);
    CHECK(a);
}

TEST_CASE_INTEGRATION(FfPlayer_seek_pause_and_volume) {
    Session s;
    s.OpenAndPrepare(Media(L"testsrc2-720p.mp4"));
    CHECK_EQ(s.player->Seek(5), 0);
    CHECK_MSG(WaitFor([&] { double p = s.player->Position(); return p >= 4.5 && p < 8; }, 8),
              "position after seek: " + std::to_string(s.player->Position()));
    CHECK_EQ(s.player->Pause(), 0);
    CHECK(WaitFor([&] { return s.player->State() == PlayerState::Paused; }));
    CHECK_EQ(s.player->Seek(-2, true), 0);
    CHECK_MSG(WaitFor([&] { double p = s.player->Position(); return p >= 2 && p < 5.5; }, 5),
              "position after relative seek: " + std::to_string(s.player->Position()));
    s.player->SetVolume(30);
    s.player->SetMuted(true);
    s.OpenAndPrepare(Media(L"testsrc2-360p-short.mp4"));
    CHECK_EQ(s.player->Volume(), 30);
    CHECK(s.player->IsMuted());
}

TEST_CASE_INTEGRATION(FfPlayer_snapshot_is_native_size) {
    Session s;
    CHECK(!s.player->Snapshot().has_value());          // nothing displayed yet (FFP_EAGAIN)
    s.OpenAndPrepare(Media(L"testsrc2-720p.mp4"));
    auto f = s.WaitSnapshot();
    CHECK_EQ(f.Width, 1280);
    CHECK_EQ(f.Height, 720);
    CHECK(f.Stride >= f.Width * 4);
    CHECK_EQ(f.Data.size(), (size_t)f.Stride * (size_t)f.Height);
    CHECK_EQ((int)f.Data[3], 255);
    bool nonBlack = false;
    for (auto b : f.Data) if (b) { nonBlack = true; break; }
    CHECK(nonBlack);
}

TEST_CASE_INTEGRATION(FfPlayer_position_events_and_play_pause_toggle) {
    Session s;
    s.OpenAndPrepare(Media(L"testsrc2-720p.mp4"));
    CHECK_MSG(WaitFor([&] { return s.positions >= 3; }, 8), "too few Position events: " + std::to_string(s.positions));
    double p0 = s.lastPosition;
    CHECK(WaitFor([&] { return s.lastPosition > p0 + 0.5; }, 6));
    CHECK_EQ(s.player->Pause(), 0);
    CHECK(WaitFor([&] { return s.player->State() == PlayerState::Paused; }));
    CHECK(s.HasState(PlayerState::Paused));
    CHECK_EQ(s.player->Play(), 0);
    CHECK(WaitFor([&] { return s.player->State() == PlayerState::Playing; }));
    CHECK_EQ(s.player->TogglePause(), 0);
    CHECK(WaitFor([&] { return s.player->State() == PlayerState::Paused; }));
}

TEST_CASE_INTEGRATION(FfPlayer_snapshot_dimensions_are_post_filter) {
    Session s;
    s.OpenAndPrepare(Media(L"testsrc2-720p.mp4"), { { L"vf", L"transpose=1" } });
    auto f = s.WaitSnapshot();
    CHECK_EQ(f.Width, 720);
    CHECK_EQ(f.Height, 1280);
}

TEST_CASE_INTEGRATION(FfPlayer_stop_then_open_reuses_instance) {
    Session s;
    s.OpenAndPrepare(Media(L"testsrc2-720p.mp4"));
    CHECK_EQ(s.player->Stop(), 0);
    CHECK(WaitFor([&] { return s.player->State() == PlayerState::Stopped; }));
    auto info = s.OpenAndPrepare(Media(L"testsrc2-360p-short.mp4"));
    CHECK(info.Width > 0);
    CHECK(WaitFor([&] { return s.player->State() == PlayerState::Playing; }));
    s.WaitSnapshot();
}

TEST_CASE_INTEGRATION(FfPlayer_snapshot_applies_sample_aspect_ratio) {
    Session s;
    auto info = s.OpenAndPrepare(Media(L"sar-720x576.mp4"));      // 720x576, SAR 16:15 -> 768x576
    CHECK_EQ(info.Width, 720);
    CHECK_EQ(info.Height, 576);
    CHECK_EQ(info.SarNum, 16);
    CHECK_EQ(info.SarDen, 15);
    CHECK_EQ(info.DisplayWidth(), 768);
    CHECK_EQ(info.DisplayHeight(), 576);
    auto f = s.WaitSnapshot();
    CHECK_EQ(f.Width, 768);
    CHECK_EQ(f.Height, 576);
}

TEST_CASE_INTEGRATION(FfPlayer_filter_change_and_invalid_filter_reports_error) {
    Session s;
    s.OpenAndPrepare(Media(L"testsrc2-720p.mp4"));
    s.WaitSnapshot();
    CHECK_EQ(s.player->SetVideoFilter(L"transpose=1"), 0);
    CHECK_MSG(WaitFor([&] { auto f = s.player->Snapshot(); return f && f->Width == 720 && f->Height == 1280; }, 8),
              "snapshot did not switch to the rotated size; log: " + s.LogText());
    CHECK(s.filterErrors.empty());

    CHECK_EQ(s.player->SetVideoFilter(L"transpose=1,eq@color=brightness=0.1:contrast=1:saturation=1:gamma=1"), 0);
    Delay(500);
    CHECK_EQ(s.player->SendFilterCommand(L"eq@color", L"brightness", L"0.3"), 0);
    Delay(500);
    CHECK_MSG(s.filterErrors.empty(), "live command raised a filter error: " + s.LogText());

    CHECK_EQ(s.player->SetVideoFilter(L"definitely_not_a_filter=1"), 0);
    CHECK(WaitFor([&] { return !s.filterErrors.empty(); }, 8));
    if (!s.filterErrors.empty()) {
        CHECK_EQ(s.filterErrors[0].Type, std::wstring(L"video"));
        CHECK(s.filterErrors[0].Code != 0);
    }
    CHECK_EQ(s.player->SetAudioFilter(L"atempo=1.5"), 0);
    CHECK_EQ(s.player->SetAudioFilter(L""), 0);
}

TEST_CASE_INTEGRATION(FfPlayer_ended_event_fires_at_end_of_media) {
    Session s;
    s.OpenAndPrepare(Media(L"testsrc2-360p-short.mp4"));       // 3 s
    CHECK_MSG(WaitFor([&] { return s.ended > 0; }, 15), "no Ended; log: " + s.LogText());
    CHECK(WaitFor([&] { return s.player->State() == PlayerState::Ended; }, 3));
}

TEST_CASE_INTEGRATION(FfPlayer_missing_file_raises_error) {
    Session s;
    s.player->Open((RepoRoot() / L"testmedia" / L"does-not-exist.mp4").wstring());
    CHECK_MSG(WaitFor([&] { return !s.errors.empty(); }, 8), "no Error event; log: " + s.LogText());
}

TEST_CASE_INTEGRATION(FfPlayer_open_options_are_applied) {
    Session s;
    s.OpenAndPrepare(Media(L"testsrc2-720p.mp4"), { { L"ss", L"6" }, { L"vf", L"hflip" } });
    CHECK_MSG(WaitFor([&] { return s.player->Position() >= 5.5; }, 8), "ss option not applied; position " + std::to_string(s.player->Position()));
}

TEST_CASE_INTEGRATION(FfPlayer_invalid_open_options_rejected) {
    Session s;
    CHECK_EQ(s.player->Open(Media(L"testsrc2-720p.mp4"), { { L"a=b", L"c" } }), -22);
    CHECK_EQ(s.player->Open(Media(L"testsrc2-720p.mp4"), { { L"a", L"c\nd" } }), -22);
    CHECK_EQ(s.player->Open(L""), -22);
}

TEST_CASE_INTEGRATION(FfPlayer_twenty_open_stop_cycles) {
    Session s;
    auto file = Media(L"testsrc2-360p-short.mp4");
    for (int i = 0; i < 20; i++) {
        size_t before = s.prepared.size();
        REQUIRE_MSG(s.player->Open(file) == 0, "Open failed");
        bool ok = WaitFor([&] { return s.prepared.size() > before || !s.errors.empty(); }, 10);
        REQUIRE_MSG(ok, "cycle " + std::to_string(i) + ": no Prepared; log: " + s.LogText());
        REQUIRE_MSG(s.errors.empty(), "cycle " + std::to_string(i) + ": " + s.LogText());
        if (i % 3 == 0) Delay(50);
        CHECK_EQ(s.player->Stop(), 0);
    }
    CHECK_EQ(s.prepared.size(), (size_t)20);
}

TEST_CASE_INTEGRATION(FfPlayer_destroy_while_playing_is_clean) {
    auto host = std::make_unique<HostWindow>();
    int err = 0;
    auto p = FfPlayer::CreateSync(host->child, host->parent, kNotifyMsg, &err);
    REQUIRE(p != nullptr);
    int prepared = 0;
    p->Prepared.add([&](const MediaPreparedInfo&) { prepared++; });
    CHECK_EQ(p->Open(Media(L"testsrc2-1080p.mp4")), 0);
    CHECK(WaitFor([&] { return prepared > 0; }));
    CHECK(WaitFor([&] { return p->Snapshot().has_value(); }));
    p.reset();                                         // joins the native thread
    host.reset();
}

TEST_CASE_INTEGRATION(FfPlayer_destroy_after_notify_window_gone_is_safe) {
    auto host = std::make_unique<HostWindow>();
    auto p = FfPlayer::CreateSync(host->child, host->parent, kNotifyMsg);
    REQUIRE(p != nullptr);
    p->Open(Media(L"testsrc2-720p.mp4"));
    Delay(500);
    DestroyWindow(host->parent);
    host->parent = nullptr;
    p.reset();
}

TEST_CASE_INTEGRATION(FfPlayer_create_async_completes_on_ui_thread) {
    HostWindow host;
    std::unique_ptr<FfPlayer> got;
    int gotErr = 1;
    bool done = false;
    DWORD uiThread = GetCurrentThreadId(), cbThread = 0;
    FfPlayer::CreateAsync(host.child, host.parent, kNotifyMsg, [&](std::unique_ptr<FfPlayer> p, int err) {
        got = std::move(p); gotErr = err; done = true; cbThread = GetCurrentThreadId();
    });
    CHECK(WaitFor([&] { return done; }, 15));
    CHECK(got != nullptr);
    CHECK_EQ(gotErr, 0);
    CHECK_EQ(cbThread, uiThread);
    got.reset();
}

TEST_CASE_INTEGRATION(FfPlayer_create_and_destroy_many_instances) {
    HostWindow host;
    for (int i = 0; i < 5; i++) {
        auto p = FfPlayer::CreateSync(host.child, host.parent, kNotifyMsg);
        REQUIRE(p != nullptr);
        CHECK(p->State() == PlayerState::Idle);
    }
}

namespace {

struct PresentedFrameSample {
    long long frameCount = 0, hostAtUs = 0, serial = 0, dxgiPresentCount = -1, dxgiQpc = -1;
    double pts = NAN, avDiffMs = NAN;
    long long decodedCount = 0, filteredCount = 0;
    double decodedPts = NAN, filteredPts = NAN;
};

struct SeekMetric {
    std::string name;
    double latencyMs = NAN, latencyAfterLastMs = NAN, rate = NAN, displayedRate = NAN, avFinalMs = NAN, avAt500Ms = NAN, avPeakAbsMs = 0;
    double startPosition = NAN, expectedDelta = NAN, finalPosition = NAN, finalVideoClock = NAN, positionError = NAN;
    double requestedSeek = NAN, keyframeSeek = NAN, keyframeDistanceMs = NAN, gopMs = NAN;
    double expectedFrames = 0, displayIntervalP95Ms = NAN, displayIntervalMaxMs = NAN;
    double dxgiIntervalP95Ms = NAN, dxgiIntervalMaxMs = NAN, maxPtsGapMs = NAN;
    double presenterIntervalP95Ms = NAN;
    long long displayedAfterSeek = 0, earlyDrops = 0, lateDrops = 0, earlyDropsAfterFirst = 0, lateDropsAfterFirst = 0;
    long long presenterFrames = 0, ptsSkippedFrames = 0, unobservedPresentFrames = 0, dxgiPresents = 0, dxgiGpuTicks = 0;
    long long d3dGpuUs = 0, d3dGpuFrames = 0;
    long long audioQueueBytesMax = 0, videoQueueBytesMax = 0, subtitleQueueBytesMax = 0;
    double audioQueueSecondsMax = 0, videoQueueSecondsMax = 0, subtitleQueueSecondsMax = 0;
    long long requests = 0, gpuSubmitUs = 0, gpuSubmitFrames = 0;
    double requestCpuMs = NAN, cpuMsPerRequest = NAN;
    bool targetFrameDisplayed = false, dxgiStats = false;
    // Post-warm-up quality (samples taken >= 500 ms after the last request).
    double avPostMeanAbsMs = NAN, avPostMaxAbsMs = NAN;
    long long avPostSamples = 0;
    // Per presented frame: |host present interval - PTS gap| and DXGI present-count irregularities.
    double paceErrP95Ms = NAN, paceErrMaxMs = NAN;
    long long dxgiPresentCountGaps = 0, presentedFrames = 0, ptsDuplicates = 0;
    double dxgiRefreshP95Ms = NAN;
    double systemBusyPct = NAN;       // whole-machine CPU busy over the measurement window (isolation indicator)
};

long long MetricCount(const std::optional<std::int64_t>& value) { return value.value_or(0); }
double MetricNumber(const std::optional<double>& value) { return value.value_or(NAN); }
long long PresenterCount(const DebugInfo& d) {
    if (!d.Presenter.FramesDirect && !d.Presenter.FramesUploaded)
        return MetricCount(d.Fps.FramesDisplayed);
    return MetricCount(d.Presenter.FramesDirect) + MetricCount(d.Presenter.FramesUploaded);
}
double P95(std::vector<double>& values) {
    if (values.empty()) return NAN;
    std::sort(values.begin(), values.end());
    const size_t at = std::min(values.size() - 1, (values.size() * 95 + 99) / 100 - 1);
    return values[at];
}
DebugInfo CurrentDebug(Session& s) {
    auto js = s.player->GetDebugInfoJson();
    return js ? ParseDebugInfo(*js) : DebugInfo{};
}
double ProcessCpuTimeMs();
// Whole-machine busy percentage between two snapshots; used to label (not excuse) a run as loaded.
struct SystemTimes { unsigned long long idle = 0, kernel = 0, user = 0; };
SystemTimes SampleSystemTimes() {
    FILETIME i{}, k{}, u{};
    SystemTimes t;
    if (!GetSystemTimes(&i, &k, &u)) return t;
    auto q = [](const FILETIME& f) { return ((unsigned long long)f.dwHighDateTime << 32) | f.dwLowDateTime; };
    t.idle = q(i); t.kernel = q(k); t.user = q(u);
    return t;
}
double SystemBusyPct(const SystemTimes& a, const SystemTimes& b) {
    const unsigned long long total = (b.kernel - a.kernel) + (b.user - a.user);   // kernel time includes idle
    const unsigned long long idle = b.idle - a.idle;
    if (b.kernel < a.kernel || total == 0 || idle > total) return NAN;
    return 100.0 * (double)(total - idle) / (double)total;
}
// Position error allowed for a keyframe-snapped seek: one GOP plus first-present/sampling slack.
double GopBoundSeconds(double gopMs) {
    return (std::isfinite(gopMs) && gopMs > 0 ? gopMs : 1000.0) / 1000.0 + 0.25;
}
bool ClockMovedAfterSeek(const DebugInfo& before, const DebugInfo& current, double expectedDelta) {
    const double oldClock = before.Sync.VideoClock.value_or(before.Position.value_or(NAN));
    const double clock = current.Sync.VideoClock.value_or(current.Position.value_or(NAN));
    if (!std::isfinite(oldClock) || !std::isfinite(clock) || std::fabs(expectedDelta) < 0.25) return false;
    const double moved = clock - oldClock;
    const double threshold = std::max(0.5, std::fabs(expectedDelta) * 0.5);
    return expectedDelta > 0 ? moved >= threshold : moved <= -threshold;
}
bool SeekToAndWaitForFrame(Session& s, double target, double timeoutSeconds = 12.0) {
    const DebugInfo before = CurrentDebug(s);
    const long long oldFrames = MetricCount(before.Fps.FramesDisplayed);
    const double expectedDelta = target - s.player->Position();
    if (s.player->Seek(target, false) != 0) return false;
    const bool moved = WaitFor([&] {
        const DebugInfo d = CurrentDebug(s);
        return ClockMovedAfterSeek(before, d, expectedDelta) && MetricCount(d.Fps.FramesDisplayed) > oldFrames;
    }, timeoutSeconds);
    if (moved) Delay(500);
    return moved;
}
SeekMetric MeasureSeek(Session& s, const std::string& name, double seconds, bool relative,
                        int windowMs = 3000, int requestCount = 1, int interRequestDelayMs = 0,
                        const char* config = "full") {
    SeekMetric m;
    m.name = name;
    const DebugInfo before = CurrentDebug(s);
    const auto beforeSampledAt = std::chrono::steady_clock::now();
    const double currentPosition = s.player->Position();
    const double expectedDelta = relative ? seconds * requestCount : seconds - currentPosition;
    m.startPosition = currentPosition;
    m.expectedDelta = expectedDelta;
    const long long oldFrames = MetricCount(before.Fps.FramesDisplayed);
    const long long oldPresenter = PresenterCount(before);
    const long long oldEarly = MetricCount(before.Sync.FrameDropsEarly);
    const long long oldLate = MetricCount(before.Sync.FrameDropsLate);
    const double processCpuStartedMs = ProcessCpuTimeMs();
    const SystemTimes systemStart = SampleSystemTimes();
    const auto started = std::chrono::steady_clock::now();
    auto lastRequestAt = started;
    double avPostSumAbs = 0;
    long long measureStartClockUs = before.ClockUs.value_or(0), measureEndClockUs = measureStartClockUs + windowMs * 1000LL;
    for (int i = 0; i < requestCount; ++i) {
        if (s.player->Seek(seconds, relative) != 0) { m.latencyMs = INFINITY; return m; }
        if (i + 1 < requestCount && interRequestDelayMs > 0) Sleep(interRequestDelayMs);
        if (requestCount > 1) {
            const DebugInfo burst = CurrentDebug(s);
            std::printf("SEEKBURST | request=%d pos=%.3f debug_pos=%.3f video_clock=%.3f state=%d\n",
                        i + 1, s.player->Position(), burst.Position.value_or(NAN), burst.Sync.VideoClock.value_or(NAN),
                        (int)s.player->State());
        }
    }
    lastRequestAt = std::chrono::steady_clock::now();
    measureStartClockUs = before.ClockUs.value_or(0) +
        std::chrono::duration_cast<std::chrono::microseconds>(lastRequestAt - beforeSampledAt).count();
    measureEndClockUs = measureStartClockUs + windowMs * 1000LL;
    const DebugInfo afterRequests = CurrentDebug(s);
    m.requests = requestCount;
    m.requestCpuMs = ProcessCpuTimeMs() - processCpuStartedMs;
    m.cpuMsPerRequest = m.requests > 0 ? m.requestCpuMs / m.requests : NAN;
    if (afterRequests.FrameTrace.D3dGpuTotalUs && before.FrameTrace.D3dGpuTotalUs)
        m.gpuSubmitUs = *afterRequests.FrameTrace.D3dGpuTotalUs - *before.FrameTrace.D3dGpuTotalUs;
    if (afterRequests.FrameTrace.D3dGpuFrames && before.FrameTrace.D3dGpuFrames)
        m.gpuSubmitFrames = *afterRequests.FrameTrace.D3dGpuFrames - *before.FrameTrace.D3dGpuFrames;
    std::vector<double> displayIntervals, presenterIntervals;
    long long lastFrames = oldFrames, firstTargetFrames = oldFrames, lastPresenter = oldPresenter;
    long long finalSerial = before.FrameTrace.Presented.Serial.value_or(-1);
    long long dropsAtFirst = 0, lateAtFirst = 0;
    auto lastFrameAt = started, lastPresenterAt = started;
    bool collecting = false;
    long long nextClockSampleMs = 0;
    DebugInfo final = before;
    auto maxCount = [](const std::optional<std::int64_t>& v, long long& dst) { if (v) dst = std::max(dst, (long long)*v); };
    auto maxSeconds = [](const std::optional<double>& v, double& dst) {
        if (v && std::isfinite(*v)) dst = std::max(dst, *v);
    };
    while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - lastRequestAt).count() < windowMs) {
        Pump();
        DebugInfo d = CurrentDebug(s);
        if (d.Valid) {
            const auto now = std::chrono::steady_clock::now();
            const long long elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastRequestAt).count();
            if (elapsedMs >= nextClockSampleMs) {
                std::printf("SEEKCLOCK | config=%s seek=%s elapsed_ms=%lld pos=%.3f audio=%.3f video=%.3f master=%.3f av_ms=%.2f drift_ms=%.2f serial=%lld decoded=%lld filtered=%lld displayed=%lld drops=%lld/%lld queues_s=%.3f/%.3f\n",
                            config, name.c_str(), elapsedMs, d.Position.value_or(NAN), d.Sync.AudioClock.value_or(NAN),
                            d.Sync.VideoClock.value_or(NAN), d.Sync.MasterClock.value_or(NAN), d.Sync.AvDiffMs.value_or(NAN),
                            d.Sync.FrameTimerDriftMs.value_or(NAN), d.FrameTrace.Presented.Serial.value_or(-1),
                            MetricCount(d.Fps.FramesDecoded), MetricCount(d.Fps.FramesFiltered),
                            MetricCount(d.Fps.FramesDisplayed), MetricCount(d.Sync.FrameDropsEarly),
                            MetricCount(d.Sync.FrameDropsLate), d.AudioQueue.Duration.value_or(NAN), d.VideoQueue.Duration.value_or(NAN));
                nextClockSampleMs = elapsedMs + 100;
            }
            const long long frames = MetricCount(d.Fps.FramesDisplayed);
            const long long presenter = PresenterCount(d);
            if (!m.targetFrameDisplayed && frames > oldFrames && ClockMovedAfterSeek(before, d, expectedDelta)) {
                m.targetFrameDisplayed = true;
                m.latencyMs = std::chrono::duration<double, std::milli>(now - started).count();
                m.latencyAfterLastMs = std::chrono::duration<double, std::milli>(now - lastRequestAt).count();
                m.rate = d.Fps.FilterOut.value_or(0.0);
                finalSerial = d.FrameTrace.Presented.Serial.value_or(-1);
                dropsAtFirst = MetricCount(d.Sync.FrameDropsEarly);
                lateAtFirst = MetricCount(d.Sync.FrameDropsLate);
                firstTargetFrames = frames;
                lastFrames = frames;
                lastPresenter = presenter;
                lastFrameAt = lastPresenterAt = now;
                collecting = true;
            } else if (collecting) {
                if (frames > lastFrames) {
                    const long long n = frames - lastFrames;
                    const double ms = std::chrono::duration<double, std::milli>(now - lastFrameAt).count() / n;
                    for (long long i = 0; i < n; ++i) displayIntervals.push_back(ms);
                    lastFrames = frames;
                    lastFrameAt = now;
                }
                if (presenter > lastPresenter) {
                    const long long n = presenter - lastPresenter;
                    const double ms = std::chrono::duration<double, std::milli>(now - lastPresenterAt).count() / n;
                    for (long long i = 0; i < n; ++i) presenterIntervals.push_back(ms);
                    lastPresenter = presenter;
                    lastPresenterAt = now;
                }
            }
            if (d.Sync.AvDiffMs && std::isfinite(*d.Sync.AvDiffMs)) {
                m.avPeakAbsMs = std::max(m.avPeakAbsMs, std::fabs(*d.Sync.AvDiffMs));
                if (!std::isfinite(m.avAt500Ms) && now - lastRequestAt >= std::chrono::milliseconds(500))
                    m.avAt500Ms = *d.Sync.AvDiffMs;
                if (now - lastRequestAt >= std::chrono::milliseconds(500)) {
                    const double a = std::fabs(*d.Sync.AvDiffMs);
                    avPostSumAbs += a;
                    m.avPostSamples++;
                    m.avPostMaxAbsMs = std::isfinite(m.avPostMaxAbsMs) ? std::max(m.avPostMaxAbsMs, a) : a;
                }
            }
            maxCount(d.AudioQueue.Bytes, m.audioQueueBytesMax);
            maxCount(d.VideoQueue.Bytes, m.videoQueueBytesMax);
            maxCount(d.SubtitleQueue.Bytes, m.subtitleQueueBytesMax);
            maxSeconds(d.AudioQueue.Duration, m.audioQueueSecondsMax);
            maxSeconds(d.VideoQueue.Duration, m.videoQueueSecondsMax);
            maxSeconds(d.SubtitleQueue.Duration, m.subtitleQueueSecondsMax);
            final = std::move(d);
        }
        Sleep(20);
    }
    if (final.FrameTrace.Presented.Serial)
        finalSerial = *final.FrameTrace.Presented.Serial;
    const long long finalFrames = MetricCount(final.Fps.FramesDisplayed);
    m.displayedAfterSeek = m.targetFrameDisplayed ? std::max(0LL, finalFrames - firstTargetFrames + 1) : 0;
    m.earlyDrops = std::max(0LL, MetricCount(final.Sync.FrameDropsEarly) - oldEarly);
    m.lateDrops = std::max(0LL, MetricCount(final.Sync.FrameDropsLate) - oldLate);
    m.presenterFrames = std::max(0LL, PresenterCount(final) - oldPresenter);
    m.avFinalMs = MetricNumber(final.Sync.AvDiffMs);
    if (m.avPostSamples > 0) m.avPostMeanAbsMs = avPostSumAbs / (double)m.avPostSamples;
    m.systemBusyPct = SystemBusyPct(systemStart, SampleSystemTimes());
    m.displayedRate = final.Fps.Displayed.value_or(0.0);
    m.finalPosition = final.Position.value_or(NAN);
    m.finalVideoClock = final.Sync.VideoClock.value_or(NAN);
    m.requestedSeek = final.Seek.Requested.value_or(NAN);
    m.keyframeSeek = final.Seek.Keyframe.value_or(NAN);
    m.keyframeDistanceMs = final.Seek.DistanceMs.value_or(NAN);
    m.gopMs = final.Seek.GopMs.value_or(NAN);
    if (std::isfinite(m.finalPosition))
        m.positionError = m.finalPosition - (m.startPosition + m.expectedDelta + windowMs / 1000.0);
    if (!m.targetFrameDisplayed)
        std::printf("SEEKDEBUG | request_count=%d start=%.3f delta=%.3f player_pos=%.3f player_duration=%.3f debug_pos=%.3f debug_duration=%.3f video_clock=%.3f state=%d debug_valid=%d\n",
                    requestCount, m.startPosition, m.expectedDelta, s.player->Position(), s.player->Duration(),
                    m.finalPosition, final.Duration.value_or(-1.0), m.finalVideoClock, (int)s.player->State(), final.Valid ? 1 : 0);
    if (m.targetFrameDisplayed && std::isfinite(m.rate))
        m.expectedFrames = std::max(0.0, (windowMs / 1000.0 - m.latencyAfterLastMs / 1000.0) * m.rate);
    m.earlyDropsAfterFirst = std::max(0LL, MetricCount(final.Sync.FrameDropsEarly) - dropsAtFirst);
    m.lateDropsAfterFirst = std::max(0LL, MetricCount(final.Sync.FrameDropsLate) - lateAtFirst);
    if (!final.FrameTrace.Events.empty() && finalSerial >= 0) {
        std::vector<const DebugFrameEvent*> presentedEvents;
        long long firstPresentedAtUs = 0;
        for (const DebugFrameEvent& event : final.FrameTrace.Events) {
            if ((event.Kind != "audio" && event.Serial.value_or(-2) != finalSerial) || !event.AtUs ||
                *event.AtUs < measureStartClockUs || *event.AtUs > measureEndClockUs) continue;
            std::printf("SEEKTRACE | config=%s seek=%s kind=%s count=%lld serial=%lld pts=%.6f at_us=%lld dxgi_present=%lld dxgi_qpc=%lld\n",
                        config, name.c_str(), event.Kind.c_str(), event.Count.value_or(-1), event.Serial.value_or(-1),
                        event.Pts.value_or(NAN), event.AtUs.value_or(-1), event.DxgiPresentCount.value_or(-1),
                        event.DxgiSyncQpcTime.value_or(-1));
            if (event.Kind == "presented") presentedEvents.push_back(&event);
        }
        if (!presentedEvents.empty()) {
            firstPresentedAtUs = presentedEvents.front()->AtUs.value_or(0);
            m.targetFrameDisplayed = true;
            m.latencyAfterLastMs = std::max(0.0, (firstPresentedAtUs - measureStartClockUs) / 1000.0);
            m.latencyMs = std::chrono::duration<double, std::milli>(lastRequestAt - started).count() + m.latencyAfterLastMs;
            m.expectedFrames = std::isfinite(m.rate) ?
                std::max(0.0, (windowMs / 1000.0 - m.latencyAfterLastMs / 1000.0) * m.rate) : 0.0;
            m.displayedAfterSeek = (long long)presentedEvents.size();
            displayIntervals.clear();
            presenterIntervals.clear();
            std::vector<double> ptsIntervals, qpcIntervals, paceErrors;
            LARGE_INTEGER qpcFrequency{};
            QueryPerformanceFrequency(&qpcFrequency);
            bool haveDxgi = true;
            m.presentedFrames = (long long)presentedEvents.size();
            for (size_t i = 0; i < presentedEvents.size(); ++i) {
                const DebugFrameEvent& event = *presentedEvents[i];
                if (!event.DxgiValid.value_or(false) || !event.DxgiPresentCount || !event.DxgiSyncQpcTime)
                    haveDxgi = false;
                if (i == 0) {
                    std::printf("SEEKFRAME | config=%s seek=%s idx=0 pts=%.6f pts_gap_ms=nan host_gap_ms=nan dxgi_present_delta=nan d3d_gpu_us=%lld\n",
                                config, name.c_str(), event.Pts.value_or(NAN), event.D3dGpuLastUs.value_or(-1));
                    continue;
                }
                const DebugFrameEvent& previous = *presentedEvents[i - 1];
                double hostGapMs = NAN, ptsGapMs = NAN;
                long long presentDelta = -1;
                if (event.AtUs && previous.AtUs && *event.AtUs > *previous.AtUs) {
                    hostGapMs = (*event.AtUs - *previous.AtUs) / 1000.0;
                    displayIntervals.push_back(hostGapMs);
                }
                if (event.Pts && previous.Pts) ptsGapMs = (*event.Pts - *previous.Pts) * 1000.0;
                if (std::isfinite(hostGapMs) && std::isfinite(ptsGapMs) && ptsGapMs > 0)
                    paceErrors.push_back(std::fabs(hostGapMs - ptsGapMs));
                if (event.DxgiPresentCount && previous.DxgiPresentCount) {
                    presentDelta = *event.DxgiPresentCount - *previous.DxgiPresentCount;
                    if (presentDelta != 1) m.dxgiPresentCountGaps++;
                }
                if (event.Pts && previous.Pts && *event.Pts <= *previous.Pts) m.ptsDuplicates++;
                std::printf("SEEKFRAME | config=%s seek=%s idx=%zu pts=%.6f pts_gap_ms=%.3f host_gap_ms=%.3f dxgi_present_delta=%lld d3d_gpu_us=%lld\n",
                            config, name.c_str(), i, event.Pts.value_or(NAN), ptsGapMs, hostGapMs, presentDelta,
                            event.D3dGpuLastUs.value_or(-1));
                if (event.Pts && previous.Pts) {
                    const double gapMs = (*event.Pts - *previous.Pts) * 1000.0;
                    if (gapMs > 0) {
                        ptsIntervals.push_back(gapMs);
                        m.maxPtsGapMs = std::isfinite(m.maxPtsGapMs) ? std::max(m.maxPtsGapMs, gapMs) : gapMs;
                        const double nominalMs = m.rate > 0 ? 1000.0 / m.rate : NAN;
                        if (std::isfinite(nominalMs) && gapMs > nominalMs * 1.5)
                            m.ptsSkippedFrames += std::max(1LL, (long long)std::llround(gapMs / nominalMs) - 1);
                    }
                }
                // The DXGI sync sample only advances when the refresh counter does; normalise by the refresh delta so
                // a stale sample is not reported as a ~1 s present interval.
                if (qpcFrequency.QuadPart > 0 && event.DxgiSyncQpcTime && previous.DxgiSyncQpcTime &&
                    *event.DxgiSyncQpcTime > *previous.DxgiSyncQpcTime) {
                    double refreshDelta = 1.0;
                    if (event.DxgiSyncRefreshCount && previous.DxgiSyncRefreshCount &&
                        *event.DxgiSyncRefreshCount > *previous.DxgiSyncRefreshCount)
                        refreshDelta = (double)(*event.DxgiSyncRefreshCount - *previous.DxgiSyncRefreshCount);
                    qpcIntervals.push_back(1000.0 * (*event.DxgiSyncQpcTime - *previous.DxgiSyncQpcTime) /
                                           qpcFrequency.QuadPart / refreshDelta);
                }
            }
            m.paceErrP95Ms = P95(paceErrors);
            if (!paceErrors.empty()) m.paceErrMaxMs = *std::max_element(paceErrors.begin(), paceErrors.end());
            m.dxgiStats = haveDxgi;
            if (presentedEvents.size() > 1 && presentedEvents.front()->D3dGpuTotalUs && presentedEvents.back()->D3dGpuTotalUs)
                m.d3dGpuUs = *presentedEvents.back()->D3dGpuTotalUs - *presentedEvents.front()->D3dGpuTotalUs;
            if (presentedEvents.size() > 1 && presentedEvents.front()->D3dGpuFrames && presentedEvents.back()->D3dGpuFrames)
                m.d3dGpuFrames = *presentedEvents.back()->D3dGpuFrames - *presentedEvents.front()->D3dGpuFrames;
            if (haveDxgi && presentedEvents.size() > 1) {
                m.dxgiPresents = *presentedEvents.back()->DxgiPresentCount - *presentedEvents.front()->DxgiPresentCount;
                if (presentedEvents.back()->DxgiSyncGpuTime && presentedEvents.front()->DxgiSyncGpuTime)
                    m.dxgiGpuTicks = *presentedEvents.back()->DxgiSyncGpuTime - *presentedEvents.front()->DxgiSyncGpuTime;
            }
            m.earlyDropsAfterFirst = 0;
            m.lateDropsAfterFirst = 0;
            for (const DebugFrameEvent& event : final.FrameTrace.Events) {
                if (event.Serial.value_or(-2) != finalSerial || !event.AtUs || *event.AtUs < firstPresentedAtUs ||
                    *event.AtUs > measureEndClockUs) continue;
                if (event.Kind == "early_drop") m.earlyDropsAfterFirst++;
                if (event.Kind == "late_drop") m.lateDropsAfterFirst++;
            }
            m.displayIntervalP95Ms = P95(displayIntervals);
            if (!displayIntervals.empty()) m.displayIntervalMaxMs = *std::max_element(displayIntervals.begin(), displayIntervals.end());
            m.dxgiIntervalP95Ms = P95(qpcIntervals);
            if (!qpcIntervals.empty()) m.dxgiIntervalMaxMs = *std::max_element(qpcIntervals.begin(), qpcIntervals.end());
            m.presenterIntervalP95Ms = m.displayIntervalP95Ms;
        }
    }
    if (!std::isfinite(m.displayIntervalP95Ms)) {
        m.displayIntervalP95Ms = P95(displayIntervals);
        if (!displayIntervals.empty()) m.displayIntervalMaxMs = *std::max_element(displayIntervals.begin(), displayIntervals.end());
        m.presenterIntervalP95Ms = P95(presenterIntervals);
    }
    return m;
}
void PrintSeekMetric(const char* phase, const char* config, const SeekMetric& m) {
    std::printf("SEEKMETRIC | phase=%s config=%s seek=%s | latency_ms=%.1f latency_after_last_ms=%.1f target_frame=%s displayed=%lld expected=%.1f filter_fps=%.3f displayed_fps=%.3f drops_early=%lld drops_late=%lld drops_after_first=%lld/%lld pts_skipped=%lld | pos_start=%.3f delta=%.3f pos_end=%.3f pos_error=%.3f vclock_end=%.3f seek_requested=%.3f keyframe=%.3f snap_ms=%.2f gop_ms=%.2f | host_present_p95_ms=%.1f host_max_ms=%.1f dxgi_stats=%s dxgi_present_p95_ms=%.1f dxgi_present_max_ms=%.1f dxgi_presents=%lld dxgi_gpu_ticks=%lld d3d_gpu_us=%lld gpu_frames=%lld avg_gpu_us=%.2f presenter_frames=%lld requests=%lld cpu_ms_per_request=%.2f request_gpu_us=%lld request_gpu_frames=%lld | av_500_ms=%.2f av_final_ms=%.2f av_peak_abs_ms=%.2f | queue_max_bytes=a%lld,v%lld,s%lld queue_max_seconds=a%.3f,v%.3f,s%.3f\n",
                phase, config, m.name.c_str(), m.latencyMs, m.latencyAfterLastMs, m.targetFrameDisplayed ? "yes" : "no",
                m.displayedAfterSeek, m.expectedFrames, m.rate, m.displayedRate, m.earlyDrops, m.lateDrops,
                m.earlyDropsAfterFirst, m.lateDropsAfterFirst, m.ptsSkippedFrames,
                m.startPosition, m.expectedDelta, m.finalPosition, m.positionError, m.finalVideoClock,
                m.requestedSeek, m.keyframeSeek, m.keyframeDistanceMs, m.gopMs,
                m.displayIntervalP95Ms, m.displayIntervalMaxMs, m.dxgiStats ? "yes" : "no",
                m.dxgiIntervalP95Ms, m.dxgiIntervalMaxMs, m.dxgiPresents, m.dxgiGpuTicks, m.d3dGpuUs, m.d3dGpuFrames,
                m.d3dGpuFrames > 0 ? (double)m.d3dGpuUs / m.d3dGpuFrames : NAN, m.presenterFrames,
                m.requests, m.cpuMsPerRequest, m.gpuSubmitUs, m.gpuSubmitFrames,
                m.avAt500Ms, m.avFinalMs, m.avPeakAbsMs, m.audioQueueBytesMax, m.videoQueueBytesMax, m.subtitleQueueBytesMax,
                m.audioQueueSecondsMax, m.videoQueueSecondsMax, m.subtitleQueueSecondsMax);
    std::fflush(stdout);
}
void PrintSeekQuality(const char* config, const SeekMetric& m) {
    std::printf("SEEKQUAL | config=%s seek=%s presented=%lld pts_skipped=%lld pts_dups=%lld pace_err_p95_ms=%.2f pace_err_max_ms=%.2f dxgi_present_gaps=%lld dxgi_refresh_p95_ms=%.2f av_post500_mean_abs_ms=%.2f av_post500_max_abs_ms=%.2f av_post500_samples=%lld pos_error=%.3f gop_bound=%.3f system_busy_pct=%.1f\n",
                config, m.name.c_str(), m.presentedFrames, m.ptsSkippedFrames, m.ptsDuplicates, m.paceErrP95Ms, m.paceErrMaxMs,
                m.dxgiPresentCountGaps, m.dxgiIntervalP95Ms, m.avPostMeanAbsMs, m.avPostMaxAbsMs, m.avPostSamples,
                m.positionError, GopBoundSeconds(m.gopMs), m.systemBusyPct);
    std::fflush(stdout);
}

// Warm a freshly opened session up so the first measured seek does not pay one-time costs (CUDA/D3D shader and
// pool creation, audio device start, ivtc state). Returns true when presentation reached steady state: filter-rate
// presentation with no drops over a full second, which is the isolation precondition for the strict checks.
bool WarmUpSteadyState(Session& s, int maxSeconds = 12) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(maxSeconds);
    while (std::chrono::steady_clock::now() < deadline) {
        const DebugInfo a = CurrentDebug(s);
        const long long fa = MetricCount(a.Fps.FramesDisplayed);
        const long long da = MetricCount(a.Sync.FrameDropsEarly) + MetricCount(a.Sync.FrameDropsLate);
        const auto t0 = std::chrono::steady_clock::now();
        Delay(1000);
        const DebugInfo b = CurrentDebug(s);
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const double rate = b.Fps.FilterOut.value_or(0.0);
        const long long shown = MetricCount(b.Fps.FramesDisplayed) - fa;
        const long long drops = MetricCount(b.Sync.FrameDropsEarly) + MetricCount(b.Sync.FrameDropsLate) - da;
        if (rate > 1.0 && drops == 0 && shown >= (long long)(rate * sec * 0.9)) return true;
    }
    return false;
}

struct SeekConfig {
    const char* name;
    OpenOptions options;
    bool acrylic = false, requiresCuda = false, requiresD3d11va = false;
    const char* ivtcFilter = nullptr;   // must be in the active graph, and in caps to run at all
    bool requiresD3dPresenter = false;
};

struct StepTiming {
    bool Presented = false, PositionEvent = false;
    double PresentMs = NAN, PositionMs = NAN, CpuMs = NAN;
    long long Frames = 0, DxgiPresentDelta = 0, DxgiGpuTicks = 0, D3dGpuUs = 0, D3dGpuFrames = 0;
};

double ProcessCpuTimeMs() {
    FILETIME created{}, exited{}, kernel{}, user{};
    ULARGE_INTEGER k{}, u{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return NAN;
    k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
    return (k.QuadPart + u.QuadPart) * 0.0001;
}

StepTiming MeasureStep(Session& s, int direction, int timeoutMs = 10000) {
    StepTiming result;
    const DebugInfo before = CurrentDebug(s);
    const long long oldFrames = MetricCount(before.Fps.FramesDisplayed);
    const int oldPositions = s.positions;
    const double cpuBefore = ProcessCpuTimeMs();
    const auto started = std::chrono::steady_clock::now();
    if (s.player->StepFrame(direction) != 0) return result;
    const auto deadline = started + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        Pump();
        const DebugInfo d = CurrentDebug(s);
        const auto now = std::chrono::steady_clock::now();
        const long long frames = MetricCount(d.Fps.FramesDisplayed);
        if (!result.Presented && frames > oldFrames) {
            result.Presented = true;
            result.PresentMs = std::chrono::duration<double, std::milli>(now - started).count();
            result.Frames = frames - oldFrames;
            if (d.FrameTrace.DxgiPresentCount && before.FrameTrace.DxgiPresentCount)
                result.DxgiPresentDelta = *d.FrameTrace.DxgiPresentCount - *before.FrameTrace.DxgiPresentCount;
            if (d.FrameTrace.DxgiSyncGpuTime && before.FrameTrace.DxgiSyncGpuTime)
                result.DxgiGpuTicks = *d.FrameTrace.DxgiSyncGpuTime - *before.FrameTrace.DxgiSyncGpuTime;
            if (d.FrameTrace.D3dGpuTotalUs && before.FrameTrace.D3dGpuTotalUs)
                result.D3dGpuUs = *d.FrameTrace.D3dGpuTotalUs - *before.FrameTrace.D3dGpuTotalUs;
            if (d.FrameTrace.D3dGpuFrames && before.FrameTrace.D3dGpuFrames)
                result.D3dGpuFrames = *d.FrameTrace.D3dGpuFrames - *before.FrameTrace.D3dGpuFrames;
        }
        if (!result.PositionEvent && s.positions > oldPositions && s.player->State() == PlayerState::Paused) {
            result.PositionEvent = true;
            result.PositionMs = std::chrono::duration<double, std::milli>(now - started).count();
        }
        if (result.Presented && result.PositionEvent) break;
        Sleep(1);
    }
    result.CpuMs = ProcessCpuTimeMs() - cpuBefore;
    return result;
}

struct StepBurstTiming {
    long long Requests = 0, Accepted = 0, Busy = 0, Displayed = 0, DxgiGpuTicks = 0, D3dGpuUs = 0, D3dGpuFrames = 0;
    double SubmitMs = NAN, ResumeAfterLastMs = NAN, PositionDelta = NAN, CpuMs = NAN;
};

StepBurstTiming MeasureStepBurst(Session& s, int direction, int requests, int repeatMs, int timeoutMs = 5000) {
    StepBurstTiming result;
    const DebugInfo before = CurrentDebug(s);
    const long long oldFrames = MetricCount(before.Fps.FramesDisplayed);
    const double oldPosition = s.player->Position();
    const double cpuBefore = ProcessCpuTimeMs();
    const auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < requests; ++i) {
        // No backend queue: a request made while a step is in flight is rejected with FFP_EBUSY and never runs.
        const int rc = s.player->StepFrame(direction);
        if (rc == 0) result.Accepted++;
        else if (rc == FFP_EBUSY) result.Busy++;
        else break;
        result.Requests++;
        if (i + 1 < requests) {
            const auto due = started + std::chrono::milliseconds((i + 1) * repeatMs);
            while (std::chrono::steady_clock::now() < due) {
                Pump();
                Sleep(1);
            }
        }
    }
    const auto lastRequestAt = std::chrono::steady_clock::now();
    result.SubmitMs = std::chrono::duration<double, std::milli>(lastRequestAt - started).count();
    const auto deadline = lastRequestAt + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        Pump();
        const DebugInfo d = CurrentDebug(s);
        const long long frames = MetricCount(d.Fps.FramesDisplayed);
        if (frames >= oldFrames + result.Accepted) {
            result.Displayed = frames - oldFrames;
            result.ResumeAfterLastMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - lastRequestAt).count();
            if (d.FrameTrace.DxgiSyncGpuTime && before.FrameTrace.DxgiSyncGpuTime)
                result.DxgiGpuTicks = *d.FrameTrace.DxgiSyncGpuTime - *before.FrameTrace.DxgiSyncGpuTime;
            if (d.FrameTrace.D3dGpuTotalUs && before.FrameTrace.D3dGpuTotalUs)
                result.D3dGpuUs = *d.FrameTrace.D3dGpuTotalUs - *before.FrameTrace.D3dGpuTotalUs;
            if (d.FrameTrace.D3dGpuFrames && before.FrameTrace.D3dGpuFrames)
                result.D3dGpuFrames = *d.FrameTrace.D3dGpuFrames - *before.FrameTrace.D3dGpuFrames;
            break;
        }
        Sleep(1);
    }
    const DebugInfo after = CurrentDebug(s);
    if (!result.Displayed) result.Displayed = std::max(0LL, MetricCount(after.Fps.FramesDisplayed) - oldFrames);
    result.PositionDelta = s.player->Position() - oldPosition;
    result.CpuMs = ProcessCpuTimeMs() - cpuBefore;
    if (!std::isfinite(result.ResumeAfterLastMs) && result.Displayed > 0)
        result.ResumeAfterLastMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - lastRequestAt).count();
    return result;
}

struct HeldKeyRun {
    int Started = 0, Done = 0, Rejected = 0, BusyCalls = 0, DroppedRepeats = 0, StartedAfterRelease = 0;
    bool InFlightAtRelease = false;
    double SettleAfterReleaseMs = NAN, MaxStepMs = NAN, PositionDelta = NAN, PositionDriftAfterSettle = NAN, FrozenMs = 0;
    long long FramesDelta = 0, FramesAfterSettle = 0;
    std::vector<double> StepMs;
};

// A held F / D key as the app sees it: a FrameStepController whose "key held" query is a test flag (the app reads
// GetAsyncKeyState) and whose OS auto-repeat is simulated (first repeat 250 ms after the press, then every 33 ms).
// The key is released as soon as holdSteps steps have completed, which is while the next step is already in flight.
HeldKeyRun RunHeldKey(Session& s, int direction, int holdSteps) {
    using clock = std::chrono::steady_clock;
    HeldKeyRun r;
    const int vk = direction > 0 ? 'F' : 'D';
    bool down = false;
    std::vector<clock::time_point> starts, ends;
    FrameStepController ctl({
        [&](int, int dir) {
            const int rc = s.player->StepFrame(dir);
            if (rc == FFP_EBUSY) r.BusyCalls++;
            if (rc == 0) starts.push_back(clock::now());
            return rc;
        },
        [&](int key) { return down && key == vk; },
        [] { return (uint64_t)GetTickCount64(); }});
    s.onStepDone = [&](int, bool moved) { ends.push_back(clock::now()); ctl.StepDone(moved); };
    auto displayed = [&] { return MetricCount(CurrentDebug(s).Fps.FramesDisplayed); };
    const long long frames0 = displayed();
    const double pos0 = s.player->Position();
    const auto t0 = clock::now();
    down = true;
    ctl.KeyDown(vk, 0, direction, false);
    auto nextRepeat = t0 + std::chrono::milliseconds(250);
    clock::time_point releaseAt = t0;
    size_t startsAtRelease = 0;
    bool released = false;
    while (!released && clock::now() - t0 < std::chrono::seconds(60)) {
        Pump();
        const auto now = clock::now();
        if (now >= nextRepeat) { ctl.KeyDown(vk, 0, direction, true); nextRepeat += std::chrono::milliseconds(33); }
        if ((int)ends.size() >= holdSteps) {
            down = false;
            releaseAt = now;
            startsAtRelease = starts.size();
            r.InFlightAtRelease = ctl.InFlight();
            released = true;
        }
        Sleep(1);
    }
    bool keyUpSent = false;
    while (ctl.InFlight() && clock::now() - releaseAt < std::chrono::seconds(20)) {
        Pump();
        if (!keyUpSent && clock::now() - releaseAt >= std::chrono::milliseconds(30)) { ctl.KeyUp(vk); keyUpSent = true; }   // the late WM_KEYUP
        Sleep(1);
    }
    if (!keyUpSent) ctl.KeyUp(vk);
    r.SettleAfterReleaseMs = std::chrono::duration<double, std::milli>(clock::now() - releaseAt).count();
    r.Started = (int)starts.size();
    r.Done = (int)ends.size();
    r.Rejected = ctl.Rejected();
    r.DroppedRepeats = ctl.DroppedRepeats();
    r.StartedAfterRelease = (int)(starts.size() - startsAtRelease);
    for (size_t i = 0; i < ends.size() && i < starts.size(); ++i)
        r.StepMs.push_back(std::chrono::duration<double, std::milli>(ends[i] - starts[i]).count());
    r.MaxStepMs = r.StepMs.empty() ? NAN : *std::max_element(r.StepMs.begin(), r.StepMs.end());
    const long long framesSettled = displayed();
    const double posSettled = s.player->Position();
    r.FramesDelta = framesSettled - frames0;
    r.PositionDelta = posSettled - pos0;
    r.FrozenMs = std::max(350.0, 2.0 * (std::isfinite(r.MaxStepMs) ? r.MaxStepMs : 0.0));
    Delay((int)r.FrozenMs);
    r.FramesAfterSettle = displayed() - framesSettled;
    r.PositionDriftAfterSettle = std::fabs(s.player->Position() - posSettled);
    s.onStepDone = nullptr;
    return r;
}

}

TEST_CASE_INTEGRATION(FfPlayer_post_seek_metrics_on_arib_broadcast_pipeline) {
    const fs::path path = RepoRoot() / L"testmedia" / L"arib" / L"tbs-captions.ts";
    REQUIRE_MSG(fs::exists(path), "missing testmedia\\arib\\tbs-captions.ts");
    auto caps = FfPlayer::ProbeCaps();
    REQUIRE_MSG(caps.has_value(), "cannot probe libffplay capabilities");
    const PresenterCap* d3d = caps->Presenter("d3d11");
    const bool cudaMpeg2 = caps->DeviceOk("cuda") && caps->Decoder("mpeg2video", "cuda") && caps->Decoder("mpeg2video", "cuda")->ok;
    const bool d3d11vaMpeg2 = caps->DeviceOk("d3d11va") && caps->Decoder("mpeg2video", "d3d11va") && caps->Decoder("mpeg2video", "d3d11va")->ok;
    const bool ivtcCuda = caps->Filter("ivtc_cuda") != nullptr;
    const bool ivtcD3d11 = caps->Filter("ivtc_d3d11") != nullptr;
    const bool d3dOk = d3d && d3d->ok;
    const bool cudaZeroCopy = d3dOk && d3d->importCuda && d3d->importCudaZeroCopy;
    std::printf("SEEKCAPS | adapter=%s cuda=%s mpeg2_cuda=%s ivtc_cuda=%s ivtc_d3d11=%s d3d11=%s cuda_zero_copy=%s mpeg2_d3d11va=%s\n",
                caps->adapter.name.c_str(), caps->DeviceOk("cuda") ? "yes" : "no", cudaMpeg2 ? "yes" : "no",
                ivtcCuda ? "yes" : "no", ivtcD3d11 ? "yes" : "no", d3dOk ? "yes" : "no", cudaZeroCopy ? "yes" : "no", d3d11vaMpeg2 ? "yes" : "no");
    std::fflush(stdout);

    const OpenOptions full = {{L"hwaccel", L"cuda"}, {L"hwframes", L"keep"}, {L"presenter", L"d3d11"},
                              {L"vf", L"ivtc_cuda"}, {L"superimpose", L"1"}, {L"stats", L"1"}};
    OpenOptions captionsOff = full;
    captionsOff[4].second = L"0";
    captionsOff.emplace_back(L"sn", L"1");
    OpenOptions ivtcOff = full; ivtcOff[3].second.clear();
    OpenOptions presenterSdl = full; presenterSdl[2].second = L"sdl";
    OpenOptions d3d11va = {{L"hwaccel", L"d3d11va"}, {L"hwframes", L"keep"}, {L"presenter", L"d3d11"}, {L"vf", L""}, {L"stats", L"1"}};
    OpenOptions fullD3d11 = full; fullD3d11[0].second = L"d3d11va"; fullD3d11[3].second = L"ivtc_d3d11";
    OpenOptions softwareSdl = {{L"presenter", L"sdl"}, {L"vf", L""}, {L"stats", L"1"}};
    OpenOptions softwareD3d = {{L"presenter", L"d3d11"}, {L"vf", L""}, {L"stats", L"1"}};
    const SeekConfig configs[] = {
        {"full", full, true, true, false, "ivtc_cuda", true},
        {"full-d3d11", fullD3d11, true, false, true, "ivtc_d3d11", true},
        {"captions-off", captionsOff, true, true, false, "ivtc_cuda", true},
        {"acrylic-off", full, false, true, false, "ivtc_cuda", true},
        {"ivtc-off", ivtcOff, true, true, false, nullptr, true},
        {"presenter-sdl", presenterSdl, false, true, false, "ivtc_cuda", false},
        {"hw-d3d11va", d3d11va, false, false, true, nullptr, true},
        {"software-sdl", softwareSdl},
        {"software-d3d11", softwareD3d, false, false, false, nullptr, true},
    };
    bool ranConfig = false;
    for (const SeekConfig& config : configs) {
        if (config.requiresCuda && !cudaMpeg2) { std::printf("SEEKMETRIC | phase=skip config=%s reason=CUDA MPEG-2 decoder unavailable\n", config.name); continue; }
        if (config.requiresD3d11va && !d3d11vaMpeg2) { std::printf("SEEKMETRIC | phase=skip config=%s reason=D3D11VA MPEG-2 decoder unavailable\n", config.name); continue; }
        if (config.ivtcFilter && !caps->Filter(config.ivtcFilter)) { std::printf("SEEKMETRIC | phase=skip config=%s reason=%s unavailable\n", config.name, config.ivtcFilter); continue; }
        if (config.requiresD3dPresenter && !d3dOk) { std::printf("SEEKMETRIC | phase=skip config=%s reason=D3D11 presenter unavailable\n", config.name); continue; }
        Session s;
        OpenOptions openOptions = config.options;
        if (std::string(config.name) == "full") openOptions.emplace_back(L"frame_trace", L"1");
        const int openRc = s.player->Open(path.wstring(), openOptions);
        if (openRc != 0 || !WaitFor([&] { return !s.prepared.empty() || !s.errors.empty(); }, 20.0) || !s.errors.empty()) {
            std::printf("SEEKMETRIC | phase=skip config=%s reason=open_failed rc=%d errors=%llu\n",
                        config.name, openRc, (unsigned long long)s.errors.size());
            continue;
        }
        DebugInfo d = CurrentDebug(s);
        const bool actualCuda = !config.requiresCuda || d.Video.HwaccelDevice == "cuda";
        const bool actualD3d = !config.requiresD3dPresenter || d.Presenter.Name == "d3d11";
        if (!actualCuda || !actualD3d) {
            std::printf("SEEKMETRIC | phase=skip config=%s reason=route_not_active decoder=%s presenter=%s\n",
                        config.name, d.Video.HwaccelDevice.c_str(), d.Presenter.Name.c_str());
            continue;
        }
        if (config.acrylic) {
            BackdropRegion region{};
            region.x = 160; region.y = 90; region.width = 320; region.height = 180; region.corner_radius = 24.f;
            region.tint_color = 0xFF202020u; region.tint_opacity = 0.22f;
            region.luminosity_color = 0xFFFFFFFFu; region.luminosity_opacity = 0.08f;
            if (s.player->SetBackdropRegions(&region, 1) != 0) {
                std::printf("SEEKMETRIC | phase=skip config=%s reason=acrylic_api_unavailable\n", config.name);
                continue;
            }
        }
        if (!WaitFor([&] { return MetricCount(CurrentDebug(s).Fps.FramesDisplayed) > 0; }, 12.0)) {
            std::printf("SEEKMETRIC | phase=skip config=%s reason=no_presented_frame\n", config.name);
            continue;
        }
        const bool steady = WarmUpSteadyState(s);
        {   // Effective timer granularity: the dummy audio driver's pacing (and so the master clock) depends on it.
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < 20; i++) Sleep(1);
            const double sleep1Ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 20.0;
            std::printf("SEEKTIMER | config=%s sleep1_ms=%.2f\n", config.name, sleep1Ms);
        }
        std::printf("SEEKWARM | config=%s steady=%s\n", config.name, steady ? "yes" : "no");
        std::fflush(stdout);
        d = CurrentDebug(s);
        const bool actualIvtc = !config.ivtcFilter ||
            std::find(d.Filters.VfFilters.begin(), d.Filters.VfFilters.end(), config.ivtcFilter) != d.Filters.VfFilters.end();
        if (!actualIvtc) {
            std::printf("SEEKMETRIC | phase=skip config=%s reason=%s_not_active vf=%s nodes=%llu\n",
                        config.name, config.ivtcFilter, d.Filters.VfGraph.c_str(), (unsigned long long)d.Filters.VfNodes.size());
            continue;
        }
        std::printf("SEEKBASELINE | config=%s displayed_fps=%.3f filter_fps=%.3f frames=%lld drops_early=%lld drops_late=%lld\n",
                        config.name, d.Fps.Displayed.value_or(0.0), d.Fps.FilterOut.value_or(0.0),
                        MetricCount(d.Fps.FramesDisplayed), MetricCount(d.Sync.FrameDropsEarly), MetricCount(d.Sync.FrameDropsLate));
            std::printf("SEEKROUTE | config=%s duration=%.3f decoder=%s frame_path=%s presenter=%s vf=%s caption=%s superimpose=%s cuda_zero_copy=%lld cuda_copied=%lld acrylic_regions=%d\n",
                    config.name, d.Duration.value_or(-1.0), d.Video.HwaccelDevice.c_str(), d.Video.FramePath.c_str(), d.Presenter.Name.c_str(), d.Filters.VfGraph.c_str(),
                    d.CaptionLayer.Present ? "yes" : "no", d.SuperimposeLayer.Present ? "yes" : "no",
                    MetricCount(d.Presenter.FramesCudaZeroCopy), MetricCount(d.Presenter.FramesCudaCopied), d.Presenter.Acrylic.Regions.value_or(0));
        std::fflush(stdout);
        if (std::string(config.name) == "full") {
            CHECK_MSG(d.CaptionLayer.Present, "user-like TBS pipeline did not open the ARIB caption layer");
            CHECK_MSG(d.Presenter.Acrylic.Regions.value_or(0) == 1, "Acrylic region did not reach the D3D11 presenter");
            CHECK_MSG(d.Presenter.FramesCudaZeroCopy.value_or(0) > 0, "CUDA frames did not reach D3D11 through zero-copy import");
        }
        ranConfig = true;
        if (std::string(config.name) == "full") {
            const long long baselineFrames = MetricCount(d.Fps.FramesDisplayed);
            const long long baselineEarly = MetricCount(d.Sync.FrameDropsEarly);
            const long long baselineLate = MetricCount(d.Sync.FrameDropsLate);
            const auto baselineStart = std::chrono::steady_clock::now();
            DebugInfo baselineEnd = d;
            while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - baselineStart).count() < 3000) {
                Pump();
                baselineEnd = CurrentDebug(s);
                Sleep(20);
            }
            const double baselineSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - baselineStart).count();
            const long long baselineDisplayed = std::max(0LL, MetricCount(baselineEnd.Fps.FramesDisplayed) - baselineFrames);
            const double baselineExpected = baselineEnd.Fps.FilterOut.value_or(0.0) * baselineSeconds;
            std::printf("SEEKSTEADY | config=full duration_s=%.3f displayed=%lld expected=%.1f filter_fps=%.3f displayed_fps=%.3f drops_early=%lld drops_late=%lld\n",
                        baselineSeconds, baselineDisplayed, baselineExpected, baselineEnd.Fps.FilterOut.value_or(0.0),
                        baselineEnd.Fps.Displayed.value_or(0.0),
                        std::max(0LL, MetricCount(baselineEnd.Sync.FrameDropsEarly) - baselineEarly),
                        std::max(0LL, MetricCount(baselineEnd.Sync.FrameDropsLate) - baselineLate));
            std::fflush(stdout);
            CHECK_MSG(steady, "full pipeline never reached 1 s of drop-free steady presentation during warm-up");
            CHECK_MSG(baselineDisplayed >= (long long)(baselineExpected * 0.90),
                      "steady playback displayed fewer than 90% of filter-output frames");
            CHECK_MSG(std::max(0LL, MetricCount(baselineEnd.Sync.FrameDropsEarly) - baselineEarly) +
                      std::max(0LL, MetricCount(baselineEnd.Sync.FrameDropsLate) - baselineLate) == 0,
                      "steady playback dropped frames after warm-up");
        }
        struct Scenario { const char* name; double start, seek; bool relative; };
        const Scenario scenarios[] = {{"+10", 20.0, 10.0, true}, {"-10", 40.0, -10.0, true},
                                      {"+60", 10.0, 60.0, true}, {"-60", 80.0, -60.0, true},
                                      {"absolute-50", 20.0, 50.0, false}};
        // Throwaway seeks (not measured): the first seek after open pays one-time decoder/graph/audio-device
        // costs. Doing one of each direction keeps the measured +/-10/+/-60/absolute seeks comparable
        // whether the test runs alone or late in the full suite.
        if (std::string(config.name) == "full") {
            if (SeekToAndWaitForFrame(s, 30.0)) { (void)s.player->Seek(-5.0, true); Delay(1500); }
            (void)SeekToAndWaitForFrame(s, 45.0);
            Delay(1000);
        }
        // The "full" config carries CHECKs; the others are measured/printed only.
        const bool gate = std::string(config.name) == "full";
        auto checkQuality = [&](const SeekMetric& metric, const std::string& label) {
            PrintSeekQuality(config.name, metric);
            CHECK_MSG(metric.targetFrameDisplayed, label + " did not display on the target timeline");
            CHECK_MSG(metric.latencyAfterLastMs <= (label == "burst-20x1" ? 150.0 : 250.0),
                      label + " first target frame took " + std::to_string(metric.latencyAfterLastMs) + " ms after the last request");
            // Zero skipped frames after the first presented frame (checked on filter-output PTS gaps, which are
            // independent of wall-clock load), and no early/late drops once presentation resumed.
            CHECK_MSG(metric.ptsSkippedFrames == 0, label + " skipped " + std::to_string(metric.ptsSkippedFrames) +
                      " output frames after the first presented frame (max pts gap " + std::to_string(metric.maxPtsGapMs) + " ms)");
            CHECK_MSG(metric.ptsDuplicates == 0, label + " presented a non-increasing PTS");
            CHECK_MSG(metric.earlyDropsAfterFirst + metric.lateDropsAfterFirst == 0,
                      label + " dropped frames after the first presented frame");
            CHECK_MSG(metric.expectedFrames <= 0 || metric.displayedAfterSeek >= metric.expectedFrames * 0.95,
                      label + " displayed fewer than 95% of expected post-seek frames");
            // The measured decoded keyframe itself must be the nearest one (half a GOP); the
            // position sampled three seconds later additionally includes wall-clock sampling slack.
            CHECK_MSG(std::isfinite(metric.requestedSeek) && std::isfinite(metric.keyframeSeek) &&
                      std::isfinite(metric.gopMs) && metric.gopMs > 0 &&
                      std::fabs(metric.keyframeSeek - metric.requestedSeek) * 1000.0 <= metric.gopMs * 0.5 + 40.0,
                      label + " did not land at the closest keyframe: requested " +
                      std::to_string(metric.requestedSeek) + " s, landed " + std::to_string(metric.keyframeSeek) + " s");
            CHECK_MSG(std::isfinite(metric.positionError) && std::fabs(metric.positionError) <= GopBoundSeconds(metric.gopMs),
                      label + " position error " + std::to_string(metric.positionError) + " s exceeded the GOP bound " +
                      std::to_string(GopBoundSeconds(metric.gopMs)) + " s");
            // A-V: about 40 ms once the 0.5 s alignment window has passed. The audio/video clocks are quantised
            // by one frame (~42 ms), so gate the mean at 40 ms and the worst sample at two frames.
            CHECK_MSG(metric.avPostSamples > 0, label + " produced no A-V samples after 500 ms");
            CHECK_MSG(std::isfinite(metric.avPostMeanAbsMs) && metric.avPostMeanAbsMs <= 40.0,
                      label + " mean |A-V| after 0.5 s was " + std::to_string(metric.avPostMeanAbsMs) + " ms");
            CHECK_MSG(std::isfinite(metric.avPostMaxAbsMs) && metric.avPostMaxAbsMs <= 85.0,
                      label + " max |A-V| after 0.5 s was " + std::to_string(metric.avPostMaxAbsMs) + " ms");
            // Presentation pacing: presented host gap tracks the PTS gap (load shows up here as a few late
            // frames, not as frame loss), so only the extreme is bounded.
            CHECK_MSG(!std::isfinite(metric.paceErrMaxMs) || metric.paceErrMaxMs <= 120.0,
                      label + " presented-frame pacing error exceeded 120 ms (" + std::to_string(metric.paceErrMaxMs) + ")");
            if (metric.dxgiStats) {
                // DXGI frame statistics sample the compositor asynchronously: its present count
                // can remain unchanged for several host presents, then jump. Report those sample
                // gaps, but use per-frame host PTS and D3D11 GPU queries to assert frame coverage.
                CHECK_MSG(metric.d3dGpuFrames <= 0 || metric.d3dGpuFrames >= (long long)(metric.presentedFrames * 0.9) - 1,
                          label + " GPU timing covered fewer than 90% of presented frames");
            }
        };
        for (const Scenario& scenario : scenarios) {
            if (!SeekToAndWaitForFrame(s, scenario.start)) {
                std::printf("SEEKMETRIC | phase=skip config=%s seek=%s reason=baseline_seek_unconfirmed\n", config.name, scenario.name);
                continue;
            }
            Delay(1000);   // let the previous seek's audio alignment finish before this seek is measured
            SeekMetric metric = MeasureSeek(s, scenario.name, scenario.seek, scenario.relative, 3000, 1, 0, config.name);
            PrintSeekMetric("after", config.name, metric);
            if (gate) {
                checkQuality(metric, scenario.name);
                if (std::string(scenario.name) == "absolute-50") {
                    // Rapid relative seeks accumulate on the requested timeline, not on the keyframe-snapped display
                    // position: 20 x +1 s must end within one GOP of start + 20 s (+ the 3 s window).
                    Delay(1000);
                    const SeekMetric rapid = MeasureSeek(s, "burst-20x1", 1.0, true, 3000, 20, 30, "full-burst");
                    PrintSeekMetric("after", "full-burst", rapid);
                    checkQuality(rapid, "burst-20x1");
                }
            }
        }
    }
    REQUIRE_MSG(ranConfig, "no supported decoder/presenter configuration could run seek measurements");

    // This fixture is 5.98 s; the 90 s TBS broadcast above provides the +/-10 s and +/-60 s cases.
    const fs::path shortPath = RepoRoot() / L"testmedia" / L"hw" / L"mpeg2-telecine-1080i.ts";
    REQUIRE_MSG(fs::exists(shortPath), "missing testmedia\\hw\\mpeg2-telecine-1080i.ts");
    Session shortSession;
    OpenOptions shortOptions = cudaMpeg2 && ivtcCuda
        ? OpenOptions{{L"hwaccel", L"cuda"}, {L"hwframes", L"keep"}, {L"presenter", d3dOk ? L"d3d11" : L"sdl"}, {L"vf", L"ivtc_cuda"}, {L"stats", L"1"}}
        : OpenOptions{{L"presenter", d3dOk ? L"d3d11" : L"sdl"}, {L"vf", L"fieldmatch,bwdif=mode=send_frame:parity=auto:deint=interlaced,decimate"}, {L"stats", L"1"}};
    REQUIRE_MSG(shortSession.player->Open(shortPath.wstring(), shortOptions) == 0, "short telecine open rejected");
    REQUIRE_MSG(WaitFor([&] { return !shortSession.prepared.empty() || !shortSession.errors.empty(); }, 20.0) && shortSession.errors.empty(),
                "short telecine fixture did not prepare");
    REQUIRE_MSG(WaitFor([&] { return MetricCount(CurrentDebug(shortSession).Fps.FramesDisplayed) > 0; }, 12.0), "short telecine had no displayed frame");
    const DebugInfo before = CurrentDebug(shortSession);
    const long long oldFrames = MetricCount(before.Fps.FramesDisplayed);
    const double oldPosition = shortSession.player->Position();
    const auto shortStart = std::chrono::steady_clock::now();
    const double shortTarget = 2.8;
    REQUIRE_MSG(shortSession.player->Seek(shortTarget, false) == 0, "short telecine absolute seek rejected");
    const double shortDelta = shortTarget - oldPosition;
    bool shortReached = false;
    double shortLatencyMs = NAN;
    DebugInfo shortEnd = before;
    while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - shortStart).count() < 2500) {
        Pump();
        shortEnd = CurrentDebug(shortSession);
        if (!shortReached && MetricCount(shortEnd.Fps.FramesDisplayed) > oldFrames && ClockMovedAfterSeek(before, shortEnd, shortDelta)) {
            shortReached = true;
            shortLatencyMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - shortStart).count();
        }
        Sleep(20);
    }
    std::printf("SEEKMETRIC | phase=after config=telecine-short seek=absolute-2.8 | latency_ms=%.1f target_frame=%s displayed_delta=%lld drops_early=%lld drops_late=%lld fps=%.3f\n",
                shortLatencyMs, shortReached ? "yes" : "no", std::max(0LL, MetricCount(shortEnd.Fps.FramesDisplayed) - oldFrames),
                std::max(0LL, MetricCount(shortEnd.Sync.FrameDropsEarly) - MetricCount(before.Sync.FrameDropsEarly)),
                std::max(0LL, MetricCount(shortEnd.Sync.FrameDropsLate) - MetricCount(before.Sync.FrameDropsLate)), shortEnd.Fps.FilterOut.value_or(0.0));
    std::fflush(stdout);
    CHECK_MSG(shortReached, "short telecine fixture did not display after absolute seek");
    CHECK_MSG(MetricCount(shortEnd.Sync.FrameDropsEarly) - MetricCount(before.Sync.FrameDropsEarly) <= 8 &&
              MetricCount(shortEnd.Sync.FrameDropsLate) - MetricCount(before.Sync.FrameDropsLate) <= 8,
              "short telecine seek dropped more than 8 frames at one stage");
}

TEST_CASE_INTEGRATION(FfPlayer_open_ss_lands_on_nearest_keyframe) {
    const fs::path path = RepoRoot() / L"testmedia" / L"arib" / L"tbs-captions.ts";
    REQUIRE_MSG(fs::exists(path), "missing broadcast fixture for saved-position seek");
    Session s;
    REQUIRE_MSG(s.player->Open(path.wstring(), {{L"ss", L"40.23"}, {L"presenter", L"sdl"},
                                              {L"stats", L"1"}}) == 0, "resume-position open rejected");
    REQUIRE_MSG(WaitFor([&] { return !s.prepared.empty() || !s.errors.empty(); }, 20.0) && s.errors.empty(),
                "resume-position open did not prepare");
    REQUIRE_MSG(WaitFor([&] { return CurrentDebug(s).Seek.Keyframe.has_value() &&
                                  CurrentDebug(s).Seek.GopMs.has_value(); }, 10.0),
                "resume-position open did not decode a keyframe and measure its GOP");
    const DebugInfo d = CurrentDebug(s);
    const double requested = d.Seek.Requested.value_or(NAN);
    const double keyframe = d.Seek.Keyframe.value_or(NAN);
    const double gopMs = d.Seek.GopMs.value_or(NAN);
    std::printf("SEEKRESUME | requested=%.6f keyframe=%.6f distance_ms=%.2f gop_ms=%.2f\n",
                requested, keyframe, (keyframe - requested) * 1000.0, gopMs);
    CHECK_MSG(std::isfinite(requested) && std::isfinite(keyframe) && std::isfinite(gopMs) && gopMs > 0 &&
              std::fabs(keyframe - requested) * 1000.0 <= gopMs * 0.5 + 40.0,
              "saved/open ss must land on the nearest keyframe rather than the following GOP");
}

TEST_CASE_INTEGRATION(FfPlayer_step_frame_next_previous_after_ivtc_and_d3d11) {
    Session s;
    const fs::path path = RepoRoot() / L"testmedia" / L"hw" / L"mpeg2-telecine-1080i.ts";
    REQUIRE_MSG(fs::exists(path), "missing testmedia\\hw\\mpeg2-telecine-1080i.ts");
    auto caps = FfPlayer::ProbeCaps();
    REQUIRE_MSG(caps.has_value(), "cannot probe libffplay capabilities");
    const PresenterCap* d3d = caps->Presenter("d3d11");
    const DecoderCap* mpeg2Cuda = caps->Decoder("mpeg2video", "cuda");
    const bool userLikeHardware = d3d && d3d->ok && caps->DeviceOk("cuda") && mpeg2Cuda && mpeg2Cuda->ok &&
                                  caps->Filter("ivtc_cuda");

    // Software decoding + the CPU recipe on the d3d11 presenter (system frames uploaded by the presenter), SDL only without it.
    const bool d3dPresenter = d3d && d3d->ok;
    const OpenOptions ivtc = {
        { L"vf", L"fieldmatch,bwdif=mode=send_frame:parity=auto:deint=interlaced,decimate" },
        { L"presenter", d3dPresenter ? L"d3d11" : L"sdl" },
    };
    auto displayedFrames = [](FfPlayer& player) -> long long {
        auto js = player.GetDebugInfoJson();
        if (!js) return 0;
        DebugInfo d = ParseDebugInfo(*js);
        return d.Valid ? d.Fps.FramesDisplayed.value_or(0) : 0;
    };
    s.OpenAndPrepare(path.wstring(), ivtc);
    REQUIRE_MSG(WaitFor([&] { return displayedFrames(*s.player) > 0; }, 8), "no IVTC output frame was displayed");
    if (d3dPresenter) {
        const auto js = s.player->GetDebugInfoJson();
        const DebugInfo d = js ? ParseDebugInfo(*js) : DebugInfo{};
        CHECK_MSG(d.Valid && d.Presenter.Name == "d3d11", "CPU IVTC frame-step did not run on the d3d11 presenter");
    }
    Delay(800);
    REQUIRE_MSG(s.player->State() == PlayerState::Playing, "playback was not running before the first step");
    const double beforePlayingStep = s.player->Position();
    REQUIRE_MSG(std::isfinite(beforePlayingStep) && beforePlayingStep > 0.2,
                "IVTC playback did not advance before stepping");
    const int playingStepEvents = s.positions;
    const long long playingStepFrames = displayedFrames(*s.player);
    const StepTiming firstNextStep = MeasureStep(s, +1, 15000);
    REQUIRE_MSG(firstNextStep.Presented && firstNextStep.PositionEvent,
                "step while playing did not present one frame and auto-pause");
    std::printf("FRAMESTEP_LATENCY | fixture=telecine-software direction=F present_ms=%.2f position_ms=%.2f cpu_ms=%.2f\n",
                firstNextStep.PresentMs, firstNextStep.PositionMs, firstNextStep.CpuMs);
    // Single wall-clock samples are load sensitive (full suite): hard-bound them loosely and gate the median below.
    CHECK_MSG(firstNextStep.PresentMs <= 1000.0, "forward step exceeded a 1000 ms presentation latency ceiling");
    std::vector<double> softwareFTimes{ firstNextStep.PresentMs };
    CHECK(s.positions > playingStepEvents);
    CHECK(displayedFrames(*s.player) > playingStepFrames);
    double current = s.player->Position();
    REQUIRE_MSG(std::isfinite(current) && current > 0.2, "first stepped IVTC position is invalid");

    std::vector<double> pts{ current };
    for (int i = 0; i < 4; ++i) {
        const double before = pts.back();
        const int oldEvents = s.positions;
        const long long oldDisplayedFrames = displayedFrames(*s.player);
        const StepTiming nextStep = MeasureStep(s, +1, 15000);
        REQUIRE_MSG(nextStep.Presented && nextStep.PositionEvent && s.player->Position() > before + 0.03,
                    "next-frame step did not complete at the next IVTC output frame");
        std::printf("FRAMESTEP_LATENCY | fixture=telecine-software direction=F index=%d present_ms=%.2f position_ms=%.2f cpu_ms=%.2f\n",
                    i + 1, nextStep.PresentMs, nextStep.PositionMs, nextStep.CpuMs);
        softwareFTimes.push_back(nextStep.PresentMs);
        CHECK_MSG(nextStep.PresentMs <= 1000.0, "forward step exceeded a 1000 ms presentation latency ceiling");
        CHECK(s.positions > oldEvents);
        const double next = s.player->Position();
        pts.push_back(next);
        CHECK_MSG(std::fabs((next - before) - (1001.0 / 24000.0)) < 0.012,
                  "IVTC frame delta was " + std::to_string(next - before));
        CHECK_MSG(displayedFrames(*s.player) > oldDisplayedFrames,
                  "the stepped IVTC frame was not presented while paused");
    }

    {
        std::vector<double> sorted = softwareFTimes;
        std::sort(sorted.begin(), sorted.end());
        CHECK_MSG(sorted[sorted.size() / 2] <= 250.0, "software IVTC forward-step median latency exceeded 250 ms");
    }
    {
        const long long settledFrames = displayedFrames(*s.player);
        const double settledPosition = s.player->Position();
        Delay(400);
        CHECK_MSG(s.player->State() == PlayerState::Paused, "player left the paused state after stepping");
        CHECK_MSG(displayedFrames(*s.player) == settledFrames, "a frame was presented after the step completed (paused drift)");
        CHECK_MSG(std::fabs(s.player->Position() - settledPosition) < 0.00001, "position moved after the step completed");
    }

    const double predecessor = pts[pts.size() - 2];
    const int oldEvents = s.positions;
    const long long oldDisplayedFrames = displayedFrames(*s.player);
    const StepTiming previousStep = MeasureStep(s, -1, 20000);
    REQUIRE_MSG(previousStep.Presented && previousStep.PositionEvent &&
                std::fabs(s.player->Position() - predecessor) < 0.00001,
                "previous-frame step did not return to the exact preceding filtered PTS; expected=" +
          std::to_string(predecessor) + " actual=" + std::to_string(s.player->Position()) +
          " state=" + std::to_string((int)s.player->State()) + " debug=" +
          s.player->GetDebugInfoJson().value_or("<none>") + " log=" + s.LogText());
    std::printf("FRAMESTEP_LATENCY | fixture=telecine-software direction=D present_ms=%.2f position_ms=%.2f cpu_ms=%.2f\n",
                previousStep.PresentMs, previousStep.PositionMs, previousStep.CpuMs);
    CHECK_MSG(previousStep.PresentMs <= 1500.0, "previous-frame step exceeded a 1500 ms fallback latency budget");
    CHECK(s.positions > oldEvents);
    CHECK_MSG(displayedFrames(*s.player) > oldDisplayedFrames,
              "the previous IVTC frame was not presented while paused");
    for (size_t i = 1; i < pts.size(); ++i)
        CHECK(pts[i] > pts[i - 1]);
    std::printf("FRAMESTEP | IVTC output positions:");
    for (double p : pts) std::printf(" %.6f", p);
    std::printf(" | previous returned %.6f (expected %.6f)\\n", s.player->Position(), predecessor);
    std::fflush(stdout);

    /* When this machine has an MPEG-2 D3D11VA decoder, also prove that a paused single-step
     * reaches the D3D11 presenter with a direct hardware surface (no filter/download path). */
    const DecoderCap* mpeg2D3d = caps->Decoder("mpeg2video", "d3d11va");
    if (d3d && d3d->ok && caps->DeviceOk("d3d11va") && mpeg2D3d && mpeg2D3d->ok) {
        REQUIRE_MSG(s.player->Stop() == 0, "Stop failed");
        s.OpenAndPrepare(path.wstring(), {
            { L"hwaccel", L"d3d11va" }, { L"hwframes", L"keep" }, { L"presenter", L"d3d11" }, { L"vf", L"" },
        });
        REQUIRE_MSG(WaitFor([&] { return displayedFrames(*s.player) > 0; }, 8), "no unfiltered hardware output frame was displayed");
        Delay(500);
        REQUIRE_MSG(s.player->Pause() == 0, "pause failed");
        REQUIRE(WaitFor([&] { return s.player->State() == PlayerState::Paused; }, 5));
        const double before = s.player->Position();
        const int oldEvents2 = s.positions;
        auto directFrames = [&]() -> long long {
            auto js = s.player->GetDebugInfoJson();
            if (!js) return -1;
            DebugInfo d = ParseDebugInfo(*js);
            return d.Valid && d.Presenter.Name == "d3d11" ? d.Presenter.FramesDirect.value_or(0) : -1;
        };
        const long long oldDirect = directFrames();
        if (oldDirect >= 0) {
            REQUIRE_MSG(s.player->StepFrame(+1) == 0, "next-frame request failed");
            REQUIRE_MSG(WaitFor([&] {
                return s.positions > oldEvents2 && s.player->State() == PlayerState::Paused &&
                       s.player->Position() > before + 0.02;
            }, 15), "D3D11VA next-frame step did not complete");
            CHECK_MSG(directFrames() > oldDirect, "stepped D3D11VA frame was not presented directly");
            std::printf("FRAMESTEP | D3D11VA direct presenter frames %lld -> %lld, position %.6f\\n",
                        oldDirect, directFrames(), s.player->Position());
            std::fflush(stdout);
        } else {
            std::printf("FRAMESTEP | SKIP D3D11VA direct counter: D3D11 presenter fell back to SDL\\n");
            std::fflush(stdout);
        }
    } else {
        std::printf("FRAMESTEP | SKIP D3D11VA direct-surface subcase: MPEG-2 D3D11VA unavailable\\n");
        std::fflush(stdout);
    }

    if (!userLikeHardware) {
        std::printf("FRAMESTEP | SKIP CUDA/IVTC/D3D11 latency cases: required capabilities unavailable\\n");
        std::fflush(stdout);
    } else {
        const OpenOptions userLike = {
            { L"hwaccel", L"cuda" }, { L"hwframes", L"keep" }, { L"presenter", L"d3d11" },
            { L"vf", L"ivtc_cuda" }, { L"superimpose", L"1" }, { L"stats", L"1" },
        };
        auto median = [](std::vector<double> values) {
            if (values.empty()) return static_cast<double>(NAN);
            std::sort(values.begin(), values.end());
            return values[values.size() / 2];
        };
        auto runUserLikeSteps = [&](const fs::path& media, const char* fixture, double target, const OpenOptions& options) {
            const std::string fx = fixture;
            REQUIRE_MSG(s.player->Stop() == 0, fx + " Stop failed");
            s.OpenAndPrepare(media.wstring(), options);
            REQUIRE_MSG(WaitFor([&] { return displayedFrames(*s.player) > 0; }, 12),
                        fx + " produced no initial filtered frame");
            REQUIRE_MSG(s.player->Pause() == 0 && WaitFor([&] { return s.player->State() == PlayerState::Paused; }, 5),
                        fx + " could not pause before the GOP-boundary seek");
            const long long beforeSeekFrames = displayedFrames(*s.player);
            REQUIRE_MSG(s.player->Seek(target, false) == 0, fx + " could not seek to the GOP start");
            REQUIRE_MSG(WaitFor([&] { return displayedFrames(*s.player) > beforeSeekFrames &&
                                  s.player->State() == PlayerState::Paused; }, 15),
                        fx + " did not present the first frame of the GOP while paused");
            const DebugInfo seekInfo = CurrentDebug(s);
            const double gopMs = seekInfo.Seek.GopMs.value_or(NAN);
            std::printf("FRAMESTEP_SEEK | fixture=%s requested=%.6f keyframe=%.6f distance_ms=%.2f gop_ms=%.2f direction=%s\n",
                        fixture, seekInfo.Seek.Requested.value_or(NAN), seekInfo.Seek.Keyframe.value_or(NAN),
                        seekInfo.Seek.DistanceMs.value_or(NAN), gopMs, seekInfo.Seek.Direction.c_str());

            auto expectSettled = [&](const std::string& what) {
                const DebugInfo a = CurrentDebug(s);
                const long long frames = MetricCount(a.Fps.FramesDisplayed);
                const double position = s.player->Position();
                const double audioClock = a.Sync.AudioClock.value_or(NAN);
                Delay(350);
                const DebugInfo b = CurrentDebug(s);
                CHECK_MSG(s.player->State() == PlayerState::Paused, fx + " " + what + " left the paused state");
                CHECK_MSG(MetricCount(b.Fps.FramesDisplayed) == frames, fx + " " + what + " presented a frame after completing");
                CHECK_MSG(std::fabs(s.player->Position() - position) < 0.00001, fx + " " + what + " position drifted while paused");
                if (std::isfinite(audioClock) && b.Sync.AudioClock && std::isfinite(*b.Sync.AudioClock))
                    CHECK_MSG(std::fabs(*b.Sync.AudioClock - audioClock) < 0.05, fx + " " + what + " audio clock advanced (not silent)");
            };

            // Warm-up pair, not measured: first F/D after open creates the filter-graph/hw-frame state.
            const double warmStart = s.player->Position();
            REQUIRE_MSG(MeasureStep(s, +1, 20000).Presented, fx + " warm-up F did not present");
            REQUIRE_MSG(MeasureStep(s, -1, 30000).Presented, fx + " warm-up D did not present");
            CHECK_MSG(std::fabs(s.player->Position() - warmStart) < 0.00001, fx + " warm-up F/D did not return to the start frame");

            const double pos0 = s.player->Position();
            const StepTiming f = MeasureStep(s, +1, 15000);
            REQUIRE_MSG(f.Presented && f.PositionEvent, fx + " F did not present and pause");
            const double pos1 = s.player->Position();
            const double fd = pos1 - pos0;
            CHECK_MSG(fd > 0.02 && fd < 0.1, fx + " F did not advance one IVTC output frame, delta=" + std::to_string(fd));
            std::printf("FRAMESTEP_LATENCY | fixture=%s direction=F present_ms=%.2f position_ms=%.2f cpu_ms=%.2f dxgi_present_delta=%lld dxgi_gpu_ticks=%lld d3d_gpu_us=%lld gpu_frames=%lld\n",
                        fixture, f.PresentMs, f.PositionMs, f.CpuMs, f.DxgiPresentDelta, f.DxgiGpuTicks, f.D3dGpuUs, f.D3dGpuFrames);
            expectSettled("F");

            // D inside the GOP: must be exactly the filter-output frame F came from.
            const StepTiming d = MeasureStep(s, -1, 30000);
            REQUIRE_MSG(d.Presented && d.PositionEvent && std::fabs(s.player->Position() - pos0) < 0.00001,
                        fx + " D did not return to the adjacent IVTC output frame; expected=" + std::to_string(pos0) +
                        " actual=" + std::to_string(s.player->Position()));
            std::printf("FRAMESTEP_LATENCY | fixture=%s direction=D present_ms=%.2f position_ms=%.2f cpu_ms=%.2f dxgi_present_delta=%lld dxgi_gpu_ticks=%lld d3d_gpu_us=%lld gpu_frames=%lld\n",
                        fixture, d.PresentMs, d.PositionMs, d.CpuMs, d.DxgiPresentDelta, d.DxgiGpuTicks, d.D3dGpuUs, d.D3dGpuFrames);
            expectSettled("D");

            // D at the GOP boundary: pos0 is the first frame displayed after the keyframe seek, so its predecessor
            // lives in the previous GOP. Three attempts, each D followed by an F that must round-trip exactly to
            // pos0; latency is gated on the best attempt (load-robust) and the median (bounded).
            std::vector<double> boundaryTimes;
            double boundaryPrev = NAN;
            for (int attempt = 0; attempt < 3; ++attempt) {
                const StepTiming back = MeasureStep(s, -1, 30000);
                REQUIRE_MSG(back.Presented && back.PositionEvent, fx + " GOP-boundary D stalled (attempt " + std::to_string(attempt) + ")");
                const double prev = s.player->Position();
                if (attempt == 0) boundaryPrev = prev;
                CHECK_MSG(prev < pos0 - 0.5 * fd && pos0 - prev <= 2.5 * fd,
                          fx + " GOP-boundary D did not land on the adjacent preceding output frame: pos0=" +
                          std::to_string(pos0) + " prev=" + std::to_string(prev) + " frame=" + std::to_string(fd));
                CHECK_MSG(std::fabs(prev - boundaryPrev) < 0.00001, fx + " GOP-boundary D returned different frames across attempts");
                boundaryTimes.push_back(back.PresentMs);
                std::printf("FRAMESTEP_GOP | fixture=%s attempt=%d direction=D present_ms=%.2f prev=%.6f pos0=%.6f gop_ms=%.2f\n",
                            fixture, attempt, back.PresentMs, prev, pos0, gopMs);
                expectSettled("GOP-boundary D");
                const StepTiming forward = MeasureStep(s, +1, 15000);
                REQUIRE_MSG(forward.Presented && forward.PositionEvent && std::fabs(s.player->Position() - pos0) < 0.00001,
                            fx + " F after GOP-boundary D did not round-trip to the same frame; expected=" +
                            std::to_string(pos0) + " actual=" + std::to_string(s.player->Position()));
            }
            CHECK_MSG(*std::min_element(boundaryTimes.begin(), boundaryTimes.end()) <= 150.0,
                      fx + " GOP-boundary D best latency exceeded one-GOP 150 ms budget");
            CHECK_MSG(median(boundaryTimes) <= 180.0, fx + " GOP-boundary D median latency exceeded 180 ms");

            std::vector<double> fTimes, dTimes, fCpu, dCpu;
            for (int i = 0; i < 5; ++i) {
                const StepTiming burstStep = MeasureStep(s, +1, 10000);
                REQUIRE_MSG(burstStep.Presented && burstStep.PositionEvent, fx + " repeated F stalled");
                fTimes.push_back(burstStep.PresentMs);
                fCpu.push_back(burstStep.CpuMs);
            }
            for (int i = 0; i < 3; ++i) {
                const StepTiming burstStep = MeasureStep(s, -1, 30000);
                REQUIRE_MSG(burstStep.Presented && burstStep.PositionEvent, fx + " repeated D stalled");
                dTimes.push_back(burstStep.PresentMs);
                dCpu.push_back(burstStep.CpuMs);
            }
            const double fCpuMean = std::accumulate(fCpu.begin(), fCpu.end(), 0.0) / fCpu.size();
            const double dCpuMean = std::accumulate(dCpu.begin(), dCpu.end(), 0.0) / dCpu.size();
            std::printf("FRAMESTEP_SEQUENCE | fixture=%s direction=F count=%llu wait_each_present=1 median_present_ms=%.2f max_present_ms=%.2f mean_cpu_ms=%.2f\n",
                        fixture, (unsigned long long)fTimes.size(), median(fTimes), *std::max_element(fTimes.begin(), fTimes.end()), fCpuMean);
            std::printf("FRAMESTEP_SEQUENCE | fixture=%s direction=D count=%llu wait_each_present=1 median_present_ms=%.2f max_present_ms=%.2f mean_cpu_ms=%.2f\n",
                        fixture, (unsigned long long)dTimes.size(), median(dTimes), *std::max_element(dTimes.begin(), dTimes.end()), dCpuMean);
            CHECK_MSG(median(fTimes) <= 120.0, fx + " repeated F median latency exceeded 120 ms");
            CHECK_MSG(median(dTimes) <= 150.0, fx + " repeated D median latency exceeded one-GOP 150 ms budget");
            expectSettled("step sequence");

            // Every accepted request presents exactly one frame, and nothing runs after the last accepted step completes.
            auto checkBurst = [&](const char* dir, const StepBurstTiming& b, int sign, double resumeBudgetMs) {
                std::printf("FRAMESTEP_HOLDBURST | fixture=%s direction=%s requests=%lld accepted=%lld busy=%lld interval_ms=30 submitted_ms=%.2f displayed=%lld position_delta=%.6f resume_after_last_ms=%.2f cpu_ms=%.2f dxgi_gpu_ticks=%lld d3d_gpu_us=%lld gpu_frames=%lld\n",
                            fixture, dir, b.Requests, b.Accepted, b.Busy, b.SubmitMs, b.Displayed, b.PositionDelta,
                            b.ResumeAfterLastMs, b.CpuMs, b.DxgiGpuTicks, b.D3dGpuUs, b.D3dGpuFrames);
                const std::string what = fx + " held " + dir;
                CHECK_MSG(b.Accepted >= 1 && b.Accepted + b.Busy == b.Requests, what + " requests were neither accepted nor rejected with FFP_EBUSY");
                CHECK_MSG(b.Displayed == b.Accepted, what + " presented " + std::to_string(b.Displayed) + " frames for " + std::to_string(b.Accepted) + " accepted steps (a queue would present more)");
                CHECK_MSG(std::fabs(b.PositionDelta - sign * b.Accepted * fd) <= 0.6 * fd, what + " moved " + std::to_string(b.PositionDelta) + " s for " + std::to_string(b.Accepted) + " accepted steps");
                CHECK_MSG(std::isfinite(b.ResumeAfterLastMs) && b.ResumeAfterLastMs <= resumeBudgetMs, what + " exceeded " + std::to_string((int)resumeBudgetMs) + " ms after the last request");
            };
            const StepBurstTiming heldF = MeasureStepBurst(s, +1, 5, 30, 5000);
            checkBurst("F", heldF, +1, 250.0);
            expectSettled("held F");
            const StepBurstTiming heldD = MeasureStepBurst(s, -1, 3, 30, 15000);
            checkBurst("D", heldD, -1, 400.0);
            expectSettled("held D");
        };

        runUserLikeSteps(path, "telecine-gop-boundary", 3.0, userLike);
        const fs::path aribPath = RepoRoot() / L"testmedia" / L"arib" / L"tbs-captions.ts";
        REQUIRE_MSG(fs::exists(aribPath), "missing testmedia\\arib\\tbs-captions.ts for user-like step test");
        runUserLikeSteps(aribPath, "tbs-cuda-ivtc", 40.0, userLike);
        const DecoderCap* mpeg2D3d11va = caps->Decoder("mpeg2video", "d3d11va");
        if (mpeg2D3d11va && mpeg2D3d11va->ok && caps->Filter("ivtc_d3d11")) {
            OpenOptions d3d11Like = userLike;
            d3d11Like[0].second = L"d3d11va";
            d3d11Like[3].second = L"ivtc_d3d11";
            runUserLikeSteps(path, "telecine-d3d11-ivtc", 3.0, d3d11Like);
            runUserLikeSteps(aribPath, "tbs-d3d11-ivtc", 40.0, d3d11Like);
        } else {
            std::printf("FRAMESTEP | SKIP D3D11 IVTC latency cases: d3d11va MPEG-2 or ivtc_d3d11 unavailable\n");
        }
    }
}

TEST_CASE_INTEGRATION(FfPlayer_held_step_key_runs_steps_only_while_the_key_is_down) {
    Session s;
    const fs::path path = RepoRoot() / L"testmedia" / L"hw" / L"mpeg2-telecine-1080i.ts";
    REQUIRE_MSG(fs::exists(path), "missing testmedia\\hw\\mpeg2-telecine-1080i.ts");
    auto caps = FfPlayer::ProbeCaps();
    REQUIRE_MSG(caps.has_value(), "cannot probe libffplay capabilities");
    const PresenterCap* d3d = caps->Presenter("d3d11");
    const OpenOptions ivtc = {
        { L"vf", L"fieldmatch,bwdif=mode=send_frame:parity=auto:deint=interlaced,decimate" },
        { L"presenter", d3d && d3d->ok ? L"d3d11" : L"sdl" }, { L"stats", L"1" },
    };
    s.OpenAndPrepare(path.wstring(), ivtc);
    REQUIRE_MSG(WaitFor([&] { return MetricCount(CurrentDebug(s).Fps.FramesDisplayed) > 0; }, 8), "no frame was displayed");
    Delay(800);
    REQUIRE_MSG(s.player->Pause() == 0 && WaitFor([&] { return s.player->State() == PlayerState::Paused; }, 5), "pause failed");
    REQUIRE_MSG(MeasureStep(s, +1, 15000).Presented, "warm-up F did not present");
    const double fd = 1001.0 / 24000.0;

    struct Case { const char* name; int direction; int steps; };
    const Case cases[] = { {"F", +1, 8}, {"D", -1, 4}, {"F", +1, 8}, {"D", -1, 3} };
    for (const Case& c : cases) {
        const HeldKeyRun r = RunHeldKey(s, c.direction, c.steps);
        std::vector<double> sorted = r.StepMs;
        std::sort(sorted.begin(), sorted.end());
        const double median = sorted.empty() ? NAN : sorted[sorted.size() / 2];
        std::printf("FRAMESTEP_HELD | direction=%s hold_steps=%d started=%d done=%d started_after_release=%d inflight_at_release=%d busy_calls=%d dropped_repeats=%d median_step_ms=%.2f max_step_ms=%.2f settle_after_release_ms=%.2f frames=%lld position_delta=%.6f frozen_ms=%.0f frames_after_settle=%lld drift_after_settle=%.6f\n",
                    c.name, c.steps, r.Started, r.Done, r.StartedAfterRelease, r.InFlightAtRelease ? 1 : 0, r.BusyCalls,
                    r.DroppedRepeats, median, r.MaxStepMs, r.SettleAfterReleaseMs, r.FramesDelta, r.PositionDelta, r.FrozenMs,
                    r.FramesAfterSettle, r.PositionDriftAfterSettle);
        std::fflush(stdout);
        const std::string what = std::string("held ") + c.name + ": ";
        CHECK_MSG(r.Done >= c.steps, what + "fewer steps completed than were held for");
        CHECK_MSG(r.Started == r.Done, what + "a started step did not complete exactly once");
        CHECK_MSG(r.Rejected == 0 && r.BusyCalls == 0, what + "a step was submitted while another was in flight (FFP_EBUSY)");
        CHECK_MSG(r.StartedAfterRelease == 0, what + "a step started after the key was released");
        CHECK_MSG(r.Done == c.steps + (r.InFlightAtRelease ? 1 : 0), what + "more than the in-flight step ran after the release");
        CHECK_MSG(r.FramesDelta == r.Done, what + "presented " + std::to_string(r.FramesDelta) + " frames for " + std::to_string(r.Done) + " steps");
        CHECK_MSG(std::fabs(r.PositionDelta - c.direction * r.Done * fd) <= 0.6 * fd,
                  what + "moved " + std::to_string(r.PositionDelta) + " s for " + std::to_string(r.Done) + " steps");
        CHECK_MSG(std::isfinite(r.SettleAfterReleaseMs) && r.SettleAfterReleaseMs <= 1.2 * r.MaxStepMs + 100.0,
                  what + "took longer than one step time to stop after the release");
        CHECK_MSG(r.FramesAfterSettle == 0 && r.PositionDriftAfterSettle < 0.00001, what + "frames or position kept changing after the last step");
        CHECK_MSG(s.player->State() == PlayerState::Paused, what + "left the paused state");
        if (c.direction < 0) CHECK_MSG(r.DroppedRepeats > 0, what + "no auto-repeat arrived while a D step was in flight (nothing was dropped)");
    }
}

namespace {

std::string CaptureCommand(const std::wstring& cmdline) {
    std::string out;
    FILE* f = _wpopen((L"\"" + cmdline + L"\"").c_str(), L"rt");
    if (!f) return out;
    char buf[4096];
    while (std::fgets(buf, sizeof buf, f)) out += buf;
    _pclose(f);
    return out;
}

struct KeyList {
    std::vector<double> rel;   // PTS of every key packet of the first video stream, media-relative, ascending
    double start = 0;
};

// Ground truth from the stream itself: ffprobe's packet list of the first video stream, key flag K.
KeyList ProbeKeys(const fs::path& media) {
    const fs::path probe = RepoRoot() / L"ffmpeg" / L"dist" / L"win64" / L"ffprobe.exe";
    REQUIRE_MSG(fs::exists(probe), "missing ffmpeg\\dist\\win64\\ffprobe.exe");
    KeyList k;
    const std::string packets = CaptureCommand(L"\"" + probe.wstring() + L"\" -v error -select_streams v:0 -show_entries packet=pts_time,flags -of csv=p=0 \"" + media.wstring() + L"\"");
    const std::string fmt = CaptureCommand(L"\"" + probe.wstring() + L"\" -v error -show_entries format=start_time -of csv=p=0 \"" + media.wstring() + L"\"");
    k.start = std::atof(fmt.c_str());
    size_t pos = 0;
    while (pos < packets.size()) {
        size_t eol = packets.find('\n', pos);
        if (eol == std::string::npos) eol = packets.size();
        const std::string line = packets.substr(pos, eol - pos);
        pos = eol + 1;
        const size_t comma = line.find(',');
        if (comma == std::string::npos || comma + 1 >= line.size() || line[comma + 1] != 'K') continue;
        k.rel.push_back(std::atof(line.c_str()) - k.start);
    }
    std::sort(k.rel.begin(), k.rel.end());
    k.rel.erase(std::unique(k.rel.begin(), k.rel.end(), [](double a, double b) { return std::fabs(a - b) < 0.0005; }), k.rel.end());
    return k;
}

constexpr double kKeyTol = 0.003;
bool SameTime(double a, double b) { return std::isfinite(a) && std::isfinite(b) && std::fabs(a - b) <= kKeyTol; }
// The first key strictly after p / the last key strictly before p (NAN: none), as the API defines them.
double ExpectedKey(const std::vector<double>& keys, double p, int dir) {
    if (dir > 0) {
        for (double x : keys) if (x > p + 0.002) return x;
    } else {
        for (size_t i = keys.size(); i-- > 0;) if (keys[i] < p - 0.002) return keys[i];
    }
    return NAN;
}
// The key closest to target among the keys on the right side of origin (dir < 0 before it, dir > 0 after it, 0 any);
// an equidistant pair resolves to the earlier one. NAN: none qualifies.
double ExpectedNear(const std::vector<double>& keys, double target, int dir, double origin) {
    double best = NAN;
    for (double k : keys) {
        if (dir < 0 && !(k < origin - 0.002)) continue;
        if (dir > 0 && !(k > origin + 0.002)) continue;
        if (!std::isfinite(best) || std::fabs(k - target) < std::fabs(best - target) - 1e-6) best = k;
    }
    return best;
}
// The two keys around target are (nearly) equidistant: ffprobe's rounded PTS cannot decide which one is nearer.
bool NearTie(const std::vector<double>& keys, double target) {
    for (size_t i = 0; i + 1 < keys.size(); ++i)
        if (keys[i] <= target && target < keys[i + 1]) return std::fabs((target - keys[i]) - (keys[i + 1] - target)) < 0.02;
    return false;
}
int KeyIndex(const std::vector<double>& keys, double t) {
    for (size_t i = 0; i < keys.size(); ++i) if (SameTime(keys[i], t)) return (int)i;
    return -1;
}

Session::KeyDone DoKeySeek(Session& s, int dir, double timeoutSeconds = 15) {
    const int before = s.keyDones;
    const int rc = s.player->SeekKeyframe(dir);
    REQUIRE_MSG(rc == 0, "SeekKeyframe rejected: " + std::to_string(rc));
    REQUIRE_MSG(WaitFor([&] { return s.keyDones > before; }, timeoutSeconds), "no KeyframeSeekDone");
    return s.lastKey;
}

void WaitPaused(Session& s) {
    REQUIRE_MSG(s.player->Pause() == 0 && WaitFor([&] { return s.player->State() == PlayerState::Paused; }, 5), "pause failed");
    Delay(200);
}

void WaitFirstFrame(Session& s) {
    REQUIRE_MSG(WaitFor([&] { return MetricCount(CurrentDebug(s).Fps.FramesDisplayed) > 0; }, 10), "no frame was displayed");
}

struct KeyFixture { const wchar_t* rel; const char* name; };
const KeyFixture kKeyFixtures[] = {
    { L"hw\\mpeg2-telecine-1080i.ts", "mpeg2-telecine-1080i.ts (MPEG-2 TS, ~0.4 s GOP)" },
    { L"testsrc2-720p.ts", "testsrc2-720p.ts (H.264 TS, 2 s GOP)" },
    { L"testsrc2-1080p-60s.mp4", "testsrc2-1080p-60s.mp4 (H.264 MP4, 2 s GOP)" },
};

OpenOptions KeyOptions() { return { { L"presenter", L"sdl" }, { L"stats", L"1" } }; }

}

// Paused: every press lands exactly on the adjacent key packet of the stream, the walk covers the whole GOP sequence,
// the first / last keyframe is a no-op, a mid-GOP frame goes to the keyframe of its own GOP (Left) or the next (Right).
TEST_CASE_INTEGRATION(FfPlayer_keyframe_seek_paused_walks_every_keyframe) {
    for (const KeyFixture& fx : kKeyFixtures) {
        const fs::path path = RepoRoot() / L"testmedia" / fx.rel;
        REQUIRE_MSG(fs::exists(path), std::string("missing fixture ") + fx.name);
        const KeyList keys = ProbeKeys(path);
        REQUIRE_MSG(keys.rel.size() >= 5, std::string("too few key packets in ") + fx.name);
        Session s;
        s.OpenAndPrepare(path.wstring(), KeyOptions());
        WaitFirstFrame(s);
        Delay(300);
        WaitPaused(s);
        const std::string tag = std::string(fx.name) + ": ";
        std::printf("KEYSEEK | fixture=%s key_packets=%zu start=%.6f first=%.6f last=%.6f\n", fx.name, keys.rel.size(), keys.start, keys.rel.front(), keys.rel.back());

        std::string walked;
        int forward = 0;
        for (int i = 0; i < (int)keys.rel.size() + 2; ++i) {
            const double before = s.player->Position();
            const double expected = ExpectedKey(keys.rel, before, +1);
            const auto d = DoKeySeek(s, +1);
            if (!std::isfinite(expected)) {
                CHECK_MSG(!d.moved, tag + "Right past the last keyframe moved");
                CHECK_MSG(SameTime(s.player->Position(), before), tag + "Right at the last keyframe changed the position");
                CHECK_MSG(s.player->State() == PlayerState::Paused, tag + "no-op left the paused state");
                break;
            }
            CHECK_MSG(d.moved, tag + "Right did not move although a keyframe follows " + std::to_string(before));
            CHECK_MSG(SameTime(d.key, expected), tag + "Right landed on " + std::to_string(d.key) + ", the next key packet is " + std::to_string(expected));
            CHECK_MSG(SameTime(d.position, expected) && SameTime(s.player->Position(), expected), tag + "the presented frame is not the keyframe");
            CHECK_MSG(s.player->State() == PlayerState::Paused, tag + "Right left the paused state");
            ++forward;
            char b[32]; std::snprintf(b, sizeof b, " %.3f", d.key); walked += b;
        }
        std::printf("KEYSEEK | %s paused Right walk (%d steps):%s\n", fx.name, forward, walked.c_str());
        CHECK_MSG(SameTime(s.player->Position(), keys.rel.back()), tag + "the Right walk did not end on the last keyframe");

        walked.clear();
        int backward = 0;
        for (int i = 0; i < (int)keys.rel.size() + 2; ++i) {
            const double before = s.player->Position();
            const double expected = ExpectedKey(keys.rel, before, -1);
            const auto d = DoKeySeek(s, -1);
            if (!std::isfinite(expected)) {
                CHECK_MSG(!d.moved, tag + "Left at the first keyframe moved");
                CHECK_MSG(SameTime(s.player->Position(), before), tag + "Left at the first keyframe changed the position");
                break;
            }
            CHECK_MSG(d.moved, tag + "Left did not move");
            CHECK_MSG(SameTime(d.key, expected), tag + "Left landed on " + std::to_string(d.key) + ", the previous key packet is " + std::to_string(expected));
            CHECK_MSG(SameTime(d.position, expected) && SameTime(s.player->Position(), expected), tag + "the presented frame is not the keyframe");
            ++backward;
            char b[32]; std::snprintf(b, sizeof b, " %.3f", d.key); walked += b;
        }
        std::printf("KEYSEEK | %s paused Left walk (%d steps):%s\n", fx.name, backward, walked.c_str());
        CHECK_MSG(SameTime(s.player->Position(), keys.rel.front()), tag + "the Left walk did not end on the first keyframe");
        CHECK_MSG(backward == forward, tag + "forward and backward walks are not the same sequence");

        // Mid-GOP: three frames after keyframe 2, Left returns to keyframe 2 and Right goes to keyframe 3.
        const auto to2 = DoKeySeek(s, +1);
        const auto to2b = DoKeySeek(s, +1);
        REQUIRE_MSG(to2.moved && to2b.moved && SameTime(to2b.key, keys.rel[2]), tag + "could not reach keyframe 2");
        for (int i = 0; i < 3; ++i) {
            const int done = s.stepDones;
            REQUIRE_MSG(s.player->StepFrame(+1) == 0 && WaitFor([&] { return s.stepDones > done; }, 15), tag + "frame step failed");
        }
        const double mid = s.player->Position();
        CHECK_MSG(mid > keys.rel[2] + 0.002 && mid < keys.rel[3], tag + "frame steps did not stay inside the GOP");
        const auto left = DoKeySeek(s, -1);
        CHECK_MSG(left.moved && SameTime(left.key, keys.rel[2]), tag + "Left from mid-GOP did not return to the GOP's keyframe");
        const auto left2 = DoKeySeek(s, -1);
        CHECK_MSG(left2.moved && SameTime(left2.key, keys.rel[1]), tag + "Left on a keyframe did not go to the one before it");
        const auto right = DoKeySeek(s, +1);
        CHECK_MSG(right.moved && SameTime(right.key, keys.rel[2]), tag + "Right from keyframe 1 did not reach keyframe 2");

        // A timed relative seek lands exactly on the key packet closest to its target, on the right side of the origin.
        const double origin = right.key;
        const double timedDelta = std::min(7.0, 0.5 * s.prepared.back().Duration);
        const double expected = ExpectedNear(keys.rel, origin + timedDelta, +1, origin);
        REQUIRE_MSG(std::isfinite(expected), tag + "no key after the timed relative seek's target");
        REQUIRE_MSG(s.player->Seek(timedDelta, true) == 0, tag + "relative seek rejected");
        CHECK_MSG(WaitFor([&] { return SameTime(s.player->Position(), expected); }, 10),
                  tag + "a timed relative seek from " + std::to_string(origin) + " did not land on the closest key " + std::to_string(expected) + ", position " + std::to_string(s.player->Position()));
    }
}

// D (previous output frame) across a GOP boundary: from a keyframe and from the frame after it, on short and long GOPs.
// The predecessor of a keyframe is the last frame of the previous GOP, however far back that GOP's keyframe is.
TEST_CASE_INTEGRATION(FfPlayer_step_frame_previous_crosses_a_keyframe_boundary) {
    for (const KeyFixture& fx : kKeyFixtures) {
        const fs::path path = RepoRoot() / L"testmedia" / fx.rel;
        REQUIRE_MSG(fs::exists(path), std::string("missing fixture ") + fx.name);
        const KeyList keys = ProbeKeys(path);
        REQUIRE_MSG(keys.rel.size() >= 4, std::string("too few key packets in ") + fx.name);
        Session s;
        s.OpenAndPrepare(path.wstring(), KeyOptions());
        WaitFirstFrame(s);
        Delay(300);
        WaitPaused(s);
        const std::string tag = std::string(fx.name) + ": ";
        REQUIRE_MSG(DoKeySeek(s, +1).moved && DoKeySeek(s, +1).moved, tag + "could not reach keyframe 2");
        const double key = s.player->Position();
        REQUIRE_MSG(SameTime(key, keys.rel[2]), tag + "not on keyframe 2");

        auto step = [&](int dir) {
            const int done = s.stepDones;
            REQUIRE_MSG(s.player->StepFrame(dir) == 0 && WaitFor([&] { return s.stepDones > done; }, 30), tag + "step did not complete");
            return s.player->Position();
        };
        const double next = step(+1);
        const double fd = next - key;
        REQUIRE_MSG(fd > 0.005 && fd < 0.2, tag + "F from the keyframe did not advance one frame, delta=" + std::to_string(fd));
        CHECK_MSG(SameTime(step(-1), key), tag + "D from the frame after a keyframe did not return to the keyframe");
        const double prev = step(-1);
        CHECK_MSG(prev < key - 0.5 * fd && key - prev <= 2.5 * fd,
                  tag + "D from a keyframe did not land on the last frame of the previous GOP: key=" + std::to_string(key) +
                  " prev=" + std::to_string(prev) + " frame=" + std::to_string(fd));
        CHECK_MSG(SameTime(step(+1), key), tag + "F after the cross-GOP D did not return to the keyframe");
        const double prev2 = step(-1);
        CHECK_MSG(SameTime(prev2, prev), tag + "second cross-GOP D landed elsewhere");
        const double prev3 = step(-1);
        CHECK_MSG(prev3 < prev - 0.5 * fd && prev - prev3 <= 1.5 * fd, tag + "D inside the previous GOP did not step one frame");
        CHECK_MSG(s.player->State() == PlayerState::Paused, tag + "left the paused state");
    }
}

// Playing: a step forward lands on the next key and playback continues; a step back pauses on the landing (as D / F do);
// chains of requests walk the keyframes.
TEST_CASE_INTEGRATION(FfPlayer_keyframe_seek_while_playing_walks_the_keyframes_and_keeps_playing) {
    for (const KeyFixture& fx : kKeyFixtures) {
        const fs::path path = RepoRoot() / L"testmedia" / fx.rel;
        REQUIRE_MSG(fs::exists(path), std::string("missing fixture ") + fx.name);
        const KeyList keys = ProbeKeys(path);
        Session s;
        s.OpenAndPrepare(path.wstring(), KeyOptions());
        WaitFirstFrame(s);
        Delay(1200);
        const std::string tag = std::string(fx.name) + ": ";
        std::string walked;

        int prev = -1;
        for (int i = 0; i < 4; ++i) {
            const double before = s.player->Position();
            const auto d = DoKeySeek(s, +1);
            const int idx = KeyIndex(keys.rel, d.key);
            CHECK_MSG(d.moved && idx >= 0, tag + "playing Right did not land on a key packet");
            CHECK_MSG(SameTime(d.position, d.key), tag + "the presented frame is not the keyframe");
            if (i == 0) CHECK_MSG(d.key > before - 0.3, tag + "first playing Right landed behind the playback position");
            else CHECK_MSG(idx == prev + 1, tag + "playing Right chain skipped or repeated a keyframe");
            prev = idx;
            char b[32]; std::snprintf(b, sizeof b, " %.3f", d.key); walked += b;
        }
        std::printf("KEYSEEK | %s playing Right chain:%s\n", fx.name, walked.c_str());
        const double landed = s.lastKey.key;
        Delay(700);
        CHECK_MSG(s.player->State() == PlayerState::Playing, tag + "playback did not continue after the keyframe seek forward");
        CHECK_MSG(s.player->Position() > landed + 0.3, tag + "the position did not advance after the keyframe seek forward");

        walked.clear();
        prev = -1;
        for (int i = 0; i < 3; ++i) {
            const auto d = DoKeySeek(s, -1);
            const int idx = KeyIndex(keys.rel, d.key);
            CHECK_MSG(d.moved && idx >= 0, tag + "playing Left did not land on a key packet");
            CHECK_MSG(SameTime(d.position, d.key), tag + "the presented frame is not the keyframe");
            if (i > 0) CHECK_MSG(idx == prev - 1, tag + "playing Left chain skipped or repeated a keyframe");
            prev = idx;
            char b[32]; std::snprintf(b, sizeof b, " %.3f", d.key); walked += b;
        }
        std::printf("KEYSEEK | %s playing Left chain:%s\n", fx.name, walked.c_str());
        CHECK_MSG(s.player->State() == PlayerState::Paused, tag + "a keyframe step back must leave the player paused");

        // To the first keyframe: no-op there, playback goes on.
        for (int i = 0; i < (int)keys.rel.size() + 2; ++i) {
            if (!DoKeySeek(s, -1).moved) break;
        }
        CHECK_MSG(SameTime(s.lastKey.position, -1.0) || !s.lastKey.moved, tag + "unexpected final result");
        Delay(300);
        CHECK_MSG(s.player->State() != PlayerState::Error && s.errors.empty(), tag + "error after the walk");
        const int beforeTimed = s.positions;
        REQUIRE_MSG(s.player->Seek(1.0, true) == 0, tag + "relative seek rejected");
        CHECK_MSG(WaitFor([&] { return s.positions > beforeTimed; }, 5), tag + "no position event after a timed seek");
    }
}

namespace {

struct KeyHeldRun {
    int Started = 0, Done = 0, Rejected = 0, Busy = 0, DroppedRepeats = 0, StartedAfterRelease = 0, DoneAfterSettle = 0;
    bool InFlightAtRelease = false;
    std::vector<double> Landed;
    std::vector<int> Moved;
};

// A held Left / Right as the app sees it: the shared controller with a test flag for "key down" and simulated OS
// auto-repeat (first repeat 250 ms after the press, then every 33 ms), released once holdSteps seeks have completed.
KeyHeldRun RunHeldKeyframeKey(Session& s, int direction, int holdSteps, int action = 1) {
    using clock = std::chrono::steady_clock;
    KeyHeldRun r;
    const int vk = direction > 0 ? VK_RIGHT : VK_LEFT;
    bool down = false;
    size_t startsAtRelease = 0, starts = 0;
    FrameStepController ctl({
        [&](int action, int dir) {
            const int rc = action >= 100 ? s.player->Skip(dir * (action - 100.0)) : action == 1 ? s.player->SeekKeyframe(dir) : s.player->StepFrame(dir);
            if (rc == FFP_EBUSY) r.Busy++;
            if (rc == 0) ++starts;
            return rc;
        },
        [&](int key) { return down && key == vk; },
        [] { return (uint64_t)GetTickCount64(); }});
    s.onKeyDone = [&](const Session::KeyDone& d) { r.Landed.push_back(d.key); r.Moved.push_back(d.moved); ctl.StepDone(d.moved); };
    const auto t0 = clock::now();
    down = true;
    ctl.KeyDown(vk, action, direction, false);
    auto nextRepeat = t0 + std::chrono::milliseconds(250);
    clock::time_point releaseAt = t0;
    bool released = false;
    while (!released && clock::now() - t0 < std::chrono::seconds(60)) {
        Pump();
        const auto now = clock::now();
        if (now >= nextRepeat) { ctl.KeyDown(vk, action, direction, true); nextRepeat += std::chrono::milliseconds(33); }
        if ((int)r.Landed.size() >= holdSteps) {
            down = false;
            releaseAt = now;
            startsAtRelease = starts;
            r.InFlightAtRelease = ctl.InFlight();
            released = true;
        }
        Sleep(1);
    }
    bool keyUpSent = false;
    while (ctl.InFlight() && clock::now() - releaseAt < std::chrono::seconds(20)) {
        Pump();
        if (!keyUpSent && clock::now() - releaseAt >= std::chrono::milliseconds(30)) { ctl.KeyUp(vk); keyUpSent = true; }
        Sleep(1);
    }
    if (!keyUpSent) ctl.KeyUp(vk);
    r.Started = (int)starts;
    r.Done = (int)r.Landed.size();
    r.Rejected = ctl.Rejected();
    r.DroppedRepeats = ctl.DroppedRepeats();
    r.StartedAfterRelease = (int)(starts - startsAtRelease);
    const int doneAtSettle = (int)r.Landed.size();
    Delay(600);   // nothing may still be queued behind the release
    r.DoneAfterSettle = (int)r.Landed.size() - doneAtSettle;
    s.onKeyDone = nullptr;
    return r;
}

}

// Held arrow: one keyframe seek in flight at a time, repeats dropped, chained from the done event while the key is
// down, nothing left running after the release. Paused (exact walk) and playing.
TEST_CASE_INTEGRATION(FfPlayer_held_arrow_runs_one_keyframe_seek_at_a_time_and_stops_on_release) {
    const KeyFixture& fx = kKeyFixtures[0];
    const fs::path path = RepoRoot() / L"testmedia" / fx.rel;
    REQUIRE_MSG(fs::exists(path), "missing fixture");
    const KeyList keys = ProbeKeys(path);
    for (const bool paused : {true, false}) {
        Session s;
        s.OpenAndPrepare(path.wstring(), KeyOptions());
        WaitFirstFrame(s);
        Delay(300);
        if (paused) WaitPaused(s);
        const std::string tag = std::string(paused ? "paused " : "playing ") + "held Right: ";
        const int hold = 4;
        const KeyHeldRun r = RunHeldKeyframeKey(s, +1, hold);
        std::printf("KEYSEEK_HELD | %s hold=%d started=%d done=%d started_after_release=%d inflight_at_release=%d busy=%d dropped_repeats=%d done_after_settle=%d landed:",
                    paused ? "paused" : "playing", hold, r.Started, r.Done, r.StartedAfterRelease, r.InFlightAtRelease ? 1 : 0, r.Busy,
                    r.DroppedRepeats, r.DoneAfterSettle);
        for (double k : r.Landed) std::printf(" %.3f", k);
        std::printf("\n");
        std::fflush(stdout);
        CHECK_MSG(r.Done >= hold, tag + "fewer seeks completed than were held for");
        CHECK_MSG(r.Started == r.Done, tag + "a started seek did not complete exactly once");
        CHECK_MSG(r.Rejected == 0 && r.Busy == 0, tag + "a seek was submitted while another was in flight (FFP_EBUSY)");
        CHECK_MSG(r.StartedAfterRelease == 0, tag + "a seek started after the key was released");
        CHECK_MSG(r.Done == hold + (r.InFlightAtRelease ? 1 : 0), tag + "more than the in-flight seek ran after the release");
        CHECK_MSG(r.DoneAfterSettle == 0, tag + "a seek was still queued behind the release");
        CHECK_MSG(r.DroppedRepeats > 0, tag + "no auto-repeat arrived while a seek was in flight");
        for (size_t i = 0; i < r.Landed.size(); ++i) {
            const int idx = KeyIndex(keys.rel, r.Landed[i]);
            CHECK_MSG(idx >= 0, tag + "a held landing is not a key packet");
            if (i > 0) CHECK_MSG(idx == KeyIndex(keys.rel, r.Landed[i - 1]) + 1, tag + "the held walk skipped or repeated a keyframe");
        }
        CHECK_MSG(s.player->State() == (paused ? PlayerState::Paused : PlayerState::Playing), tag + "wrong state after the release");
    }
}

// IVTC / deinterlace graph: the landing is still the source keyframe; the frame shown is the first one the filter
// graph outputs, so its source PTS can follow the keyframe by the filter's reordering, never precede it.
TEST_CASE_INTEGRATION(FfPlayer_keyframe_seek_with_ivtc_filter_graph) {
    const fs::path path = RepoRoot() / L"testmedia" / L"hw" / L"mpeg2-telecine-1080i.ts";
    REQUIRE_MSG(fs::exists(path), "missing fixture");
    const KeyList keys = ProbeKeys(path);
    for (const wchar_t* vf : { L"fieldmatch,bwdif=mode=send_frame:parity=auto:deint=interlaced,decimate", L"bwdif=mode=send_frame:parity=auto:deint=interlaced" }) {
        Session s;
        OpenOptions opts = KeyOptions();
        opts.push_back({ L"vf", vf });
        s.OpenAndPrepare(path.wstring(), opts);
        WaitFirstFrame(s);
        Delay(300);
        WaitPaused(s);
        const std::string tag = std::string("vf=") + ToUtf8(vf) + ": ";
        int previousIdx = -1;
        for (int i = 0; i < 5; ++i) {
            const double beforeOutput = s.player->Position();
            const auto d = DoKeySeek(s, +1);
            REQUIRE_MSG(d.moved, tag + "Right did not move");
            const int idx = KeyIndex(keys.rel, d.key);
            std::printf("KEYSEEK_IVTC | vf=%ls right #%d before_output=%.6f key(source)=%.6f output=%.6f key_index=%d\n", vf, i, beforeOutput, d.key, d.position, idx);
            std::fflush(stdout);
            // The decoder frame behind the first output is the keyframe or one of the next few frames, in this GOP.
            int nearest = -1;
            for (size_t k = 0; k < keys.rel.size(); ++k) if (keys.rel[k] <= d.key + 0.002) nearest = (int)k;
            CHECK_MSG(nearest >= 0 && d.key - keys.rel[nearest] < 0.2, tag + "landing is not at the start of a GOP");
            if (previousIdx >= 0) CHECK_MSG(nearest == previousIdx + 1, tag + "Right chain did not advance one keyframe");
            previousIdx = nearest;
            CHECK_MSG(SameTime(s.player->Position(), d.position), tag + "reported position is not the presented output frame");
        }
        for (int i = 0; i < 3; ++i) {
            const double beforeSource = s.lastKey.key;
            const auto d = DoKeySeek(s, -1);
            REQUIRE_MSG(d.moved, tag + "Left did not move");
            std::printf("KEYSEEK_IVTC | vf=%ls left #%d before_source=%.6f key(source)=%.6f output=%.6f key_index=%d\n", vf, i, beforeSource, d.key, d.position, KeyIndex(keys.rel, d.key));
            std::fflush(stdout);
            CHECK_MSG(d.key < beforeSource - 0.002, tag + "Left did not move back in source time");
        }
    }
}

namespace {

// Skip targets chosen to cross GOPs, to hit the wrong side of the origin (nearest key is the origin itself) and the ends.
const double kSkipSteps[] = { +3, +0.1, -0.1, +1, -1, +4, -0.3, +0.3, -4, +2.2, -10, +10 };

}

// Skip (the shared held-key timed seek): every landing is exactly the closest key packet to displayed + seconds, on
// the right side of the displayed frame, and the play state is kept; with no qualifying key nothing moves.
TEST_CASE_INTEGRATION(FfPlayer_skip_lands_on_the_closest_key_packet) {
    for (const KeyFixture& fx : kKeyFixtures) {
        const fs::path path = RepoRoot() / L"testmedia" / fx.rel;
        REQUIRE_MSG(fs::exists(path), std::string("missing fixture ") + fx.name);
        const KeyList keys = ProbeKeys(path);
        for (const bool paused : { true, false }) {
            Session s;
            s.OpenAndPrepare(path.wstring(), KeyOptions());
            WaitFirstFrame(s);
            Delay(300);
            const std::string tag = std::string(fx.name) + (paused ? " paused: " : " playing: ");
            double origin = NAN;
            if (paused) {
                WaitPaused(s);
                const auto first = DoKeySeek(s, +1);
                REQUIRE_MSG(first.moved, tag + "no first key");
                origin = first.key;
            }
            std::string walked;
            using clock = std::chrono::steady_clock;
            double landKey = NAN;                // playing: the last landing, the displayed frame advances from it in real time
            clock::time_point landAt = clock::now();
            for (double sec : kSkipSteps) {
                if (!paused && s.player->State() != PlayerState::Playing) break;   // the short clip ended
                const int dir = sec > 0 ? +1 : -1;
                double expected;
                if (paused) {
                    if (NearTie(keys.rel, origin + sec)) continue;
                    expected = ExpectedNear(keys.rel, origin + sec, dir, origin);
                } else {
                    // The displayed frame advances a little before the command runs: only steps whose answer does not
                    // depend on that are checked.
                    origin = std::isfinite(landKey) ? landKey + std::chrono::duration<double>(clock::now() - landAt).count() : s.player->Position();
                    expected = ExpectedNear(keys.rel, origin + sec, dir, origin);
                    bool stable = true;
                    for (double drift : { -0.1, 0.0, 0.2 }) {
                        const double e = ExpectedNear(keys.rel, origin + drift + sec, dir, origin + drift);
                        stable = stable && (std::isfinite(e) == std::isfinite(expected)) && (!std::isfinite(e) || SameTime(e, expected));
                    }
                    if (!stable) continue;
                }
                const int before = s.keyDones;
                const int rc = s.player->Skip(sec);
                REQUIRE_MSG(rc == 0, tag + "Skip rejected: " + std::to_string(rc));
                REQUIRE_MSG(WaitFor([&] { return s.keyDones > before; }, 15), tag + "no done event");
                const auto d = s.lastKey;
                if (d.moved) { landKey = d.key; landAt = clock::now(); }
                char b[64]; std::snprintf(b, sizeof b, " [%+.1f -> %.3f]", sec, d.moved ? d.key : -1.0); walked += b;
                if (!std::isfinite(expected)) {
                    CHECK_MSG(!d.moved, tag + "moved although no key qualifies (origin " + std::to_string(origin) + ", " + std::to_string(sec) + " s)");
                    continue;
                }
                CHECK_MSG(d.moved && SameTime(d.key, expected),
                          tag + "skip " + std::to_string(sec) + " s from " + std::to_string(origin) + " landed at " + std::to_string(d.key) + ", closest qualifying key is " + std::to_string(expected));
                CHECK_MSG(d.moved && SameTime(d.position, d.key), tag + "the presented frame is not the key packet");
                if (paused) {
                    CHECK_MSG(SameTime(s.player->Position(), d.position), tag + "paused position is not the landing");
                    CHECK_MSG(s.player->State() == PlayerState::Paused, tag + "a skip must not resume a paused player");
                    if (d.moved) origin = d.key;
                } else if (s.player->State() != PlayerState::Ended) {
                    CHECK_MSG(s.player->State() == PlayerState::Playing, tag + "a skip must keep a playing player playing");
                }
            }
            std::printf("SKIP | %s%s\n", tag.c_str(), walked.c_str());
            std::fflush(stdout);
            CHECK_MSG(s.errors.empty(), tag + "player error");
        }
    }
}

namespace {

// Playing: the first position sample inside the window around the seek target is the landing (taken within a frame or two).
double FirstPositionNear(Session& s, double target, double window, double timeoutSeconds) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    while (clock::now() - t0 < std::chrono::duration<double>(timeoutSeconds)) {
        Pump();
        const double p = s.player->Position();
        // Position() answers the requested target itself until the seek has landed: not a landing
        if (std::isfinite(p) && std::fabs(p - target) > 1e-6 && std::fabs(p - target) <= window) return p;
        Sleep(1);
    }
    return NAN;
}

}

// ffp_seek (seek bar, absolute and relative, coalesced) lands on the closest key packet too: including the 4.0 s case
// that used to land one key early when paused; relative seeks obey the direction rule; the play state is kept.
TEST_CASE_INTEGRATION(FfPlayer_seek_lands_on_the_closest_key_packet_paused_and_playing) {
    for (const KeyFixture& fx : kKeyFixtures) {
        const fs::path path = RepoRoot() / L"testmedia" / fx.rel;
        REQUIRE_MSG(fs::exists(path), std::string("missing fixture ") + fx.name);
        const KeyList keys = ProbeKeys(path);
        {
            Session s;
            const double dur = s.OpenAndPrepare(path.wstring(), KeyOptions()).Duration;
            WaitFirstFrame(s);
            Delay(300);
            WaitPaused(s);
            const std::string tag = std::string(fx.name) + " paused: ";
            std::string walked;
            for (double t : { 4.0, 0.0, 2.9, 5.3, 1.0, 0.5 * dur, dur - 0.3, 3.1 }) {
                const double target = std::min(t, dur - 0.05);
                if (NearTie(keys.rel, target)) continue;
                const double expected = ExpectedNear(keys.rel, target, 0, 0);
                REQUIRE_MSG(s.player->Seek(target, false) == 0, tag + "seek rejected");
                bool ok = WaitFor([&] { return SameTime(s.player->Position(), expected); }, 10);
                if (ok) { Delay(400); ok = SameTime(s.player->Position(), expected); }   // not just the echo of the request
                char b[64]; std::snprintf(b, sizeof b, " [%.2f -> %.3f]", target, s.player->Position()); walked += b;
                CHECK_MSG(ok, tag + "seek to " + std::to_string(target) + " landed at " + std::to_string(s.player->Position()) + ", closest key is " + std::to_string(expected));
            }
            // Relative: the nearest key to a short hop is the origin itself; the right side wins.
            for (double hop : { +0.1, -0.1, +1.0, -1.0, +7.0, -7.0 }) {
                const double origin = s.player->Position();
                const double target = origin + hop;
                if (NearTie(keys.rel, target)) continue;
                const double expected = ExpectedNear(keys.rel, target, hop > 0 ? +1 : -1, origin);
                const int pos0 = s.positions;
                REQUIRE_MSG(s.player->Seek(hop, true) == 0, tag + "relative seek rejected");
                if (!std::isfinite(expected)) { Delay(800); continue; }
                bool ok = WaitFor([&] { return s.positions > pos0 && SameTime(s.player->Position(), expected); }, 10);
                if (ok) { Delay(400); ok = SameTime(s.player->Position(), expected); }
                char b[64]; std::snprintf(b, sizeof b, " [%+.1f from %.3f -> %.3f]", hop, origin, s.player->Position()); walked += b;
                CHECK_MSG(ok, tag + "relative seek " + std::to_string(hop) + " from " + std::to_string(origin) + " landed at " + std::to_string(s.player->Position()) + ", closest qualifying key is " + std::to_string(expected));
                if (!ok) break;
            }
            CHECK_MSG(s.player->State() == PlayerState::Paused, tag + "timed seeks must keep a paused player paused");
            std::printf("SEEKNEAR | %s%s\n", tag.c_str(), walked.c_str());
            std::fflush(stdout);
        }
        {
            Session s;
            const double dur = s.OpenAndPrepare(path.wstring(), KeyOptions()).Duration;
            WaitFirstFrame(s);
            const std::string tag = std::string(fx.name) + " playing: ";
            double maxGap = 0;
            for (size_t i = 0; i + 1 < keys.rel.size(); ++i) maxGap = std::max(maxGap, keys.rel[i + 1] - keys.rel[i]);
            std::string walked;
            for (double frac : { 0.85, 0.2, 0.65, 0.3 }) {
                const double target = frac * dur;
                if (NearTie(keys.rel, target)) continue;
                const double expected = ExpectedNear(keys.rel, target, 0, 0);
                REQUIRE_MSG(s.player->Seek(target, false) == 0, tag + "seek rejected");
                const double p = FirstPositionNear(s, target, 0.5 * maxGap + 0.05, 6);
                char b[64]; std::snprintf(b, sizeof b, " [%.2f -> %.3f]", target, p); walked += b;
                CHECK_MSG(std::isfinite(p) && p >= expected - 0.1 && p <= expected + 0.25,
                          tag + "seek to " + std::to_string(target) + " first showed " + std::to_string(p) + ", closest key is " + std::to_string(expected));
                if (s.player->State() != PlayerState::Playing) break;
            }
            CHECK_MSG(s.player->State() == PlayerState::Playing || s.player->State() == PlayerState::Ended, tag + "timed seeks must keep a playing player playing");
            std::printf("SEEKNEAR | %s%s\n", tag.c_str(), walked.c_str());
            std::fflush(stdout);
        }
    }
}

// Resume position (the "ss" open option): the first presented frame is the key packet closest to it.
TEST_CASE_INTEGRATION(FfPlayer_open_ss_lands_exactly_on_the_closest_key_packet) {
    for (const KeyFixture& fx : kKeyFixtures) {
        const fs::path path = RepoRoot() / L"testmedia" / fx.rel;
        REQUIRE_MSG(fs::exists(path), std::string("missing fixture ") + fx.name);
        const KeyList keys = ProbeKeys(path);
        for (double ss : { 4.0, 5.3, 2.9, 0.0, 1.0 }) {
            if (NearTie(keys.rel, ss)) continue;
            const double expected = ExpectedNear(keys.rel, ss, 0, 0);
            Session s;
            OpenOptions opts = KeyOptions();
            char b[32]; std::snprintf(b, sizeof b, "%.3f", ss);
            const std::string bs = b;
            opts.push_back({ L"ss", std::wstring(bs.begin(), bs.end()) });
            opts.push_back({ L"start_paused", L"1" });
            s.OpenAndPrepare(path.wstring(), opts);
            WaitFirstFrame(s);
            const bool ok = WaitFor([&] { return SameTime(s.player->Position(), expected); }, 5);
            std::printf("SSNEAR | %s ss=%.3f -> %.3f (closest key %.3f)\n", fx.name, ss, s.player->Position(), expected);
            std::fflush(stdout);
            CHECK_MSG(ok, std::string(fx.name) + ": ss=" + bs + " presented " + std::to_string(s.player->Position()) + ", closest key is " + std::to_string(expected));
        }
    }
}

// Alt+Left pauses like D (repeated separate taps, longer apart than any GOP, walk exactly one keyframe each starting from
// playing, and leave the player Paused); Alt+Right keeps the play state and lands exactly on the next key; timed skips
// keep the play state.
TEST_CASE_INTEGRATION(FfPlayer_keyframe_back_taps_pause_forward_keeps_playing) {
    const KeyFixture& fx = kKeyFixtures[0];
    const fs::path path = RepoRoot() / L"testmedia" / fx.rel;
    REQUIRE_MSG(fs::exists(path), "missing fixture");
    const KeyList keys = ProbeKeys(path);
    {
        Session s;
        s.OpenAndPrepare(path.wstring(), KeyOptions());
        WaitFirstFrame(s);
        Delay(3000);
        REQUIRE_MSG(s.player->State() == PlayerState::Playing, "not playing before the first tap");
        const std::string tag = "Alt+Left taps: ";
        int prev = -1;
        std::string walked;
        for (int i = 0; i < 4; ++i) {
            const auto d = DoKeySeek(s, -1);
            REQUIRE_MSG(d.moved, tag + "tap " + std::to_string(i) + " did not move");
            const int idx = KeyIndex(keys.rel, d.key);
            char b[32]; std::snprintf(b, sizeof b, " %.3f", d.key); walked += b;
            CHECK_MSG(idx >= 0, tag + "landing is not a key packet");
            if (i > 0) CHECK_MSG(idx == prev - 1, tag + "tap " + std::to_string(i) + " did not move back exactly one keyframe (stuck or skipped)");
            prev = idx;
            CHECK_MSG(s.player->State() == PlayerState::Paused, tag + "the step back did not leave the player paused");
            Delay(700);
            CHECK_MSG(s.player->State() == PlayerState::Paused && SameTime(s.player->Position(), d.position), tag + "the paused landing moved");
        }
        std::printf("KEYSEEK_TAPS | %s%s\n", tag.c_str(), walked.c_str());
        std::fflush(stdout);
        const auto again = DoKeySeek(s, -1);
        (void)again;
        CHECK_MSG(s.player->State() == PlayerState::Paused, tag + "a step back from paused did not stay paused");
        // Paused stays paused through a timed skip; Play, then a timed skip keeps playing.
        const int k0 = s.keyDones;
        REQUIRE_MSG(s.player->Skip(+0.5) == 0 && WaitFor([&] { return s.keyDones > k0; }, 10), tag + "skip while paused failed");
        Delay(300);
        CHECK_MSG(s.player->State() == PlayerState::Paused, tag + "a timed skip resumed a paused player");
        REQUIRE_MSG(s.player->Play() == 0 && WaitFor([&] { return s.player->State() == PlayerState::Playing; }, 5), tag + "Play failed");
        const int k1 = s.keyDones;
        REQUIRE_MSG(s.player->Skip(-0.5) == 0 && WaitFor([&] { return s.keyDones > k1; }, 10), tag + "skip while playing failed");
        Delay(300);
        CHECK_MSG(s.player->State() == PlayerState::Playing, tag + "a timed skip paused a playing player");
    }
    {
        Session s;
        s.OpenAndPrepare(path.wstring(), KeyOptions());
        WaitFirstFrame(s);
        Delay(800);
        const std::string tag = "Alt+Right taps: ";
        std::string walked;
        for (int i = 0; i < 3; ++i) {
            const double before = s.player->Position();
            const auto d = DoKeySeek(s, +1);
            REQUIRE_MSG(d.moved, tag + "tap " + std::to_string(i) + " did not move");
            char b[32]; std::snprintf(b, sizeof b, " %.3f", d.key); walked += b;
            CHECK_MSG(KeyIndex(keys.rel, d.key) >= 0, tag + "landing is not a key packet");
            const double expected = ExpectedKey(keys.rel, before + 0.05, +1);
            CHECK_MSG(d.key > before - 0.1 && (SameTime(d.key, expected) || SameTime(d.key, ExpectedKey(keys.rel, before - 0.1, +1)) || SameTime(d.key, ExpectedKey(keys.rel, before + 0.25, +1))),
                      tag + "tap " + std::to_string(i) + " from " + std::to_string(before) + " did not land on the next key (" + std::to_string(d.key) + ")");
            CHECK_MSG(s.player->State() == PlayerState::Playing, tag + "the step forward paused a playing player");
            Delay(700);
            CHECK_MSG(s.player->State() == PlayerState::Playing && s.player->Position() > d.position + 0.3, tag + "playback did not continue after the step forward");
        }
        std::printf("KEYSEEK_TAPS | %s%s\n", tag.c_str(), walked.c_str());
        std::fflush(stdout);
    }
}

// Held timed arrow (Right = +10 s): one skip in flight at a time, each landing the closest key to the previous landing
// + 10 s, nothing left running after the release.
TEST_CASE_INTEGRATION(FfPlayer_held_skip_arrow_chains_closest_key_landings_and_stops_on_release) {
    const KeyFixture& fx = kKeyFixtures[2];
    const fs::path path = RepoRoot() / L"testmedia" / fx.rel;
    REQUIRE_MSG(fs::exists(path), "missing fixture");
    const KeyList keys = ProbeKeys(path);
    for (const bool paused : { true, false }) {
        Session s;
        s.OpenAndPrepare(path.wstring(), KeyOptions());
        WaitFirstFrame(s);
        Delay(300);
        if (paused) WaitPaused(s);
        const std::string tag = std::string(paused ? "paused " : "playing ") + "held +10 s: ";
        const int hold = 3;
        const KeyHeldRun r = RunHeldKeyframeKey(s, +1, hold, 110);
        std::printf("SKIP_HELD | %s hold=%d started=%d done=%d started_after_release=%d inflight_at_release=%d busy=%d dropped_repeats=%d done_after_settle=%d landed:",
                    paused ? "paused" : "playing", hold, r.Started, r.Done, r.StartedAfterRelease, r.InFlightAtRelease ? 1 : 0, r.Busy,
                    r.DroppedRepeats, r.DoneAfterSettle);
        for (double k : r.Landed) std::printf(" %.3f", k);
        std::printf("\n");
        std::fflush(stdout);
        CHECK_MSG(r.Done >= hold, tag + "fewer skips completed than were held for");
        CHECK_MSG(r.Started == r.Done, tag + "a started skip did not complete exactly once");
        CHECK_MSG(r.Rejected == 0 && r.Busy == 0, tag + "a skip was submitted while another was in flight");
        CHECK_MSG(r.StartedAfterRelease == 0, tag + "a skip started after the key was released");
        CHECK_MSG(r.Done == hold + (r.InFlightAtRelease ? 1 : 0), tag + "more than the in-flight skip ran after the release");
        CHECK_MSG(r.DoneAfterSettle == 0, tag + "a skip was still queued behind the release");
        CHECK_MSG(r.DroppedRepeats > 0, tag + "no auto-repeat arrived while a skip was in flight");
        for (size_t i = 0; i < r.Landed.size(); ++i) {
            const bool known = i > 0;
            CHECK_MSG(KeyIndex(keys.rel, r.Landed[i]) >= 0, tag + "a held landing is not a key packet");
            if (known) {
                const double expected = ExpectedNear(keys.rel, r.Landed[i - 1] + 10.0, +1, r.Landed[i - 1]);
                CHECK_MSG(paused ? SameTime(r.Landed[i], expected) : std::fabs(r.Landed[i] - r.Landed[i - 1] - 10.0) <= 2.5,
                          tag + "landing " + std::to_string(i) + " is not the key closest to the previous landing + 10 s");
            }
        }
        CHECK_MSG(s.player->State() == (paused ? PlayerState::Paused : PlayerState::Playing), tag + "wrong state after the release");
    }
}
