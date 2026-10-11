#include "wvptest.h"

#include <string>

#include "wvp/PipelineSession.h"

using namespace wvp;

namespace {

MediaInfo Info(const std::string& codec, int w, int h, const std::string& pix, bool withAudio = true) {
    MediaInfo m;
    if (withAudio) {
        StreamInfo a;
        a.Index = 0; a.Type = StreamType::Audio; a.Codec = "aac";
        m.Streams.push_back(a);
    }
    StreamInfo v;
    v.Index = withAudio ? 1 : 0; v.Type = StreamType::Video; v.Codec = codec; v.Width = w; v.Height = h; v.PixFmt = pix; v.Selected = true;
    m.Streams.push_back(v);
    return m;
}

Plan D3d11IvtcPlan() {
    PlanRequest r;
    r.content = { "mpeg2video", 8, 1920, 1080, false };
    r.settings.deinterlace = DeinterlaceMode::Ivtc;
    return PipelinePlanner(fakecaps::Nvidia()).Make(r);
}

DebugInfo Dbg(const std::string& device, const std::string& hwframes, long long decoded) {
    DebugInfo d;
    d.Valid = true;
    d.HasVideo = true;
    d.Video.HwaccelDevice = device;
    d.Video.Hwframes = hwframes;
    d.Fps.FramesDecoded = decoded;
    return d;
}

}

TEST_CASE(PipelineSession_bit_depth_and_content) {
    CHECK_EQ(PipelineSession::BitDepthOf("yuv420p"), 8);
    CHECK_EQ(PipelineSession::BitDepthOf("nv12"), 8);
    CHECK_EQ(PipelineSession::BitDepthOf("yuv420p10le"), 10);
    CHECK_EQ(PipelineSession::BitDepthOf("p010le"), 10);
    CHECK_EQ(PipelineSession::BitDepthOf("yuv422p12le"), 12);
    CHECK_EQ(PipelineSession::BitDepthOf("gray10le"), 10);
    CHECK_EQ(PipelineSession::BitDepthOf("P010LE"), 10);
    CHECK_EQ(PipelineSession::BitDepthOf(""), 8);
    ContentInfo c = PipelineSession::ContentOf(Info("hevc", 3840, 2160, "yuv420p10le"));
    CHECK(c.Known());
    CHECK_EQ(c.codec, std::string("hevc"));
    CHECK_EQ(c.width, 3840);
    CHECK_EQ(c.height, 2160);
    CHECK_EQ(c.bitDepth, 10);
    CHECK(!PipelineSession::ContentOf(MediaInfo{}).Known());
    MediaInfo audioOnly;
    StreamInfo a; a.Type = StreamType::Audio; a.Codec = "mp3";
    audioOnly.Streams.push_back(a);
    CHECK(!PipelineSession::ContentOf(audioOnly).Known());
}

TEST_CASE(PipelineSession_cache_and_changed_flag) {
    PipelineSession s;
    CHECK(!s.BeforeOpen("a.mp4").Known());
    auto p = s.OnPrepared("a.mp4", Info("h264", 1920, 1080, "yuv420p"));
    CHECK(p.content.Known() && p.changed);
    CHECK_EQ(s.CacheSize(), (size_t)1);
    ContentInfo again = s.BeforeOpen("a.mp4");
    CHECK_EQ(again.codec, std::string("h264"));
    CHECK_EQ(again.height, 1080);
    p = s.OnPrepared("a.mp4", Info("h264", 1920, 1080, "yuv420p"));
    CHECK(!p.changed);
    p = s.OnPrepared("a.mp4", Info("hevc", 1920, 1080, "yuv420p10le"));
    CHECK(p.changed);
    CHECK_EQ(s.Cached("a.mp4")->codec, std::string("hevc"));
    s.BeforeOpen("song.mp3");
    p = s.OnPrepared("song.mp3", MediaInfo{});
    CHECK(!p.content.Known() && !p.changed);
    CHECK(!s.Cached("song.mp3").has_value());
}

TEST_CASE(PipelineSession_cache_is_lru_64) {
    PipelineSession s;
    auto put = [&](int i) {
        std::string u = "f" + std::to_string(i) + ".mp4";
        s.BeforeOpen(u);
        s.OnPrepared(u, Info("h264", 100 + i, 100, "yuv420p"));
    };
    for (int i = 0; i < 64; i++) put(i);
    CHECK_EQ(s.CacheSize(), PipelineSession::kCacheSize);
    CHECK(s.Cached("f0.mp4").has_value());
    s.BeforeOpen("f0.mp4");                     // touch f0: f1 becomes the oldest
    put(64);
    CHECK_EQ(s.CacheSize(), (size_t)64);
    CHECK(s.Cached("f0.mp4").has_value());
    CHECK(!s.Cached("f1.mp4").has_value());
    CHECK(s.Cached("f64.mp4").has_value());
    for (int i = 65; i < 200; i++) put(i);
    CHECK_EQ(s.CacheSize(), (size_t)64);
}

