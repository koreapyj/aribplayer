// Pure: no window, no Direct2D, no clipboard; log files and settings live in temp dirs only.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "Commands.h"
#include "LogicTest.h"
#include "Services.h"
#include "features/DebugOverlayModel.h"
#include "features/DisplayInfo.h"
#include "features/Logging.h"
#include "features/LoggingFormat.h"
#include "features/SettingsService.h"
#include "wvp/CapabilitySet.h"
#include "wvp/DebugInfo.h"
#include "wvp/FilterManager.h"
#include "wvp/PlaybackController.h"

namespace {
using namespace app;
using namespace app::dbg;

struct TempDir {
    std::filesystem::path p;
    TempDir() {
        wchar_t buf[MAX_PATH]{};
        GetTempPathW(MAX_PATH, buf);
        static int n = 0;
        p = std::filesystem::path(buf) / (L"aribplayer-debug-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(n++));
        std::filesystem::create_directories(p);
    }
    ~TempDir() { std::error_code ec; std::filesystem::remove_all(p, ec); }
};

std::string ReadFile(const std::filesystem::path& f) {
    std::ifstream in(f, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool Has(const std::vector<OverlayLine>& v, const std::string& needle, LineKind* kind = nullptr) {
    for (const auto& l : v)
        if (l.text.find(needle) != std::string::npos) { if (kind) *kind = l.kind; return true; }
    return false;
}
int CountKind(const std::vector<OverlayLine>& v, LineKind k) {
    int n = 0;
    for (const auto& l : v) n += l.kind == k;
    return n;
}

const char* kFullJson = R"JSON({
 "position": 12.34, "duration": 60.0, "paused": false, "speed": 1.5, "presenter": {"name": "d3d11", "stats": {"renderer": "d3d11:videoprocessor", "route": "vp"}},
 "video": {"stream_index": 0, "codec": "h264", "decoder": "h264", "hwaccel": "d3d11va", "pix_fmt": "d3d11", "hw_pix_fmt": "d3d11", "sw_pix_fmt": "nv12",
   "width": 1920, "height": 1080, "sar": "1:1", "frame_rate": 29.97, "out_width": 1920, "out_height": 1080, "out_pix_fmt": "yuv420p",
   "time_base": "1/90000", "start_time": 0.0, "hwaccel_requested": "d3d11va", "hwaccel_device": "d3d11va", "hwaccel_error": "",
   "frame_path": "hw_download", "frame_path_counts": {"hw_direct": 0, "hw_download": 370, "software": 0},
   "pix_fmt_decoded": "d3d11", "pix_fmt_after_transfer": "nv12"},
 "audio": {"stream_index": 1, "codec": "aac", "decoder": "aac", "sample_rate": 48000, "channels": 2, "channel_layout": "stereo", "sample_fmt": "fltp",
   "time_base": "1/48000", "start_time": 0.0, "out_device": "Speakers (dummy)", "out_sample_rate": 48000, "out_channels": 2, "hw_buf_size": 8192,
   "diff_avg": 0.001, "latency_ms": 42.0},
 "subtitle": null,
 "filters": {"vf_requested": "hwupload,ivtc_d3d11,hwdownload,format=nv12", "af_requested": "atempo=1.5",
   "vf_graph": "buffer=video_size=1920x1080,hwupload,ivtc_d3d11,hwdownload,format=nv12,scale=w=1920:h=1080,format=pix_fmts=yuv420p,buffersink",
   "af_graph": "abuffer=sample_rate=48000,atempo=1.5,aformat=sample_fmts=fltp,abuffersink",
   "vf_filters": ["buffer","hwupload","ivtc_d3d11","hwdownload","format","scale","format","buffersink"],
   "af_filters": ["abuffer","atempo","aformat","abuffersink"],
   "vf_nodes": [{"name":"buffer","instance":"Parsed_buffer_0","args":"","hw_device":""},{"name":"hwupload","instance":"Parsed_hwupload_1","args":"","hw_device":"d3d11va"},
                {"name":"ivtc_d3d11","instance":"Parsed_ivtc_d3d11_2","args":"","hw_device":"d3d11va"},{"name":"hwdownload","instance":"Parsed_hwdownload_3","args":"","hw_device":""}],
   "af_nodes": []},
 "sync": {"master": "audio", "audio_clock": 12.30, "video_clock": 12.31, "external_clock": 12.31, "master_clock": 12.30, "av_diff_ms": -8.5,
   "frame_drops_early": 0, "frame_drops_late": 2, "frames_duplicated": 0, "frame_timer_drift_ms": 0.4},
 "queues": {"audio": {"packets": 12, "bytes": 34567, "duration": 0.48}, "video": {"packets": 25, "bytes": 912345, "duration": 1.0}, "subtitle": {"packets": 0, "bytes": 0, "duration": 0.0}}
})JSON";

const char* kFallbackJson = R"JSON({
 "position": 1.0, "duration": 10.0, "paused": false, "speed": 2.0, "presenter": {"name": "metal"},
 "video": {"stream_index": 0, "codec": "mpeg4", "decoder": "mpeg4", "hwaccel": "none", "width": 426, "height": 240, "sar": "1:1", "frame_rate": 25.0,
   "out_width": 426, "out_height": 240, "out_pix_fmt": "yuv420p", "time_base": "1/25", "start_time": 0.0,
   "hwaccel_requested": "d3d11va", "hwaccel_device": "none", "hwaccel_error": "decoder mpeg4 does not support d3d11va",
   "frame_path": "software", "frame_path_counts": {"hw_direct": 0, "hw_download": 0, "software": 120}, "pix_fmt_decoded": "yuv420p", "pix_fmt_after_transfer": ""},
 "audio": {"stream_index": 1, "codec": "aac", "decoder": "aac", "sample_rate": 44100, "channels": 2, "channel_layout": "stereo", "sample_fmt": "fltp"},
 "filters": {"vf_requested": "yadif=mode=1", "af_requested": "atempo=2", "vf_graph": "buffer=video_size=426x240,format=pix_fmts=yuv420p,buffersink",
   "af_graph": "abuffer=sample_rate=44100,aformat=sample_fmts=fltp,abuffersink", "vf_filters": ["buffer","format","buffersink"], "af_filters": ["abuffer","aformat","abuffersink"]},
 "sync": {"master": "audio", "av_diff_ms": 3.0}, "queues": {}
})JSON";

