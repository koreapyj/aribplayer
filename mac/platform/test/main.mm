// wvp_platform_mac_test: unit tests of the Mac VideoPlatform (adapter list, probe content and its libffplay merge when built
// with libffplay, presenter creation on a CAMetalLayer view). Usage: wvp_platform_mac_test [name-substring-filter]
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <exception>
#include <set>
#include <string>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
#include "ffplay.h"
}

#include "wvp/mac/MacVideoPlatform.h"
#include "wvptest.h"
#if WVP_MAC_HAVE_LIBFFPLAY
#include "wvp/FfPlayer.h"
#endif

using namespace wvp;
using wvp::mac::MacVideoPlatform;
using wvp::mac::MetalPresenter;

namespace {

bool IsLuidShape(const std::string& s) {
    if (s.size() != 17 || s[8] != ':') return false;
    for (size_t i = 0; i < s.size(); i++)
        if (i != 8 && !std::isxdigit((unsigned char)s[i])) return false;
    return true;
}

NSView* MetalView() {
    NSView* v = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 64, 36)];
    v.wantsLayer = YES;
    v.layer = [CAMetalLayer layer];
    return v;
}

}

TEST_CASE(adapter_id_text_round_trips) {
    const std::string id = MacVideoPlatform::AdapterIdOf(0x0000000100000ABCull);
    CHECK_EQ(id, std::string("00000001:00000abc"));
    std::uint64_t back = 0;
    CHECK(MacVideoPlatform::RegistryIdOf(id, back));
    CHECK_EQ(back, 0x0000000100000ABCull);
    CHECK(!MacVideoPlatform::RegistryIdOf("apple-m3", back));
    CHECK(!MacVideoPlatform::RegistryIdOf("0000000100000abc", back));
}

TEST_CASE(adapters_lists_every_metal_device) {
    MacVideoPlatform p;
    const auto adapters = p.Adapters();
    REQUIRE(!adapters.empty());
    std::set<std::string> ids;
    bool anyOutputs = false;
    for (const AdapterEntry& a : adapters) {
        CHECK_MSG(IsLuidShape(a.luid), a.luid);
        CHECK(ids.insert(a.luid).second);
        CHECK(!a.name.empty());
        CHECK(a.hasPci);
        CHECK(a.vendorId != 0);
        anyOutputs |= a.hasOutputs;
    }
    CHECK(anyOutputs);
    // every adapter survives GroupPhysicalGpus (the synthetic PCI location)
    const auto gpus = GroupPhysicalGpus(adapters);
    CHECK_EQ(gpus.size(), adapters.size());
    for (const AdapterEntry& a : adapters) CHECK_EQ(CanonicalLuid(gpus, a.luid), a.luid);
}

TEST_CASE(display_adapter_is_a_listed_adapter) {
    MacVideoPlatform p;
    const auto adapters = p.Adapters();
    const std::string display = p.DisplayAdapter(nullptr);
    CHECK(!display.empty());
    bool listed = false;
    for (const AdapterEntry& a : adapters) listed |= a.luid == display;
    CHECK_MSG(listed, display);
    // a view without a window falls back to the main display
    NSView* v = MetalView();
    CHECK_EQ(p.DisplayAdapter((__bridge void*)v), display);
}

