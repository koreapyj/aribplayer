// Video GPU selection with simulated multi-adapter capability sets, the Video GPU setting and device-loss presenter errors.
#include "wvptest.h"

#include <algorithm>

#include "wvp/AppSettings.h"
#include "wvp/PipelineSession.h"
#include "wvp/VideoGpu.h"

using namespace wvp;

namespace {

const char* kIgpu = "00000000:0000a1b2";
const char* kDgpu = "00000000:0001d2c4";

AdapterCandidate Igpu(bool av1 = true) {
    AdapterCandidate c;
    c.luid = kIgpu; c.name = "Intel(R) UHD Graphics"; c.vendorId = 0x8086; c.deviceId = 0x46a6;
    c.caps = fakecaps::Intel();
    if (!av1)
        c.caps.decoders.erase(std::remove_if(c.caps.decoders.begin(), c.caps.decoders.end(), [](const DecoderCap& d) { return d.codec == "av1"; }),
                              c.caps.decoders.end());
    return c;
}

AdapterCandidate Dgpu() {
    AdapterCandidate c;
    c.luid = kDgpu; c.name = "NVIDIA GeForce RTX 4060 Laptop GPU"; c.vendorId = 0x10de; c.deviceId = 0x28e0;
    c.caps = fakecaps::Nvidia();
    return c;
}

PlanRequest Req(const char* codec, DeinterlaceMode m = DeinterlaceMode::Off) {
    PlanRequest r;
    r.content = { codec, 8, 1920, 1080, false };
    r.settings.deinterlace = m;
    return r;
}

}

TEST_CASE(VideoGpu_laptop_capable_igpu_display_picks_igpu) {
    for (const char* codec : { "h264", "hevc", "mpeg2video" }) {
        VideoGpuChoice ch = SelectVideoGpu({ Igpu(), Dgpu() }, kIgpu, {}, Req(codec, DeinterlaceMode::On));
        CHECK_EQ(ch.index, 0);
        CHECK(!ch.crossAdapter && !ch.manual);
        CHECK(ch.total.Weighted() == 0 && ch.total.deviceCopies == 0);
        CHECK_EQ(ch.runnerUp, 1);
        CHECK_EQ(ch.runnerUpTotal.deviceCopies, 1);   // the dGPU's cross-adapter copy
    }
}

TEST_CASE(VideoGpu_igpu_without_av1_moves_av1_to_dgpu_only) {
    VideoGpuChoice av1 = SelectVideoGpu({ Igpu(false), Dgpu() }, kIgpu, {}, Req("av1"));
    CHECK_EQ(av1.index, 1);
    CHECK(av1.crossAdapter);
    CHECK(av1.total.Weighted() == 0 && av1.total.deviceCopies == 1);
    CHECK_MSG(av1.reason.find("+1 cross-adapter copy") != std::string::npos, av1.reason);
    CHECK_EQ(av1.runnerUp, 0);
    CHECK(av1.runnerUpTotal.Weighted() > 0);   // software decoding on the iGPU

    CHECK_EQ(SelectVideoGpu({ Igpu(false), Dgpu() }, kIgpu, {}, Req("h264")).index, 0);
}

TEST_CASE(VideoGpu_desktop_dgpu_display_picks_dgpu_with_cuda_allowed) {
    VideoGpuChoice ch = SelectVideoGpu({ Igpu(), Dgpu() }, kDgpu, {}, Req("h264"));
    CHECK_EQ(ch.index, 1);
    CHECK(!ch.crossAdapter);
    PlanRequest cuda = Req("h264");
    cuda.decoder = DecoderPref::Forced;
    cuda.forcedDecoder = "cuda";
    VideoGpuChoice c = SelectVideoGpu({ Igpu(), Dgpu() }, kDgpu, {}, cuda);
    CHECK_EQ(c.index, 1);
    CHECK_EQ(c.plan.hwaccel, std::string("cuda"));
}