const char* kOldJson = R"JSON({"position": 1.5, "duration": 10, "video": {"codec": "h264", "width": 640, "height": 360}})JSON";

OverlayContext Ctx(const char* json) {
    OverlayContext c;
    c.haveJson = true;
    c.info = wvp::ParseDebugInfo(json);
    c.mode = "now playing";
    return c;
}
}

LOGIC_TEST(debug_overlay_full_sample_lines) {
    OverlayContext c = Ctx(kFullJson);
    c.requestedSpeed = 1.5;
    c.deinterlaceRequested = "ivtc";
    c.url = "C:\\media\\clip.mp4";
    c.openOptions = "hwaccel=d3d11va vf=hwupload,ivtc_d3d11,hwdownload,format=nv12 filter_hw_device=d3d11va";
    auto L = BuildOverlayLines(c);
    LT_CHECK(L.size() > 15);
    LT_CHECK(Has(L, "output    d3d11:videoprocessor"));
    LT_CHECK(Has(L, "frame_path hw_download  (hw_direct 0 | hw_download 370 | software 0)"));
    LT_CHECK(Has(L, "codec h264  decoder h264  |  1920x1080 -> 1920x1080  SAR 1:1  29.97 fps"));
    LT_CHECK(Has(L, "requested d3d11va -> actual d3d11va"));
    LT_CHECK(Has(L, "decoded d3d11 | after transfer nv12 | out yuv420p"));
    LT_CHECK(Has(L, "48000 Hz  2 ch  stereo  fltp"));
    LT_CHECK(Has(L, "device \"Speakers (dummy)\""));
    LT_CHECK(Has(L, "hw_buf 8192"));
    LT_CHECK(Has(L, "vf req"));
    LT_CHECK(Has(L, "auto-inserted: scale, format"));
    LT_CHECK(Has(L, "buffer > hwupload[d3d11va] > ivtc_d3d11[d3d11va] > hwdownload"));
    LT_CHECK(Has(L, "IVTC: ivtc_d3d11 (d3d11va), residual-combing fallback unavailable"));
    LT_CHECK(Has(L, "GPU decode -> CPU download -> d3d11 upload + ivtc_d3d11 -> CPU download -> d3d11:videoprocessor"));
    LT_CHECK(Has(L, "requested 1.50 | atempo in graph 1.50"));
    LT_CHECK(Has(L, "master audio | clocks a 12.300 v 12.310 ext 12.310 master 12.300 | A-V -8.5 ms"));
    LT_CHECK(Has(L, "drops early 0 late 2 | duplicated 0"));
    LT_CHECK(Has(L, "queues    a 12 pkt 33.8KB 0.48s | v 25 pkt 891.0KB 1.00s | s 0 pkt 0B 0.00s"));
    LT_CHECK(Has(L, "v#0 start 0.000 tb 1/90000 | a#1 start 0.000 tb 1/48000"));
    LT_CHECK(Has(L, "12.34 / 60.00 s  paused no"));
    LT_CHECK(Has(L, "options   hwaccel=d3d11va"));
    LT_CHECK_EQ(CountKind(L, LineKind::Warn), 0);
    LT_CHECK(L[0].kind == LineKind::Header);
}

LOGIC_TEST(debug_overlay_includes_resume_decision) {
    OverlayContext c;
    c.resumeDecision = "resume at 42 seconds";
    LT_CHECK(Has(BuildOverlayLines(c), "resume    resume at 42 seconds"));
}

LOGIC_TEST(debug_overlay_reports_acrylic_and_solid_fallback_reason) {
    OverlayContext c = Ctx(kFullJson);
    c.info.Presenter.Present = true;
    c.info.Presenter.Name = "d3d11";
    c.info.Presenter.Acrylic.Present = true;
    c.info.Presenter.Acrylic.Regions = 2;
    c.info.Presenter.Acrylic.BlurMs = 0.37;
    auto active = BuildOverlayLines(c);
    LT_CHECK(Has(active, "acrylic: 2 regions, 0.4 ms"));
    c.acrylicFallbackReason = "battery saver";
    LineKind kind{};
    LT_CHECK(Has(BuildOverlayLines(c), "acrylic: solid fallback (battery saver)", &kind));
    LT_CHECK(kind == LineKind::Warn);
}

LOGIC_TEST(debug_overlay_hw_fallback_mismatch_is_highlighted) {
    OverlayContext c = Ctx(kFallbackJson);
    c.requestedSpeed = 2.0;
    c.deinterlaceRequested = "ivtc";
    auto L = BuildOverlayLines(c);
    LineKind k{};
    LT_CHECK(Has(L, "requested d3d11va -> actual none  MISMATCH  error: decoder mpeg4 does not support d3d11va", &k));
    LT_CHECK(k == LineKind::Warn);
    LT_CHECK(Has(L, "vf graph", &k) && k == LineKind::Warn);
    LT_CHECK(Has(L, "MISMATCH (missing: yadif)"));
    LT_CHECK(Has(L, "af graph", &k) && k == LineKind::Warn);
    LT_CHECK(Has(L, "MISMATCH (missing: atempo)"));
    LT_CHECK(Has(L, "requested 2.00 | atempo in graph none  MISMATCH", &k) && k == LineKind::Warn);
    LT_CHECK(Has(L, "ivtc requested, NOT in the actual graph", &k) && k == LineKind::Warn);
    LT_CHECK(Has(L, "frame_path software", &k) && k == LineKind::Normal);
    LT_CHECK(Has(L, "output    metal"));
    LT_CHECK_EQ(CountKind(L, LineKind::Warn), 5);
}

