// ffp_get_debug_info / ffp_set_log_callback: parser unit tests + headless libffplay integration tests
// (hidden host window, SDL_AUDIODRIVER=dummy set in main).
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "ffplay.h"
#include "wvp/DebugInfo.h"
#include "wvp/FfPlayer.h"
#include "wvptest.h"

using namespace wvp;
namespace fs = std::filesystem;

namespace {

constexpr UINT kDbgNotifyMsg = WM_APP + 57;

LRESULT CALLBACK DbgHostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == kDbgNotifyMsg) { FfPlayer::HandleNotify(w, l); return 0; }
    return DefWindowProcW(h, m, w, l);
}

void EnsureDbgClass() {
    static ATOM a = [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DbgHostProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"WvpDbgTestHost";
        return RegisterClassW(&wc);
    }();
    (void)a;
}

fs::path DbgRepoRoot() {
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

std::wstring DbgMedia(const wchar_t* name) { return (DbgRepoRoot() / L"testmedia" / name).wstring(); }

void DbgPump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}

template <class F>
bool DbgWaitFor(F cond, double seconds = 10) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds((long long)(seconds * 1000));
    while (std::chrono::steady_clock::now() < end) {
        DbgPump();
        if (cond()) return true;
        Sleep(15);
    }
    DbgPump();
    return cond();
}

struct DbgSession {
    HWND parent = nullptr, child = nullptr;
    std::unique_ptr<FfPlayer> player;
    int prepared = 0;
    int errors = 0;

    DbgSession() {
        EnsureDbgClass();
        HINSTANCE inst = GetModuleHandleW(nullptr);
        parent = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"WvpDbgTestHost", L"host", WS_POPUP, -32000, -32000,
                                 640, 360, nullptr, nullptr, inst, nullptr);
        child = CreateWindowExW(0, L"STATIC", L"video", WS_CHILD | WS_VISIBLE, 0, 0, 640, 360, parent, nullptr, inst, nullptr);
        int err = 0;
        player = FfPlayer::CreateSync(child, parent, kDbgNotifyMsg, &err);
        REQUIRE_MSG(player != nullptr, "CreateSync failed: " + std::to_string(err));
        player->Prepared.add([this](const MediaPreparedInfo&) { prepared++; });
        player->Error.add([this](const PlayerErrorInfo&) { errors++; });
    }
    ~DbgSession() {
        player.reset();
        if (parent) DestroyWindow(parent);
    }

    void Open(const std::wstring& file, const OpenOptions& opts = {}) {
        int before = prepared;
        REQUIRE_MSG(player->Open(file, opts) == 0, "Open failed");
        REQUIRE_MSG(DbgWaitFor([&] { return prepared > before || errors > 0; }), "no Prepared event");
        REQUIRE_MSG(errors == 0, "open error");
    }

    template <class P>
    DebugInfo WaitInfo(P pred, double seconds = 10) {
        DebugInfo last;
        DbgWaitFor([&] {
            auto js = player->GetDebugInfoJson();
            if (!js) return false;
            last = ParseDebugInfo(*js);
            return last.Valid && pred(last);
        }, seconds);
        return last;
    }
};

bool Has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

struct LogCollector {
    std::mutex mu;
    std::vector<std::pair<int, std::string>> lines;
    void Install() {
        FfPlayer::SetLogCallback([this](int level, const std::string& line) {
            std::lock_guard<std::mutex> lk(mu);
            lines.emplace_back(level, line);
        });
    }
    ~LogCollector() { FfPlayer::SetLogCallback(nullptr); }
    size_t Count() { std::lock_guard<std::mutex> lk(mu); return lines.size(); }
    bool Any(const char* needle) {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& l : lines) if (Has(l.second, needle)) return true;
        return false;
    }
};

const char* kFullJson = R"({
 "position": 12.34, "duration": 60.0, "paused": false, "speed": 1.5, "renderer": "sdl:direct3d11",
 "video": {"stream_index":0,"codec":"h264","decoder":"h264_cuvid","hwaccel":"d3d11va","pix_fmt":"yuv420p",
   "hw_pix_fmt":"d3d11","sw_pix_fmt":"nv12","width":1920,"height":1080,"sar":"1:1","frame_rate":29.97,
   "out_width":1280,"out_height":720,"out_pix_fmt":"yuv420p","time_base":"1/90000","start_time":0.0,
   "hwaccel_requested":"d3d11va","hwaccel_device":"none","hwaccel_error":"no hwaccel","frame_path":"software",
   "frame_path_counts":{"hw_direct":0,"hw_download":3,"software":7},"pix_fmt_decoded":"yuv420p","pix_fmt_after_transfer":""},
 "audio": {"stream_index":1,"codec":"aac","decoder":"aac","sample_rate":48000,"channels":2,"channel_layout":"stereo",
   "sample_fmt":"fltp","time_base":"1/48000","start_time":0.021,"out_device":"dummy","out_sample_rate":48000,
   "out_channels":2,"hw_buf_size":8192,"diff_avg":0.001,"latency_ms":42.0},
 "subtitle": {"stream_index":2,"codec":"subrip","time_base":"1/1000","start_time":0.0},
 "filters": {"vf_requested":"hflip","af_requested":"atempo=1.5",
   "vf_graph":"buffer=a,hflip,buffersink","af_graph":"abuffer=b,atempo=1.5,abuffersink",
   "vf_filters":["buffer","hflip","buffersink"],"af_filters":["abuffer","atempo","abuffersink"],
   "vf_nodes":[{"name":"hflip","instance":"Parsed_hflip_0","args":"","hw_device":""},{"name":"bwdif_d3d11","instance":"u","args":"a=1","hw_device":"d3d11va"}],"af_nodes":[]},
 "sync": {"master":"audio","audio_clock":12.30,"video_clock":12.31,"external_clock":12.31,"master_clock":12.30,
   "av_diff_ms":-8.5,"frame_drops_early":1,"frame_drops_late":2,"frames_duplicated":0,"frame_timer_drift_ms":0.5},
 "queues": {"audio":{"packets":12,"bytes":34567,"duration":0.48},"video":{"packets":25,"bytes":912345,"duration":1.0},
   "subtitle":{"packets":0,"bytes":0,"duration":0.0}}
})";

}