TEST_CASE(VideoGpu_tie_prefers_display_then_order) {
    AdapterCandidate a = Igpu(), b = Igpu();
    b.luid = "00000000:0000beef"; b.name = "second";
    CHECK_EQ(SelectVideoGpu({ a, b }, b.luid, {}, Req("h264")).index, 1);
    CHECK_EQ(SelectVideoGpu({ a, b }, "", {}, Req("h264")).index, 0);   // display adapter unknown: both cross, candidate order
}

TEST_CASE(VideoGpu_manual_choice_wins_and_missing_falls_back) {
    VideoGpuPref dg{ kDgpu, "NVIDIA GeForce RTX 4060 Laptop GPU", 0x10de, 0x28e0 };
    VideoGpuChoice m = SelectVideoGpu({ Igpu(), Dgpu() }, kIgpu, dg, Req("h264"));
    CHECK(m.manual && m.index == 1 && m.crossAdapter);
    CHECK_EQ(m.total.deviceCopies, 1);

    VideoGpuPref moved = dg;   // LUIDs change across reboots: vendor + device id still match
    moved.luid = "00000000:00099999";
    CHECK_EQ(FindPreferred({ Igpu(), Dgpu() }, moved), 1);
    CHECK(SelectVideoGpu({ Igpu(), Dgpu() }, kIgpu, moved, Req("h264")).manual);

    VideoGpuPref gone{ "00000000:00000bad", "Radeon eGPU", 0x1002, 0x73df };
    VideoGpuChoice f = SelectVideoGpu({ Igpu(), Dgpu() }, kIgpu, gone, Req("h264"));
    CHECK(!f.manual && f.manualMissing && f.index == 0);
    CHECK_MSG(f.reason.find("Radeon eGPU not present") != std::string::npos, f.reason);
}

TEST_CASE(VideoGpu_no_adapter_means_software) {
    VideoGpuChoice ch = SelectVideoGpu({}, "", {}, Req("h264"));
    CHECK_EQ(ch.index, -1);
    CHECK_MSG(ch.reason.find("software decoding + SDL") != std::string::npos, ch.reason);
}

TEST_CASE(VideoGpu_cross_adapter_copy_never_outweighs_a_host_copy) {
    PlanCost host1; host1.hostCopies = 1;
    PlanCost zero;
    CHECK(TotalWithCrossAdapter(zero, true) < TotalWithCrossAdapter(host1, false));
    CHECK(TotalWithCrossAdapter(zero, false) < TotalWithCrossAdapter(zero, true));
}

TEST_CASE(Settings_video_gpu_round_trip) {
    AppSettings def;
    CHECK(def.videoGpu.Automatic());
    std::string json = SettingsStore::ToJson(def);
    CHECK(json.find("\"videoGpu\": \"auto\"") != std::string::npos || json.find("\"videoGpu\":\"auto\"") != std::string::npos);
    AppSettings s;
    s.videoGpu = { kDgpu, "NVIDIA GeForce RTX 4060 Laptop GPU", 0x10de, 0x28e0 };
    AppSettings back;
    REQUIRE(SettingsStore::FromJson(SettingsStore::ToJson(s), back));
    CHECK(back.videoGpu == s.videoGpu);
    AppSettings odd;
    REQUIRE(SettingsStore::FromJson(R"({"videoGpu": 42})", odd));
    CHECK(odd.videoGpu.Automatic());
    REQUIRE(SettingsStore::FromJson(R"({"videoGpu": {"name": "x", "vendorId": 999999}})", odd));
    CHECK(odd.videoGpu.vendorId == 0xFFFF);
}

TEST_CASE(Session_presenter_device_lost_is_not_a_fallback) {
    PipelineSession s;
    Plan p;
    PresenterErrorInfo lost;
    lost.Presenter = L"d3d11"; lost.Error = L"Present failed"; lost.DeviceLost = true; lost.RemovedReason = "0x887a0005";
    CHECK(!s.OnPresenterError(p, lost).has_value());
    PresenterErrorInfo plain;
    plain.Presenter = L"d3d11"; plain.Error = L"Present failed";
    auto fb = s.OnPresenterError(p, plain);
    REQUIRE(fb.has_value());
    CHECK_EQ(fb->key, std::string("presenter:d3d11"));
}

