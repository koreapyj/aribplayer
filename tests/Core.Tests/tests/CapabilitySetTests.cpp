#include "wvptest.h"

#include <string>

#include "wvp/CapabilitySet.h"

using namespace wvp;

namespace {

const char* kSample = R"({
  "schema": 1, "ffmpeg": "n7.1.4-3+3",
  "adapter": {"name": "NVIDIA GeForce RTX 5070", "vendor_id": 4318, "luid": "00000000:0001d2c4", "driver": "32.0.15.8129"},
  "features": ["hwframes_keep", "presenter_d3d11", "presenter_params"],
  "devices": [{"type": "cuda", "ok": true, "name": "NVIDIA GeForce RTX 5070", "error": ""},
              {"type": "qsv", "ok": false, "error": "no device"}],
  "decoders": [{"codec": "h264", "hwaccel": "cuda", "decoder": "h264", "ok": true, "bit_depths": [8, 10],
                "max_width": 4096, "max_height": 2304, "error": ""}],
  "filters": [{"name": "bwdif_cuda", "formats": "list", "pix_fmts": ["cuda"], "options": ["mode", "parity", "deint"], "hw_device": true},
              {"name": "setpts", "formats": "any"},
              {"name": "hwmap", "formats": "dynamic", "hw_device": true},
              {"name": "weird", "formats": "bogus", "options": []}],
  "interop": [{"from": "d3d11", "to": "opencl", "method": "map", "ok": false, "error": "nope"},
              {"from": "d3d11va", "to": "qsv", "method": "map", "ok": true},
              {"from": "bogus!", "to": "cuda", "method": "map", "ok": true}],
  "presenters": [{"name": "d3d11", "ok": true, "inputs": ["d3d11", "system", "???"], "import": {"cuda": true, "opencl": false},
                  "vp": {"rotate": true, "mirror": true, "procamp": true, "brightness": [-100, 100], "contrast": [0, 10],
                         "saturation": [0, 5.5], "deinterlace": ["bob", "adaptive"], "telecine": false},
                  "shader": true},
                 {"name": "sdl", "ok": true, "inputs": ["system"]}],
  "unknown_member": 42
})";

std::vector<std::pair<std::string, CapabilitySet>> AllFakes() {
    std::vector<std::pair<std::string, CapabilitySet>> v;
    v.emplace_back("nvidia", fakecaps::Nvidia());
    v.emplace_back("nvidia_nopres", fakecaps::Nvidia(false, false));
    v.emplace_back("future_ivtc", fakecaps::NvidiaFutureIvtcVariant());
    v.emplace_back("intel", fakecaps::Intel());
    v.emplace_back("amd", fakecaps::Amd());
    v.emplace_back("none", fakecaps::None());
    v.emplace_back("legacy", fakecaps::Legacy());
    return v;
}

}