TEST_CASE(DebugInfo_parses_full_document) {
    DebugInfo d = ParseDebugInfo(kFullJson);
    REQUIRE(d.Valid);
    CHECK_EQ(*d.Position, 12.34);
    CHECK_EQ(*d.Duration, 60.0);
    CHECK(!d.Paused);
    CHECK_EQ(*d.Speed, 1.5);
    REQUIRE(d.HasVideo);
    CHECK_EQ(d.Video.Codec, std::string("h264"));
    CHECK_EQ(d.Video.Decoder, std::string("h264_cuvid"));
    CHECK_EQ(d.Video.Hwaccel, std::string("d3d11va"));
    CHECK_EQ(d.Video.HwPixFmt, std::string("d3d11"));
    CHECK_EQ(*d.Video.Width, 1920);
    CHECK_EQ(*d.Video.OutHeight, 720);
    CHECK_EQ(d.Video.Sar, std::string("1:1"));
    CHECK_EQ(d.Video.TimeBase, std::string("1/90000"));
    REQUIRE(d.HasAudio);
    CHECK_EQ(d.Audio.Decoder, std::string("aac"));
    CHECK_EQ(*d.Audio.HwBufSize, 8192);
    CHECK_EQ(*d.Audio.LatencyMs, 42.0);
    CHECK_EQ(d.Audio.OutDevice, std::string("dummy"));
    REQUIRE(d.HasSubtitle);
    CHECK_EQ(d.Subtitle.Codec, std::string("subrip"));
    CHECK_EQ(d.Filters.VfRequested, std::string("hflip"));
    CHECK_EQ(d.Filters.VfFilters.size(), (size_t)3);
    CHECK_EQ(d.Filters.VfFilters[1], std::string("hflip"));
    CHECK_EQ(d.Filters.AfFilters.size(), (size_t)3);
    CHECK_EQ(d.Sync.Master, std::string("audio"));
    CHECK_EQ(*d.Sync.AvDiffMs, -8.5);
    CHECK_EQ(*d.Sync.FrameDropsLate, (std::int64_t)2);
    CHECK_EQ(*d.AudioQueue.Packets, (std::int64_t)12);
    CHECK_EQ(*d.VideoQueue.Bytes, (std::int64_t)912345);
    CHECK_EQ(*d.VideoQueue.Duration, 1.0);
    CHECK_EQ(d.Renderer, std::string("sdl:direct3d11"));
    CHECK_EQ(d.Video.HwaccelRequested, std::string("d3d11va"));
    CHECK_EQ(d.Video.HwaccelDevice, std::string("none"));
    CHECK_EQ(d.Video.HwaccelError, std::string("no hwaccel"));
    CHECK_EQ(d.Video.FramePath, std::string("software"));
    CHECK_EQ(*d.Video.FramesHwDownload, (std::int64_t)3);
    CHECK_EQ(*d.Video.FramesSoftware, (std::int64_t)7);
    CHECK_EQ(d.Video.PixFmtDecoded, std::string("yuv420p"));
    REQUIRE(d.Filters.VfNodes.size() == 2);
    CHECK_EQ(d.Filters.VfNodes[1].Name, std::string("bwdif_d3d11"));
    CHECK_EQ(d.Filters.VfNodes[1].HwDevice, std::string("d3d11va"));
    CHECK_EQ(d.Filters.VfNodes[0].Instance, std::string("Parsed_hflip_0"));
}

TEST_CASE(DebugInfo_parses_arib_audio_model) {
    const DebugInfo d = ParseDebugInfo(R"({"arib_audio":{"preference":"audio2","playing":"silence","switches":5,"silence_ms":2005.5,"rebinds":3,
        "roles":[{"role":"audio1","present":true,"stream_index":1,"pid":274,"component_tag":16,"starved":false,"buffered_ms":900.0},
                 {"role":"audio2","present":false,"stream_index":-1,"pid":-1,"component_tag":-1,"starved":true,"buffered_ms":0.0}]}})");
    CHECK(d.Valid && d.AribAudio.Present);
    CHECK(d.AribAudio.Preference == "audio2" && d.AribAudio.Playing == "silence");
    CHECK(d.AribAudio.Switches && *d.AribAudio.Switches == 5 && d.AribAudio.Rebinds && *d.AribAudio.Rebinds == 3);
    CHECK(d.AribAudio.SilenceMs && std::abs(*d.AribAudio.SilenceMs - 2005.5) < 1e-6);
    CHECK(d.AribAudio.Audio1.Present && d.AribAudio.Audio1.Pid == 274 && d.AribAudio.Audio1.ComponentTag == 16 && !d.AribAudio.Audio1.Starved);
    CHECK(!d.AribAudio.Audio2.Present && d.AribAudio.Audio2.Starved && d.AribAudio.Audio2.StreamIndex == -1);
    CHECK(!ParseDebugInfo(R"({"arib_audio":null})").AribAudio.Present);
    CHECK(!ParseDebugInfo(R"({"position":1})").AribAudio.Present);
}

