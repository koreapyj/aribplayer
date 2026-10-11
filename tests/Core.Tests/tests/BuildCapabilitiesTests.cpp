#include "wvptest.h"

#include <string>

#include "wvp/CapabilitySet.h"

using namespace wvp;

TEST_CASE(Caps_build_table_is_this_ffmpeg_build) {
    CapabilitySet b = BuildCapabilities();
    CHECK_EQ(b.source, std::string("build"));
    CHECK(!b.ffmpegVersion.empty());
    CHECK(b.HasFeature("hwframes_keep") && b.HasFeature("upload_hw_sink") && !b.HasFeature("presenter_d3d11"));
    for (const char* n : {"fieldmatch", "decimate", "bwdif", "yadif", "hwupload", "hwdownload", "ivtc_d3d11"})
        CHECK_MSG(b.Filter(n) != nullptr, n);
    const FilterCap* ivtc = b.Filter("ivtc_d3d11");
    REQUIRE(ivtc != nullptr);
    CHECK(ivtc->formats == FilterCap::Formats::List && ivtc->hwDevice && ivtc->optionsKnown);
    REQUIRE(ivtc->pixFmts.size() == 1);
    CHECK_EQ(ivtc->pixFmts[0], std::string("d3d11"));
    const DecoderCap* h264 = b.Decoder("h264", "d3d11va");
    REQUIRE(h264 != nullptr);
    CHECK_EQ(h264->decoder, std::string("h264"));
    for (const DecoderCap& d : b.decoders) CHECK_MSG(d.hwaccel != "cuda", d.codec);
}