namespace {
AdapterEntry Entry(const char* luid, int vendor, int device, bool pci, int bus, bool outputs) {
    AdapterEntry e;
    e.luid = luid; e.name = "GPU"; e.vendorId = vendor; e.deviceId = device;
    e.hasPci = pci; e.bus = bus; e.hasOutputs = outputs;
    return e;
}
}

TEST_CASE(PhysicalGpu_aliases_of_one_gpu_are_one_candidate_without_cross_copy) {
    // the same RTX twice in DXGI (one entry behind an indirect display driver): same PCI address
    std::vector<PhysicalGpu> g = GroupPhysicalGpus({ Entry("00000000:000188ca", 0x10de, 0x2c05, true, 1, true),
                                                     Entry("00000000:00025217", 0x10de, 0x2c05, true, 1, false) });
    REQUIRE(g.size() == 1);
    CHECK_EQ(g[0].canonical.luid, std::string("00000000:000188ca"));
    REQUIRE(g[0].aliasLuids.size() == 1);
    CHECK_EQ(CanonicalLuid(g, "00000000:00025217"), std::string("00000000:000188ca"));
    // the window's monitor on the alias is the canonical GPU's monitor: no cross-adapter copy for it
    AdapterCandidate c = Dgpu();
    c.luid = g[0].canonical.luid;
    VideoGpuChoice ch = SelectVideoGpu({ c }, CanonicalLuid(g, "00000000:00025217"), {}, Req("h264"));
    CHECK(ch.index == 0 && !ch.crossAdapter);
    // the entry that drives a monitor is canonical even when it comes second
    std::vector<PhysicalGpu> h = GroupPhysicalGpus({ Entry("A", 0x10de, 1, true, 1, false), Entry("B", 0x10de, 1, true, 1, true) });
    REQUIRE(h.size() == 1);
    CHECK_EQ(h[0].canonical.luid, std::string("B"));
    CHECK_EQ(CanonicalLuid(h, "A"), std::string("B"));
}

TEST_CASE(PhysicalGpu_identical_cards_at_different_pci_addresses_stay_two) {
    std::vector<PhysicalGpu> g = GroupPhysicalGpus({ Entry("A", 0x10de, 0x2c05, true, 1, true), Entry("B", 0x10de, 0x2c05, true, 2, false) });
    REQUIRE(g.size() == 2);
    CHECK_EQ(CanonicalLuid(g, "B"), std::string("B"));
}

TEST_CASE(PhysicalGpu_virtual_adapter_without_pci_is_never_a_candidate) {
    // unambiguous: an alias of the one physical GPU with the same vendor + device id
    std::vector<PhysicalGpu> one = GroupPhysicalGpus({ Entry("P", 0x10de, 7, true, 1, true), Entry("V", 0x10de, 7, false, 0, true) });
    REQUIRE(one.size() == 1);
    CHECK_EQ(one[0].canonical.luid, std::string("P"));
    CHECK_EQ(CanonicalLuid(one, "V"), std::string("P"));
    // ambiguous (two identical cards) or no match: dropped
    std::vector<PhysicalGpu> two = GroupPhysicalGpus({ Entry("P1", 0x10de, 7, true, 1, true), Entry("P2", 0x10de, 7, true, 2, true),
                                                       Entry("V", 0x10de, 7, false, 0, true) });
    CHECK(two.size() == 2 && CanonicalLuid(two, "V").empty());
    std::vector<PhysicalGpu> none = GroupPhysicalGpus({ Entry("V", 0x1af4, 9, false, 0, true) });
    CHECK(none.empty() && CanonicalLuid(none, "V").empty());
}