TEST_CASE(DebugInfo_parses_presenter_acrylic_stats) {
    const DebugInfo d = ParseDebugInfo(R"({"presenter":{"name":"d3d11","acrylic":{"regions":2,"frames_blurred":10,"failures":0,"blur_ms":0.37}}})");
    REQUIRE(d.Valid && d.Presenter.Present && d.Presenter.Acrylic.Present);
    CHECK_EQ(d.Presenter.Name, std::string("d3d11"));
    CHECK_EQ(*d.Presenter.Acrylic.Regions, 2);
    CHECK_EQ(*d.Presenter.Acrylic.FramesBlurred, (std::int64_t)10);
    CHECK_EQ(*d.Presenter.Acrylic.Failures, (std::int64_t)0);
    CHECK_EQ(*d.Presenter.Acrylic.BlurMs, 0.37);
}

TEST_CASE(DebugInfo_parses_frame_trace) {
    const DebugInfo d = ParseDebugInfo(R"({"seek":{"valid":true,"requested":2.4,"keyframe":2.35,"distance_ms":-50,
        "gop_ms":500,"direction":"backward"},"frame_trace":{"decoded":{"count":10,"pts":2.5,"serial":3,"at_us":100},
        "filtered":{"count":8,"pts":2.4,"serial":3,"at_us":120},
        "presented":{"count":7,"pts":2.3,"serial":3,"at_us":140},
        "early_drop":{"count":1,"pts":2.2,"serial":3,"at_us":130},
        "late_drop":{"count":2,"pts":2.1,"serial":3,"at_us":135},
        "presenter_drop":{"count":0,"pts":null,"serial":null,"at_us":null},
        "dxgi_valid":true,"dxgi_present_count":7,"dxgi_present_refresh_count":9,
        "dxgi_sync_refresh_count":11,"dxgi_sync_qpc_time":123456,"dxgi_sync_gpu_time":654321,
        "gpu_total_us":900,"gpu_frames":7,"gpu_last_us":130,
        "events":[{"kind":"presented","count":7,"pts":2.3,"serial":3,"at_us":140,
                   "dxgi_valid":true,"dxgi_present_count":7,"dxgi_sync_refresh_count":11,
                   "dxgi_sync_qpc_time":123456,"dxgi_sync_gpu_time":654321,
                   "gpu_total_us":900,"gpu_frames":7,"gpu_last_us":130}]}})");
    REQUIRE(d.Valid && d.FrameTrace.Present);
    CHECK(d.Seek.Valid);
    CHECK_EQ(d.Seek.Direction, std::string("backward"));
    CHECK_EQ(*d.Seek.Keyframe, 2.35);
    CHECK_EQ(*d.Seek.GopMs, 500.0);
    CHECK_EQ(*d.FrameTrace.Decoded.Count, (std::int64_t)10);
    CHECK_EQ(*d.FrameTrace.Filtered.Pts, 2.4);
    CHECK_EQ(*d.FrameTrace.Presented.Serial, (std::int64_t)3);
    CHECK_EQ(*d.FrameTrace.Presented.AtUs, (std::int64_t)140);
    CHECK_EQ(*d.FrameTrace.LateDrop.Pts, 2.1);
    CHECK(!d.FrameTrace.PresenterDrop.Pts.has_value());
    REQUIRE(d.FrameTrace.DxgiValid.has_value());
    CHECK(*d.FrameTrace.DxgiValid);
    CHECK_EQ(*d.FrameTrace.DxgiPresentCount, (std::int64_t)7);
    CHECK_EQ(*d.FrameTrace.DxgiSyncQpcTime, (std::int64_t)123456);
    REQUIRE(d.FrameTrace.Events.size() == (size_t)1);
    CHECK_EQ(d.FrameTrace.Events[0].Kind, std::string("presented"));
    CHECK_EQ(*d.FrameTrace.Events[0].Pts, 2.3);
    CHECK_EQ(*d.FrameTrace.D3dGpuTotalUs, (std::int64_t)900);
    CHECK_EQ(*d.FrameTrace.Events[0].D3dGpuLastUs, (std::int64_t)130);
}

TEST_CASE(DebugInfo_tolerates_missing_keys_and_old_dll) {
    DebugInfo d = ParseDebugInfo(R"({"position":1.5,"duration":10,"video":{"codec":"h264"}})");
    REQUIRE(d.Valid);
    CHECK(d.HasVideo);
    CHECK(!d.HasAudio);
    CHECK(!d.HasSubtitle);
    CHECK_EQ(d.Video.Codec, std::string("h264"));
    CHECK(!d.Video.Width.has_value());
    CHECK(d.Video.Decoder.empty());
    CHECK(!d.Sync.AvDiffMs.has_value());
    CHECK(d.Sync.Master.empty());
    CHECK(!d.VideoQueue.Packets.has_value());
    CHECK(d.Filters.VfFilters.empty());
    CHECK(!d.Speed.has_value());
}