LOGIC_TEST(debug_overlay_path_change_midstream_is_warned) {
    std::string j = kFullJson;
    size_t at = j.find("\"hw_direct\": 0");
    j.replace(at, 14, "\"hw_direct\": 5");
    OverlayContext c = Ctx(j.c_str());
    c.requestedSpeed = 1.5;
    auto L = BuildOverlayLines(c);
    LineKind k{};
    LT_CHECK(Has(L, "PATH CHANGED", &k) && k == LineKind::Warn);
}

LOGIC_TEST(debug_overlay_old_dll_missing_keys) {
    OverlayContext c = Ctx(kOldJson);
    auto L = BuildOverlayLines(c);
    LT_CHECK(Has(L, "output    ?"));
    LT_CHECK(Has(L, "codec h264  decoder ?  |  640x360 -> ?  SAR ?  ? fps"));
    LT_CHECK(Has(L, "requested - -> actual ?"));
    LT_CHECK(Has(L, "A-V ? ms"));
    LT_CHECK(Has(L, "position  1.50 / 10.00 s"));
    LT_CHECK_EQ(CountKind(L, LineKind::Warn), 0);
    LT_CHECK(!Has(L, "MISMATCH"));
    OverlayContext none;
    none.mode = "now playing";
    auto N = BuildOverlayLines(none);
    LT_CHECK(Has(N, "no debug info"));
    OverlayContext bad = Ctx("not json");
    LT_CHECK(Has(BuildOverlayLines(bad), "no debug info"));
}

LOGIC_TEST(debug_overlay_graph_helpers) {
    auto n = GraphFilterNames("[in]hwupload,ivtc_d3d11=a=1:b='x,y',eq@color=brightness=0.1;scale=w=2:h=2[out]");
    LT_CHECK_EQ(n.size(), (size_t)4);
    LT_CHECK_EQ(n[0], std::string("hwupload"));
    LT_CHECK_EQ(n[1], std::string("ivtc_d3d11"));
    LT_CHECK_EQ(n[2], std::string("eq"));
    LT_CHECK_EQ(n[3], std::string("scale"));
    GraphCompare c = CompareGraph("yadif,eq@color=brightness=0.1", {"buffer", "yadif", "scale", "eq", "format", "buffersink"});
    LT_CHECK(c.known && !c.mismatch);
    LT_CHECK_EQ(c.autoInserted.size(), (size_t)2);
    c = CompareGraph("eq,yadif", {"buffer", "yadif", "eq", "buffersink"});   // wrong order counts as missing
    LT_CHECK(c.mismatch);
    LT_CHECK(!CompareGraph("yadif", {}).known);
    bool any = false;
    LT_CHECK_EQ(AtempoProduct("abuffer,atempo=0.5,atempo=0.5,aformat", &any), 0.25);
    LT_CHECK(any);
    LT_CHECK_EQ(AtempoProduct("abuffer=x,atempo=tempo=1.5,abuffersink"), 1.5);
    LT_CHECK_EQ(AtempoProduct("abuffer,aformat", &any), 1.0);
    LT_CHECK(!any);
}

LOGIC_TEST(debug_overlay_ivtc_deint_implementation_line) {
    OverlayContext c;
    c.haveJson = true;
    c.info.Valid = true;
    c.info.Filters.VfFilters = {"buffer", "fieldmatch", "decimate", "buffersink"};
    bool warn = true;
    LT_CHECK_EQ(DescribeIvtcDeint(c, &warn), std::string("IVTC: fieldmatch+decimate (CPU)"));
    LT_CHECK(!warn);
    c.info.Filters.VfFilters = {"buffer", "fieldmatch", "bwdif", "decimate", "buffersink"};
    LT_CHECK_EQ(DescribeIvtcDeint(c, &warn), std::string("IVTC: fieldmatch+bwdif(interlaced)+decimate (CPU)"));
    c.info.Filters.VfFilters = {"buffer", "ivtc_d3d11", "bwdif_d3d11", "buffersink"};
    LT_CHECK_EQ(DescribeIvtcDeint(c, &warn), std::string("IVTC: ivtc_d3d11 (d3d11) + bwdif_d3d11(interlaced)"));
    LT_CHECK(!warn);
    c.info.Filters.VfFilters = {"buffer", "bwdif", "buffersink"};
    LT_CHECK_EQ(DescribeIvtcDeint(c, &warn), std::string("bwdif (CPU)"));
    c.info.Filters.VfFilters = {"buffer", "hwupload", "ivtc_d3d11", "hwdownload", "buffersink"};
    LT_CHECK_EQ(DescribeIvtcDeint(c, &warn), std::string("IVTC: ivtc_d3d11 (d3d11), residual-combing fallback unavailable"));
    c.info.Filters.VfFilters = {"buffer", "buffersink"};
    c.deinterlaceRequested = "deinterlace";
    c.info.Filters.VfFilters = {"buffer", "buffersink"};
    LT_CHECK(DescribeIvtcDeint(c, &warn).find("NOT in the actual graph") != std::string::npos);
    LT_CHECK(warn);
    c.info.Filters.VfFilters.clear();
    LT_CHECK(DescribeIvtcDeint(c, &warn).find("graph unknown") != std::string::npos);
    LT_CHECK(!warn);
    c.deinterlaceRequested.clear();
    LT_CHECK_EQ(DescribeIvtcDeint(c), std::string("none"));
}

