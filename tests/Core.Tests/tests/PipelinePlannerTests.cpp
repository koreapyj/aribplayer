// Pure logic with fake capabilities.
#include "wvptest.h"

#include <algorithm>
#include <cstdio>

#include "wvp/FilterManager.h"
#include "wvp/PipelinePlanner.h"

using namespace wvp;

namespace {

enum class Case { A, B, C };

PlanRequest Req(Case c) {
    PlanRequest r;
    switch (c) {
    case Case::A:
        r.content = { "mpeg2video", 8, 1920, 1080, false };
        r.settings.deinterlace = DeinterlaceMode::Ivtc;
        break;
    case Case::B:
        r.content = { "h264", 8, 1920, 1080, false };
        r.settings.deinterlace = DeinterlaceMode::On;
        r.settings.deinterlacePriority = DeinterlacePriority::Quality;
        break;
    case Case::C:
        r.content = { "hevc", 10, 1920, 1080, false };
        r.settings.deinterlace = DeinterlaceMode::Off;
        r.settings.brightness = 0.1;
        r.settings.contrast = 1.2;
        r.settings.rotation = Rotation::Cw90;
        break;
    }
    return r;
}

Plan Run(const CapabilitySet& caps, const PlanRequest& r) { return PipelinePlanner(caps).Make(r); }

void Expect(const Plan& p, int host, int dev, const char* hwaccel, bool keep, const char* presenter, const std::wstring& vf) {
    CHECK(p.ok);
    CHECK_EQ(p.cost.Weighted(), host);
    CHECK_EQ(p.cost.deviceCopies, dev);
    CHECK_EQ(p.hwaccel, std::string(hwaccel));
    CHECK_EQ(p.hwframesKeep, keep);
    CHECK_EQ(p.presenter, std::string(presenter));
    CHECK_EQ(p.videoFilter, vf);
    if (!p.ok || p.cost.Weighted() != host || p.cost.deviceCopies != dev || p.videoFilter != vf) std::printf("    plan: %s\n", p.Summary().c_str());
}

const wchar_t* kBwdif = L"bwdif=mode=send_frame:parity=auto:deint=all";
const wchar_t* kEqC = L"eq@color=brightness=0.1:contrast=1.2:saturation=1:gamma=1";

FilterCap* FindFilter(CapabilitySet& c, const std::string& n) {
    for (auto& f : c.filters) if (f.name == n) return &f;
    return nullptr;
}
void RemoveFilter(CapabilitySet& c, const std::string& n) {
    c.filters.erase(std::remove_if(c.filters.begin(), c.filters.end(), [&](const FilterCap& f) { return f.name == n; }), c.filters.end());
}

}

TEST_CASE(Plan_legacy_parity_with_FilterManager) {
    IvtcTuning tunings[2];
    tunings[1].fieldOrder = 1;
    tunings[1].combThreshold = 12.5;
    tunings[1].pixelFormat = L"yuv420p";
    const Rotation rots[] = { Rotation::None, Rotation::Cw90, Rotation::Rotate180, Rotation::Ccw90 };
    const DeinterlaceMode dms[] = { DeinterlaceMode::Off, DeinterlaceMode::On, DeinterlaceMode::Ivtc };
    const DeinterlacePriority prios[] = { DeinterlacePriority::Efficiency, DeinterlacePriority::Quality };
    long cases = 0;
    {
        PipelinePlanner planner(fakecaps::Legacy());
        for (const IvtcTuning& t : tunings)
        for (DeinterlaceMode dm : dms)
        for (DeinterlacePriority prio : prios)
        for (int rate = 0; rate < 2; rate++)
        for (int only = 0; only < 2; only++)
        for (Rotation rot : rots)
        for (int fh = 0; fh < 2; fh++)
        for (int fv = 0; fv < 2; fv++)
        for (int eq = 0; eq < 2; eq++)
        for (int sp = 0; sp < 2; sp++)
        for (int adv = 0; adv < 2; adv++)
        for (int raw = 0; raw < 2; raw++) {
            FilterSettings s;
            s.deinterlace = dm;
            s.deinterlacePriority = prio;
            s.deinterlaceRate = rate ? DeinterlaceRate::Field : DeinterlaceRate::Frame;
            s.deinterlaceOnlyInterlaced = only != 0;
            s.rotation = rot;
            s.flipHorizontal = fh != 0;
            s.flipVertical = fv != 0;
            if (eq) { s.brightness = 0.2; s.contrast = 1.5; s.saturation = 0.5; s.gamma = 1.1; }
            s.speed = sp ? 1.5 : 1.0;
            s.advancedMode = adv != 0;
            if (raw) s.customVideoFilter = L"hue=s=0";
            FilterManager fm;
            fm.SetIvtcTuning(t);
            fm.Apply(s);
            PlanRequest r;
            r.settings = s;
            r.ivtc = t;
            r.policy = PlanPolicy::Legacy;
            Plan p = planner.Make(r);
            cases++;
            if (p.videoFilter != fm.VideoFilter() || !p.filterHwDevice.empty()) {
                CHECK_EQ(p.videoFilter, fm.VideoFilter());
                CHECK(p.filterHwDevice.empty());
                return;
            }
            CHECK(p.ok);
            CHECK(p.hwaccel.empty());
            CHECK_EQ(p.presenter, std::string("sdl"));
        }
    }
    CHECK(cases > 10000);
}