TEST_CASE(DebugInfo_handles_nulls_and_garbage) {
    DebugInfo d = ParseDebugInfo(R"({"position":null,"duration":null,"video":null,"audio":null,"subtitle":null,
        "sync":{"master":"video","av_diff_ms":null,"audio_clock":null},"queues":{"audio":null}})");
    REQUIRE(d.Valid);
    CHECK(!d.Position.has_value());
    CHECK(!d.HasVideo);
    CHECK(!d.HasAudio);
    CHECK(!d.HasSubtitle);
    CHECK_EQ(d.Sync.Master, std::string("video"));
    CHECK(!d.Sync.AvDiffMs.has_value());
    CHECK(!d.Sync.AudioClock.has_value());
    CHECK(!ParseDebugInfo("{bad").Valid);
    CHECK(!ParseDebugInfo("").Valid);
    CHECK(!ParseDebugInfo("[1,2]").Valid);
}

TEST_CASE_INTEGRATION(DebugInfo_null_when_nothing_open) {
    DbgSession s;
    CHECK(!s.player->GetDebugInfoJson().has_value());
}

TEST_CASE_INTEGRATION(DebugInfo_playing_clip_reports_decoder_graphs_and_sync) {
    DbgSession s;
    s.Open(DbgMedia(L"testsrc2-360p-short.mp4"));
    DebugInfo d = s.WaitInfo([](const DebugInfo& i) {
        return i.HasVideo && i.HasAudio && !i.Filters.VfGraph.empty() && !i.Filters.AfGraph.empty() &&
               i.Sync.MasterClock && *i.Sync.MasterClock > 0.1;
    });
    REQUIRE(d.Valid);
    REQUIRE(d.HasVideo);
    REQUIRE(d.HasAudio);
    CHECK(!d.Video.Decoder.empty());
    CHECK(!d.Video.Codec.empty());
    CHECK(!d.Video.Hwaccel.empty());
    CHECK(!d.Video.PixFmt.empty());
    CHECK(!d.Audio.Decoder.empty());
    CHECK(Has(d.Filters.VfGraph, "buffer"));
    CHECK(Has(d.Filters.VfGraph, "buffersink"));
    CHECK(Has(d.Filters.AfGraph, "abuffer"));
    CHECK(Has(d.Filters.AfGraph, "abuffersink"));
    REQUIRE(d.Filters.VfFilters.size() >= 2);
    CHECK_EQ(d.Filters.VfFilters.front(), std::string("buffer"));
    CHECK_EQ(d.Filters.VfFilters.back(), std::string("buffersink"));
    CHECK(!d.Sync.Master.empty());
    REQUIRE(d.Sync.AvDiffMs.has_value());
    CHECK(std::fabs(*d.Sync.AvDiffMs) <= 1000.0);
    for (const DebugQueue* q : {&d.AudioQueue, &d.VideoQueue, &d.SubtitleQueue}) {
        REQUIRE(q->Packets.has_value());
        CHECK(*q->Packets >= 0);
        CHECK(*q->Bytes >= 0);
        CHECK(*q->Duration >= 0.0);
    }
    REQUIRE(d.Duration.has_value());
    CHECK(*d.Duration > 0.0);
    REQUIRE(d.Sync.MasterClock.has_value());
    CHECK(*d.Sync.MasterClock >= -0.5 && *d.Sync.MasterClock <= *d.Duration + 1.0);
    CHECK(d.Video.Width.value_or(0) > 0);
    CHECK(d.Audio.SampleRate.value_or(0) > 0);
    CHECK(d.Audio.HwBufSize.value_or(0) > 0);
    CHECK(!d.Audio.OutDevice.empty());
    CHECK(!d.Paused);
    if (auto js = s.player->GetDebugInfoJson()) std::printf("DEBUGINFO-JSON %s\n", js->c_str());
}

TEST_CASE_INTEGRATION(DebugInfo_1080p_clip_sane) {
    DbgSession s;
    s.Open(DbgMedia(L"testsrc2-1080p-60s.mp4"));
    DebugInfo d = s.WaitInfo([](const DebugInfo& i) {
        return i.HasVideo && !i.Filters.VfGraph.empty() && i.Position && *i.Position > 0.5;
    });
    REQUIRE(d.Valid);
    CHECK_EQ(d.Video.Width.value_or(0), 1920);
    CHECK_EQ(d.Video.Height.value_or(0), 1080);
    CHECK(!d.Video.Decoder.empty());
    CHECK(Has(d.Filters.VfGraph, "buffersink"));
    CHECK(d.Sync.AvDiffMs.has_value() && std::fabs(*d.Sync.AvDiffMs) <= 1000.0);
    CHECK(d.Duration.value_or(0) > 50.0);
    s.player->Pause();
    DebugInfo p = s.WaitInfo([](const DebugInfo& i) { return i.Paused; });
    CHECK(p.Paused);
}

TEST_CASE_INTEGRATION(DebugInfo_video_filter_change_updates_vf_graph) {
    DbgSession s;
    s.Open(DbgMedia(L"testsrc2-720p.mp4"));
    DebugInfo d0 = s.WaitInfo([](const DebugInfo& i) { return !i.Filters.VfGraph.empty(); });
    REQUIRE(d0.Valid);
    CHECK(!Has(d0.Filters.VfGraph, "hflip"));
    REQUIRE(s.player->SetVideoFilter(L"hflip") == 0);
    DebugInfo d1 = s.WaitInfo([](const DebugInfo& i) { return Has(i.Filters.VfGraph, "hflip"); }, 8);
    CHECK(Has(d1.Filters.VfGraph, "hflip"));
    CHECK_EQ(d1.Filters.VfRequested, std::string("hflip"));
    bool listed = false;
    for (auto& f : d1.Filters.VfFilters) if (f == "hflip") listed = true;
    CHECK(listed);
}