LOGIC_TEST(debug_overlay_deint_request_matches_any_hw_variant) {
    OverlayContext c;
    c.haveJson = true;
    c.info.Valid = true;
    bool warn = true;
    auto graph = [&](std::vector<std::string> mid) {
        mid.insert(mid.begin(), "buffer");
        mid.push_back("buffersink");
        c.info.Filters.VfFilters = mid;
        return DescribeIvtcDeint(c, &warn);
    };

    c.deinterlaceRequested = "deinterlace";
    LT_CHECK_EQ(graph({"bwdif_d3d11"}), std::string("bwdif_d3d11 (d3d11)"));
    LT_CHECK(!warn);
    LT_CHECK_EQ(graph({"hwupload", "bwdif_d3d11", "hwdownload"}), std::string("bwdif_d3d11 (d3d11)"));
    LT_CHECK(!warn);
    LT_CHECK_EQ(graph({"bwdif"}), std::string("bwdif (CPU)"));
    LT_CHECK(!warn);
    c.info.Filters.VfNodes = {{"bwdif_vulkan", "", "", "vulkan"}};
    LT_CHECK_EQ(graph({"bwdif_vulkan"}), std::string("bwdif_vulkan (vulkan)"));
    LT_CHECK(!warn);
    c.info.Filters.VfNodes.clear();

    c.deinterlaceRequested = "ivtc";
    LT_CHECK_EQ(graph({"ivtc_d3d11"}), std::string("IVTC: ivtc_d3d11 (d3d11), residual-combing fallback unavailable"));
    LT_CHECK(!warn);
    LT_CHECK_EQ(graph({"ivtc_d3d11", "bwdif_d3d11"}), std::string("IVTC: ivtc_d3d11 (d3d11) + bwdif_d3d11(interlaced)"));
    LT_CHECK(!warn);
    LT_CHECK(graph({"hwupload", "ivtc_d3d11", "hwdownload"}).rfind("IVTC: ivtc_d3d11 (d3d11)", 0) == 0);
    LT_CHECK(!warn);
    LT_CHECK_EQ(graph({"fieldmatch", "bwdif", "decimate"}), std::string("IVTC: fieldmatch+bwdif(interlaced)+decimate (CPU)"));
    LT_CHECK(!warn);

    LT_CHECK_EQ(graph({"scale"}), std::string("ivtc requested, NOT in the actual graph"));
    LT_CHECK(warn);
    LT_CHECK_EQ(graph({"bwdif_d3d11"}), std::string("bwdif_d3d11 (d3d11); ivtc requested, NOT in the actual graph"));
    LT_CHECK(warn);
    c.deinterlaceRequested = "deinterlace";
    LT_CHECK_EQ(graph({"yadif_d3d11"}), std::string("yadif_d3d11 (d3d11)"));   // On is satisfied by any bwdif/yadif variant
    LT_CHECK(!warn);
    LT_CHECK_EQ(graph({"scale"}), std::string("deinterlace requested, NOT in the actual graph"));
    LT_CHECK(warn);
    LT_CHECK_EQ(graph({"deinterlace_d3d11"}), std::string("deinterlace_d3d11 (d3d11); deinterlace requested, NOT in the actual graph"));
    LT_CHECK(warn);

    c.deinterlaceRequested = "deinterlace";
    wvp::Plan plan;
    wvp::PlanStep step;
    step.name = "bwdif";
    step.reason = "priority quality: no GPU deinterlacer -> bwdif";
    plan.steps.push_back(step);
    c.plan = plan;
    LT_CHECK_EQ(graph({"bwdif"}), std::string("bwdif (CPU); priority quality: no GPU deinterlacer -> bwdif"));
    LT_CHECK(!warn);
}

LOGIC_TEST(debug_overlay_hw_round_trip_path_line_is_generic) {
    OverlayContext c = Ctx(kFullJson);
    c.info.Filters.VfFilters = {"buffer", "hwupload", "bwdif_d3d11", "hwdownload", "buffersink"};
    c.info.Filters.VfNodes.clear();
    LT_CHECK(Has(BuildOverlayLines(c), "upload + bwdif_d3d11 -> CPU download -> "));
    c.info.Filters.VfFilters = {"buffer", "hwupload", "ivtc_d3d11", "buffersink"};   // the presenter takes d3d11: no round trip
    LT_CHECK(!Has(BuildOverlayLines(c), "upload + ivtc_d3d11"));
}

LOGIC_TEST(debug_overlay_wrap_and_truncate) {
    std::vector<OverlayLine> in = {{LineKind::Normal, "short"},
                                   {LineKind::Warn, "options   hwaccel=d3d11va -vf \"yadif=mode=1,eq=brightness=0.1\" -af \"atempo=1.5\" \"C:\\media\\some long folder\\clip.mp4\""},
                                   {LineKind::Dim, std::string(500, 'x')}};
    auto out = WrapLines(in, 40);
    LT_CHECK_EQ(out[0].text, std::string("short"));
    int rowsOfSecond = 0, rowsOfThird = 0;
    for (size_t i = 1; i < out.size(); i++) {
        LT_CHECK(Utf8Len(out[i].text) <= 40);
        if (out[i].kind == LineKind::Warn) { rowsOfSecond++; if (rowsOfSecond > 1) LT_CHECK(out[i].text.rfind("  ", 0) == 0); }
        if (out[i].kind == LineKind::Dim) rowsOfThird++;
    }
    LT_CHECK(rowsOfSecond >= 2 && rowsOfSecond <= 4);
    LT_CHECK_EQ(rowsOfThird, 4);
    LT_CHECK(out.back().text.size() >= 3 && out.back().text.substr(out.back().text.size() - 3) == "...");
    // code point counting: a 40 column line of 2-byte characters is not wrapped
    std::string accents;
    for (int i = 0; i < 40; i++) accents += "\xC3\xA9";
    LT_CHECK_EQ(Utf8Len(accents), 40);
    LT_CHECK_EQ(WrapLines({{LineKind::Normal, accents}}, 40).size(), (size_t)1);
    LT_CHECK_EQ(WrapLines({{LineKind::Normal, accents + "z"}}, 40).size(), (size_t)2);
    for (int cols : {1, 8, 13}) for (auto& r : WrapLines(in, cols)) LT_CHECK(!r.text.empty());
    auto clipped = ClipRows(out, 3);
    LT_CHECK_EQ(clipped.size(), (size_t)3);
    LT_CHECK(clipped.back().text.find("more rows") != std::string::npos);
    LT_CHECK_EQ(ClipRows(out, 1000).size(), out.size());
}

