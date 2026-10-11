#include "wvptest.h"

#include <algorithm>
#include <string>

#include "wvp/PipelineTable.h"

using namespace wvp;

namespace {

bool Has(const std::vector<FilterVariant>& v, const std::string& filter, FrameDomain d) {
    for (auto& x : v) if (x.filter == filter && x.domain == d) return true;
    return false;
}

void Remove(CapabilitySet& c, const std::string& name) {
    c.filters.erase(std::remove_if(c.filters.begin(), c.filters.end(), [&](const FilterCap& f) { return f.name == name; }), c.filters.end());
}

}

TEST_CASE(Table_op_of_filter_name) {
    for (const char* n : {"bwdif", "bwdif_cuda", "bwdif_opencl", "bwdif_d3d11", "bwdif_vulkan"})
        CHECK(PipelineTable::OpOfFilterName(n) == PipelineOp::Bwdif);
    for (const char* n : {"ivtc_cuda", "ivtc_d3d11", "ivtc_npp"})
        CHECK(PipelineTable::OpOfFilterName(n) == PipelineOp::Ivtc);
    CHECK(PipelineTable::OpOfFilterName("fieldmatch,bwdif,decimate") == PipelineOp::Ivtc);
    CHECK(PipelineTable::OpOfFilterName("flip_vulkan") == PipelineOp::VFlip);
    CHECK(PipelineTable::OpOfFilterName("yadif_cuda") == PipelineOp::Yadif);
    for (const char* n : {"fieldmatch", "decimate", "deinterlace_d3d11", "hwupload", "bwdifx_cuda"})
        CHECK(PipelineTable::OpOfFilterName(n) == PipelineOp::Unknown);
}

TEST_CASE(Table_building_blocks) {
    FilterCap f; f.name = "x"; f.formats = FilterCap::Formats::List;
    f.pixFmts = {"yuv420p", "nv12"};
    CHECK(PipelineTable::DomainFromPixFmts(f) == FrameDomain::System);
    f.pixFmts = {"cuda"};
    CHECK(PipelineTable::DomainFromPixFmts(f) == FrameDomain::Cuda);
    f.pixFmts = {"d3d11"};
    CHECK(PipelineTable::DomainFromPixFmts(f) == FrameDomain::D3D11);
    f.pixFmts.clear();
    CHECK(!PipelineTable::DomainFromPixFmts(f).has_value());
    f.pixFmts = {"cuda"}; f.formats = FilterCap::Formats::Dynamic;
    CHECK(!PipelineTable::DomainFromPixFmts(f).has_value());

    std::string base;
    CHECK(PipelineTable::DomainFromSuffix("ivtc_npp", &base) == FrameDomain::Cuda);
    CHECK_EQ(base, "ivtc");
    CHECK(PipelineTable::DomainFromSuffix("scale_npp") == FrameDomain::Cuda);
    CHECK(PipelineTable::DomainFromSuffix("x_cuvid") == FrameDomain::Cuda);
    CHECK(!PipelineTable::DomainFromSuffix("h264_nvenc").has_value());   // _nvenc names encoders, never a filter domain
    CHECK(PipelineTable::DomainFromSuffix("x_opencl") == FrameDomain::OpenCl);
    CHECK(PipelineTable::DomainFromSuffix("x_qsv") == FrameDomain::Qsv);
    CHECK(PipelineTable::DomainFromSuffix("x_d3d11") == FrameDomain::D3D11);
    CHECK(PipelineTable::DomainFromSuffix("x_d3d11va", &base) == FrameDomain::D3D11);
    CHECK_EQ(base, "x");
    CHECK(PipelineTable::DomainFromSuffix("x_d3d12") == FrameDomain::D3D12);
    CHECK(PipelineTable::DomainFromSuffix("x_vulkan") == FrameDomain::Vulkan);
    CHECK(PipelineTable::DomainFromSuffix("x_dxva2") == FrameDomain::Dxva2);
    CHECK(!PipelineTable::DomainFromSuffix("bwdif").has_value());
    CHECK(!PipelineTable::DomainFromSuffix("_cuda").has_value());

    CHECK(PipelineTable::OpFromBaseName("ivtc") == PipelineOp::Ivtc);
    CHECK(PipelineTable::OpFromBaseName("bwdif") == PipelineOp::Bwdif);
    CHECK(PipelineTable::OpFromBaseName("yadif") == PipelineOp::Yadif);
    CHECK(PipelineTable::OpFromBaseName("transpose") == PipelineOp::Transpose);
    CHECK(PipelineTable::OpFromBaseName("hflip") == PipelineOp::HFlip);
    CHECK(PipelineTable::OpFromBaseName("vflip") == PipelineOp::VFlip);
    CHECK(PipelineTable::OpFromBaseName("eq") == PipelineOp::Eq);
    CHECK(PipelineTable::OpFromBaseName("setpts") == PipelineOp::Setpts);
    CHECK(PipelineTable::OpFromBaseName("scale") == PipelineOp::Scale);
    CHECK(PipelineTable::OpFromBaseName("tonemap") == PipelineOp::Tonemap);
    CHECK(PipelineTable::OpFromBaseName("avgblur") == PipelineOp::Unknown);
    CHECK_EQ(std::string(OpName(PipelineOp::Ivtc)), "ivtc");
    CHECK(kAliasCount >= 2);
}

