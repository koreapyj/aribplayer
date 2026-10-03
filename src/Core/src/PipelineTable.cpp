#include "wvp/PipelineTable.h"

#include <algorithm>

namespace wvp {

const FilterAlias kAliases[] = {
    // CPU IVTC recipe: fieldmatch, residual-combing cleanup (bwdif deint=interlaced), decimate last (FFmpeg's recommended order).
    // The planner renders the options (PipelinePlanner StageText); the alias exists when all three filters are in caps.
    {"fieldmatch,bwdif,decimate", PipelineOp::Ivtc},
    {"flip_vulkan", PipelineOp::VFlip},
};
const int kAliasCount = (int)(sizeof(kAliases) / sizeof(kAliases[0]));

namespace {

struct Suffix { const char* text; FrameDomain d; };
constexpr Suffix kSuffixes[] = {
    {"_cuda", FrameDomain::Cuda}, {"_npp", FrameDomain::Cuda}, {"_cuvid", FrameDomain::Cuda},
    {"_opencl", FrameDomain::OpenCl}, {"_qsv", FrameDomain::Qsv}, {"_d3d11va", FrameDomain::D3D11}, {"_d3d11", FrameDomain::D3D11},
    {"_d3d12", FrameDomain::D3D12}, {"_vulkan", FrameDomain::Vulkan}, {"_dxva2", FrameDomain::Dxva2},
};

struct Load { const char* name; double load; };
constexpr Load kLoads[] = {
    {"fieldmatch,bwdif,decimate", 2.0}, {"bwdif", 1.0}, {"yadif", 0.7}, {"transpose", 0.3}, {"hflip", 0.2}, {"vflip", 0.2},
    {"eq", 0.3}, {"scale", 0.5}, {"tonemap", 1.0},
};

// Driver deinterlacers / video post-processors: never planned, whatever their name resolves to.
constexpr const char* kDenied[] = {
    "deinterlace_d3d11", "deinterlace_qsv", "deinterlace_vaapi", "deinterlace_amf", "deinterlace_vulkan", "vpp_qsv", "vpp_amf",
    "vpp_d3d11", "vpp_vaapi",
};
constexpr const char* kDeniedPrefixes[] = {"deinterlace_", "vpp_"};

bool IsTransition(const std::string& n) { return n == "hwupload" || n == "hwdownload" || n == "hwmap" || n == "format"; }

double CpuLoad(const std::string& key, FrameDomain d) {
    if (d != FrameDomain::System) return 0;
    for (auto& l : kLoads) if (key == l.name) return l.load;
    return 0.5;
}

}

const char* OpName(PipelineOp op) {
    switch (op) {
    case PipelineOp::Ivtc: return "ivtc";
    case PipelineOp::Bwdif: return "bwdif";
    case PipelineOp::Yadif: return "yadif";
    case PipelineOp::Transpose: return "transpose";
    case PipelineOp::HFlip: return "hflip";
    case PipelineOp::VFlip: return "vflip";
    case PipelineOp::Eq: return "eq";
    case PipelineOp::Setpts: return "setpts";
    case PipelineOp::Scale: return "scale";
    case PipelineOp::Tonemap: return "tonemap";
    default: return "unknown";
    }
}

bool FilterVariant::HasOption(const std::string& name) const {
    return !optionsKnown || std::find(options.begin(), options.end(), name) != options.end();
}

std::optional<FrameDomain> PipelineTable::DomainFromPixFmts(const FilterCap& f) {
    if (f.formats != FilterCap::Formats::List || f.pixFmts.empty()) return std::nullopt;
    for (auto& p : f.pixFmts) {
        auto d = DomainFromName(p);
        if (d && IsHardware(*d)) return d;
    }
    return FrameDomain::System;
}

std::optional<FrameDomain> PipelineTable::DomainFromSuffix(const std::string& name, std::string* base) {
    for (auto& s : kSuffixes) {
        std::string suf = s.text;
        if (name.size() > suf.size() && name.compare(name.size() - suf.size(), suf.size(), suf) == 0) {
            if (base) *base = name.substr(0, name.size() - suf.size());
            return s.d;
        }
    }
    return std::nullopt;
}

PipelineOp PipelineTable::OpFromBaseName(const std::string& b) {
    struct E { const char* n; PipelineOp op; };
    static const E m[] = {
        {"ivtc", PipelineOp::Ivtc}, {"bwdif", PipelineOp::Bwdif}, {"yadif", PipelineOp::Yadif}, {"transpose", PipelineOp::Transpose},
        {"hflip", PipelineOp::HFlip}, {"vflip", PipelineOp::VFlip}, {"eq", PipelineOp::Eq}, {"setpts", PipelineOp::Setpts},
        {"scale", PipelineOp::Scale}, {"tonemap", PipelineOp::Tonemap},
    };
    for (auto& e : m) if (b == e.n) return e.op;
    return PipelineOp::Unknown;
}

bool PipelineTable::IsDenied(const std::string& name) {
    for (const char* d : kDenied) if (name == d) return true;
    for (const char* p : kDeniedPrefixes) if (name.rfind(p, 0) == 0) return true;
    return false;
}

PipelineOp PipelineTable::OpOfFilterName(const std::string& name) {
    for (int i = 0; i < kAliasCount; i++) if (name == kAliases[i].filter) return kAliases[i].op;
    std::string base = name;
    DomainFromSuffix(name, &base);
    return OpFromBaseName(base);
}

PipelineTable PipelineTable::Discover(const CapabilitySet& caps) {
    PipelineTable t;
    std::vector<FilterVariant> sys, hw;
    auto place = [&](FilterVariant v) { (IsHardware(v.domain) ? hw : sys).push_back(std::move(v)); };

    for (auto& f : caps.filters) {
        FrameDomain dom = FrameDomain::System;
        std::string how = "default";
        std::string base = f.name;
        std::string sfxBase;
        auto sfx = DomainFromSuffix(f.name, &sfxBase);
        if (f.formats == FilterCap::Formats::Any) { dom = FrameDomain::Any; how = "pads"; }
        else if (auto p = DomainFromPixFmts(f)) { dom = *p; how = "pads"; }
        else if (sfx) { dom = *sfx; how = "suffix"; }
        if (sfx) base = sfxBase;
        t.domains_.emplace_back(f.name, dom);
        if (IsTransition(f.name) || IsDenied(f.name)) continue;

        PipelineOp op = OpFromBaseName(base);
        if (op == PipelineOp::Unknown) {
            for (int i = 0; i < kAliasCount; ++i)
                if (f.name == kAliases[i].filter) { op = kAliases[i].op; how = "alias"; break; }
        }
        if (op == PipelineOp::Unknown) continue;
        FilterVariant v;
        v.op = op; v.filter = f.name; v.domain = dom; v.options = f.options; v.optionsKnown = f.optionsKnown; v.discoveredBy = how; v.flagsCombed = f.flagsCombed;
        v.cpuLoad = CpuLoad(base, dom);
        place(std::move(v));
    }

    // composite aliases: every part must be in caps.filters
    for (int i = 0; i < kAliasCount; ++i) {
        std::string name = kAliases[i].filter;
        if (name.find(',') == std::string::npos) continue;
        std::vector<std::string> parts;
        size_t pos = 0;
        while (true) {
            size_t c = name.find(',', pos);
            parts.push_back(name.substr(pos, c == std::string::npos ? c : c - pos));
            if (c == std::string::npos) break;
            pos = c + 1;
        }
        bool all = true;
        FilterVariant v;
        v.op = kAliases[i].op; v.filter = name; v.optionsKnown = true; v.discoveredBy = "alias";
        for (auto& p : parts) {
            const FilterCap* fc = caps.Filter(p);
            if (!fc) { all = false; break; }
            if (!fc->optionsKnown) v.optionsKnown = false;
            v.options.insert(v.options.end(), fc->options.begin(), fc->options.end());
        }
        if (!all) continue;
        v.domain = t.DomainOf(parts[0]);
        v.cpuLoad = CpuLoad(name, v.domain);
        place(std::move(v));
    }

    t.variants_ = std::move(sys);
    t.variants_.insert(t.variants_.end(), hw.begin(), hw.end());
    return t;
}

std::vector<FilterVariant> PipelineTable::VariantsFor(PipelineOp op) const {
    std::vector<FilterVariant> r;
    for (auto& v : variants_) if (v.op == op) r.push_back(v);
    return r;
}

std::optional<FilterVariant> PipelineTable::Find(const std::string& filter) const {
    for (auto& v : variants_) if (v.filter == filter) return v;
    return std::nullopt;
}

FrameDomain PipelineTable::DomainOf(const std::string& filter) const {
    for (auto& d : domains_) if (d.first == filter) return d.second;
    return FrameDomain::System;
}

}
