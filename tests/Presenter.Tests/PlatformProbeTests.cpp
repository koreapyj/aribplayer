// The Windows video platform: adapters and the per-adapter probe (in this process; the App runs it in a helper).
#include <cstdio>

#include "PresenterTestKit.h"

using namespace ptk;

TEST_CASE_INTEGRATION(Platform_probe_reports_devices_decoders_and_the_d3d11_presenter) {
    wvp::win32::WindowsVideoPlatform platform;
    const std::vector<wvp::AdapterEntry> adapters = platform.Adapters();
    if (adapters.empty()) throw wvptest::SkipFailure{ "no hardware adapter" };
    int videoAdapters = 0;
    for (const wvp::AdapterEntry& a : adapters) {
        const std::optional<wvp::CapabilitySet> caps = platform.ProbeInProcess(a);
        std::printf("PLATFORM | %s (%s): %s\n", a.name.c_str(), a.luid.c_str(), caps ? "video" : "no video");
        if (!caps) continue;
        ++videoAdapters;
        CHECK_EQ(caps->adapter.luid, a.luid);
        CHECK(caps->DeviceOk("d3d11va"));
        for (const wvp::DecoderCap& d : caps->decoders) {
            std::string depths;
            for (int b : d.bitDepths) depths += (depths.empty() ? "" : ",") + std::to_string(b);
            std::printf("PLATFORM |   %-10s %-8s %-12s ok=%d depths=[%s] max=%dx%d %s\n", d.codec.c_str(), d.hwaccel.c_str(),
                        d.decoder.c_str(), d.ok ? 1 : 0, depths.c_str(), d.maxWidth, d.maxHeight, d.error.c_str());
            if (d.ok && d.hwaccel == "d3d11va") CHECK_MSG(!d.bitDepths.empty() && d.bitDepths[0] == 8, d.codec);
        }
        const wvp::PresenterCap* p = caps->Presenter("d3d11");
        REQUIRE_MSG(p != nullptr, "no d3d11 presenter entry for " + a.name);
        std::printf("PLATFORM |   presenter d3d11 ok=%d shader=%d p010=%d %s\n", p->ok ? 1 : 0, p->shader ? 1 : 0,
                    p->vpP010 ? 1 : 0, p->error.c_str());
        CHECK(p->ok);
        CHECK(!caps->DeviceOk("cuda"));
        for (const wvp::DecoderCap& d : caps->decoders) CHECK_MSG(d.hwaccel == "d3d11va", d.codec + " " + d.hwaccel);
    }
    std::fflush(stdout);
    CHECK(videoAdapters > 0);
}

TEST_CASE_INTEGRATION(Platform_probe_child_json_round_trips) {
    wvp::win32::WindowsVideoPlatform platform;
    const std::vector<wvp::AdapterEntry> adapters = platform.Adapters();
    if (adapters.empty()) throw wvptest::SkipFailure{ "no hardware adapter" };
    const std::string json = wvp::win32::WindowsVideoPlatform::ProbeChildJson(adapters.front().luid, {});
    if (json.empty()) throw wvptest::SkipFailure{ "the first adapter cannot do video" };
    CHECK(json.find('\n') == std::string::npos);
    const std::optional<wvp::CapabilitySet> caps = wvp::ParseCapabilities(json);
    REQUIRE(caps.has_value());
    CHECK_EQ(caps->adapter.luid, adapters.front().luid);
    CHECK(caps->Presenter("d3d11") != nullptr);
}

TEST_CASE_INTEGRATION(Platform_rejects_invalid_presenter_options) {
    HiddenHost host;
    REQUIRE(host.video != nullptr);
    std::string error;
    wvp::win32::PresenterOptions o;
    o.forceCaps = "rotate=0";
    wvp::win32::WindowsVideoPlatform bad(o);
    CHECK(bad.CreatePresenter({}, host.video, &error) == nullptr);
    CHECK(!error.empty());
    o = {};
    o.route = "sdl";
    wvp::win32::WindowsVideoPlatform badRoute(o);
    error.clear();
    CHECK(badRoute.CreatePresenter({}, host.video, &error) == nullptr);
    CHECK(!error.empty());
}