TEST_CASE_INTEGRATION(DebugInfo_audio_filter_change_updates_af_graph) {
    DbgSession s;
    s.Open(DbgMedia(L"testsrc2-720p.mp4"));
    s.WaitInfo([](const DebugInfo& i) { return !i.Filters.AfGraph.empty(); });
    REQUIRE(s.player->SetAudioFilter(L"atempo=1.25") == 0);
    DebugInfo d = s.WaitInfo([](const DebugInfo& i) { return Has(i.Filters.AfGraph, "atempo"); }, 8);
    CHECK(Has(d.Filters.AfGraph, "atempo"));
    CHECK_EQ(d.Filters.AfRequested, std::string("atempo=1.25"));
}

TEST_CASE_INTEGRATION(DebugInfo_log_callback_receives_lines) {
    LogCollector lc;
    lc.Install();
    FfPlayer::SetLogLevel(48);   // AV_LOG_DEBUG
    {
        DbgSession s;
        s.Open(DbgMedia(L"testsrc2-360p-short.mp4"));
        DbgWaitFor([&] { return lc.Count() > 0; }, 5);
    }
    FfPlayer::SetLogLevel(24);
    CHECK(lc.Count() > 0);
}

TEST_CASE_INTEGRATION(DebugInfo_stats_option_delivers_status_lines_through_log_callback) {
    LogCollector lc;
    lc.Install();
    FfPlayer::SetLogLevel(32);   // AV_LOG_INFO: the status line is logged at info level
    {
        DbgSession s;
        s.Open(DbgMedia(L"testsrc2-720p.mp4"), {{L"stats", L"1"}});
        bool got = DbgWaitFor([&] { return lc.Any("A-V"); }, 8);
        CHECK(got);
        DebugInfo d = s.WaitInfo([](const DebugInfo& i) { return i.Sync.AvDiffMs.has_value(); });
        CHECK(d.Sync.AvDiffMs.has_value());
    }
    FfPlayer::SetLogLevel(24);
}

TEST_CASE_INTEGRATION(DebugInfo_log_callback_null_restores_default) {
    LogCollector lc;
    lc.Install();
    FfPlayer::SetLogCallback(nullptr);
    size_t before = lc.Count();
    {
        DbgSession s;
        s.Open(DbgMedia(L"testsrc2-360p-short.mp4"));
        DbgWaitFor([] { return false; }, 0.5);
    }
    CHECK_EQ(lc.Count(), before);
}

namespace {

// Generates a short MPEG-4 part 2 + AAC clip (a codec without any D3D11VA hwaccel) with the build's ffmpeg.exe.
std::wstring Mpeg4Clip() {
    fs::path out = DbgRepoRoot() / L"testmedia" / L"mpeg4-240p-short.mp4";
    if (fs::exists(out)) return out.wstring();
    fs::path dir = DbgRepoRoot() / L"ffmpeg" / L"dist" / L"win64";
    std::wstring cmd = L"\"" + (dir / L"ffmpeg.exe").wstring() +
        L"\" -y -v error -f lavfi -i testsrc2=size=320x240:rate=25 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 6 "
        L"-c:v mpeg4 -q:v 5 -pix_fmt yuv420p -c:a aac -shortest \"" + out.wstring() + L"\"";
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
    if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 60000);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
    return out.wstring();
}

}

TEST_CASE_INTEGRATION(DebugInfo_software_open_reports_renderer_and_software_path) {
    DbgSession s;
    s.Open(DbgMedia(L"testsrc2-720p.mp4"));
    // The graph is configured after the first decoded frame; wait for both because frame-path readiness can race the graph walk.
    DebugInfo d = s.WaitInfo([](const DebugInfo& i) {
        return i.HasVideo && !i.Video.FramePath.empty() && !i.Filters.VfNodes.empty() && i.Video.OutWidth.value_or(0) > 0;
    });
    REQUIRE(d.Valid);
    CHECK(!d.Renderer.empty());
    CHECK(d.Renderer == "vulkan" || d.Renderer.rfind("sdl:", 0) == 0);
    CHECK_EQ(d.Video.FramePath, std::string("software"));
    CHECK(d.Video.HwaccelRequested.empty());
    CHECK_EQ(d.Video.HwaccelDevice, std::string("none"));
    CHECK_EQ(d.Video.FramesHwDirect.value_or(-1), (std::int64_t)0);
    CHECK_EQ(d.Video.FramesHwDownload.value_or(-1), (std::int64_t)0);
    CHECK(d.Video.FramesSoftware.value_or(0) > 0);
    CHECK(!d.Video.PixFmtDecoded.empty());
    CHECK(!d.Video.PixFmt.empty());
    CHECK(!d.Video.OutPixFmt.empty());
    CHECK(d.Video.OutWidth.value_or(0) > 0);
    CHECK_EQ(d.Filters.VfNodes.size(), d.Filters.VfFilters.size());
    REQUIRE(!d.Filters.VfNodes.empty());
    CHECK_EQ(d.Filters.VfNodes.front().Name, std::string("buffer"));
    CHECK(!d.Filters.VfNodes.front().Instance.empty());
    CHECK(Has(d.Filters.VfNodes.front().Args, "video_size="));
}