TEST_CASE(Table_nvidia_ivtc_variants) {
    auto t = PipelineTable::Discover(fakecaps::Nvidia());
    auto v = t.VariantsFor(PipelineOp::Ivtc);
    REQUIRE(v.size() == 3);
    CHECK_EQ(v[0].filter, "fieldmatch,bwdif,decimate");
    CHECK(v[0].domain == FrameDomain::System);
    CHECK_EQ(v[0].discoveredBy, "alias");
    CHECK_EQ(v[0].cpuLoad, 2.0);
    CHECK_EQ(v[1].filter, "ivtc_d3d11");
    CHECK(v[1].domain == FrameDomain::D3D11);
    CHECK_EQ(v[1].discoveredBy, "pads");
    CHECK_EQ(v[1].cpuLoad, 0.0);
    CHECK(v[1].optionsKnown);
    CHECK(v[1].HasOption("tff"));
    CHECK(!v[1].HasOption("nonexistent"));
    CHECK(v[1].HasOption("cthresh"));
    CHECK_EQ(v[2].filter, "ivtc_cuda");
    CHECK(v[2].domain == FrameDomain::Cuda);
    CHECK_EQ(v[2].discoveredBy, "pads");
    CHECK(v[2].HasOption("cthresh"));
}

TEST_CASE(Table_ivtc_npp_by_suffix_without_table_change) {
    auto c = fakecaps::Nvidia();
    FilterCap f; f.name = "ivtc_npp"; f.formats = FilterCap::Formats::Dynamic; f.hwDevice = true;
    c.filters.push_back(f);
    auto t = PipelineTable::Discover(c);
    auto v = t.VariantsFor(PipelineOp::Ivtc);
    REQUIRE(v.size() == 4);
    CHECK_EQ(v[3].filter, "ivtc_npp");
    auto n = t.Find("ivtc_npp");
    REQUIRE(n.has_value());
    CHECK(n->op == PipelineOp::Ivtc);
    CHECK(n->domain == FrameDomain::Cuda);
    CHECK_EQ(n->discoveredBy, "suffix");
    CHECK(!n->optionsKnown);
    CHECK(n->HasOption("anything"));
    CHECK_EQ(n->cpuLoad, 0.0);
    CHECK(t.DomainOf("ivtc_npp") == FrameDomain::Cuda);
}

TEST_CASE(Table_new_invented_filter_by_pads) {
    auto c = fakecaps::Nvidia();
    FilterCap f; f.name = "bwdif_vulkan"; f.formats = FilterCap::Formats::List; f.pixFmts = {"vulkan"};
    c.filters.push_back(f);
    auto t = PipelineTable::Discover(c);
    auto b = t.VariantsFor(PipelineOp::Bwdif);
    CHECK(Has(b, "bwdif", FrameDomain::System));
    CHECK(Has(b, "bwdif_cuda", FrameDomain::Cuda));
    REQUIRE(Has(b, "bwdif_vulkan", FrameDomain::Vulkan));
    CHECK_EQ(t.Find("bwdif_vulkan")->discoveredBy, "pads");
    CHECK(t.DomainOf("bwdif_vulkan") == FrameDomain::Vulkan);
}