TEST_CASE(Plan_legacy_open_options_only_vf) {
    PipelinePlanner planner(fakecaps::Legacy());
    PlanRequest r;
    r.policy = PlanPolicy::Legacy;
    r.settings.deinterlace = DeinterlaceMode::Ivtc;
    Plan p = planner.Make(r);
    CHECK_EQ(p.videoFilter, L"fieldmatch,bwdif=mode=send_frame:parity=auto:deint=interlaced,decimate");
    REQUIRE(p.openOptions.size() == 1);
    CHECK_EQ(p.openOptions[0].first, L"vf");
    PlanRequest e;
    e.policy = PlanPolicy::Legacy;
    e.settings.deinterlace = DeinterlaceMode::Off;
    Plan pe = planner.Make(e);
    CHECK(pe.openOptions.empty());
    CHECK_EQ(pe.videoFilter, L"");
}

TEST_CASE(Plan_matrix_dump) {
    struct F { const char* n; CapabilitySet c; };
    F fakes[] = { { "Nvidia", fakecaps::Nvidia() }, { "NvidiaFutureIvtcVariant", fakecaps::NvidiaFutureIvtcVariant() }, { "Intel", fakecaps::Intel() },
                  { "Amd", fakecaps::Amd() },
                  { "None", fakecaps::None() } };
    for (const F& f : fakes)
        for (Case c : { Case::A, Case::B, Case::C }) {
            Plan p = Run(f.c, Req(c));
            std::printf("    [%s case %c] %s | vf=%ls dev=%s\n", f.n, 'A' + static_cast<int>(c), p.Summary().c_str(), p.videoFilter.c_str(),
                        p.filterHwDevice.c_str());
            CHECK(p.ok);
            CHECK(!p.Summary().empty());
        }
}

TEST_CASE(Plan_nvidia_cases) {
    CapabilitySet nv = fakecaps::Nvidia();
    Expect(Run(nv, Req(Case::A)), 0, 0, "d3d11va", true, "d3d11", L"ivtc_d3d11");
    Expect(Run(nv, Req(Case::B)), 0, 0, "d3d11va", true, "d3d11", L"bwdif_d3d11=mode=send_frame:parity=auto:deint=all");
    Plan c = Run(nv, Req(Case::C));
    Expect(c, 0, 0, "d3d11va", true, "d3d11", L"");
    CHECK_EQ(c.presenterParams.rotation, 90);
    CHECK(c.presenterParams.eq);
    CHECK_EQ(c.presenterParams.brightness, 0.1);
    CHECK_EQ(c.presenterParams.contrast, 1.2);
    CHECK(c.filterHwDevice.empty());
    PlanRequest r = Req(Case::A);
    r.decoder = DecoderPref::PreferGpu;
    Expect(Run(nv, r), 0, 0, "d3d11va", true, "d3d11", L"ivtc_d3d11");
}

TEST_CASE(Plan_nvidia_cuda_cases_when_d3d11va_is_blacklisted) {
    CapabilitySet nv = fakecaps::Nvidia();
    for (Case c : {Case::A, Case::B}) {
        const wchar_t* vf = c == Case::A ? L"ivtc_cuda" : L"bwdif_cuda=mode=send_frame:parity=auto:deint=all";
        PlanRequest r = Req(c);
        r.blacklist.insert("hwaccel:d3d11va");
        Expect(Run(nv, r), 0, 1, "cuda", true, "d3d11", vf);
        PlanRequest f = Req(c);
        f.decoder = DecoderPref::Forced;
        f.forcedDecoder = "cuda";
        Expect(Run(nv, f), 0, 1, "cuda", true, "d3d11", vf);
    }
}

TEST_CASE(Plan_nvidia_zero_copy_presenter_cases) {
    CapabilitySet nv = fakecaps::Nvidia(true, true, true);
    // The real DLL lists cuda among accepted inputs as well as in import.cuda.
    // Import semantics (and its copy count) must win over generic direct-input handling.
    nv.presenters.back().inputs.push_back(FrameDomain::Cuda);
    for (Case c : {Case::A, Case::B}) {
        PlanRequest cuda = Req(c);
        cuda.blacklist.insert("hwaccel:d3d11va");
        Plan p = Run(nv, cuda);
        Expect(p, 0, 0, "cuda", true, "d3d11",
               c == Case::A ? L"ivtc_cuda" : L"bwdif_cuda=mode=send_frame:parity=auto:deint=all");
        CHECK(p.Summary().find("present:d3d11(import cuda)") != std::string::npos);
        CHECK_EQ(p.steps.back().copy, CopyKind::None);
        CHECK_EQ(p.steps.back().copies, 0);
        CHECK_EQ(p.steps.back().reason, std::string("cuda zero-copy (registered pool)"));
        auto perPlane = fakecaps::Nvidia();
        perPlane.presenters.back().inputs.push_back(FrameDomain::Cuda);
        Plan old = Run(perPlane, cuda);
        CHECK_EQ(old.cost.hostCopies, 0);
        CHECK_EQ(old.cost.deviceCopies, 1);
        CHECK_EQ(old.steps.back().copy, CopyKind::Device);
        CHECK_EQ(old.steps.back().reason, std::string("per-plane import"));
    }
    Expect(Run(nv, Req(Case::C)), 0, 0, "d3d11va", true, "d3d11", L"");
}