TEST_CASE_INTEGRATION(DebugInfo_hwaccel_request_reports_actual_device_path_or_error) {
    DbgSession s;
    s.Open(DbgMedia(L"testsrc2-720p.mp4"), {{L"hwaccel", L"d3d11va"}});
    DebugInfo d = s.WaitInfo([](const DebugInfo& i) { return i.HasVideo && !i.Video.FramePath.empty(); });
    REQUIRE(d.Valid);
    CHECK(!d.Renderer.empty());
    CHECK_EQ(d.Video.HwaccelRequested, std::string("d3d11va"));
    if (d.Video.FramePath == "hw_direct" || d.Video.FramePath == "hw_download") {
        CHECK_EQ(d.Video.HwaccelDevice, std::string("d3d11va"));
        CHECK(!d.Video.PixFmtDecoded.empty());
        CHECK(d.Video.HwPixFmt == d.Video.PixFmtDecoded);
    } else {
        // this machine / driver cannot do it: must fall back to software and say why
        CHECK_EQ(d.Video.FramePath, std::string("software"));
        CHECK_EQ(d.Video.HwaccelDevice, std::string("none"));
        CHECK(!d.Video.HwaccelError.empty());
    }
}

TEST_CASE_INTEGRATION(DebugInfo_hwaccel_on_unsupported_codec_falls_back_to_software_with_error) {
    std::wstring clip = Mpeg4Clip();
    REQUIRE_MSG(fs::exists(clip), "could not generate the mpeg4 test clip (ffmpeg/dist/win64/ffmpeg.exe)");
    DbgSession s;
    s.Open(clip, {{L"hwaccel", L"d3d11va"}});
    DebugInfo d = s.WaitInfo([](const DebugInfo& i) { return i.HasVideo && !i.Video.FramePath.empty(); });
    REQUIRE(d.Valid);
    CHECK_EQ(d.Video.Codec, std::string("mpeg4"));
    CHECK_EQ(d.Video.HwaccelRequested, std::string("d3d11va"));
    CHECK_EQ(d.Video.HwaccelDevice, std::string("none"));
    CHECK(!d.Video.HwaccelError.empty());
    CHECK_EQ(d.Video.FramePath, std::string("software"));
    CHECK_EQ(d.Video.FramesHwDirect.value_or(-1), (std::int64_t)0);
    CHECK_EQ(d.Video.FramesHwDownload.value_or(-1), (std::int64_t)0);
    CHECK(d.Video.FramesSoftware.value_or(0) > 0);
}

namespace {

// 30 s 29.97 fps clip (generated once with the build's ffmpeg.exe).
std::wstring Clip2997() {
    fs::path out = DbgRepoRoot() / L"testmedia" / L"testsrc2-2997-360p.mp4";
    if (fs::exists(out)) return out.wstring();
    fs::path dir = DbgRepoRoot() / L"ffmpeg" / L"dist" / L"win64";
    std::wstring cmd = L"\"" + (dir / L"ffmpeg.exe").wstring() +
        L"\" -y -v error -f lavfi -i testsrc2=size=640x360:rate=30000/1001 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 30 "
        L"-c:v libx264 -pix_fmt yuv420p -c:a aac -shortest \"" + out.wstring() + L"\"";
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
    if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 60000);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
    return out.wstring();
}

bool Near(const std::optional<double>& v, double target, double tol) { return v && std::fabs(*v - target) <= tol; }

}

TEST_CASE(DebugInfo_fps_object_absent_in_old_json_is_unknown) {
    DebugInfo d = ParseDebugInfo(R"({"position":1.0,"video":{"codec":"h264"}})");
    CHECK(d.Valid);
    CHECK(!d.Fps.Present);
    CHECK(!d.Fps.Displayed.has_value());
    CHECK(!d.Fps.FramesDisplayed.has_value());
}

TEST_CASE(DebugInfo_fps_object_parses) {
    DebugInfo d = ParseDebugInfo(R"({"fps":{"stream_avg":29.97,"stream_r":29.97,"filter_out":23.976,"filter_out_source":"sink_frame_rate",
        "filter_out_time_base":"1/24000","decoded":29.9,"filtered":23.9,"displayed":23.8,"frames_decoded":300,"frames_filtered":240,
        "frames_displayed":236,"drops_early":1,"drops_late":2}})");
    REQUIRE(d.Fps.Present);
    CHECK(Near(d.Fps.StreamAvg, 29.97, 1e-9));
    CHECK(Near(d.Fps.FilterOut, 23.976, 1e-9));
    CHECK_EQ(d.Fps.FilterOutTimeBase, std::string("1/24000"));
    CHECK(Near(d.Fps.Displayed, 23.8, 1e-9));
    CHECK_EQ(d.Fps.FramesDisplayed.value_or(-1), 236);
    CHECK_EQ(d.Fps.DropsLate.value_or(-1), 2);
    DebugInfo n = ParseDebugInfo(R"({"fps":{"stream_avg":null,"filter_out":null,"displayed":null,"frames_displayed":0}})");
    CHECK(n.Fps.Present && !n.Fps.StreamAvg && !n.Fps.FilterOut && !n.Fps.Displayed);
    CHECK_EQ(n.Fps.FramesDisplayed.value_or(-1), 0);
}