TEST_CASE(Caps_parse_full_sample) {
    auto c = ParseCapabilities(kSample);
    REQUIRE(c.has_value());
    CHECK_EQ(c->schema, 1);
    CHECK_EQ(c->ffmpegVersion, "n7.1.4-3+3");
    CHECK_EQ(c->adapter.name, "NVIDIA GeForce RTX 5070");
    CHECK_EQ(c->adapter.vendorId, 0x10DE);
    CHECK_EQ(c->adapter.luid, "00000000:0001d2c4");
    CHECK_EQ(c->adapter.driver, "32.0.15.8129");
    CHECK_EQ(c->features.size(), (size_t)3);
    CHECK(c->HasFeature("presenter_params"));
    CHECK(!c->HasFeature("nope"));
    REQUIRE(c->devices.size() == 2);
    CHECK(c->DeviceOk("cuda"));
    CHECK(!c->DeviceOk("qsv"));
    CHECK(!c->DeviceOk("vulkan"));
    CHECK_EQ(c->devices[1].error, "no device");
    const DecoderCap* d = c->Decoder("h264", "cuda");
    REQUIRE(d != nullptr);
    CHECK(d->ok);
    CHECK_EQ(d->bitDepths.size(), (size_t)2);
    CHECK_EQ(d->maxWidth, 4096);
    CHECK_EQ(d->maxHeight, 2304);
    CHECK(c->Decoder("h264", "dxva2") == nullptr);
    CHECK(c->Decoder("", "cuda") == nullptr);
    REQUIRE(c->filters.size() == 4);
    const FilterCap* f = c->Filter("bwdif_cuda");
    REQUIRE(f != nullptr);
    CHECK(f->formats == FilterCap::Formats::List);
    CHECK_EQ(f->pixFmts.size(), (size_t)1);
    CHECK(f->optionsKnown);
    CHECK_EQ(f->options.size(), (size_t)3);
    CHECK(f->hwDevice);
    CHECK(c->Filter("setpts")->formats == FilterCap::Formats::Any);
    CHECK(!c->Filter("setpts")->optionsKnown);
    CHECK(c->Filter("hwmap")->formats == FilterCap::Formats::Dynamic);
    CHECK(c->Filter("weird")->formats == FilterCap::Formats::Unknown);
    CHECK(c->Filter("weird")->optionsKnown);
    CHECK(c->Filter("zzz") == nullptr);
    CHECK_EQ(c->interop.size(), (size_t)2);   // the entry with an invalid domain name is skipped
    CHECK(!c->InteropOk(FrameDomain::D3D11, FrameDomain::OpenCl));
    CHECK(c->InteropOk(FrameDomain::D3D11, FrameDomain::Qsv));
    CHECK(c->InteropOk(FrameDomain::D3D11, FrameDomain::Qsv, "map"));
    CHECK(!c->InteropOk(FrameDomain::D3D11, FrameDomain::Qsv, "copy"));
    const PresenterCap* p = c->Presenter("d3d11");
    REQUIRE(p != nullptr);
    CHECK(p->ok);
    REQUIRE(p->inputs.size() == 2);
    CHECK(p->inputs[0] == FrameDomain::D3D11);
    CHECK(p->inputs[1] == FrameDomain::System);
    CHECK(p->importCuda);
    CHECK(p->vpRotate && p->vpMirror && p->vpProcamp && !p->vpTelecine && p->shader);
    CHECK_EQ(p->vpBrightnessMin, -100.0);
    CHECK_EQ(p->vpBrightnessMax, 100.0);
    CHECK_EQ(p->vpContrastMax, 10.0);
    CHECK_EQ(p->vpSaturationMax, 5.5);
    CHECK_EQ(p->vpDeinterlace.size(), (size_t)2);
    CHECK(c->Presenter("sdl") != nullptr);
    CHECK(c->Presenter("vulkan") == nullptr);
}

TEST_CASE(Caps_parse_tolerance) {
    auto e = ParseCapabilities("{}");
    REQUIRE(e.has_value());
    CHECK_EQ(e->schema, 1);
    CHECK(e->devices.empty() && e->filters.empty() && e->presenters.empty() && e->decoders.empty() && e->interop.empty());
    CHECK(!ParseCapabilities("").has_value());
    CHECK(!ParseCapabilities("{not json").has_value());
    CHECK(!ParseCapabilities("[1,2]").has_value());
    CHECK(!ParseCapabilities("42").has_value());
    auto w = ParseCapabilities(R"({"schema":"x","adapter":5,"features":"a","devices":{"a":1},"decoders":[1,"x",{"codec":7}],
        "filters":[{"name":1,"formats":2,"pix_fmts":"cuda","hw_device":"true"}],"interop":"x","presenters":[{"name":"d3d11","inputs":"x","vp":3,"import":[1]}]})");
    REQUIRE(w.has_value());
    CHECK_EQ(w->schema, 1);
    CHECK(w->features.empty() && w->devices.empty());
    CHECK_EQ(w->decoders.size(), (size_t)1);
    CHECK_EQ(w->decoders[0].codec, "");
    REQUIRE(w->filters.size() == 1);
    CHECK(w->filters[0].formats == FilterCap::Formats::Unknown);
    CHECK(w->filters[0].pixFmts.empty());
    CHECK(!w->filters[0].hwDevice);
    REQUIRE(w->presenters.size() == 1);
    CHECK(w->presenters[0].inputs.empty());
    CHECK(!w->presenters[0].importCuda);
    CHECK(!w->presenters[0].importCudaZeroCopy);
    auto o = ParseCapabilities(R"({"filters":[{"name":"a"},{"name":"b","options":[]}]})");
    REQUIRE(o.has_value());
    CHECK(!o->filters[0].optionsKnown);
    CHECK(o->filters[1].optionsKnown);
}