TEST_CASE(Plan_nvidia_cuda_shader_absorbs_gamma_and_flip_independently_of_vp) {
    CapabilitySet nv = fakecaps::Nvidia(true, true, true);
    PresenterCap& pres = nv.presenters.back();
    pres.shader = false;
    pres.vpMirror = false;
    pres.vpBrightnessMin = -34.016; pres.vpBrightnessMax = 34.016;
    pres.vpContrastMin = .4; pres.vpContrastMax = 1.6;
    pres.vpSaturationMin = .13; pres.vpSaturationMax = 1.87;
    PlanRequest r = Req(Case::C);
    r.settings.gamma = 1.5;
    r.settings.flipHorizontal = true;
    Plan p = Run(nv, r);
    Expect(p, 0, 0, "cuda", true, "d3d11", L"");
    CHECK(p.presenterParams.flipH && p.presenterParams.eq);
    CHECK_EQ(p.presenterParams.gamma, 1.5);
    CHECK_EQ(p.presenterParams.rotation, 90);
    // Without registered-pool zero-copy, a one-device-copy CUDA path still beats
    // D3D11VA -> CPU gamma/flip -> upload (host copies sort first).
    nv.presenters.back().importCudaZeroCopy = false;
    Plan copied = Run(nv, r);
    Expect(copied, 0, 1, "cuda", true, "d3d11", L"");
    CHECK(copied.presenterParams.flipH && copied.presenterParams.eq);
}

TEST_CASE(Plan_blacklisted_cuda_import_cannot_leave_shader_only_params_on_vp_path) {
    auto nv = fakecaps::Nvidia(true, true, true);
    nv.presenters.back().inputs.push_back(FrameDomain::Cuda);
    nv.presenters.back().shader = false;
    nv.presenters.back().vpMirror = false;
    PlanRequest r = Req(Case::C);
    r.settings.gamma = 1.5;
    r.settings.flipHorizontal = true;
    r.blacklist.insert("interop:cuda>d3d11");
    Plan p = Run(nv, r);
    REQUIRE(p.ok);
    // CUDA frames may be downloaded by the fallback, but their CUDA-only shader
    // absorption must not survive once the actual presenter input is System.
    CHECK(!p.presenterParams.eq);
    CHECK(!p.presenterParams.flipH);
    CHECK(p.videoFilter.find(L"eq@color") != std::wstring::npos);
    CHECK(p.videoFilter.find(L"hflip") != std::wstring::npos);
}

TEST_CASE(Plan_shader_flag_expands_d3d11_input_only) {
    auto nv = fakecaps::Nvidia(true, true, true);
    auto& pres = nv.presenters.back();
    pres.shader = false; pres.vpMirror = false; pres.importCuda = false;
    PlanRequest r = Req(Case::C);
    r.decoder = DecoderPref::Forced;
    r.forcedDecoder = "d3d11va";
    r.settings.gamma = 1.5;
    r.settings.flipHorizontal = true;
    Plan vp = Run(nv, r);
    CHECK(vp.ok);
    CHECK(vp.presenterParams.IsIdentity() || !vp.presenterParams.eq);
    CHECK(vp.videoFilter.find(L"eq@color") != std::wstring::npos);
    CHECK(vp.videoFilter.find(L"hflip") != std::wstring::npos);
    pres.shader = true;
    Plan shader = Run(nv, r);
    Expect(shader, 0, 0, "d3d11va", true, "d3d11", L"");
    CHECK(shader.presenterParams.eq && shader.presenterParams.flipH);
}

TEST_CASE(Plan_nvidia_ivtc_npp_ties_to_ivtc_cuda) {
    CapabilitySet nv = fakecaps::NvidiaFutureIvtcVariant();
    PlanRequest cuda = Req(Case::A);
    cuda.blacklist.insert("hwaccel:d3d11va");
    Plan p = Run(nv, cuda);
    CHECK(p.ok);
    CHECK_EQ(p.cost.hostCopies, 0);
    CHECK_EQ(p.cost.deviceCopies, 1);
    CHECK(p.hwaccel == "cuda");
    // deterministic tie-break: discovery order (caps order), so ivtc_cuda wins while it is listed first
    CHECK(p.videoFilter == L"ivtc_cuda" || p.videoFilter == L"ivtc_npp");
    PlanRequest r = cuda;
    r.blacklist.insert("filter:ivtc_cuda");
    Plan q = Run(nv, r);
    Expect(q, 0, 1, "cuda", true, "d3d11", L"ivtc_npp");
    CHECK(!q.rejected.empty());
}

