// Deinterlace On / priority decision table, driver-deinterlacer denylist, chroma, and the Intel / AMD zero-copy plans
// (pure logic, fakecaps).
#include "wvptest.h"

#include <algorithm>
#include <cstdio>

#include "wvp/PipelinePlanner.h"
#include "wvp/PipelineSession.h"

using namespace wvp;

namespace {

PlanRequest Req(const char* codec, int depth, DeinterlaceMode m, DeinterlacePriority pr = DeinterlacePriority::Efficiency) {
    PlanRequest r;
    r.content = { codec, depth, 1920, 1080, false };
    r.settings.deinterlace = m;
    r.settings.deinterlacePriority = pr;
    return r;
}

Plan Run(const CapabilitySet& caps, const PlanRequest& r) { return PipelinePlanner(caps).Make(r); }

const PlanStep* Step(const Plan& p, const std::string& name) {
    for (const PlanStep& s : p.steps) if (s.name == name) return &s;
    return nullptr;
}

std::string ReasonOf(const Plan& p) {
    for (const PlanStep& s : p.steps) if (s.reason.rfind("priority ", 0) == 0) return s.reason;
    return "";
}

void ExpectSummary(const Plan& p, const std::string& summary) {
    CHECK(p.ok);
    CHECK_MSG(p.Summary() == summary, "got " + p.Summary() + ", expected " + summary);
}

void RemoveFilter(CapabilitySet& c, const std::string& n) {
    c.filters.erase(std::remove_if(c.filters.begin(), c.filters.end(), [&](const FilterCap& f) { return f.name == n; }), c.filters.end());
}

const char* kDeintOpts = "=mode=send_frame:parity=auto:deint=all";

}

TEST_CASE(Deint_gpu_always_bwdif_in_both_priorities) {
    for (DeinterlacePriority pr : { DeinterlacePriority::Efficiency, DeinterlacePriority::Quality }) {
        const char* name = pr == DeinterlacePriority::Quality ? "quality" : "efficiency";
        for (const CapabilitySet& caps : { fakecaps::Intel(), fakecaps::Amd(), fakecaps::Nvidia() }) {
            Plan p = Run(caps, Req("h264", 8, DeinterlaceMode::On, pr));
            ExpectSummary(p, "d3d11va(keep) > bwdif_d3d11 > present:d3d11 [host 0, device 0]");
            CHECK_EQ(p.videoFilter, std::string("bwdif_d3d11") + kDeintOpts);
            CHECK_EQ(ReasonOf(p), std::string("priority ") + name + ": GPU deinterlacer -> bwdif_d3d11");
        }
    }
}

TEST_CASE(Deint_cpu_only_efficiency_yadif_quality_bwdif) {
    Plan e = Run(fakecaps::None(), Req("h264", 8, DeinterlaceMode::On, DeinterlacePriority::Efficiency));
    ExpectSummary(e, "sw > yadif > present:d3d11 [host 1 (+1 sw-decode), device 0]");
    CHECK_EQ(e.videoFilter, std::string("yadif") + kDeintOpts);
    CHECK_EQ(ReasonOf(e), std::string("priority efficiency: no GPU deinterlacer -> yadif"));

    Plan q = Run(fakecaps::None(), Req("h264", 8, DeinterlaceMode::On, DeinterlacePriority::Quality));
    ExpectSummary(q, "sw > bwdif > present:d3d11 [host 1 (+1 sw-decode), device 0]");
    CHECK_EQ(q.videoFilter, std::string("bwdif") + kDeintOpts);
    CHECK_EQ(ReasonOf(q), std::string("priority quality: no GPU deinterlacer -> bwdif"));
}

TEST_CASE(Deint_rate_and_only_interlaced_reach_the_chosen_filter) {
    PlanRequest r = Req("h264", 8, DeinterlaceMode::On);
    r.settings.deinterlaceRate = DeinterlaceRate::Field;
    r.settings.deinterlaceOnlyInterlaced = true;
    CHECK_EQ(Run(fakecaps::Intel(), r).videoFilter, std::string("bwdif_d3d11=mode=send_field:parity=auto:deint=interlaced"));
    CHECK_EQ(Run(fakecaps::None(), r).videoFilter, std::string("yadif=mode=send_field:parity=auto:deint=interlaced"));
}

TEST_CASE(Deint_gpu_path_not_cheapest_says_so) {
    // Software decoding to a presenter that takes system frames only: the GPU deinterlacer would cost an upload and a
    // download, the CPU one nothing.
    PlanRequest r = Req("h264", 8, DeinterlaceMode::On);
    r.decoder = DecoderPref::Software;
    CapabilitySet caps = fakecaps::Intel();
    caps.presenters[0].inputs = { FrameDomain::System };
    Plan p = Run(caps, r);
    CHECK(p.ok);
    CHECK(Step(p, "yadif") != nullptr);
    CHECK_EQ(ReasonOf(p), std::string("priority efficiency: GPU deinterlacer not on the cheapest path -> yadif"));
}