TEST_CASE_INTEGRATION(DebugInfo_fps_displayed_matches_stream_rate_and_frames_grow) {
    DbgSession s;
    s.Open(DbgMedia(L"testsrc2-1080p-60s.mp4"));          // 25 fps
    DebugInfo d = s.WaitInfo([](const DebugInfo& i) {
        return i.Fps.Displayed && i.Fps.FramesDisplayed.value_or(0) > 60;   // ~2.4 s of playback = warm-up
    }, 15);
    REQUIRE(d.Valid);
    REQUIRE(d.Fps.Present);
    std::fprintf(stderr, "[fps json] avg=%.3f r=%.3f out=%.3f dec=%.2f flt=%.2f disp=%.2f shown=%lld\n", d.Fps.StreamAvg.value_or(-1), d.Fps.StreamR.value_or(-1),
                 d.Fps.FilterOut.value_or(-1), d.Fps.Decoded.value_or(-1), d.Fps.Filtered.value_or(-1), d.Fps.Displayed.value_or(-1),
                 (long long)d.Fps.FramesDisplayed.value_or(-1));
    CHECK(Near(d.Fps.StreamAvg, 25.0, 0.01));
    CHECK(Near(d.Fps.StreamR, 25.0, 0.01));
    CHECK(Near(d.Fps.FilterOut, 25.0, 0.01));
    CHECK_EQ(d.Fps.FilterOutSource, std::string("sink_frame_rate"));
    CHECK(!d.Fps.FilterOutTimeBase.empty());
    CHECK(Near(d.Fps.Displayed, 25.0, 3.0));
    CHECK(Near(d.Fps.Filtered, 25.0, 4.0));
    CHECK(Near(d.Fps.Decoded, 25.0, 6.0));
    CHECK(d.Fps.FramesDecoded.value_or(0) >= d.Fps.FramesDisplayed.value_or(0));
    CHECK(d.Fps.FramesFiltered.value_or(0) >= d.Fps.FramesDisplayed.value_or(0));
    const long long shown0 = d.Fps.FramesDisplayed.value_or(0);
    DebugInfo d2 = s.WaitInfo([&](const DebugInfo& i) { return i.Fps.FramesDisplayed.value_or(0) >= shown0 + 25; }, 8);
    CHECK(d2.Fps.FramesDisplayed.value_or(0) >= shown0 + 25);
    CHECK(d2.Fps.DropsEarly.has_value() && d2.Fps.DropsLate.has_value());
}

TEST_CASE_INTEGRATION(DebugInfo_fps_filter_fps10_reports_10_out_and_displayed) {
    DbgSession s;
    s.Open(DbgMedia(L"testsrc2-1080p-60s.mp4"));
    s.WaitInfo([](const DebugInfo& i) { return i.Fps.FramesDisplayed.value_or(0) > 10; }, 10);
    REQUIRE(s.player->SetVideoFilter(L"fps=10") == 0);
    DebugInfo d = s.WaitInfo([](const DebugInfo& i) {
        return Near(i.Fps.FilterOut, 10.0, 0.01) && Near(i.Fps.Displayed, 10.0, 2.0) && Near(i.Fps.Filtered, 10.0, 2.0);
    }, 15);
    std::fprintf(stderr, "[fps json fps=10] out=%.3f tb=%s dec=%.2f flt=%.2f disp=%.2f\n", d.Fps.FilterOut.value_or(-1), d.Fps.FilterOutTimeBase.c_str(),
                 d.Fps.Decoded.value_or(-1), d.Fps.Filtered.value_or(-1), d.Fps.Displayed.value_or(-1));
    CHECK(Near(d.Fps.FilterOut, 10.0, 0.01));
    CHECK(Near(d.Fps.Displayed, 10.0, 2.0));
    CHECK(Near(d.Fps.Filtered, 10.0, 2.0));
    CHECK(Near(d.Fps.StreamAvg, 25.0, 0.01));
    CHECK(Near(d.Fps.Decoded, 25.0, 6.0));
}

TEST_CASE_INTEGRATION(DebugInfo_fps_decimate_changes_filter_out_to_23_976) {
    DbgSession s;
    s.Open(Clip2997());                            // 30000/1001
    DebugInfo d0 = s.WaitInfo([](const DebugInfo& i) {
        return i.Fps.FramesDisplayed.value_or(0) > 60 && i.Fps.Displayed;
    }, 15);
    REQUIRE(d0.Fps.Present);
    CHECK(Near(d0.Fps.StreamAvg, 29.97, 0.01));
    CHECK(Near(d0.Fps.FilterOut, 29.97, 0.01));
    CHECK(Near(d0.Fps.Displayed, 29.97, 3.0));
    REQUIRE(s.player->SetVideoFilter(L"decimate") == 0);
    DebugInfo d = s.WaitInfo([](const DebugInfo& i) {
        return Near(i.Fps.FilterOut, 23.976, 0.01) && Near(i.Fps.Displayed, 23.976, 1.5) && Near(i.Fps.Filtered, 23.976, 1.5) &&
               Near(i.Fps.Decoded, 29.97, 4.0);   // the burst while the graph was rebuilt has left the 1.5 s window
    }, 20);
    std::fprintf(stderr, "[fps json decimate] avg=%.3f out=%.4f src=%s dec=%.2f flt=%.2f disp=%.2f\n", d.Fps.StreamAvg.value_or(-1), d.Fps.FilterOut.value_or(-1),
                 d.Fps.FilterOutSource.c_str(), d.Fps.Decoded.value_or(-1), d.Fps.Filtered.value_or(-1), d.Fps.Displayed.value_or(-1));
    if (!Near(d.Fps.Displayed, 23.976, 2.5))
        std::fprintf(stderr, "[fps decimate diag] pos=%.3f paused=%d master_clock=%.3f vclock=%.3f shown=%lld filtered=%lld decoded=%lld vq=%lld pkts\n", d.Position.value_or(-1), (int)d.Paused,
                     d.Sync.MasterClock.value_or(-1), d.Sync.VideoClock.value_or(-1), (long long)d.Fps.FramesDisplayed.value_or(-1),
                     (long long)d.Fps.FramesFiltered.value_or(-1), (long long)d.Fps.FramesDecoded.value_or(-1), (long long)d.VideoQueue.Packets.value_or(-1));
    CHECK(Near(d.Fps.StreamAvg, 29.97, 0.01));
    CHECK(Near(d.Fps.FilterOut, 23.976, 0.01));
    CHECK(Near(d.Fps.Filtered, 23.976, 2.5));
    CHECK(Near(d.Fps.Displayed, 23.976, 2.5));
    CHECK(Near(d.Fps.Decoded, 29.97, 5.0));
    CHECK(Has(d.Filters.VfGraph, "decimate"));
}