TEST_CASE(Plan_nvidia_presenter_variants) {
    Expect(Run(fakecaps::Nvidia(false), Req(Case::A)), 1, 0, "", false, "sdl", L"fieldmatch,bwdif=mode=send_frame:parity=auto:deint=interlaced,decimate");
    Expect(Run(fakecaps::Nvidia(true, false), Req(Case::A)), 0, 0, "d3d11va", true, "d3d11", L"ivtc_d3d11");
    // no cuda import: cuda ivtc then hwdownload + upload = 2 host copies, software decode + upload = 1
    PlanRequest noD3d11va = Req(Case::A);
    noD3d11va.blacklist.insert("hwaccel:d3d11va");
    noD3d11va.blacklist.insert("filter:ivtc_d3d11");
    Expect(Run(fakecaps::Nvidia(true, false), noD3d11va), 1, 0, "", false, "d3d11", L"fieldmatch,bwdif=mode=send_frame:parity=auto:deint=interlaced,decimate");
}

TEST_CASE(Plan_intel_amd_cases) {
    for (CapabilitySet caps : { fakecaps::Intel(), fakecaps::Amd() }) {
        Expect(Run(caps, Req(Case::A)), 0, 0, "d3d11va", true, "d3d11", L"ivtc_d3d11");
        Expect(Run(caps, Req(Case::B)), 0, 0, "d3d11va", true, "d3d11", L"bwdif_d3d11=mode=send_frame:parity=auto:deint=all");
        PlanRequest cpuIvtc = Req(Case::A);
        cpuIvtc.blacklist.insert("filter:ivtc_d3d11");
        Expect(Run(caps, cpuIvtc), 1, 0, "", false, "d3d11", L"fieldmatch,bwdif=mode=send_frame:parity=auto:deint=interlaced,decimate");
        PlanRequest cpuDeint = Req(Case::B);
        cpuDeint.blacklist.insert("filter:bwdif_d3d11");
        Plan b = Run(caps, cpuDeint);
        CHECK(b.ok);
        CHECK_EQ(b.cost.Weighted(), 2);
        CHECK_EQ(b.hwaccel, std::string("d3d11va"));
        CHECK(!b.hwframesKeep);
        Plan c = Run(caps, Req(Case::C));
        CHECK(c.ok);
        CHECK_EQ(c.cost.hostCopies, 0);
        CHECK_EQ(c.hwaccel, std::string("d3d11va"));
    }
}

TEST_CASE(Plan_ivtc_d3d11_without_cuda_is_zero_copy) {
    for (CapabilitySet caps : { fakecaps::Intel(), fakecaps::Amd() }) {
        Plan a = Run(caps, Req(Case::A));
        Expect(a, 0, 0, "d3d11va", true, "d3d11", L"ivtc_d3d11");
        bool download = false;
        for (auto& s : a.steps) download = download || s.name == "hwdownload";
        CHECK(!download);

        PlanRequest sw = Req(Case::A);
        sw.decoder = DecoderPref::Software;
        Plan b = Run(caps, sw);
        CHECK(b.ok);
        CHECK(b.hwaccel.empty());
        CHECK_EQ(b.presenter, std::string("d3d11"));
        CHECK_EQ(b.cost.hostCopies, 1);
        CHECK_EQ(b.cost.deviceCopies, 0);
        CHECK(b.videoFilter.find(L"hwupload") != std::wstring::npos);
        CHECK(b.videoFilter.find(L"ivtc_d3d11") != std::wstring::npos);
        CHECK(b.videoFilter.find(L"hwdownload") == std::wstring::npos);
        if (b.cost.hostCopies != 1 || b.videoFilter.find(L"ivtc_d3d11") == std::wstring::npos) std::printf("    plan: %s\n", b.Summary().c_str());
    }
}

TEST_CASE(Plan_none_cases) {
    CapabilitySet none = fakecaps::None();
    Expect(Run(none, Req(Case::A)), 1, 0, "", false, "sdl", L"fieldmatch,bwdif=mode=send_frame:parity=auto:deint=interlaced,decimate");
    // software-decode penalty: floor(DecodeLoad) host copies on top of the sdl upload
    Expect(Run(none, Req(Case::B)), 2, 0, "", false, "sdl", kBwdif);
    Expect(Run(none, Req(Case::C)), 3, 0, "", false, "sdl", std::wstring(L"transpose=1,") + kEqC);
}

TEST_CASE(Plan_discovers_new_hw_ivtc_without_table_change) {
    CapabilitySet caps = fakecaps::Nvidia();
    RemoveFilter(caps, "ivtc_cuda");
    RemoveFilter(caps, "ivtc_d3d11");
    FilterCap f;
    f.name = "ivtc_vulkan";
    f.formats = FilterCap::Formats::List;
    f.pixFmts = { "vulkan" };
    f.options = { "tff", "cthresh" };
    f.optionsKnown = true;
    f.hwDevice = true;
    caps.filters.push_back(f);
    caps.devices.push_back({ "vulkan", true, "fake vulkan", "" });
    caps.interop.push_back({ FrameDomain::D3D11, FrameDomain::Vulkan, "map", true, "" });
    for (auto& p : caps.presenters) if (p.name == "d3d11") p.inputs.push_back(FrameDomain::Vulkan);
    Plan p = Run(caps, Req(Case::A));
    Expect(p, 0, 0, "d3d11va", true, "d3d11", L"hwmap=derive_device=vulkan,ivtc_vulkan");
}