TEST_CASE(Table_other_ops_and_loads) {
    auto t = PipelineTable::Discover(fakecaps::Nvidia());
    struct E { PipelineOp op; const char* sys; double load; };
    for (E e : {E{PipelineOp::Bwdif, "bwdif", 1.0}, E{PipelineOp::Yadif, "yadif", 0.7}, E{PipelineOp::Transpose, "transpose", 0.3},
                E{PipelineOp::HFlip, "hflip", 0.2}, E{PipelineOp::VFlip, "vflip", 0.2}, E{PipelineOp::Eq, "eq", 0.3},
                E{PipelineOp::Scale, "scale", 0.5}, E{PipelineOp::Setpts, "setpts", 0.0}}) {
        auto v = t.VariantsFor(e.op);
        REQUIRE_MSG(!v.empty(), std::string(e.sys));
        CHECK_EQ(v[0].filter, e.sys);
        CHECK_EQ(v[0].cpuLoad, e.load);
    }
    CHECK(t.VariantsFor(PipelineOp::Setpts)[0].domain == FrameDomain::Any);
    CHECK(Has(t.VariantsFor(PipelineOp::Transpose), "transpose_d3d11", FrameDomain::D3D11));
    CHECK(Has(t.VariantsFor(PipelineOp::Tonemap), "tonemap_d3d11", FrameDomain::D3D11));
    CHECK(Has(t.VariantsFor(PipelineOp::Scale), "scale_cuda", FrameDomain::Cuda));
    CHECK(Has(PipelineTable::Discover(fakecaps::Intel()).VariantsFor(PipelineOp::Scale), "scale_qsv", FrameDomain::Qsv));
    CHECK_EQ(PipelineTable::Discover(fakecaps::Intel()).Find("scale_qsv")->discoveredBy, "pads");
    CHECK(t.VariantsFor(PipelineOp::Unknown).empty());
}

TEST_CASE(Table_ffmpeg_own_opencl_filters_stay_in_their_hw_domain) {
    // nothing probes or plans OpenCL devices, but the filters FFmpeg ships must never be mistaken for CPU filters
    auto caps = ParseCapabilities(R"({"filters": [
        {"name": "transpose_opencl", "formats": "list", "pix_fmts": ["opencl"], "options": ["dir"]},
        {"name": "tonemap_opencl", "formats": "dynamic"},
        {"name": "avgblur_opencl", "formats": "list", "pix_fmts": ["opencl"]}]})");
    REQUIRE(caps.has_value());
    auto t = PipelineTable::Discover(*caps);
    CHECK(Has(t.VariantsFor(PipelineOp::Transpose), "transpose_opencl", FrameDomain::OpenCl));
    CHECK(Has(t.VariantsFor(PipelineOp::Tonemap), "tonemap_opencl", FrameDomain::OpenCl));
    CHECK(t.DomainOf("avgblur_opencl") == FrameDomain::OpenCl);
    CHECK(!t.Find("avgblur_opencl").has_value());
}

TEST_CASE(Table_transitions_and_unknown_ops_not_variants) {
    auto t = PipelineTable::Discover(fakecaps::Nvidia());
    for (const char* n : {"hwupload", "hwdownload", "hwmap", "format", "vpp_qsv", "null", "fieldmatch", "decimate"})
        CHECK_MSG(!t.Find(n).has_value(), n);
    CHECK(t.DomainOf("hwupload") == FrameDomain::System);
    CHECK(t.DomainOf("hwmap") == FrameDomain::System);
    CHECK(t.DomainOf("null") == FrameDomain::Any);
    CHECK(t.DomainOf("fieldmatch") == FrameDomain::System);
    CHECK(t.DomainOf("does_not_exist") == FrameDomain::System);
    CHECK(t.DomainOf("") == FrameDomain::System);
    auto q = PipelineTable::Discover(fakecaps::Intel());
    CHECK(q.DomainOf("vpp_qsv") == FrameDomain::Qsv);
    CHECK(q.DomainOf("deinterlace_qsv") == FrameDomain::Qsv);
}

