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
        r.settings.speed = 1.5;
        break;
    }
    return r;
}

Plan Run(const CapabilitySet& caps, const PlanRequest& r) { return PipelinePlanner(caps).Make(r); }

void Expect(const Plan& p, int host, int dev, const char* hwaccel, bool keep, const char* presenter, const std::string& vf) {
    CHECK(p.ok);
    CHECK_EQ(p.cost.Weighted(), host);
    CHECK_EQ(p.cost.deviceCopies, dev);
    CHECK_EQ(p.hwaccel, std::string(hwaccel));
    CHECK_EQ(p.hwframesKeep, keep);
    CHECK(!p.steps.empty() && p.steps.back().name == std::string("present:") + presenter);
    CHECK_EQ(p.videoFilter, vf);
    if (!p.ok || p.cost.Weighted() != host || p.cost.deviceCopies != dev || p.videoFilter != vf) std::printf("    plan: %s\n", p.Summary().c_str());
}

const char* kBwdif = "bwdif=mode=send_frame:parity=auto:deint=all";
const char* kSetpts = "setpts=PTS/1.5";

FilterCap* FindFilter(CapabilitySet& c, const std::string& n) {
    for (auto& f : c.filters) if (f.name == n) return &f;
    return nullptr;
}
void RemoveFilter(CapabilitySet& c, const std::string& n) {
    c.filters.erase(std::remove_if(c.filters.begin(), c.filters.end(), [&](const FilterCap& f) { return f.name == n; }), c.filters.end());
}

}

TEST_CASE(Plan_matrix_dump) {
    struct F { const char* n; CapabilitySet c; };
    F fakes[] = { { "Nvidia", fakecaps::Nvidia() }, { "Intel", fakecaps::Intel() }, { "Amd", fakecaps::Amd() },
                  { "None", fakecaps::None() } };
    for (const F& f : fakes)
        for (Case c : { Case::A, Case::B, Case::C }) {
            Plan p = Run(f.c, Req(c));
            std::printf("    [%s case %c] %s | vf=%s dev=%s\n", f.n, 'A' + static_cast<int>(c), p.Summary().c_str(), p.videoFilter.c_str(),
                        p.filterHwDevice.c_str());
            CHECK(p.ok);
            CHECK(!p.Summary().empty());
        }
}

TEST_CASE(Plan_nvidia_cases) {
    CapabilitySet nv = fakecaps::Nvidia();
    Expect(Run(nv, Req(Case::A)), 0, 0, "d3d11va", true, "d3d11", "ivtc_d3d11");
    Expect(Run(nv, Req(Case::B)), 0, 0, "d3d11va", true, "d3d11", "bwdif_d3d11=mode=send_frame:parity=auto:deint=all");
    Plan c = Run(nv, Req(Case::C));
    Expect(c, 0, 0, "d3d11va", true, "d3d11", kSetpts);
    CHECK(c.filterHwDevice.empty());
    PlanRequest r = Req(Case::A);
    r.decoder = DecoderPref::PreferGpu;
    Expect(Run(nv, r), 0, 0, "d3d11va", true, "d3d11", "ivtc_d3d11");
}

TEST_CASE(Plan_nvidia_presenter_variants) {
    Expect(Run(fakecaps::Nvidia(false), Req(Case::A)), 1, 0, "", false, "d3d11", "fieldmatch,bwdif=mode=send_frame:parity=auto:deint=interlaced,decimate");
    // without d3d11va: software decoding, one upload into ivtc_d3d11 (d3d12va / dxva2 frames would need a download)
    PlanRequest noD3d11va = Req(Case::A);
    noD3d11va.blacklist.insert("hwaccel:d3d11va");
    Expect(Run(fakecaps::Nvidia(), noD3d11va), 1, 0, "", false, "d3d11", "format=nv12,hwupload,ivtc_d3d11");
}

