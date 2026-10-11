// ffp_get_debug_info / ffp_set_log_callback: headless libffplay integration tests
// (recording presenter, SDL_AUDIODRIVER=dummy set in main).
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
#include "wvp/win32/Utf.h"
#include "TestPlayer.h"
#include "wvptest.h"

using namespace wvp;
namespace fs = std::filesystem;

namespace {

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

std::string DbgMedia(const char* name) { return ToUtf8((DbgRepoRoot() / L"testmedia" / fs::path(ToWide(name))).wstring()); }

template <class F>
bool DbgWaitFor(F cond, double seconds = 10) { return wvptest::WaitFor(cond, seconds); }

struct DbgSession {
    std::unique_ptr<FfPlayer> player;
    int prepared = 0;
    int errors = 0;

    DbgSession() {
        int err = 0;
        // a hwaccel open may create a default device (the recording presenter brings none)
        player = wvptest::CreateTestPlayer(nullptr, &err, false);
        REQUIRE_MSG(player != nullptr, "FfPlayer::Create failed: " + std::to_string(err));
        player->Prepared.add([this](const MediaPreparedInfo&) { prepared++; });
        player->Error.add([this](const PlayerErrorInfo&) { errors++; });
    }
    ~DbgSession() { player.reset(); }

    void Open(const std::string& file, const OpenOptions& opts = {}) {
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

}

TEST_CASE_INTEGRATION(DebugInfo_null_when_nothing_open) {
    DbgSession s;
    CHECK(!s.player->GetDebugInfoJson().has_value());
}

TEST_CASE_INTEGRATION(DebugInfo_playing_clip_reports_decoder_graphs_and_sync) {
    DbgSession s;
    s.Open(DbgMedia("testsrc2-360p-short.mp4"));
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
    s.Open(DbgMedia("testsrc2-1080p-60s.mp4"));
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
    s.Open(DbgMedia("testsrc2-720p.mp4"));
    DebugInfo d0 = s.WaitInfo([](const DebugInfo& i) { return !i.Filters.VfGraph.empty(); });
    REQUIRE(d0.Valid);
    CHECK(!Has(d0.Filters.VfGraph, "hflip"));
    REQUIRE(s.player->SetVideoFilter("hflip") == 0);
    DebugInfo d1 = s.WaitInfo([](const DebugInfo& i) { return Has(i.Filters.VfGraph, "hflip"); }, 8);
    CHECK(Has(d1.Filters.VfGraph, "hflip"));
    CHECK_EQ(d1.Filters.VfRequested, std::string("hflip"));
    bool listed = false;
    for (auto& f : d1.Filters.VfFilters) if (f == "hflip") listed = true;
    CHECK(listed);
}

TEST_CASE_INTEGRATION(DebugInfo_audio_filter_change_updates_af_graph) {
    DbgSession s;
    s.Open(DbgMedia("testsrc2-720p.mp4"));
    s.WaitInfo([](const DebugInfo& i) { return !i.Filters.AfGraph.empty(); });
    REQUIRE(s.player->SetAudioFilter("atempo=1.25") == 0);
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
        s.Open(DbgMedia("testsrc2-360p-short.mp4"));
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
        s.Open(DbgMedia("testsrc2-720p.mp4"), {{"stats", "1"}});
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
        s.Open(DbgMedia("testsrc2-360p-short.mp4"));
        DbgWaitFor([] { return false; }, 0.5);
    }
    CHECK_EQ(lc.Count(), before);
}

namespace {

// Generates a short MPEG-4 part 2 + AAC clip (a codec without any D3D11VA hwaccel) with the build's ffmpeg.exe.
std::string Mpeg4Clip() {
    fs::path out = DbgRepoRoot() / L"testmedia" / L"mpeg4-240p-short.mp4";
    if (fs::exists(out)) return ToUtf8(out.wstring());
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
    return ToUtf8(out.wstring());
}

}

TEST_CASE_INTEGRATION(DebugInfo_software_open_reports_presenter_and_software_path) {
    DbgSession s;
    s.Open(DbgMedia("testsrc2-720p.mp4"));
    // The graph is configured after the first decoded frame; wait for both because frame-path readiness can race the graph walk.
    DebugInfo d = s.WaitInfo([](const DebugInfo& i) {
        return i.HasVideo && !i.Video.FramePath.empty() && !i.Filters.VfNodes.empty() && i.Video.OutWidth.value_or(0) > 0;
    });
    REQUIRE(d.Valid);
    CHECK(d.Presenter.Present);
    CHECK_EQ(d.Presenter.Name, std::string("recording"));
    CHECK(d.Presenter.Error.empty());
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
    s.Open(DbgMedia("testsrc2-720p.mp4"), {{"hwaccel", "d3d11va"}});
    DebugInfo d = s.WaitInfo([](const DebugInfo& i) { return i.HasVideo && !i.Video.FramePath.empty(); });
    REQUIRE(d.Valid);
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
    std::string clip = Mpeg4Clip();
    REQUIRE_MSG(fs::exists(clip), "could not generate the mpeg4 test clip (ffmpeg/dist/win64/ffmpeg.exe)");
    DbgSession s;
    s.Open(clip, {{"hwaccel", "d3d11va"}});
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
std::string Clip2997() {
    fs::path out = DbgRepoRoot() / L"testmedia" / L"testsrc2-2997-360p.mp4";
    if (fs::exists(out)) return ToUtf8(out.wstring());
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
    return ToUtf8(out.wstring());
}

bool Near(const std::optional<double>& v, double target, double tol) { return v && std::fabs(*v - target) <= tol; }

}

TEST_CASE_INTEGRATION(DebugInfo_fps_displayed_matches_stream_rate_and_frames_grow) {
    DbgSession s;
    s.Open(DbgMedia("testsrc2-1080p-60s.mp4"));          // 25 fps
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
    s.Open(DbgMedia("testsrc2-1080p-60s.mp4"));
    s.WaitInfo([](const DebugInfo& i) { return i.Fps.FramesDisplayed.value_or(0) > 10; }, 10);
    REQUIRE(s.player->SetVideoFilter("fps=10") == 0);
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
    REQUIRE(s.player->SetVideoFilter("decimate") == 0);
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
    s.Open(DbgMedia("audio.m4a"));
    DebugInfo d = s.WaitInfo([](const DebugInfo& i) { return i.HasAudio; });
    REQUIRE(d.Valid);
    CHECK(d.Fps.Present);
    CHECK(!d.Fps.Displayed.has_value());
    CHECK(!d.Fps.FilterOut.has_value());
    CHECK(!d.Fps.StreamAvg.has_value());
    CHECK_EQ(d.Fps.FramesDisplayed.value_or(-1), 0);
}

// ARIB audio model (fixture f1b: ffmpeg's own choice would be the commentary that the PMT removes at ~5 s).
TEST_CASE_INTEGRATION(DebugInfo_arib_audio_keeps_playing_after_the_commentary_is_removed) {
    DbgSession s;
    s.Open(DbgMedia("arib-audio/f1b-a2-preferred-by-ffmpeg.ts"));
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
    // the audio clock keeps running through the PMT change at 5 s
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
    s.Open(DbgMedia("testsrc2-360p-short.mp4"));
    auto mi = s.player->GetMediaInfo();
    REQUIRE(mi.has_value());
    CHECK(!mi->AribAudio.Present);
    DebugInfo d = s.WaitInfo([](const DebugInfo& i) { return i.HasAudio; });
    CHECK(!d.AribAudio.Present);
}