LOGIC_TEST(debug_overlay_geometry) {
    for (double scale : {1.0, 1.25, 1.5, 2.0}) {
        for (ShellMode m : {ShellMode::NowPlaying, ShellMode::FullScreen, ShellMode::MiniPlayer}) {
            int cw = m == ShellMode::MiniPlayer ? (int)(480 * scale) : (int)(1280 * scale), ch = m == ShellMode::MiniPlayer ? (int)(270 * scale) : (int)(720 * scale);
            float charW = (float)(7.2 * scale), lineH = (float)(16 * scale);
            int bar = (int)(117 * scale);
            OverlayGeometry g = ComputeOverlayGeometry(cw, ch, scale, m, charW, lineH, bar);
            if (m == ShellMode::MiniPlayer) { LT_CHECK(!g.visible || g.cols < kMaxCols); }
            if (!g.visible) continue;
            LT_CHECK(g.x >= (float)(int)std::lround(8 * scale) - 0.01f);
            float top = (m == ShellMode::NowPlaying ? (float)std::lround(kStripDip * scale) : 0.f) + (float)std::lround(8 * scale);
            LT_CHECK_EQ(g.y, top);
            LT_CHECK(g.x + g.w <= (float)cw);
            LT_CHECK(g.y + g.h <= (float)(ch - bar));
            LT_CHECK(g.cols >= 8 && g.cols <= kMaxCols && g.rows >= 2);
            LT_CHECK(g.w >= g.cols * charW);
        }
    }
    LT_CHECK(!ComputeOverlayGeometry(100, 100, 1, ShellMode::NowPlaying, 7, 16, 117).visible);
    OverlayGeometry big = ComputeOverlayGeometry(3840, 2160, 1, ShellMode::FullScreen, 7, 16, 117);
    LT_CHECK_EQ(big.cols, kMaxCols);
}

LOGIC_TEST(debug_overlay_tab_routing) {
    LT_CHECK(DecideTab(false, false) == TabAction::ToggleOverlay);
    LT_CHECK(DecideTab(true, false) == TabAction::FocusTraversal);
    LT_CHECK(DecideTab(false, true) == TabAction::Ignore);
    LT_CHECK(LookupKey(0x09, 0) == CommandId::ToggleDebugOverlay);
    LT_CHECK_EQ(ShortcutText(CommandId::ToggleDebugOverlay), std::wstring(L"Tab"));
    LT_CHECK(LookupKey(0x09, kModShift) == CommandId::None);
}

LOGIC_TEST(log_quote_arg) {
    using log::QuoteArg;
    LT_CHECK_EQ(QuoteArg("d3d11va"), std::string("d3d11va"));
    LT_CHECK_EQ(QuoteArg("d3d11va", true), std::string("\"d3d11va\""));
    LT_CHECK_EQ(QuoteArg("C:\\a b\\c.mp4"), std::string("\"C:\\a b\\c.mp4\""));
    LT_CHECK_EQ(QuoteArg("C:\\dir with space\\"), std::string("\"C:\\dir with space\\\\\""));
    LT_CHECK_EQ(QuoteArg("say \"hi\""), std::string("\"say \\\"hi\\\"\""));
    LT_CHECK_EQ(QuoteArg("a\\\"b"), std::string("\"a\\\\\\\"b\""));
    LT_CHECK_EQ(QuoteArg(""), std::string("\"\""));
    LT_CHECK_EQ(QuoteArg("a,b"), std::string("\"a,b\""));
}

LOGIC_TEST(log_options_line) {
    wvp::OpenOptions o = {{"hwaccel", "d3d11va"}, {"vf", "a b"}};
    LT_CHECK_EQ(log::FormatOptionsLine(o), std::string("hwaccel=d3d11va vf=\"a b\""));
    LT_CHECK_EQ(log::FormatOptionsLine({}), std::string("(none)"));
}

namespace {
class OpenRecorder : public wvp::IMediaPlayer {
public:
    std::vector<std::pair<std::string, wvp::OpenOptions>> opened;
    wvp::PlayerState State() const override { return wvp::PlayerState::Idle; }
    int Volume() const override { return 100; }
    void SetVolume(int) override {}
    bool IsMuted() const override { return false; }
    void SetMuted(bool) override {}
    double Position() override { return 0; }
    double Duration() override { return 0; }
    int Open(const std::string& url, const wvp::OpenOptions& o) override { opened.emplace_back(url, o); return 0; }
    int Stop() override { return 0; }
    int Play() override { return 0; }
    int Pause() override { return 0; }
    int TogglePause() override { return 0; }
    int Seek(double, bool) override { return 0; }
    int CycleStream(wvp::StreamType) override { return 0; }
    int SetVideoFilter(const std::string&) override { return 0; }
    int SetAudioFilter(const std::string&) override { return 0; }
    int SendFilterCommand(const std::string&, const std::string&, const std::string&) override { return 0; }
    std::optional<wvp::MediaInfo> GetMediaInfo() override { return std::nullopt; }
    std::optional<wvp::BgraFrame> Snapshot() override { return std::nullopt; }
};
}