TEST_CASE(Plan_intel_amd_cases) {
    for (CapabilitySet caps : { fakecaps::Intel(), fakecaps::Amd() }) {
        Expect(Run(caps, Req(Case::A)), 0, 0, "d3d11va", true, "d3d11", "ivtc_d3d11");
        Expect(Run(caps, Req(Case::B)), 0, 0, "d3d11va", true, "d3d11", "bwdif_d3d11=mode=send_frame:parity=auto:deint=all");
        PlanRequest cpuIvtc = Req(Case::A);
        cpuIvtc.blacklist.insert("filter:ivtc_d3d11");
        Expect(Run(caps, cpuIvtc), 1, 0, "", false, "d3d11", "fieldmatch,bwdif=mode=send_frame:parity=auto:deint=interlaced,decimate");
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

TEST_CASE(Plan_ivtc_d3d11_is_zero_copy) {
    for (CapabilitySet caps : { fakecaps::Intel(), fakecaps::Amd() }) {
        Plan a = Run(caps, Req(Case::A));
        Expect(a, 0, 0, "d3d11va", true, "d3d11", "ivtc_d3d11");
        bool download = false;
        for (auto& s : a.steps) download = download || s.name == "hwdownload";
        CHECK(!download);

        PlanRequest sw = Req(Case::A);
        sw.decoder = DecoderPref::Software;
        Plan b = Run(caps, sw);
        CHECK(b.ok);
        CHECK(b.hwaccel.empty());
        CHECK_EQ(b.cost.hostCopies, 1);
        CHECK_EQ(b.cost.deviceCopies, 0);
        CHECK(b.videoFilter.find("hwupload") != std::string::npos);
        CHECK(b.videoFilter.find("ivtc_d3d11") != std::string::npos);
        CHECK(b.videoFilter.find("hwdownload") == std::string::npos);
        if (b.cost.hostCopies != 1 || b.videoFilter.find("ivtc_d3d11") == std::string::npos) std::printf("    plan: %s\n", b.Summary().c_str());
    }
}

TEST_CASE(Plan_none_cases) {
    CapabilitySet none = fakecaps::None();
    Expect(Run(none, Req(Case::A)), 1, 0, "", false, "d3d11", "fieldmatch,bwdif=mode=send_frame:parity=auto:deint=interlaced,decimate");
    // software-decode penalty: floor(DecodeLoad) host copies on top of the upload of system frames
    Expect(Run(none, Req(Case::B)), 2, 0, "", false, "d3d11", kBwdif);
    Expect(Run(none, Req(Case::C)), 3, 0, "", false, "d3d11", kSetpts);
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
    Expect(p, 0, 0, "d3d11va", true, "d3d11", "hwmap=derive_device=vulkan,ivtc_vulkan");
}

TEST_CASE(Plan_blacklist) {
    CapabilitySet nv = fakecaps::Nvidia();
    {
        for (const char* type : { "d3d11va", "d3d12va" }) {
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
        Expect(Run(nv, r), 1, 0, "", false, "d3d11", "fieldmatch,bwdif=mode=send_frame:parity=auto:deint=interlaced,decimate");
    }
    {
        PlanRequest r = Req(Case::B);
        r.blacklist.insert("hwframes_keep");
        Plan p = Run(nv, r);
        CHECK(p.ok);
        CHECK(!p.hwframesKeep);
    }
}

TEST_CASE(Plan_decoder_preferences) {
    CapabilitySet intel = fakecaps::Intel();
    {
        PlanRequest r = Req(Case::A);
        r.decoder = DecoderPref::PreferGpu;
        Expect(Run(intel, r), 0, 0, "d3d11va", true, "d3d11", "ivtc_d3d11");
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
        r.forcedDecoder = "d3d11va";
        r.blacklist.insert("hwaccel:d3d11va");
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

TEST_CASE(Plan_uses_the_platform_presenter) {
    CapabilitySet mac = fakecaps::Mac();
    PlanRequest hevc = Req(Case::C);
    hevc.settings.speed = 1;
    Expect(Run(mac, hevc), 0, 0, "videotoolbox", true, "metal", "");
    Plan deint = Run(mac, Req(Case::B));   // no GPU deinterlacer: decode on VideoToolbox, download, CPU bwdif, upload
    Expect(deint, 2, 0, "videotoolbox", false, "metal", kBwdif);
    CapabilitySet none = fakecaps::Intel();
    none.presenters.clear();
    Plan p = Run(none, Req(Case::A));
    CHECK(!p.ok);
    CHECK_EQ(p.error, std::string("no presenter available"));
    CapabilitySet failed = fakecaps::Intel();
    failed.presenters[0].ok = false;
    CHECK(!Run(failed, Req(Case::A)).ok);
}

TEST_CASE(Plan_vcodec_only_for_a_non_default_build_decoder) {
    CapabilitySet nv = fakecaps::Nvidia();
    PlanRequest r = Req(Case::C);
    Plan p = Run(nv, r);
    CHECK(p.decoder.empty());
    bool found = true;
    for (auto& kv : p.openOptions) if (kv.first == "vcodec") found = false;
    CHECK(found);
    for (DecoderCap& d : nv.decoders) if (d.codec == "hevc" && d.hwaccel == "d3d11va") d.decoder = "hevc_d3d11";
    Plan q = Run(nv, r);
    CHECK_EQ(q.hwaccel, std::string("d3d11va"));
    CHECK_EQ(q.decoder, std::string("hevc_d3d11"));
    auto vc = std::find_if(q.openOptions.begin(), q.openOptions.end(), [](const auto& kv) { return kv.first == "vcodec"; });
    REQUIRE(vc != q.openOptions.end());
    CHECK_EQ(vc->second, std::string("hevc_d3d11"));
    CHECK(PipelinePlanner::Diff(p, q).kind == PlanDeltaKind::Reopen);
    PlanRequest unknown = r;
    unknown.content = ContentInfo{};
    CHECK(Run(nv, unknown).decoder.empty());
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
    FilterCap* f = FindFilter(caps, "ivtc_d3d11");
    REQUIRE(f != nullptr);
    f->options = { "tff" };
    f->optionsKnown = true;
    PlanRequest r = Req(Case::A);
    r.ivtc.fieldOrder = 1;
    r.ivtc.combThreshold = 12;
    CHECK_EQ(Run(fakecaps::Nvidia(), r).videoFilter, "ivtc_d3d11=tff=1:cthresh=12");
    CHECK_EQ(Run(caps, r).videoFilter, "ivtc_d3d11=tff=1");
}

TEST_CASE(Plan_tokenizer) {
    auto t = PipelinePlanner::TokenizeChain("a=1:b=2, c , d=x\\,y,e='p,q'");
    REQUIRE(t.has_value());
    REQUIRE(t->size() == 4);
    CHECK_EQ((*t)[0], "a=1:b=2");
    CHECK_EQ((*t)[1], "c");
    CHECK_EQ((*t)[2], "d=x\\,y");
    CHECK_EQ((*t)[3], "e='p,q'");
    CHECK(!PipelinePlanner::TokenizeChain("split[a][b];[a]hflip").has_value());
    CHECK(!PipelinePlanner::TokenizeChain("[in]hue=s=0").has_value());
    auto e = PipelinePlanner::TokenizeChain("");
    REQUIRE(e.has_value());
    CHECK(e->empty());
    CHECK_EQ(PipelinePlanner::TokenFilterName(" eq@color=brightness=1"), std::string("eq"));
    CHECK_EQ(PipelinePlanner::TokenFilterName("hue=s=0"), std::string("hue"));
    CHECK_EQ(PipelinePlanner::TokenFilterName("vflip"), std::string("vflip"));
}

TEST_CASE(Plan_raw_chain_transition_into_d3d11) {
    PlanRequest r = Req(Case::B);
    r.settings.deinterlace = DeinterlaceMode::Off;
    r.settings.customVideoFilter = "bwdif_d3d11";
    r.decoder = DecoderPref::Software;
    Plan p = Run(fakecaps::Nvidia(false), r);   // the presenter takes system frames only: the frames leave the GPU again
    CHECK(p.ok);
    CHECK_EQ(p.videoFilter, "format=nv12,hwupload,bwdif_d3d11,hwdownload,format=nv12");
    CHECK_EQ(p.filterHwDevice, std::string("d3d11va"));
    Plan q = Run(fakecaps::Nvidia(), r);
    CHECK_EQ(q.steps.back().name, std::string("present:d3d11"));
    CHECK_EQ(q.videoFilter, "format=nv12,hwupload,bwdif_d3d11");
    CHECK_EQ(q.cost.deviceCopies, 0);
    CHECK_EQ(q.cost.hostCopies, 1);
    r.decoder = DecoderPref::Auto;
    r.settings.customVideoFilter.clear();
    r.settings.deinterlace = DeinterlaceMode::Ivtc;
    Plan keep = Run(fakecaps::Nvidia(), r);
    CHECK_MSG(keep.hwframesKeep && keep.cost.hostCopies == 0, keep.Summary());
}

TEST_CASE(Plan_raw_chain_verbatim_and_opaque) {
    PlanRequest r = Req(Case::B);
    r.settings.deinterlace = DeinterlaceMode::Off;
    r.settings.customVideoFilter = "hue=s=0 , vflip";
    Plan p = Run(fakecaps::None(), r);
    CHECK_EQ(p.videoFilter, "hue=s=0 , vflip");
    r.settings.customVideoFilter = "split[a][b];[a]hflip[c];[c][b]hstack";
    Plan q = Run(fakecaps::None(), r);
    CHECK_EQ(q.videoFilter, "split[a][b];[a]hflip[c];[c][b]hstack");
    r.settings.customVideoFilter = "format=nv12,hwupload,ivtc_opencl,hwdownload,format=nv12";
    Plan o = Run(fakecaps::Nvidia(), r);
    CHECK(o.ok);
    CHECK_EQ(o.videoFilter, "format=nv12,hwupload,ivtc_opencl,hwdownload,format=nv12");
    CHECK(o.filterHwDevice.empty());   // opaque custom graph: the user supplies any hw device themselves
}

TEST_CASE(Plan_advanced_mode_skips_presets) {
    PlanRequest r = Req(Case::C);
    r.settings.advancedMode = true;
    r.settings.customVideoFilter = "hue=s=0";
    Plan p = Run(fakecaps::None(), r);
    CHECK_EQ(p.videoFilter, "hue=s=0");
}

TEST_CASE(Plan_diff) {
    CapabilitySet none = fakecaps::None();
    PlanRequest r = Req(Case::C);
    Plan a = Run(none, r);
    CHECK_EQ(PipelinePlanner::Diff(a, a).kind, PlanDeltaKind::None);
    PlanRequest r2 = r;
    r2.settings.speed = 2;
    CHECK(PipelinePlanner::Diff(a, Run(none, r2)).kind == PlanDeltaKind::GraphRebuild);
    CapabilitySet nv = fakecaps::Nvidia();
    Plan x = Run(nv, r);
    PlanRequest r5 = r;
    r5.decoder = DecoderPref::Software;
    CHECK(PipelinePlanner::Diff(x, Run(nv, r5)).kind == PlanDeltaKind::Reopen);
    Plan y = x;
    y.hwframesKeep = !x.hwframesKeep;
    CHECK(PipelinePlanner::Diff(x, y).kind == PlanDeltaKind::Reopen);
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
    REQUIRE(o.size() >= 3);
    CHECK_EQ(o[0].first, "hwaccel");
    CHECK_EQ(o[0].second, "d3d11va");
    CHECK_EQ(o[1].first, "hwframes");
    CHECK_EQ(o[1].second, "keep");
    for (auto& kv : o) CHECK(kv.first != "presenter" && kv.first != "presenter_params");
    // Detector timing defaults may precede the graph; verify the graph option and its value by key.
    auto vf = std::find_if(o.begin(), o.end(), [](const auto& option) { return option.first == "vf"; });
    REQUIRE(vf != o.end());
    CHECK_EQ(vf->second, p.videoFilter);
    Plan s = Run(fakecaps::None(), Req(Case::C));
    for (auto& kv : s.openOptions) CHECK(kv.first != "hwaccel" && kv.first != "hwframes");
    PlanRequest plain = Req(Case::C);
    plain.settings.speed = 1;
    Plan k = Run(fakecaps::Nvidia(), plain);
    for (auto& kv : k.openOptions) CHECK(kv.first != "vf");
    PlanRequest dl = Req(Case::B);
    dl.blacklist.insert("hwframes_keep");
    Plan d = Run(fakecaps::Nvidia(), dl);
    REQUIRE(!d.hwaccel.empty());
    CHECK(std::find(d.openOptions.begin(), d.openOptions.end(), std::make_pair(std::string("hwframes"), std::string("download"))) !=
          d.openOptions.end());
}

namespace {
bool HasNote(const Plan& p, const char* n) { return std::find(p.notes.begin(), p.notes.end(), std::string(n)) != p.notes.end(); }
const char* kResidual = "mode=send_frame:parity=auto:deint=interlaced";
}

TEST_CASE(Plan_ivtc_gpu_without_flags_combed_emits_no_deinterlacer_and_notes_it) {
    Plan p = Run(fakecaps::Nvidia(), Req(Case::A));
    Expect(p, 0, 0, "d3d11va", true, "d3d11", "ivtc_d3d11");
    CHECK(HasNote(p, "residual-combing fallback: unavailable (GPU ivtc)"));
    // user rate/only-interlaced do not leak into IVTC
    PlanRequest r = Req(Case::A);
    r.settings.deinterlaceRate = DeinterlaceRate::Field;
    r.settings.deinterlaceOnlyInterlaced = true;
    Expect(Run(fakecaps::Nvidia(), r), 0, 0, "d3d11va", true, "d3d11", "ivtc_d3d11");
}

TEST_CASE(Plan_ivtc_cpu_recipe_has_fieldmatch_bwdif_interlaced_decimate_and_no_note) {
    Plan p = Run(fakecaps::None(), Req(Case::A));
    Expect(p, 1, 0, "", false, "d3d11", "fieldmatch,bwdif=mode=send_frame:parity=auto:deint=interlaced,decimate");
    CHECK(!HasNote(p, "residual-combing fallback: unavailable (GPU ivtc)"));
    PlanRequest r = Req(Case::A);
    r.settings.deinterlaceRate = DeinterlaceRate::Field;   // ignored for IVTC
    Expect(Run(fakecaps::None(), r), 1, 0, "", false, "d3d11", "fieldmatch,bwdif=mode=send_frame:parity=auto:deint=interlaced,decimate");
}

TEST_CASE(Plan_ivtc_flags_combed_appends_same_domain_bwdif) {
    CapabilitySet nv = fakecaps::Nvidia();
    for (FilterCap& f : nv.filters) if (f.name == "ivtc_d3d11") f.flagsCombed = true;
    Plan d = Run(nv, Req(Case::A));
    Expect(d, 0, 0, "d3d11va", true, "d3d11", "ivtc_d3d11,bwdif_d3d11=mode=send_frame:parity=auto:deint=interlaced");
    CHECK(!HasNote(d, "residual-combing fallback: unavailable (GPU ivtc)"));
    CapabilitySet in = fakecaps::Intel();
    for (FilterCap& f : in.filters) if (f.name == "ivtc_d3d11") f.flagsCombed = true;
    RemoveFilter(in, "bwdif_d3d11");
    PlanRequest r = Req(Case::A);
    r.decoder = DecoderPref::PreferGpu;
    Plan q = Run(in, r);
    CHECK(q.ok);
    if (q.videoFilter.find("ivtc_d3d11") != std::string::npos) {
        CHECK(q.videoFilter.find(kResidual) == std::string::npos);
        CHECK(HasNote(q, "residual-combing fallback: unavailable (no d3d11 bwdif)"));
    }
}

TEST_CASE(Plan_deinterlace_modes_are_exclusive_stages) {
    PlanRequest r = Req(Case::B);   // On
    CHECK(Run(fakecaps::None(), r).videoFilter.find("fieldmatch") == std::string::npos);
    r.settings.deinterlace = DeinterlaceMode::Ivtc;
    CHECK(Run(fakecaps::None(), r).videoFilter.find("deint=all") == std::string::npos);
}

TEST_CASE(Plan_cuda_filters_of_the_build_are_never_planned) {
    // The build's CUDA filters are classified (domain Cuda) but no device, decoder, upload, interop or presenter input reaches
    // them: not even capabilities that still list a CUDA device, CUDA decoders, a CUDA presenter input and CUDA interop.
    CapabilitySet nv = fakecaps::Nvidia();
    CapabilitySet stale = nv;
    stale.devices.push_back({ "cuda", true, "NVIDIA GeForce RTX 5070", "" });
    for (DecoderCap d : nv.decoders)
        if (d.hwaccel == "d3d11va") { d.hwaccel = "cuda"; stale.decoders.push_back(d); }
    stale.presenters.back().inputs.push_back(FrameDomain::Cuda);
    stale.interop.push_back({ FrameDomain::D3D11, FrameDomain::Cuda, "map", true, "" });
    for (const CapabilitySet& caps : { nv, stale })
        for (Case c : { Case::A, Case::B }) {
            PlanRequest r = Req(c);
            r.blacklist.insert("filter:ivtc_d3d11");
            r.blacklist.insert("filter:bwdif_d3d11");
            r.blacklist.insert("filter:yadif_d3d11");
            for (const Plan& p : { Run(caps, r), [&] { PlanRequest f = r; f.decoder = DecoderPref::Forced; f.forcedDecoder = "cuda";
                                                         return Run(caps, f); }() }) {
                CHECK_MSG(p.ok, p.error);
                CHECK_MSG(p.hwaccel != "cuda" && p.filterHwDevice != "cuda", p.Summary());
                CHECK_MSG(p.videoFilter.find("_cuda") == std::string::npos, p.videoFilter);
                for (const PlanStep& st : p.steps) CHECK_MSG(st.in != FrameDomain::Cuda && st.out != FrameDomain::Cuda, st.name);
            }
            PlanRequest f = r;
            f.decoder = DecoderPref::Forced;
            f.forcedDecoder = "cuda";
            CHECK(HasNote(Run(caps, f), "forced decoder unknown: using automatic choice"));
        }
    PlanRequest raw = Req(Case::B);
    raw.settings.deinterlace = DeinterlaceMode::Off;
    raw.settings.customVideoFilter = "bwdif_cuda";
    Plan p = Run(stale, raw);
    CHECK(!p.ok);
    CHECK_EQ(p.error, std::string("no valid pipeline for this request"));
}