TEST_CASE(Caps_round_trip_every_fake) {
    for (auto& [name, c] : AllFakes()) {
        std::string j = CapabilitiesToJson(c);
        auto p = ParseCapabilities(j);
        CHECK_MSG(p.has_value(), name);
        if (!p) continue;
        CHECK_MSG(CapabilitiesToJson(*p) == j, name);
        CHECK_EQ(p->source, c.source);
        CHECK_EQ(p->filters.size(), c.filters.size());
        CHECK_EQ(p->devices.size(), c.devices.size());
        CHECK_EQ(p->decoders.size(), c.decoders.size());
        CHECK_EQ(p->interop.size(), c.interop.size());
        CHECK_EQ(p->presenters.size(), c.presenters.size());
        CHECK_EQ(p->features.size(), c.features.size());
        CHECK_EQ(p->adapter.vendorId, c.adapter.vendorId);
        for (size_t i = 0; i < c.filters.size() && i < p->filters.size(); ++i) {
            CHECK_EQ(p->filters[i].name, c.filters[i].name);
            CHECK(p->filters[i].formats == c.filters[i].formats);
            CHECK_EQ(p->filters[i].optionsKnown, c.filters[i].optionsKnown);
            CHECK_EQ(p->filters[i].hwDevice, c.filters[i].hwDevice);
            CHECK(p->filters[i].pixFmts == c.filters[i].pixFmts);
            CHECK(p->filters[i].options == c.filters[i].options);
        }
        for (size_t i = 0; i < c.presenters.size() && i < p->presenters.size(); ++i) {
            CHECK(p->presenters[i].inputs == c.presenters[i].inputs);
            CHECK_EQ(p->presenters[i].vpSaturationMax, c.presenters[i].vpSaturationMax);
            CHECK_EQ(p->presenters[i].importCuda, c.presenters[i].importCuda);
            CHECK_EQ(p->presenters[i].importCudaZeroCopy, c.presenters[i].importCudaZeroCopy);
        }
    }
}