TEST_CASE(probe_reports_videotoolbox_and_metal) {
    MacVideoPlatform p;
    const auto adapters = p.Adapters();
    REQUIRE(!adapters.empty());
    const std::optional<CapabilitySet> caps = p.Probe(adapters.front());
    REQUIRE(caps.has_value());
    CHECK_EQ(caps->source, std::string("probe"));
    CHECK_EQ(caps->adapter.luid, adapters.front().luid);
    CHECK_EQ(caps->adapter.name, adapters.front().name);
    CHECK(caps->DeviceOk("videotoolbox"));
    const DecoderCap* h264 = caps->Decoder("h264", "videotoolbox");
    const DecoderCap* hevc = caps->Decoder("hevc", "videotoolbox");
    REQUIRE(h264 && hevc);
    CHECK(h264->ok);
    CHECK(hevc->ok);
    CHECK(h264->bitDepths.empty());
    CHECK_EQ(h264->decoder, std::string("h264"));
    CHECK(caps->Decoder("mpeg2video", "videotoolbox") != nullptr);
    CHECK(caps->Decoder("vp9", "videotoolbox") != nullptr);
    CHECK(caps->Decoder("av1", "videotoolbox") != nullptr);
    const PresenterCap* metal = caps->Presenter("metal");
    REQUIRE(metal != nullptr);
    CHECK_MSG(metal->ok, metal->error);
    REQUIRE(metal->inputs.size() == 2);
    CHECK(metal->inputs[0] == FrameDomain::VideoToolbox);
    CHECK(metal->inputs[1] == FrameDomain::System);
    CHECK(metal->shader);
    CHECK(!metal->vpTelecine);
#if WVP_MAC_HAVE_LIBFFPLAY
    // merged with libffplay's probe (FfPlayer::ProbeEngine on the same VideoToolbox device): the engine's answer
    // reached the set (its FFmpeg version; interop / features as the engine reports them), the platform's parts stay
    CHECK(!caps->ffmpegVersion.empty());
#else
    CHECK(caps->features.empty());
    CHECK(caps->interop.empty());
#endif
    // round-trips through the caps JSON
    const std::string json = CapabilitiesToJson(*caps);
    const std::optional<CapabilitySet> back = ParseCapabilities(json);
    REQUIRE(back.has_value());
    CHECK_EQ(CapabilitiesToJson(*back), json);
}

#if WVP_MAC_HAVE_LIBFFPLAY
TEST_CASE(engine_probe_on_videotoolbox_device) {
    // libffplay's portable probe (the half Probe merges) answers on the macOS build with the device it was given.
    AVBufferRef* vt = nullptr;
    REQUIRE(av_hwdevice_ctx_create(&vt, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0) >= 0);
    AVBufferRef* devices[1] = {vt};
    const std::optional<CapabilitySet> engine = FfPlayer::ProbeEngine(devices, 1);
    av_buffer_unref(&vt);
    REQUIRE(engine.has_value());
    std::printf("    libffplay %s: %zu device(s), %zu decoder(s), %zu interop, %zu feature(s)\n", FfPlayer::Version().c_str(),
                engine->devices.size(), engine->decoders.size(), engine->interop.size(), engine->features.size());
    CHECK(!engine->ffmpegVersion.empty());
    CHECK(!FfPlayer::Version().empty());
}
#endif

TEST_CASE(build_capabilities_list_videotoolbox_decoders) {
    // wvp::mac::BuildCapabilities: the darwin ffmpeg-caps.inc + the videotoolbox lines ffcapsdump does not print, so
    // MergeBuildCapabilities keeps the probe's VideoToolbox decoders and the planner can choose hwaccel=videotoolbox.
    const CapabilitySet build = wvp::mac::BuildCapabilities();
    CHECK_EQ(build.source, std::string("build"));
    for (const char* codec : {"h264", "hevc", "mpeg2video", "av1"}) {
        const DecoderCap* d = build.Decoder(codec, "videotoolbox");
        REQUIRE_MSG(d != nullptr, codec);
        CHECK_MSG(d->ok && d->decoder == codec, codec);
    }
    MacVideoPlatform p;
    const auto adapters = p.Adapters();
    REQUIRE(!adapters.empty());
    const std::optional<CapabilitySet> caps = p.Probe(adapters.front());
    REQUIRE(caps.has_value());
    const CapabilitySet merged = MergeBuildCapabilities(*caps, build);
    const DecoderCap* hevc = merged.Decoder("hevc", "videotoolbox");
    REQUIRE(hevc != nullptr);
    CHECK(hevc->ok);
    std::printf("    build: %zu filter(s), %zu decoder line(s); merged: %zu decoder(s)\n", build.filters.size(), build.decoders.size(),
                merged.decoders.size());
#if WVP_MAC_HAVE_LIBFFPLAY
    CHECK(!build.filters.empty());
    CHECK(build.Filter("ivtc_videotoolbox") != nullptr);
#endif
}