TEST_CASE_INTEGRATION(DebugInfo_fps_null_before_first_frame_and_no_video) {
    DbgSession s;
    s.Open(DbgMedia(L"audio.m4a"));
    DebugInfo d = s.WaitInfo([](const DebugInfo& i) { return i.HasAudio; });
    REQUIRE(d.Valid);
    CHECK(d.Fps.Present);
    CHECK(!d.Fps.Displayed.has_value());
    CHECK(!d.Fps.FilterOut.has_value());
    CHECK(!d.Fps.StreamAvg.has_value());
    CHECK_EQ(d.Fps.FramesDisplayed.value_or(-1), 0);
}

TEST_CASE(DebugInfo_parses_hwframes_keys_and_tolerates_old_dll) {
    DebugInfo d = ParseDebugInfo(R"({"video":{"codec":"h264","hwaccel_requested":"cuda","hwaccel_device":"cuda",
        "hwframes_requested":"keep","hwframes":"download",
        "frame_path_counts":{"hw_direct":0,"hw_download":12,"software":0,"sink_download":12}}})");
    REQUIRE(d.Valid && d.HasVideo);
    CHECK_EQ(d.Video.HwframesRequested, std::string("keep"));
    CHECK_EQ(d.Video.Hwframes, std::string("download"));
    REQUIRE(d.Video.FramesSinkDownload.has_value());
    CHECK_EQ(*d.Video.FramesSinkDownload, 12);
    DebugInfo o = ParseDebugInfo(R"({"video":{"codec":"h264","frame_path_counts":{"hw_direct":1,"hw_download":2,"software":3}}})");
    REQUIRE(o.Valid && o.HasVideo);
    CHECK(o.Video.HwframesRequested.empty() && o.Video.Hwframes.empty());
    CHECK(!o.Video.FramesSinkDownload.has_value());
    CHECK_EQ(*o.Video.FramesSoftware, 3);
    DebugInfo n = ParseDebugInfo(R"({"video":{"hwframes":null,"frame_path_counts":{"sink_download":null}}})");
    CHECK(n.Video.Hwframes.empty() && !n.Video.FramesSinkDownload.has_value());
}

// ARIB audio model (fixture f1b: ffmpeg's own choice would be the commentary that the PMT removes at ~5 s).
TEST_CASE_INTEGRATION(DebugInfo_arib_audio_keeps_playing_after_the_commentary_is_removed) {
    DbgSession s;
    s.Open(DbgMedia(L"arib-audio/f1b-a2-preferred-by-ffmpeg.ts"));
    auto mi = s.player->GetMediaInfo();
    REQUIRE(mi.has_value());
    CHECK(mi->AribAudio.Present);
    CHECK_EQ(mi->AribAudio.Audio1, 1);
    CHECK_EQ(s.player->AudioPreference(), 0);
    DebugInfo d = s.WaitInfo([](const DebugInfo& i) { return i.AribAudio.Present && i.Sync.AudioClock && *i.Sync.AudioClock > 0.5; });
    REQUIRE(d.AribAudio.Present);
    CHECK_EQ(d.AribAudio.Preference, std::string("audio1"));
    CHECK_EQ(d.AribAudio.Audio1.Pid, 274);
    CHECK_EQ(d.AribAudio.Audio1.ComponentTag, 16);
    CHECK_EQ(s.player->SetAudioPreference(1), 0);
    CHECK_EQ(s.player->AudioPreference(), 1);
    CHECK_EQ(s.player->SetAudioPreference(2), -22);
    // the audio clock keeps running through the PMT change at 5 s (it froze when audio 2 was the selected stream)
    d = s.WaitInfo([](const DebugInfo& i) { return i.Sync.AudioClock && *i.Sync.AudioClock > 7.0; }, 20);
    REQUIRE(d.Sync.AudioClock.has_value());
    CHECK(*d.Sync.AudioClock > 7.0);
    CHECK_EQ(d.AribAudio.Preference, std::string("audio2"));
    CHECK_EQ(d.AribAudio.Playing, std::string("audio1"));
    CHECK(d.AribAudio.Audio2.Starved);
    CHECK(d.AribAudio.SilenceMs.value_or(1e9) < 1.0);
}

TEST_CASE_INTEGRATION(DebugInfo_arib_audio_absent_for_plain_media) {
    DbgSession s;
    s.Open(DbgMedia(L"testsrc2-360p-short.mp4"));
    auto mi = s.player->GetMediaInfo();
    REQUIRE(mi.has_value());
    CHECK(!mi->AribAudio.Present);
    DebugInfo d = s.WaitInfo([](const DebugInfo& i) { return i.HasAudio; });
    CHECK(!d.AribAudio.Present);
}