TEST_CASE(Deint_gpu_yadif_never_when_the_build_has_that_domain_bwdif) {
    PlanRequest r = Req("h264", 8, DeinterlaceMode::On);
    r.blacklist = { "filter:bwdif_d3d11" };
    Plan i = Run(fakecaps::Intel(), r);   // no other GPU bwdif: back to the CPU, still not yadif_d3d11
    CHECK(Step(i, "yadif_d3d11") == nullptr);
    CHECK(Step(i, "yadif") != nullptr);
    CHECK_EQ(ReasonOf(i), std::string("priority efficiency: no GPU deinterlacer -> yadif"));
}

TEST_CASE(Deint_gpu_yadif_only_in_a_domain_without_bwdif) {
    CapabilitySet caps = fakecaps::Intel();
    RemoveFilter(caps, "bwdif_d3d11");
    Plan p = Run(caps, Req("h264", 8, DeinterlaceMode::On, DeinterlacePriority::Quality));
    ExpectSummary(p, "d3d11va(keep) > yadif_d3d11 > present:d3d11 [host 0, device 0]");
    CHECK_EQ(ReasonOf(p), std::string("priority quality: GPU deinterlacer -> yadif_d3d11"));
}

TEST_CASE(Deint_priority_change_is_live_on_cpu_and_nothing_on_gpu) {
    PlanRequest e = Req("h264", 8, DeinterlaceMode::On, DeinterlacePriority::Efficiency);
    PlanRequest q = Req("h264", 8, DeinterlaceMode::On, DeinterlacePriority::Quality);
    CHECK(PipelinePlanner::Diff(Run(fakecaps::Intel(), e), Run(fakecaps::Intel(), q)).kind == PlanDeltaKind::None);
    CHECK(PipelinePlanner::Diff(Run(fakecaps::None(), e), Run(fakecaps::None(), q)).kind == PlanDeltaKind::GraphRebuild);
}

TEST_CASE(Deint_ivtc_cleanup_stays_bwdif) {
    PlanRequest r = Req("mpeg2video", 8, DeinterlaceMode::Ivtc, DeinterlacePriority::Efficiency);
    r.blacklist = { "hwaccel:d3d11va", "hwaccel:d3d12va", "hwaccel:dxva2" };
    Plan p = Run(fakecaps::None(), r);
    CHECK_EQ(p.videoFilter, std::string("fieldmatch,bwdif=mode=send_frame:parity=auto:deint=interlaced,decimate"));
}

TEST_CASE(Table_driver_deinterlacers_are_denied) {
    for (const char* n : { "deinterlace_d3d11", "deinterlace_qsv", "deinterlace_vaapi", "deinterlace_amf", "vpp_qsv", "vpp_amf",
                           "deinterlace_vulkan", "vpp_d3d11", "deinterlace_future" })
        CHECK_MSG(PipelineTable::IsDenied(n), n);
    for (const char* n : { "bwdif", "bwdif_d3d11", "yadif_cuda", "ivtc_d3d11", "fieldmatch", "scale_vaapi" })
        CHECK_MSG(!PipelineTable::IsDenied(n), n);

    // Every vendor's caps plus the driver deinterlacers of other builds, even with the options our deinterlace stage sets.
    for (CapabilitySet caps : { fakecaps::Intel(), fakecaps::Amd(), fakecaps::Nvidia() }) {
        for (const char* n : { "deinterlace_d3d11", "deinterlace_vaapi", "deinterlace_amf", "vpp_d3d11" }) {
            FilterCap f;
            f.name = n; f.formats = FilterCap::Formats::List; f.pixFmts = { "d3d11" };
            f.options = { "mode", "parity", "deint" }; f.optionsKnown = true; f.hwDevice = true;
            caps.filters.push_back(f);
        }
        PipelineTable t = PipelineTable::Discover(caps);
        for (const FilterVariant& v : t.All()) CHECK_MSG(!PipelineTable::IsDenied(v.filter), v.filter);
        for (PipelineOp op : { PipelineOp::Bwdif, PipelineOp::Yadif, PipelineOp::Ivtc })
            for (const FilterVariant& v : t.VariantsFor(op)) CHECK_MSG(!PipelineTable::IsDenied(v.filter), v.filter);
        for (DeinterlaceMode m : { DeinterlaceMode::On, DeinterlaceMode::Ivtc })
            for (DeinterlacePriority pr : { DeinterlacePriority::Efficiency, DeinterlacePriority::Quality }) {
                PlanRequest r = Req("mpeg2video", 8, m, pr);
                // force every candidate away from the GPU filters, so a denied filter would be the only GPU option left
                r.blacklist = { "filter:bwdif_d3d11", "filter:yadif_d3d11", "filter:ivtc_d3d11" };
                Plan p = Run(caps, r);
                CHECK(p.ok);
                for (const PlanStep& s : p.steps) CHECK_MSG(!PipelineTable::IsDenied(s.name), p.Summary());
                CHECK_MSG(p.videoFilter.find("deinterlace_") == std::string::npos && p.videoFilter.find("vpp_") == std::string::npos,
                          p.Summary());
            }
    }
}