TEST_CASE(Table_alias_composite_requires_both_parts) {
    auto c = fakecaps::Nvidia();
    Remove(c, "decimate");
    auto t = PipelineTable::Discover(c);
    CHECK(!t.Find("fieldmatch,bwdif,decimate").has_value());
    auto v = t.VariantsFor(PipelineOp::Ivtc);
    REQUIRE(v.size() == 2);
    CHECK_EQ(v[0].filter, "ivtc_d3d11");
    CHECK_EQ(v[1].filter, "ivtc_cuda");
    auto c2 = fakecaps::Nvidia();
    Remove(c2, "fieldmatch");
    CHECK(!PipelineTable::Discover(c2).Find("fieldmatch,bwdif,decimate").has_value());
    auto c3 = fakecaps::None();
    Remove(c3, "bwdif");
    CHECK(!PipelineTable::Discover(c3).Find("fieldmatch,bwdif,decimate").has_value());
    CHECK(PipelineTable::Discover(fakecaps::None()).Find("fieldmatch,bwdif,decimate").has_value());
}

TEST_CASE(Table_flags_combed_is_copied_to_the_variant) {
    auto c = fakecaps::Nvidia();
    for (auto& f : c.filters) if (f.name == "ivtc_cuda") f.flagsCombed = true;
    auto t = PipelineTable::Discover(c);
    CHECK(t.Find("ivtc_cuda")->flagsCombed);
    CHECK(!t.Find("ivtc_d3d11")->flagsCombed);
    CHECK(!t.Find("fieldmatch,bwdif,decimate")->flagsCombed);
}

TEST_CASE(Table_single_alias_flip_vulkan) {
    auto c = fakecaps::None();
    FilterCap f; f.name = "flip_vulkan"; f.formats = FilterCap::Formats::List; f.pixFmts = {"vulkan"};
    c.filters.push_back(f);
    auto t = PipelineTable::Discover(c);
    auto v = t.VariantsFor(PipelineOp::VFlip);
    REQUIRE(v.size() == 2);
    CHECK_EQ(v[0].filter, "vflip");
    CHECK_EQ(v[1].filter, "flip_vulkan");
    CHECK(v[1].domain == FrameDomain::Vulkan);
    CHECK_EQ(v[1].discoveredBy, "alias");
}

TEST_CASE(Table_pads_win_over_suffix_and_has_option) {
    auto c = fakecaps::None();
    FilterCap f; f.name = "yadif_cuda"; f.formats = FilterCap::Formats::List; f.pixFmts = {"yuv420p"};
    c.filters.push_back(f);
    auto t = PipelineTable::Discover(c);
    CHECK(t.DomainOf("yadif_cuda") == FrameDomain::System);
    CHECK_EQ(t.Find("yadif_cuda")->discoveredBy, "pads");
    FilterVariant v;
    CHECK(v.HasOption("whatever"));
    v.optionsKnown = true;
    CHECK(!v.HasOption("whatever"));
    v.options = {"a", "b"};
    CHECK(v.HasOption("b"));
    CHECK(!v.HasOption("c"));
}

TEST_CASE(Table_every_fake_has_system_variants_first) {
    CapabilitySet fakes[] = {fakecaps::Nvidia(), fakecaps::Intel(), fakecaps::Amd(), fakecaps::None()};
    for (auto& c : fakes) {
        auto t = PipelineTable::Discover(c);
        bool seenHw = false;
        for (auto& v : t.All()) {
            if (IsHardware(v.domain)) seenHw = true;
            else CHECK_MSG(!seenHw, v.filter);
        }
        CHECK(!t.VariantsFor(PipelineOp::Ivtc).empty());
        CHECK(!t.VariantsFor(PipelineOp::Bwdif).empty());
    }
    CHECK_EQ(PipelineTable::Discover(fakecaps::None()).VariantsFor(PipelineOp::Ivtc).size(), (size_t)1);
}