TEST_CASE(Plan_blacklist) {
    CapabilitySet nv = fakecaps::Nvidia();
    {
        for (const char* type : { "d3d11va", "cuda" }) {
            PlanRequest r = Req(Case::B);
            r.blacklist.insert(std::string("hwaccel:") + type);
            Plan p = Run(nv, r);
            CHECK(p.ok);
            CHECK(p.hwaccel != type);
            bool listed = false;
            for (auto& x : p.rejected) if (x.reason.find(std::string("hwaccel:") + type) != std::string::npos) listed = true;
            CHECK(listed);
        }
    }
    {
        PlanRequest r = Req(Case::A);
        r.blacklist.insert("filter:ivtc_d3d11");
        Expect(Run(nv, r), 0, 1, "cuda", true, "d3d11", L"ivtc_cuda");
        r.blacklist.insert("filter:ivtc_cuda");
        Expect(Run(nv, r), 1, 0, "", false, "d3d11", L"fieldmatch,bwdif=mode=send_frame:parity=auto:deint=interlaced,decimate");
    }
    {
        PlanRequest r = Req(Case::A);
        r.blacklist.insert("presenter:d3d11");
        Plan p = Run(nv, r);
        CHECK(p.ok);
        CHECK_EQ(p.presenter, std::string("sdl"));
    }
    {
        PlanRequest r = Req(Case::B);
        r.blacklist.insert("hwframes_keep");
        Plan p = Run(nv, r);
        CHECK(p.ok);
        CHECK(!p.hwframesKeep);
    }
    {
        PlanRequest r = Req(Case::A);
        r.blacklist.insert("hwaccel:d3d11va");
        r.blacklist.insert("interop:cuda>d3d11");
        Plan p = Run(nv, r);
        CHECK(p.ok);
        CHECK(p.cost.Weighted() >= 1);
    }
}

TEST_CASE(Plan_decoder_preferences) {
    CapabilitySet intel = fakecaps::Intel();
    {
        PlanRequest r = Req(Case::A);
        r.decoder = DecoderPref::PreferGpu;
        Expect(Run(intel, r), 0, 0, "d3d11va", true, "d3d11", L"ivtc_d3d11");
        r.blacklist.insert("filter:ivtc_d3d11");
        Plan p = Run(intel, r);
        // ivtc_d3d11 blacklisted: d3d11va keep -> hwdownload -> CPU recipe -> upload: 2 host copies
        CHECK_EQ(p.hwaccel, std::string("d3d11va"));
        CHECK_EQ(p.cost.Weighted(), 2);
    }
    {
        PlanRequest r = Req(Case::B);
        r.decoder = DecoderPref::Software;
        Plan p = Run(fakecaps::Nvidia(), r);
        CHECK(p.hwaccel.empty());
        CHECK(!p.hwframesKeep);
    }
    {
        PlanRequest r = Req(Case::B);
        r.decoder = DecoderPref::Forced;
        r.forcedDecoder = "d3d11va";
        Plan p = Run(fakecaps::Nvidia(), r);
        CHECK(p.ok);
        CHECK_EQ(p.hwaccel, std::string("d3d11va"));
    }
    {
        PlanRequest r = Req(Case::B);
        r.decoder = DecoderPref::Forced;
        r.forcedDecoder = "cuda";
        r.blacklist.insert("hwaccel:cuda");
        Plan p = Run(fakecaps::Nvidia(), r);
        CHECK(p.hwaccel.empty());
        CHECK(!p.notes.empty());
    }
    {
        PlanRequest r = Req(Case::B);
        r.decoder = DecoderPref::PreferGpu;
        Plan p = Run(fakecaps::None(), r);
        CHECK(p.ok);
        CHECK(p.hwaccel.empty());
        CHECK(!p.notes.empty());
    }
}

TEST_CASE(Plan_presenter_preferences) {
    {
        PlanRequest r = Req(Case::A);
        r.presenter = PresenterPref::Sdl;
        Plan p = Run(fakecaps::Intel(), r);
        CHECK_EQ(p.presenter, std::string("sdl"));
    }
    {
        PlanRequest r = Req(Case::A);
        r.presenter = PresenterPref::D3D11;
        Plan p = Run(fakecaps::Intel(), r);
        CHECK_EQ(p.presenter, std::string("d3d11"));
        Plan q = Run(fakecaps::None(), r);
        CHECK_EQ(q.presenter, std::string("sdl"));
        CHECK(!q.notes.empty());
    }
}

TEST_CASE(Plan_unknown_content_note) {
    PlanRequest r;
    r.settings.deinterlace = DeinterlaceMode::Ivtc;
    Plan p = Run(fakecaps::Nvidia(), r);
    CHECK(p.ok);
    bool note = false;
    for (auto& n : p.notes) if (n.find("content unknown") != std::string::npos) note = true;
    CHECK(note);
}