TEST_CASE(Caps_domain_names) {
    CHECK_EQ(std::string(DomainName(FrameDomain::System)), "system");
    CHECK_EQ(std::string(DomainName(FrameDomain::Cuda)), "cuda");
    CHECK_EQ(std::string(DomainName(FrameDomain::D3D11)), "d3d11");
    CHECK_EQ(std::string(DomainName(FrameDomain::D3D12)), "d3d12");
    CHECK_EQ(std::string(DomainName(FrameDomain::OpenCl)), "opencl");
    CHECK_EQ(std::string(DomainName(FrameDomain::Qsv)), "qsv");
    CHECK_EQ(std::string(DomainName(FrameDomain::Dxva2)), "dxva2");
    CHECK_EQ(std::string(DomainName(FrameDomain::Vulkan)), "vulkan");
    CHECK_EQ(std::string(DomainName(FrameDomain::Any)), "any");
    CHECK(DomainFromName("cuda") == FrameDomain::Cuda);
    CHECK(DomainFromName("d3d11") == FrameDomain::D3D11);
    CHECK(DomainFromName("d3d11va") == FrameDomain::D3D11);
    CHECK(DomainFromName("d3d12va") == FrameDomain::D3D12);
    CHECK(DomainFromName("dxva2_vld") == FrameDomain::Dxva2);
    CHECK(DomainFromName("opencl") == FrameDomain::OpenCl);
    CHECK(DomainFromName("any") == FrameDomain::Any);
    CHECK(DomainFromName("system") == FrameDomain::System);
    for (const char* sw : {"yuv420p", "nv12", "p010le", "yuv420p10le", "gray", "rgb24", "bgra"})
        CHECK_MSG(DomainFromName(sw) == FrameDomain::System, sw);
    CHECK(!DomainFromName("").has_value());
    CHECK(!DomainFromName("YUV420P").has_value());
    CHECK(!DomainFromName("a b").has_value());
    CHECK(!DomainFromName("x-y").has_value());
    CHECK(DomainOfDeviceType("d3d11va") == FrameDomain::D3D11);
    CHECK(DomainOfDeviceType("d3d12va") == FrameDomain::D3D12);
    CHECK(DomainOfDeviceType("cuda") == FrameDomain::Cuda);
    CHECK(DomainOfDeviceType("dxva2") == FrameDomain::Dxva2);
    CHECK(DomainOfDeviceType("qsv") == FrameDomain::Qsv);
    CHECK(DomainOfDeviceType("opencl") == FrameDomain::OpenCl);
    CHECK(DomainOfDeviceType("vulkan") == FrameDomain::Vulkan);
    CHECK(!DomainOfDeviceType("d3d11").has_value());
    CHECK(!DomainOfDeviceType("").has_value());
    for (FrameDomain d : {FrameDomain::D3D11, FrameDomain::D3D12, FrameDomain::Cuda, FrameDomain::Dxva2, FrameDomain::Qsv, FrameDomain::OpenCl, FrameDomain::Vulkan}) {
        CHECK(IsHardware(d));
        CHECK(DomainOfDeviceType(DeviceTypeOfDomain(d)) == d);
    }
    CHECK_EQ(DeviceTypeOfDomain(FrameDomain::D3D11), "d3d11va");
    CHECK_EQ(DeviceTypeOfDomain(FrameDomain::System), "");
    CHECK_EQ(DeviceTypeOfDomain(FrameDomain::Any), "");
    CHECK(!IsHardware(FrameDomain::System));
    CHECK(!IsHardware(FrameDomain::Any));
}

TEST_CASE(Caps_fake_nvidia_contents) {
    auto c = fakecaps::Nvidia();
    CHECK_EQ(c.adapter.vendorId, 0x10DE);
    for (const char* t : {"cuda", "d3d11va", "d3d12va", "dxva2"}) CHECK_MSG(c.DeviceOk(t), t);
    CHECK(!c.DeviceOk("qsv") && !c.DeviceOk("opencl"));
    CHECK(c.HasFeature("hwframes_keep"));
    CHECK(c.HasFeature("presenter_d3d11"));
    CHECK(c.Filter("ivtc_cuda") != nullptr);
    CHECK(c.Filter("tonemap_cuda") != nullptr);
    for (const char* f : {"bwdif_cuda", "yadif_cuda", "scale_cuda", "transpose_cuda", "ivtc_d3d11", "ivtc_cuda", "fieldmatch", "decimate", "hwmap", "setpts"})
        CHECK_MSG(c.Filter(f) != nullptr, f);
    CHECK(c.Filter("bwdif_cuda")->optionsKnown);
    CHECK(c.Filter("eq")->pixFmts.size() == 4);
    CHECK(!c.Filter("eq")->optionsKnown);
    CHECK(c.Filter("hwupload")->hwDevice);
    CHECK(!c.Filter("hwdownload")->hwDevice);
    CHECK(c.Decoder("h264", "cuda") != nullptr);
    CHECK(c.Decoder("av1", "d3d11va") != nullptr);
    CHECK(c.Decoder("av1", "dxva2") == nullptr);
    CHECK(c.Decoder("h264", "qsv") == nullptr);
    CHECK_EQ(c.Decoder("mpeg2video", "cuda")->maxWidth, 4096);
    CHECK_EQ(c.Decoder("hevc", "d3d11va")->bitDepths.size(), (size_t)2);
    CHECK(c.interop.empty());
    CHECK(c.Filter("ivtc_opencl") == nullptr);
    CHECK(c.Presenter("sdl") != nullptr);
    REQUIRE(c.Presenter("d3d11") != nullptr);
    CHECK(c.Presenter("d3d11")->importCuda);
    CHECK(!c.Presenter("d3d11")->importCudaZeroCopy);
    CHECK(fakecaps::Nvidia(true, true, true).Presenter("d3d11")->importCudaZeroCopy);
    CHECK(!fakecaps::Nvidia(true, false).Presenter("d3d11")->importCuda);
    auto np = fakecaps::Nvidia(false, false);
    CHECK(np.Presenter("d3d11") == nullptr);
    CHECK(!np.HasFeature("presenter_d3d11"));
    auto nv = fakecaps::NvidiaFutureIvtcVariant();
    REQUIRE(nv.Filter("ivtc_npp") != nullptr);
    CHECK(nv.Filter("ivtc_npp")->formats == FilterCap::Formats::Dynamic);
    CHECK(nv.Filter("ivtc_npp")->pixFmts.empty());
    CHECK(nv.Filter("ivtc_cuda") != nullptr);
}