LOGIC_TEST(log_open_hook_hardware_decoding_and_vf) {
    OpenRecorder rec;
    wvp::PlaybackController ctl(rec);
    wvp::FilterManager fm;
    wvp::PlanPrefs pp = wvp::PlanPrefsFor("d3d11va");
    (void)fm.SetPlanPreferences(pp.decoder, pp.forcedDecoder);
    (void)fm.SetCapabilities(wvp::fakecaps::Intel());
    wvp::FilterSettings fs;
    fs.deinterlace = wvp::DeinterlaceMode::On;
    fs.speed = 1.5;
    (void)fm.Apply(fs);
    ctl.SetFilters(fm.Provider());
    std::string seenUrl, seenOpts;
    wvp::OpenOptions seen;
    ctl.onOpening = [&](const std::string& url, const wvp::OpenOptions& o) {
        seenUrl = url; seen = o;
        seenOpts = log::FormatOptionsLine(o);
    };
    ctl.SetQueue({wvp::MediaItem("C:\\clips\\a b.mp4")}, 0, true);
    LT_CHECK_EQ(rec.opened.size(), (size_t)1);
    LT_CHECK(seen == rec.opened[0].second);
    LT_CHECK_EQ(seenUrl, std::string("C:\\clips\\a b.mp4"));
    LT_CHECK(seenOpts.find("hwaccel=d3d11va") != std::string::npos);
    std::string vf = fm.VideoFilter();
    LT_CHECK(!vf.empty());
    LT_CHECK(seenOpts.find("vf=") != std::string::npos);
    bool foundVf = false;
    for (auto& kv : seen) if (kv.first == "vf" && kv.second == vf) foundVf = true;
    LT_CHECK(foundVf);
    pp = wvp::PlanPrefsFor("off");
    (void)fm.SetPlanPreferences(pp.decoder, pp.forcedDecoder);
    ctl.PlayAt(0);
    LT_CHECK(rec.opened.back().second.size() >= 1);
    for (auto& kv : rec.opened.back().second) LT_CHECK(kv.first != "hwaccel");
}

LOGIC_TEST(log_levels_format_and_paths) {
    LT_CHECK_EQ(*log::ParseLevel(L"verbose"), 40);
    LT_CHECK_EQ(*log::ParseLevel(L"TRACE"), 56);
    LT_CHECK_EQ(*log::ParseLevel(L"quiet"), -8);
    LT_CHECK_EQ(*log::ParseLevel(L"error"), 16);
    LT_CHECK_EQ(*log::ParseLevel(L"24"), 24);
    LT_CHECK(!log::ParseLevel(L"loud") && !log::ParseLevel(L"") && !log::ParseLevel(L"12x"));
    log::Stamp t{2026, 10, 4, 9, 5, 7, 42};
    LT_CHECK_EQ(log::FormatLine(t, log::kInfo, "open", "url: x"), std::string("2026-10-04 09:05:07.042 [INFO ] open: url: x"));
    LT_CHECK_EQ(log::FormatLine(t, log::kError, "player", "boom"), std::string("2026-10-04 09:05:07.042 [ERROR] player: boom"));
    LT_CHECK_EQ(log::DebugLogPath(L"C:\\x\\logs\\", t), std::wstring(L"C:\\x\\logs\\aribplayer-20261004.log"));
    LT_CHECK_EQ(log::LogsDir("C:\\Users\\u\\AppData\\Local\\aribplayer\\settings.json"), std::string("C:\\Users\\u\\AppData\\Local\\aribplayer\\logs"));
    LT_CHECK(log::IsStatusLine("  12.31 A-V: -0.008 fd=   0 aq=   34KB vq=  890KB sq=    0B"));
    LT_CHECK(log::IsStatusLine("  4.00 M-V:  0.000 fd=   0 aq=    0KB vq=    0KB sq=    0B"));
    LT_CHECK(!log::IsStatusLine("[h264 @ 000001] profile High"));
    std::wstring u = log::UsageText();
    for (const wchar_t* k : {L"--console", L"--log-level=", L"--stats", L"--log-file=", L"--help", L"Tab", L"Debug logging"})
        LT_CHECK(u.find(k) != std::wstring::npos);
}

LOGIC_TEST(log_sink_file_lines_status_throttle_and_threads) {
    TempDir td;
    auto path = td.p / L"nested" / L"t.log";
    log::Sink s;
    LT_CHECK(!s.Active());
    s.Write(log::kInfo, "x", "dropped while inactive");
    LT_CHECK(s.OpenFile(path.wstring(), false));
    LT_CHECK(s.Active() && s.HasFile(false) && !s.HasFile(true));
    s.Write(log::kInfo, "app", "hello");
    s.Write(log::kWarning, "av", "line1\r\nline2\n\n");
    s.Write(log::kInfo, "av", "  1.00 A-V: -0.010 fd=   0 aq=   10KB vq=   30KB sq=    0B \r");
    s.Write(log::kInfo, "av", "  1.03 A-V: -0.011 fd=   0 aq=   10KB vq=   30KB sq=    0B \r");   // throttled (< 500 ms)
    LT_CHECK_EQ(s.StatusLinesWritten(), 1);
    std::vector<std::thread> th;
    for (int i = 0; i < 4; i++)
        th.emplace_back([&s, i] { for (int k = 0; k < 150; k++) s.Write(log::kDebug, "t", "thread " + std::to_string(i) + " msg " + std::to_string(k)); });
    for (auto& t : th) t.join();
    s.Flush();
    std::string txt = ReadFile(path);
    LT_CHECK(txt.find("dropped") == std::string::npos);
    LT_CHECK(txt.find("[INFO ] app: hello\r\n") != std::string::npos);
    LT_CHECK(txt.find("[WARN ] av: line1\r\n") != std::string::npos && txt.find("[WARN ] av: line2\r\n") != std::string::npos);
    LT_CHECK(txt.find("A-V: -0.010") != std::string::npos && txt.find("A-V: -0.011") == std::string::npos);
    int lines = 0, bad = 0;
    std::istringstream in(txt);
    for (std::string l; std::getline(in, l);) {
        lines++;
        if (l.size() < 24 || l[4] != '-' || l[10] != ' ' || l[19] != '.' || l.back() != '\r') bad++;
    }
    LT_CHECK_EQ(lines, 4 + 600);
    LT_CHECK_EQ(bad, 0);
    s.CloseFile(false);
    LT_CHECK(!s.Active());
}