TEST_CASE(probe_of_unknown_adapter_is_nullopt) {
    MacVideoPlatform p;
    AdapterEntry a;
    a.luid = "00000000:00000000";
    a.name = "none";
    CHECK(!p.Probe(a).has_value());
    AdapterEntry bad;
    bad.luid = "apple-m3";
    CHECK(!p.Probe(bad).has_value());
}

TEST_CASE(create_presenter_rejects_bad_views) {
    MacVideoPlatform p;
    const auto adapters = p.Adapters();
    REQUIRE(!adapters.empty());
    std::string err;
    CHECK(p.CreatePresenter(adapters.front(), nullptr, &err) == nullptr);
    CHECK(!err.empty());
    err.clear();
    NSView* plain = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 64, 36)];
    plain.wantsLayer = YES;   // a CALayer, not a CAMetalLayer
    CHECK(p.CreatePresenter(adapters.front(), (__bridge void*)plain, &err) == nullptr);
    CHECK(!err.empty());
    err.clear();
    AdapterEntry unknown;
    unknown.luid = "00000000:00000000";
    NSView* metal = MetalView();
    CHECK(p.CreatePresenter(unknown, (__bridge void*)metal, &err) == nullptr);
    CHECK(!err.empty());
}

TEST_CASE(create_presenter_on_metal_layer_view) {
    MacVideoPlatform p;
    const auto adapters = p.Adapters();
    REQUIRE(!adapters.empty());
    NSView* view = MetalView();
    std::string err;
    PresenterHandle h = p.CreatePresenter(adapters.front(), (__bridge void*)view, &err);
    REQUIRE_MSG(h != nullptr, err);
    CHECK(err.empty());
    REQUIRE(h->Native() != nullptr);
    CHECK_EQ(std::string(h->Native()->name), std::string("metal"));
    CHECK(h->Native()->open != nullptr);
    CHECK(h->Native()->present != nullptr);
    CHECK_EQ(h->AdapterId(), adapters.front().luid);
    CHECK(h->Devices().empty());
    CHECK(h->OnlyGivenDevices());
    MetalPresenter* mp = dynamic_cast<MetalPresenter*>(h.get());
    REQUIRE(mp != nullptr);
    CHECK(mp->VideoToolboxDevice() != nullptr);
    CHECK(((CAMetalLayer*)view.layer).device != nil);   // the platform set the adapter's device on the bare layer
    mp->ViewChanged();   // nothing open: must be a no-op
    h.reset();   // clean destruction
}

int main(int argc, char** argv) {
    @autoreleasepool {
        [NSApplication sharedApplication];   // NSView needs an app context on the main thread
        std::string filter;
        if (argc > 1) filter = argv[1];
        int run = 0, failed = 0;
        for (auto& t : wvptest::Registry()) {
            if (!filter.empty() && std::string(t.name).find(filter) == std::string::npos) continue;
            std::printf("[ RUN  ] %s\n", t.name);
            std::fflush(stdout);
            wvptest::Cur() = {};
            try { t.fn(); }
            catch (const wvptest::RequireFailure&) {}
            catch (const std::exception& e) { wvptest::Fail(__FILE__, __LINE__, std::string("exception: ") + e.what()); }
            catch (...) { wvptest::Fail(__FILE__, __LINE__, "unknown exception"); }
            run++;
            if (wvptest::Cur().failures) { failed++; std::printf("[ FAIL ] %s\n", t.name); }
            else std::printf("[  OK  ] %s\n", t.name);
            std::fflush(stdout);
        }
        std::printf("\n%d test(s) run, %d failed\n", run, failed);
        return failed ? 1 : 0;
    }
}
