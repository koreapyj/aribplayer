#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

#include "wvp/DebugInfo.h"
#include "wvptest.h"

using namespace wvp;

namespace {

const char* kFullJson = R"({
 "position": 12.34, "duration": 60.0, "paused": false, "speed": 1.5,
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

bool Near(const std::optional<double>& v, double target, double tol) { return v && std::fabs(*v - target) <= tol; }

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
    const DebugInfo d = ParseDebugInfo(R"({"presenter":{"name":"d3d11","stats":{"acrylic":{"regions":2,"frames_blurred":10,"failures":0,"blur_ms":0.37}}}})");
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
        "events":[{"kind":"presented","count":7,"pts":2.3,"serial":3,"at_us":140}]},
        "presenter":{"name":"d3d11","stats":{"renderer":"d3d11:videoprocessor","frame_stats":{"dxgi_valid":true,
        "dxgi_present_count":7,"dxgi_present_refresh_count":9,"dxgi_sync_refresh_count":11,"dxgi_sync_qpc_time":123456,
        "dxgi_sync_gpu_time":654321,"gpu_total_us":900,"gpu_frames":7,"gpu_last_us":130}}}})");
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
    CHECK(*d.FrameTrace.EarlyDrop.Count == 1);
    REQUIRE(d.FrameTrace.Events.size() == (size_t)1);
    CHECK_EQ(d.FrameTrace.Events[0].Kind, std::string("presented"));
    CHECK_EQ(*d.FrameTrace.Events[0].Pts, 2.3);
    CHECK_EQ(d.Presenter.Renderer, std::string("d3d11:videoprocessor"));
    REQUIRE(d.Presenter.DxgiValid.has_value());
    CHECK(*d.Presenter.DxgiValid);
    CHECK_EQ(*d.Presenter.DxgiPresentCount, (std::int64_t)7);
    CHECK_EQ(*d.Presenter.DxgiSyncQpcTime, (std::int64_t)123456);
    CHECK_EQ(*d.Presenter.D3dGpuTotalUs, (std::int64_t)900);
    CHECK_EQ(*d.Presenter.D3dGpuLastUs, (std::int64_t)130);
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

TEST_CASE(DebugInfo_parses_hwframes_keys_and_tolerates_old_dll) {
    DebugInfo d = ParseDebugInfo(R"({"video":{"codec":"h264","hwaccel_requested":"d3d11va","hwaccel_device":"d3d11va",
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