TEST_CASE(Plan_option_mapping_only_exposed_options) {
    CapabilitySet caps = fakecaps::Nvidia();
    FilterCap* f = FindFilter(caps, "ivtc_cuda");
    REQUIRE(f != nullptr);
    f->options = { "tff" };
    f->optionsKnown = true;
    PlanRequest r = Req(Case::A);
    r.ivtc.fieldOrder = 1;
    r.ivtc.combThreshold = 12;
    Plan d = Run(fakecaps::Nvidia(), r);
    CHECK_EQ(d.videoFilter, L"ivtc_d3d11=tff=1:cthresh=12");
    r.blacklist.insert("hwaccel:d3d11va");
    Plan p = Run(caps, r);
    CHECK_EQ(p.videoFilter, L"ivtc_cuda=tff=1");
    CapabilitySet full = fakecaps::Nvidia();
    Plan q = Run(full, r);
    CHECK_EQ(q.videoFilter, L"ivtc_cuda=tff=1:cthresh=12");
}

TEST_CASE(Plan_tokenizer) {
    auto t = PipelinePlanner::TokenizeChain(L"a=1:b=2, c , d=x\\,y,e='p,q'");
    REQUIRE(t.has_value());
    REQUIRE(t->size() == 4);
    CHECK_EQ((*t)[0], L"a=1:b=2");
    CHECK_EQ((*t)[1], L"c");
    CHECK_EQ((*t)[2], L"d=x\\,y");
    CHECK_EQ((*t)[3], L"e='p,q'");
    CHECK(!PipelinePlanner::TokenizeChain(L"split[a][b];[a]hflip").has_value());
    CHECK(!PipelinePlanner::TokenizeChain(L"[in]hue=s=0").has_value());
    auto e = PipelinePlanner::TokenizeChain(L"");
    REQUIRE(e.has_value());
    CHECK(e->empty());
    CHECK_EQ(PipelinePlanner::TokenFilterName(L" eq@color=brightness=1"), std::string("eq"));
    CHECK_EQ(PipelinePlanner::TokenFilterName(L"hue=s=0"), std::string("hue"));
    CHECK_EQ(PipelinePlanner::TokenFilterName(L"vflip"), std::string("vflip"));
}

TEST_CASE(Plan_raw_chain_transition_into_cuda) {
    PlanRequest r = Req(Case::B);
    r.settings.deinterlace = DeinterlaceMode::Off;
    r.settings.brightness = 0.1;
    r.settings.customVideoFilter = L"bwdif_cuda";
    r.decoder = DecoderPref::Software;
    r.presenter = PresenterPref::Sdl;
    Plan p = Run(fakecaps::Nvidia(), r);
    CHECK(p.ok);
    CHECK_EQ(p.videoFilter, L"eq@color=brightness=0.1:contrast=1:saturation=1:gamma=1,format=nv12,hwupload,bwdif_cuda,hwdownload,format=nv12");
    CHECK_EQ(p.filterHwDevice, std::string("cuda"));
    r.presenter = PresenterPref::Auto;
    Plan q = Run(fakecaps::Nvidia(), r);
    CHECK_EQ(q.presenter, std::string("d3d11"));
    CHECK_EQ(q.videoFilter, L"eq@color=brightness=0.1:contrast=1:saturation=1:gamma=1,format=nv12,hwupload,bwdif_cuda");
    CHECK_EQ(q.cost.deviceCopies, 1);
    CHECK_EQ(q.cost.hostCopies, 1);
    r.decoder = DecoderPref::Auto;
    r.settings.customVideoFilter.clear();
    r.settings.brightness = 0;
    r.settings.deinterlace = DeinterlaceMode::Ivtc;
    Plan keep = Run(fakecaps::Nvidia(), r);
    CHECK_MSG(keep.hwframesKeep && keep.cost.hostCopies == 0, keep.Summary());
}

TEST_CASE(Plan_raw_chain_verbatim_and_opaque) {
    PlanRequest r = Req(Case::B);
    r.settings.deinterlace = DeinterlaceMode::Off;
    r.settings.customVideoFilter = L"hue=s=0 , vflip";
    Plan p = Run(fakecaps::None(), r);
    CHECK_EQ(p.videoFilter, L"hue=s=0 , vflip");
    r.settings.customVideoFilter = L"split[a][b];[a]hflip[c];[c][b]hstack";
    Plan q = Run(fakecaps::None(), r);
    CHECK_EQ(q.videoFilter, L"split[a][b];[a]hflip[c];[c][b]hstack");
    r.settings.customVideoFilter = L"format=nv12,hwupload,ivtc_opencl,hwdownload,format=nv12";
    Plan o = Run(fakecaps::Nvidia(), r);
    CHECK(o.ok);
    CHECK_EQ(o.videoFilter, L"format=nv12,hwupload,ivtc_opencl,hwdownload,format=nv12");
    CHECK(o.filterHwDevice.empty());   // opaque custom graph: the user supplies any hw device themselves
}

TEST_CASE(Plan_raw_blocks_absorption) {
    PlanRequest r = Req(Case::C);
    r.settings.customVideoFilter = L"hue=s=0";
    Plan p = Run(fakecaps::Nvidia(), r);
    CHECK(p.ok);
    CHECK(p.presenterParams.IsIdentity());
    std::printf("    raw-absorb plan: %s | %ls\n", p.Summary().c_str(), p.videoFilter.c_str());
    CHECK(p.videoFilter.size() >= 7 && p.videoFilter.substr(p.videoFilter.size() - 7) == L"hue=s=0");
}