TEST_CASE(Caps_fake_other_contents) {
    auto i = fakecaps::Intel();
    CHECK_EQ(i.adapter.vendorId, 0x8086);
    CHECK(i.DeviceOk("qsv") && i.DeviceOk("d3d11va") && !i.DeviceOk("opencl") && !i.DeviceOk("cuda"));
    CHECK(!i.InteropOk(FrameDomain::D3D11, FrameDomain::OpenCl, "map"));
    CHECK(i.InteropOk(FrameDomain::D3D11, FrameDomain::Qsv));
    CHECK(i.Filter("vpp_qsv") != nullptr && i.Filter("scale_qsv") != nullptr && i.Filter("deinterlace_qsv") != nullptr);
    CHECK(!i.Presenter("d3d11")->importCuda);
    auto a = fakecaps::Amd();
    CHECK_EQ(a.adapter.vendorId, 0x1002);
    CHECK(!a.InteropOk(FrameDomain::D3D11, FrameDomain::OpenCl));
    CHECK(!a.InteropOk(FrameDomain::D3D11, FrameDomain::Qsv));
    auto n = fakecaps::None();
    CHECK(n.devices.empty() && n.decoders.empty() && n.features.empty() && n.interop.empty());
    CHECK(n.Filter("fieldmatch") != nullptr && n.Filter("hwupload") != nullptr);
    CHECK(n.Presenter("d3d11") == nullptr);
    auto l = fakecaps::Legacy();
    CHECK_EQ(l.source, "legacy");
    CHECK(!l.DeviceOk("opencl"));
    CHECK(l.Filter("ivtc_opencl") == nullptr);
    CHECK(l.decoders.empty() && l.features.empty());
    CHECK(!l.HasFeature("hwframes_keep"));
    CHECK_EQ(l.presenters.size(), (size_t)1);
    CHECK(fakecaps::Nvidia().source.rfind("fake:", 0) == 0);
}

TEST_CASE(Caps_cuda_zero_copy_round_trip_and_old_json_default) {
    auto c = ParseCapabilities(R"({"presenters":[{"name":"d3d11","import":{"cuda":true,"cuda_zero_copy":true}}]})");
    REQUIRE(c.has_value());
    REQUIRE(c->Presenter("d3d11") != nullptr);
    CHECK(c->Presenter("d3d11")->importCudaZeroCopy);
    auto copy = ParseCapabilities(CapabilitiesToJson(*c));
    REQUIRE(copy.has_value());
    CHECK(copy->Presenter("d3d11")->importCudaZeroCopy);
    auto old = ParseCapabilities(R"({"presenters":[{"name":"d3d11","import":{"cuda":true}}]})");
    REQUIRE(old.has_value());
    CHECK(!old->Presenter("d3d11")->importCudaZeroCopy);
}

TEST_CASE(Caps_flags_combed_parses_defaults_false_and_round_trips) {
    auto c = ParseCapabilities(R"({"filters": [{"name": "ivtc_cuda", "formats": "list", "pix_fmts": ["cuda"], "flags_combed": true},
                                               {"name": "ivtc_d3d11", "formats": "list", "pix_fmts": ["d3d11"]}]})");
    REQUIRE(c.has_value());
    REQUIRE(c->filters.size() == 2);
    CHECK(c->filters[0].flagsCombed);
    CHECK(!c->filters[1].flagsCombed);
    std::string j = CapabilitiesToJson(*c);
    CHECK(j.find("flags_combed") != std::string::npos);
    auto p = ParseCapabilities(j);
    REQUIRE(p.has_value());
    CHECK(p->filters[0].flagsCombed);
    CHECK(!p->filters[1].flagsCombed);
    CHECK_EQ(CapabilitiesToJson(*p), j);
}