TEST_CASE(PipelineSession_content_reopen_claim_once_per_url_chain) {
    PipelineSession s;
    CHECK(s.ClaimContentReopen("a"));
    CHECK(!s.ClaimContentReopen("a"));       // the reopen's own Prepared must not reopen again
    s.BeforeOpen("a");
    CHECK(!s.ClaimContentReopen("a"));
    CHECK(s.ClaimContentReopen("b"));
    CHECK(s.ClaimContentReopen("a"));        // another file in between: a new chain
}

TEST_CASE(PipelineSession_debug_info_fallbacks_after_warmup) {
    Plan p = D3d11IvtcPlan();
    REQUIRE(p.ok);
    REQUIRE(p.hwaccel == "d3d11va" && p.hwframesKeep);
    PipelineSession s;
    s.BeforeOpen("a");
    // not warmed up yet
    CHECK(!s.OnDebugInfo(p, Dbg("none", "keep", 39)).has_value());
    CHECK(!s.OnDebugInfo(p, Dbg("d3d11va", "keep", 100)).has_value());
    // device not reported: no decision
    CHECK(!s.OnDebugInfo(p, Dbg("", "", 100)).has_value());
    auto f = s.OnDebugInfo(p, Dbg("dxva2", "keep", 40));
    REQUIRE(f.has_value());
    CHECK_EQ(f->key, std::string("hwaccel:d3d11va"));
    CHECK(s.Blacklist().count("hwaccel:d3d11va") == 1);
    // at most one per generation
    CHECK(!s.OnDebugInfo(p, Dbg("none", "download", 500)).has_value());
    // next generation: hwframes keep that turned into download
    s.BeforeOpen("a");
    auto g = s.OnDebugInfo(p, Dbg("d3d11va", "download", 60));
    REQUIRE(g.has_value());
    CHECK_EQ(g->key, std::string("hwframes_keep"));
    // the same key cannot be blamed twice
    s.BeforeOpen("a");
    CHECK(!s.OnDebugInfo(p, Dbg("d3d11va", "download", 60)).has_value());
    // frames counted from the frame path counters when fps is missing
    PipelineSession s2;
    s2.BeforeOpen("b");
    DebugInfo d = Dbg("none", "", 0);
    d.Fps.FramesDecoded.reset();
    d.Video.FramesHwDownload = 30;
    d.Video.FramesSoftware = 10;
    auto h = s2.OnDebugInfo(p, d);
    REQUIRE(h.has_value());
    CHECK_EQ(h->key, std::string("hwaccel:d3d11va"));
    PlanRequest r;
    r.content = { "h264", 8, 640, 360, false };
    r.decoder = DecoderPref::Software;
    Plan sw = PipelinePlanner(fakecaps::Nvidia()).Make(r);
    PipelineSession s3;
    s3.BeforeOpen("c");
    CHECK(!s3.OnDebugInfo(sw, Dbg("none", "", 500)).has_value());
}

TEST_CASE(PipelineSession_filter_errors) {
    Plan p = D3d11IvtcPlan();
    REQUIRE(p.ok);
    PipelineSession s;
    s.BeforeOpen("a");
    auto f = s.OnFilterError(p, std::string(R"({"type":"video","filter":"ivtc_d3d11","code":-22,"message":"x"})"));
    REQUIRE(f.has_value());
    CHECK_EQ(f->key, std::string("filter:ivtc_d3d11"));
    CHECK(!s.OnFilterError(p, std::string(R"({"type":"video"})")).has_value());   // once per generation
    // hwframes fallback wins over the filter
    s.BeforeOpen("a");
    auto g = s.OnFilterError(p, std::string(R"({"type":"video","filter":"x","code":-1,"message":"m","hwframes_fallback":true})"));
    REQUIRE(g.has_value());
    CHECK_EQ(g->key, std::string("hwframes_keep"));
    s.BeforeOpen("a");
    FilterErrorInfo e;
    e.HwframesFallback = true;
    CHECK(!s.OnFilterError(p, e).has_value());
    // no hw filter step in the plan and no hwframes fallback: nothing to blame
    PipelineSession s2;
    s2.BeforeOpen("b");
    PlanRequest r;
    r.content = { "h264", 8, 640, 360, false };
    r.settings.deinterlace = DeinterlaceMode::On;
    Plan sw = PipelinePlanner(fakecaps::None()).Make(r);
    CHECK(!s2.OnFilterError(sw, std::string(R"({"type":"video","code":-22})")).has_value());
    CHECK(!s2.OnFilterError(sw, std::string("not json")).has_value());
    CHECK(s2.OnFilterError(sw, std::string(R"({"hwframes_fallback":true})")).has_value());
}