TEST_CASE(Plan_advanced_mode_skips_presets) {
    PlanRequest r = Req(Case::C);
    r.settings.advancedMode = true;
    r.settings.customVideoFilter = L"hue=s=0";
    Plan p = Run(fakecaps::None(), r);
    CHECK_EQ(p.videoFilter, L"hue=s=0");
}

TEST_CASE(Plan_diff) {
    CapabilitySet none = fakecaps::None();
    PlanRequest r = Req(Case::C);
    Plan a = Run(none, r);
    CHECK_EQ(PipelinePlanner::Diff(a, a).kind, PlanDeltaKind::None);
    PlanRequest r2 = r;
    r2.settings.brightness = 0.2;
    r2.settings.gamma = 1.5;
    Plan b = Run(none, r2);
    PlanDelta d = PipelinePlanner::Diff(a, b);
    CHECK(d.kind == PlanDeltaKind::FilterCommands);
    REQUIRE(d.commands.size() == 2);
    CHECK_EQ(d.commands[0].target, L"eq@color");
    CHECK_EQ(d.commands[0].command, L"brightness");
    CHECK_EQ(d.commands[0].argument, L"0.2");
    CHECK_EQ(d.commands[1].command, L"gamma");
    CHECK_EQ(d.commands[1].argument, L"1.5");
    PlanRequest r3 = r;
    r3.settings.rotation = Rotation::None;
    CHECK(PipelinePlanner::Diff(a, Run(none, r3)).kind == PlanDeltaKind::GraphRebuild);
    CapabilitySet nv = fakecaps::Nvidia();
    Plan x = Run(nv, r);
    PlanRequest r4 = r;
    r4.settings.rotation = Rotation::Ccw90;
    Plan y = Run(nv, r4);
    PlanDelta pd = PipelinePlanner::Diff(x, y);
    CHECK(pd.kind == PlanDeltaKind::PresenterParams);
    CHECK_EQ(pd.params.rotation, 270);
    PlanRequest r5 = r;
    r5.decoder = DecoderPref::Software;
    CHECK(PipelinePlanner::Diff(x, Run(nv, r5)).kind == PlanDeltaKind::Reopen);
}

TEST_CASE(Plan_decode_load_and_cost_ordering) {
    CHECK_EQ(PipelinePlanner::DecodeLoad({ "mpeg2video", 8, 1920, 1080, false }), 0.5);
    CHECK_EQ(PipelinePlanner::DecodeLoad({ "h264", 8, 1920, 1080, false }), 1.0);
    CHECK_EQ(PipelinePlanner::DecodeLoad({ "hevc", 10, 3840, 2160, false }), 10.0);
    CHECK_EQ(PipelinePlanner::DecodeLoad({ "av1", 8, 0, 0, false }), 3.0);
    CHECK_EQ(PipelinePlanner::DecodeLoad({ "", 8, 0, 0, false }), 1.0);
    PlanCost a{ 0, 1, 0, 0.5, 3 }, b{ 1, 0, 0, 0, 1 }, c{ 0, 1, 0, 0.5 + 1e-12, 3 }, d{ 0, 1, 0, 0.6, 2 }, e{ 0, 1, 0, 0.5, 4 };
    PlanCost f{ 0, 0, 2, 0, 0 }, g{ 1, 0, 0, 1.0, 5 };
    CHECK(g < f);   // penalty counts as host copies
    CHECK(a < b);
    CHECK(!(b < a));
    CHECK(a == c);
    CHECK(!(a < c));
    CHECK(a < d);
    CHECK(a < e);
    CHECK(!(a < a));
}

TEST_CASE(Plan_rejected_sorted_and_summary) {
    Plan p = Run(fakecaps::Nvidia(), Req(Case::B));
    CHECK(!p.Summary().empty());
    CHECK(p.Summary().find("present:d3d11") != std::string::npos);
    REQUIRE(!p.rejected.empty());
    CHECK(p.rejected.size() <= 6);
    for (size_t i = 1; i < p.rejected.size(); i++) {
        if (p.rejected[i].cost.hostCopies == 0 && p.rejected[i].cost.nodes == 0) continue;   // blacklist entries
        CHECK(!(p.rejected[i].cost < p.rejected[i - 1].cost));
    }
    for (auto& rj : p.rejected) CHECK(!rj.reason.empty());
}

TEST_CASE(Plan_open_options_and_structure) {
    PlanRequest r = Req(Case::B);
    Plan p = Run(fakecaps::Nvidia(), r);
    OpenOptions o = p.openOptions;
    REQUIRE(o.size() >= 4);
    CHECK_EQ(o[0].first, L"hwaccel");
    CHECK_EQ(o[0].second, L"d3d11va");
    CHECK_EQ(o[1].first, L"hwframes");
    CHECK_EQ(o[2].first, L"presenter");
    // Detector timing defaults may precede the graph; verify the graph option and its value by key.
    auto vf = std::find_if(o.begin(), o.end(), [](const auto& option) { return option.first == L"vf"; });
    REQUIRE(vf != o.end());
    CHECK_EQ(vf->second, p.videoFilter);
    Plan s = Run(fakecaps::None(), Req(Case::C));
    CHECK_EQ(s.vfStructure, L"transpose=1,eq@color=*");
    for (auto& kv : s.openOptions) CHECK(kv.first != L"hwaccel");
    Plan k = Run(fakecaps::Nvidia(), Req(Case::C));
    for (auto& kv : k.openOptions) CHECK(kv.first != L"vf");
}