LOGIC_TEST(log_debug_logging_setting_persists_in_temp_dir) {
    TempDir td;
    app::Services svc(td.p.wstring());
    LT_CHECK(!svc.settings.debugLogging);
    svc.settings.debugLogging = true;
    LT_CHECK(svc.settingsStore.Save(svc.settings));
    app::Services again(td.p.wstring());
    again.settings = again.settingsStore.Load();
    LT_CHECK(again.settings.debugLogging);
    LT_CHECK(log::LogsDir(svc.settingsStore.Path()).rfind(td.p.string(), 0) == 0);
}

namespace {
std::string FpsJson(const char* fps) {
    return std::string(R"JSON({"position": 5.0, "duration": 60.0, "paused": false, "speed": 1.0,
 "video": {"stream_index": 0, "codec": "h264", "decoder": "h264", "width": 640, "height": 360, "sar": "1:1", "frame_rate": 25.0},
 "fps": )JSON") + fps + "}";
}
std::vector<OverlayLine> FpsLinesFor(const std::string& json, double refreshHz) {
    OverlayContext c = Ctx(json.c_str());
    c.displayRefreshHz = refreshHz;
    return BuildOverlayLines(c);
}
}

LOGIC_TEST(fps_wrapper_parses_fps_object_and_tolerates_old_dll) {
    auto d = wvp::ParseDebugInfo(FpsJson(R"({"stream_avg":25.0,"stream_r":25.0,"filter_out":10.0,"filter_out_source":"sink_frame_rate","filter_out_time_base":"1/10",
        "decoded":24.9,"filtered":10.1,"displayed":null,"frames_decoded":120,"frames_filtered":50,"frames_displayed":49,"drops_early":1,"drops_late":2})"));
    LT_CHECK(d.Valid && d.Fps.Present);
    LT_CHECK(d.Fps.StreamAvg && *d.Fps.StreamAvg == 25.0);
    LT_CHECK(d.Fps.FilterOut && *d.Fps.FilterOut == 10.0);
    LT_CHECK_EQ(d.Fps.FilterOutTimeBase, std::string("1/10"));
    LT_CHECK_EQ(d.Fps.FilterOutSource, std::string("sink_frame_rate"));
    LT_CHECK(d.Fps.Decoded && !d.Fps.Displayed);
    LT_CHECK(d.Fps.FramesDisplayed && *d.Fps.FramesDisplayed == 49);
    LT_CHECK(d.Fps.DropsLate && *d.Fps.DropsLate == 2);
    auto old = wvp::ParseDebugInfo(kOldJson);
    LT_CHECK(old.Valid && !old.Fps.Present && !old.Fps.Displayed && !old.Fps.FilterOut);
}

LOGIC_TEST(fps_line_match_is_not_highlighted) {
    auto L = FpsLinesFor(FpsJson(R"({"stream_avg":25.0,"stream_r":25.0,"filter_out":25.0,"filter_out_time_base":"1/12800","decoded":25.1,"filtered":25.0,"displayed":24.9,
        "frames_decoded":210,"frames_filtered":205,"frames_displayed":200,"drops_early":0,"drops_late":0})"), 60);
    LineKind k{};
    LT_CHECK(Has(L, "fps       in 25.00 -> filter out 25.00 -> displayed 24.9 (refresh 60 Hz)", &k));
    LT_CHECK(k == LineKind::Normal);
    LT_CHECK(Has(L, "decoded 25.1 fps | frames decoded 210 filtered 205 displayed 200 | drops early 0 late 0"));
    LT_CHECK(Has(L, "uneven cadence (2.40 refreshes per frame, judder) (60 Hz / 25.00 fps)"));
    LT_CHECK_EQ(CountKind(L, LineKind::Warn), 0);
    LT_CHECK(!Has(L, "MISMATCH"));
}

LOGIC_TEST(fps_line_mismatch_after_warmup_is_highlighted) {
    auto L = FpsLinesFor(FpsJson(R"({"stream_avg":25.0,"stream_r":25.0,"filter_out":25.0,"decoded":25.0,"filtered":25.0,"displayed":17.5,
        "frames_decoded":210,"frames_filtered":205,"frames_displayed":120,"drops_early":0,"drops_late":40})"), 60);
    LineKind k{};
    LT_CHECK(Has(L, "displayed 17.5 (refresh 60 Hz)  MISMATCH displayed vs filter out (-30 %)", &k));
    LT_CHECK(k == LineKind::Warn);
    // within 10 %: no highlight (24.0 vs 25.0 = -4 %, 22.6 vs 25 = -9.6 %, 27.4 = +9.6 %)
    for (const char* disp : {"24.0", "22.6", "27.4"}) {
        std::string j = FpsJson((std::string(R"({"stream_avg":25.0,"filter_out":25.0,"displayed":)") + disp + R"(,"frames_displayed":200})").c_str());
        LT_CHECK_EQ(CountKind(FpsLinesFor(j, 60), LineKind::Warn), 0);
    }
    auto W = FpsLinesFor(FpsJson(R"({"stream_avg":25.0,"filter_out":25.0,"displayed":5.0,"frames_displayed":10})"), 60);
    LT_CHECK_EQ(CountKind(W, LineKind::Warn), 0);
    std::string paused = FpsJson(R"({"stream_avg":25.0,"filter_out":25.0,"displayed":5.0,"frames_displayed":200})");
    paused.replace(paused.find("\"paused\": false"), 15, "\"paused\": true");
    LT_CHECK_EQ(CountKind(FpsLinesFor(paused, 60), LineKind::Warn), 0);
    LT_CHECK_EQ(CountKind(FpsLinesFor(FpsJson(R"({"stream_avg":25.0,"filter_out":null,"displayed":5.0,"frames_displayed":200})"), 60), LineKind::Warn), 0);
    LT_CHECK_EQ(CountKind(FpsLinesFor(FpsJson(R"({"stream_avg":25.0,"filter_out":25.0,"displayed":null,"frames_displayed":200})"), 60), LineKind::Warn), 0);
}