TEST_CASE(Caps_merge_build_table_with_runtime_probe) {
    auto table = ParseCapabilities(R"({"schema": 1, "source": "build", "ffmpeg": "8.1.3-Jellyfin",
        "features": ["hwframes_keep", "presenter_params", "upload_hw_sink"],
        "decoders": [{"codec": "h264", "hwaccel": "cuda", "decoder": "h264"}, {"codec": "av1", "hwaccel": "cuda", "decoder": "av1"},
                     {"codec": "hevc", "hwaccel": "d3d11va", "decoder": "hevc"}],
        "filters": [{"name": "ivtc_cuda", "formats": "list", "pix_fmts": ["cuda"], "options": ["tff"], "hw_device": true},
                    {"name": "bwdif", "formats": "list", "pix_fmts": ["yuv420p"]}]})");
    auto probe = ParseCapabilities(R"({"schema": 1, "source": "probe", "ffmpeg": "8.1.3-Jellyfin",
        "features": ["presenter_d3d11"],
        "devices": [{"type": "cuda", "ok": true, "name": "GPU", "error": ""}],
        "decoders": [{"codec": "h264", "hwaccel": "cuda", "decoder": "h264", "ok": true, "bit_depths": [8], "max_width": 4096},
                     {"codec": "av1", "hwaccel": "cuda", "decoder": "libdav1d", "ok": true, "bit_depths": [8, 10]}],
        "presenters": [{"name": "sdl", "ok": true, "inputs": ["system"]}]})");
    REQUIRE(table.has_value() && probe.has_value());
    CapabilitySet m = MergeBuildCapabilities(*probe, *table);
    CHECK_EQ(m.source, std::string("probe"));
    REQUIRE(m.filters.size() == 2);
    CHECK(m.Filter("ivtc_cuda") && m.Filter("ivtc_cuda")->hwDevice);
    REQUIRE(m.features.size() == 4);
    CHECK_EQ(m.features[0], std::string("hwframes_keep"));
    CHECK_EQ(m.features[3], std::string("presenter_d3d11"));
    REQUIRE(m.decoders.size() == 1);   // av1: the probe names a decoder the table does not; hevc/d3d11va was not probed
    CHECK_EQ(m.decoders[0].codec, std::string("h264"));
    CHECK_EQ(m.decoders[0].maxWidth, 4096);
    CHECK(m.DeviceOk("cuda"));
    REQUIRE(m.Presenter("sdl") != nullptr);
    auto round = ParseCapabilities(CapabilitiesToJson(m));
    REQUIRE(round.has_value());
    CHECK_EQ(round->filters.size(), m.filters.size());
}

TEST_CASE(Caps_build_table_is_this_ffmpeg_build) {
    CapabilitySet b = BuildCapabilities();
    CHECK_EQ(b.source, std::string("build"));
    CHECK(!b.ffmpegVersion.empty());
    CHECK(b.HasFeature("hwframes_keep") && b.HasFeature("upload_hw_sink") && !b.HasFeature("presenter_d3d11"));
    for (const char* n : {"fieldmatch", "decimate", "bwdif", "yadif", "hwupload", "hwdownload", "ivtc_cuda"})
        CHECK_MSG(b.Filter(n) != nullptr, n);
    const FilterCap* ivtc = b.Filter("ivtc_cuda");
    REQUIRE(ivtc != nullptr);
    CHECK(ivtc->formats == FilterCap::Formats::List && ivtc->hwDevice && ivtc->optionsKnown);
    REQUIRE(ivtc->pixFmts.size() == 1);
    CHECK_EQ(ivtc->pixFmts[0], std::string("cuda"));
    const DecoderCap* h264 = b.Decoder("h264", "d3d11va");
    REQUIRE(h264 != nullptr);
    CHECK_EQ(h264->decoder, std::string("h264"));
}