namespace {
bool HasNote(const Plan& p, const char* n) { return std::find(p.notes.begin(), p.notes.end(), std::string(n)) != p.notes.end(); }
const wchar_t* kResidual = L"mode=send_frame:parity=auto:deint=interlaced";
}

TEST_CASE(Plan_ivtc_gpu_without_flags_combed_emits_no_deinterlacer_and_notes_it) {
    Plan p = Run(fakecaps::Nvidia(), Req(Case::A));
    Expect(p, 0, 0, "d3d11va", true, "d3d11", L"ivtc_d3d11");
    CHECK(HasNote(p, "residual-combing fallback: unavailable (GPU ivtc)"));
    PlanRequest cuda = Req(Case::A);
    cuda.blacklist.insert("hwaccel:d3d11va");
    Plan c = Run(fakecaps::Nvidia(), cuda);
    Expect(c, 0, 1, "cuda", true, "d3d11", L"ivtc_cuda");
    CHECK(HasNote(c, "residual-combing fallback: unavailable (GPU ivtc)"));
    // user rate/only-interlaced do not leak into IVTC
    PlanRequest r = Req(Case::A);
    r.settings.deinterlaceRate = DeinterlaceRate::Field;
    r.settings.deinterlaceOnlyInterlaced = true;
    Expect(Run(fakecaps::Nvidia(), r), 0, 0, "d3d11va", true, "d3d11", L"ivtc_d3d11");
    r.blacklist.insert("hwaccel:d3d11va");
    Expect(Run(fakecaps::Nvidia(), r), 0, 1, "cuda", true, "d3d11", L"ivtc_cuda");
}

TEST_CASE(Plan_ivtc_cpu_recipe_has_fieldmatch_bwdif_interlaced_decimate_and_no_note) {
    Plan p = Run(fakecaps::None(), Req(Case::A));
    Expect(p, 1, 0, "", false, "sdl", L"fieldmatch,bwdif=mode=send_frame:parity=auto:deint=interlaced,decimate");
    CHECK(!HasNote(p, "residual-combing fallback: unavailable (GPU ivtc)"));
    PlanRequest r = Req(Case::A);
    r.settings.deinterlaceRate = DeinterlaceRate::Field;   // ignored for IVTC
    Expect(Run(fakecaps::None(), r), 1, 0, "", false, "sdl", L"fieldmatch,bwdif=mode=send_frame:parity=auto:deint=interlaced,decimate");
}

TEST_CASE(Plan_ivtc_flags_combed_appends_same_domain_bwdif) {
    CapabilitySet nv = fakecaps::Nvidia();
    for (FilterCap& f : nv.filters) if (f.name == "ivtc_cuda" || f.name == "ivtc_d3d11") f.flagsCombed = true;
    Plan d = Run(nv, Req(Case::A));
    Expect(d, 0, 0, "d3d11va", true, "d3d11", L"ivtc_d3d11,bwdif_d3d11=mode=send_frame:parity=auto:deint=interlaced");
    CHECK(!HasNote(d, "residual-combing fallback: unavailable (GPU ivtc)"));
    PlanRequest cuda = Req(Case::A);
    cuda.blacklist.insert("hwaccel:d3d11va");
    Plan p = Run(nv, cuda);
    Expect(p, 0, 1, "cuda", true, "d3d11", L"ivtc_cuda,bwdif_cuda=mode=send_frame:parity=auto:deint=interlaced");
    CHECK(!HasNote(p, "residual-combing fallback: unavailable (GPU ivtc)"));
    CapabilitySet in = fakecaps::Intel();
    for (FilterCap& f : in.filters) if (f.name == "ivtc_d3d11") f.flagsCombed = true;
    RemoveFilter(in, "bwdif_d3d11");
    PlanRequest r = Req(Case::A);
    r.decoder = DecoderPref::PreferGpu;
    Plan q = Run(in, r);
    CHECK(q.ok);
    if (q.videoFilter.find(L"ivtc_d3d11") != std::wstring::npos) {
        CHECK(q.videoFilter.find(kResidual) == std::wstring::npos);
        CHECK(HasNote(q, "residual-combing fallback: unavailable (no d3d11 bwdif)"));
    }
}

TEST_CASE(Plan_deinterlace_modes_are_exclusive_stages) {
    PlanRequest r = Req(Case::B);   // On
    CHECK(Run(fakecaps::None(), r).videoFilter.find(L"fieldmatch") == std::wstring::npos);
    r.settings.deinterlace = DeinterlaceMode::Ivtc;
    CHECK(Run(fakecaps::None(), r).videoFilter.find(L"deint=all") == std::wstring::npos);
}