TEST_CASE(IntelAmd_zero_copy_plans) {
    for (const CapabilitySet& caps : { fakecaps::Intel(), fakecaps::Amd() }) {
        CHECK(caps.Filter("ivtc_d3d11") && caps.Filter("bwdif_d3d11"));

        Plan none = Run(caps, Req("h264", 8, DeinterlaceMode::Off));
        ExpectSummary(none, "d3d11va(keep) > present:d3d11 [host 0, device 0]");
        CHECK_EQ(none.videoFilter, std::string());

        Plan ivtc = Run(caps, Req("mpeg2video", 8, DeinterlaceMode::Ivtc));
        ExpectSummary(ivtc, "d3d11va(keep) > ivtc_d3d11 > present:d3d11 [host 0, device 0]");
        CHECK_EQ(ivtc.videoFilter, std::string("ivtc_d3d11"));

        for (DeinterlacePriority pr : { DeinterlacePriority::Efficiency, DeinterlacePriority::Quality })
            ExpectSummary(Run(caps, Req("h264", 8, DeinterlaceMode::On, pr)), "d3d11va(keep) > bwdif_d3d11 > present:d3d11 [host 0, device 0]");

        Plan hevc10 = Run(caps, Req("hevc", 10, DeinterlaceMode::Off));
        ExpectSummary(hevc10, "d3d11va(keep) > present:d3d11 [host 0, device 0]");
    }
}

TEST_CASE(Chroma_422_444_is_never_kept_on_the_gpu) {
    for (Chroma ch : { Chroma::Yuv422, Chroma::Yuv444 }) {
        PlanRequest r = Req("hevc", 10, DeinterlaceMode::Off);
        r.content.chroma = ch;
        Plan p = Run(fakecaps::Intel(), r);
        CHECK(p.ok);
        CHECK(!p.hwframesKeep);
        ExpectSummary(p, "d3d11va > present:d3d11 [host 2, device 0]");
        CHECK(std::find(p.openOptions.begin(), p.openOptions.end(), std::make_pair(std::string("hwframes"), std::string("download"))) !=
              p.openOptions.end());
        CHECK(std::any_of(p.notes.begin(), p.notes.end(), [&](const std::string& n) { return n.rfind(std::string("chroma ") + ChromaName(ch), 0) == 0; }));

        r.settings.deinterlace = DeinterlaceMode::On;   // a GPU filter only gets frames converted to 4:2:0 by format=,hwupload
        Plan d = Run(fakecaps::Intel(), r);
        CHECK(!d.hwframesKeep);
        if (Step(d, "bwdif_d3d11")) CHECK_MSG(Step(d, "hwupload") != nullptr, d.Summary());
    }
}

TEST_CASE(Chroma_of_pixel_formats) {
    struct Row { const char* fmt; Chroma c; };
    const Row rows[] = {
        { "nv12", Chroma::Yuv420 }, { "p010le", Chroma::Yuv420 }, { "yuv420p10le", Chroma::Yuv420 }, { "", Chroma::Yuv420 },
        { "yuv422p10le", Chroma::Yuv422 }, { "y210le", Chroma::Yuv422 }, { "yuyv422", Chroma::Yuv422 }, { "p210le", Chroma::Yuv422 },
        { "yuv444p", Chroma::Yuv444 }, { "vuyx", Chroma::Yuv444 }, { "xv30le", Chroma::Yuv444 }, { "y410", Chroma::Yuv444 },
        { "gbrp10le", Chroma::Yuv444 },
    };
    for (const Row& r : rows) CHECK_MSG(PipelineSession::ChromaOf(r.fmt) == r.c, r.fmt);
}

TEST_CASE(Caps_presenter_srv_p010_and_forced_round_trip) {
    CapabilitySet c = fakecaps::Intel();
    PresenterCap* d3 = nullptr;
    for (auto& p : c.presenters) if (p.name == "d3d11") d3 = &p;
    REQUIRE(d3 != nullptr);
    CHECK(d3->vpP010 && d3->decoderSrvNv12 && d3->decoderSrvP010);
    d3->vpP010 = false; d3->decoderSrvP010 = false; d3->forced = { "p010=0", "p010_srv=0" };
    auto back = ParseCapabilities(CapabilitiesToJson(c));
    REQUIRE(back.has_value());
    const PresenterCap* b = back->Presenter("d3d11");
    REQUIRE(b != nullptr);
    CHECK(!b->vpP010 && b->decoderSrvNv12 && !b->decoderSrvP010);
    CHECK(b->forced.size() == 2 && b->forced[0] == "p010=0");
    auto old = ParseCapabilities(R"({"presenters":[{"name":"d3d11","ok":true,"vp":{"telecine":false}}]})");
    REQUIRE(old.has_value());
    CHECK(old->Presenter("d3d11")->vpP010 && old->Presenter("d3d11")->decoderSrvNv12);
}
