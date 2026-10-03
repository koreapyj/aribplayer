#include "LogicTest.h"
#include "features/BadgeLogic.h"

namespace {
using namespace app::badge;

wvp::StreamInfo Stream(wvp::StreamType t, const wchar_t* codec, int channels = 0) {
    wvp::StreamInfo s;
    s.Type = t;
    s.Codec = codec;
    s.Channels = channels;
    s.Selected = true;
    return s;
}
}

LOGIC_TEST(badge_chips_video_and_audio) {
    wvp::DebugInfo d;
    d.Valid = true;
    d.HasVideo = d.HasAudio = true;
    d.Video.HwaccelDevice = "d3d11va";
    wvp::MediaInfo m;
    m.Streams = {Stream(wvp::StreamType::Video, L"hevc"), Stream(wvp::StreamType::Audio, L"eac3", 6)};
    std::vector<Chip> want{{ChipKind::HwSw, L"H/W"}, {ChipKind::VideoCodec, L"HEVC"},
                           {ChipKind::AudioCodec, L"E-AC3"}, {ChipKind::Channels, L"5.1"}};
    LT_CHECK(BuildChips(d, m) == want);
    d.Video.HwaccelDevice = "none";
    d.Video.FramePath = "software";
    LT_CHECK(BuildChips(d, m)[0].text == L"S/W");
    LT_CHECK(AudioCodecName(L"pcm_s16le") == L"PCM");
    LT_CHECK(ChannelsText(3) == L"3ch");
}

LOGIC_TEST(badge_chips_fourcc_and_hdr) {
    wvp::MediaInfo m;
    m.Streams = {Stream(wvp::StreamType::Video, L"mpeg2video"), Stream(wvp::StreamType::Audio, L"aac", 2)};
    LT_CHECK(BuildChips(wvp::DebugInfo{}, m)[0].text == L"MPEG2VIDEO");   // no FourCC: codec name
    m.Streams[0].FourCC = L"mpg2";
    m.Streams[0].ColorTransfer = L"bt709";
    auto c = BuildChips(wvp::DebugInfo{}, m);
    LT_CHECK(c[0] == (Chip{ChipKind::VideoCodec, L"MPG2"}));
    LT_CHECK_EQ(c.size(), (size_t)3);   // no HDR chip
    m.Streams[0].ColorTransfer = L"smpte2084";
    c = BuildChips(wvp::DebugInfo{}, m);
    LT_CHECK(c[0] == (Chip{ChipKind::Hdr, L"HDR"}));
    LT_CHECK(c[1].text == L"MPG2");
    m.Streams[0].ColorTransfer = L"arib-std-b67";
    LT_CHECK(BuildChips(wvp::DebugInfo{}, m)[0].kind == ChipKind::Hdr);
}

LOGIC_TEST(badge_chips_audio_only_and_empty) {
    wvp::DebugInfo d;
    d.Valid = true;
    d.HasAudio = true;
    wvp::MediaInfo m;
    m.Streams = {Stream(wvp::StreamType::Audio, L"flac", 2)};
    auto c = BuildChips(d, m);
    LT_CHECK_EQ(c.size(), (size_t)2);   // no H/W chip without video
    LT_CHECK(c[0].kind == ChipKind::AudioCodec);
    LT_CHECK(BuildChips(wvp::DebugInfo{}, wvp::MediaInfo{}).empty());
}