LOGIC_TEST(fps_line_ivtc_29_97_to_23_976_notes_cadence) {
    auto L = FpsLinesFor(FpsJson(R"({"stream_avg":29.97,"stream_r":29.97,"filter_out":23.976,"filter_out_source":"sink_frame_rate","filter_out_time_base":"1/24000",
        "decoded":29.9,"filtered":23.9,"displayed":23.9,"frames_decoded":300,"frames_filtered":240,"frames_displayed":236,"drops_early":0,"drops_late":0})"), 60);
    LineKind k{};
    LT_CHECK(Has(L, "in 29.97 -> filter out 23.98 -> displayed 23.9 (refresh 60 Hz)", &k));
    LT_CHECK(k == LineKind::Normal);
    LT_CHECK(Has(L, "frame rate changed by the filter graph: 29.97 -> 23.98 fps"));
    LT_CHECK(Has(L, "3:2 cadence (pulldown judder) (60 Hz / 23.98 fps)"));
    LT_CHECK_EQ(CountKind(L, LineKind::Warn), 0);
}

LOGIC_TEST(fps_cadence_ratios) {
    using K = Cadence::Kind;
    LT_CHECK(DescribeCadence(23.976, 60.0).kind == K::Pulldown32);
    LT_CHECK(DescribeCadence(23.976, 59.94).kind == K::Pulldown32);
    LT_CHECK(DescribeCadence(24.0, 60.0).kind == K::Pulldown32);
    LT_CHECK(DescribeCadence(24.0, 120.0).kind == K::Even);
    LT_CHECK(DescribeCadence(30.0, 60.0).kind == K::Even);
    LT_CHECK(DescribeCadence(29.97, 60.0).kind == K::Even);
    LT_CHECK(DescribeCadence(60.0, 60.0).kind == K::Even);
    LT_CHECK(DescribeCadence(25.0, 60.0).kind == K::Uneven);
    LT_CHECK(DescribeCadence(25.0, 50.0).kind == K::Even);
    LT_CHECK(DescribeCadence(24.0, 72.0).kind == K::Even);
    LT_CHECK(DescribeCadence(120.0, 60.0).kind == K::FasterThanRefresh);
    LT_CHECK(DescribeCadence(24.0, 0.0).kind == K::Unknown);
    LT_CHECK(DescribeCadence(0.0, 60.0).kind == K::Unknown);
    LT_CHECK_EQ(DescribeCadence(23.976, 60.0).text, std::string("3:2 cadence (pulldown judder) (60 Hz / 23.98 fps)"));
    LT_CHECK_EQ(DescribeCadence(30.0, 60.0).text, std::string("even 2:2 cadence (60 Hz / 30.00 fps)"));
}

LOGIC_TEST(fps_line_old_dll_missing_fps_object) {
    OverlayContext c = Ctx(kOldJson);
    auto L = BuildOverlayLines(c);
    LT_CHECK(Has(L, "fps       in ? -> filter out ? -> displayed ? (refresh ? Hz)"));
    LT_CHECK_EQ(CountKind(L, LineKind::Warn), 0);
    OverlayContext c2 = Ctx(R"({"video": {"codec": "h264", "frame_rate": 29.97}})");
    c2.displayRefreshHz = 59.94;
    LT_CHECK(Has(BuildOverlayLines(c2), "in 29.97 -> filter out ? -> displayed ? (refresh 59.94 Hz)"));
}

LOGIC_TEST(display_refresh_ratio) {
    LT_CHECK(std::fabs(disp::RateFromRatio(60000, 1001) - 59.94) < 0.01);
    LT_CHECK_EQ(disp::RateFromRatio(60, 0), 0.0);
    LT_CHECK_EQ(disp::RateFromRatio(0, 1), 0.0);
}

LOGIC_TEST(display_refresh_rate_of_hidden_window) {
    if (GetSystemMetrics(SM_CMONITORS) < 1) return;
    HWND w = CreateWindowExW(0, L"STATIC", L"fps-test", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    LT_CHECK(w != nullptr);
    LT_CHECK(!IsWindowVisible(w));
    double hz = disp::RefreshRateHz(w);
    LT_CHECK(hz > 0 && hz < 1000);
    LT_CHECK(disp::RefreshRateHz(nullptr) > 0);
    if (w) DestroyWindow(w);
}

LOGIC_TEST(debug_overlay_shows_arib_audio_model) {
    OverlayContext c = Ctx(R"JSON({"position": 1.5, "duration": 10, "audio": {"codec": "aac", "stream_index": 1},
        "arib_audio": {"preference": "audio2", "playing": "audio1", "switches": 3, "silence_ms": 2005.3, "rebinds": 4,
        "roles": [{"role": "audio1", "present": true, "stream_index": 1, "pid": 274, "component_tag": 16, "starved": false, "buffered_ms": 812.4},
                  {"role": "audio2", "present": false, "stream_index": -1, "pid": -1, "component_tag": -1, "starved": true, "buffered_ms": 0.0}]}})JSON");
    auto L = BuildOverlayLines(c);
    LT_CHECK(Has(L, "preference audio2 | playing audio1 | switches 3 | silence 2005 ms | rebinds 4"));
    LT_CHECK(Has(L, "stream #1  pid 274  tag 0x10  buffered 812 ms"));
    LT_CHECK(Has(L, "none  (starved)"));
    LT_CHECK(!Has(BuildOverlayLines(Ctx(kOldJson)), "arib audio"));
}
