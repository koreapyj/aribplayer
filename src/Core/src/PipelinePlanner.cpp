#include "wvp/PipelinePlanner.h"

#include "wvp/FilterManager.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <map>

namespace wvp {

namespace {

std::wstring W(const std::string& s) { return std::wstring(s.begin(), s.end()); }
std::string N(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s.push_back(c < 128 ? static_cast<char>(c) : '?');
    return s;
}
std::wstring Trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) a++;
    while (b > a && iswspace(s[b - 1])) b--;
    return s.substr(a, b - a);
}
void Join(std::wstring& out, const std::wstring& part) {
    if (part.empty()) return;
    if (!out.empty()) out += L',';
    out += part;
}
int CountNodes(const std::string& filterNames) {
    return static_cast<int>(std::count(filterNames.begin(), filterNames.end(), ',')) + 1;
}

const std::wstring kEqPrefix = L"eq@color=";

std::wstring StructureOf(const std::wstring& vf) {
    std::wstring out;
    size_t i = 0;
    while (i < vf.size()) {
        bool boundary = i == 0 || vf[i - 1] == L',';
        if (boundary && vf.compare(i, kEqPrefix.size(), kEqPrefix) == 0) {
            out += kEqPrefix + L"*";
            size_t e = vf.find(L',', i);
            if (e == std::wstring::npos) break;
            i = e;
            continue;
        }
        out += vf[i++];
    }
    return out;
}

struct EqValues { double b = 0, c = 1, s = 1, g = 1; };

std::optional<EqValues> ParseEq(const std::wstring& vf) {
    size_t i = 0;
    while (i < vf.size()) {
        if ((i == 0 || vf[i - 1] == L',') && vf.compare(i, kEqPrefix.size(), kEqPrefix) == 0) {
            size_t e = vf.find(L',', i);
            std::wstring args = vf.substr(i + kEqPrefix.size(), e == std::wstring::npos ? std::wstring::npos : e - i - kEqPrefix.size());
            EqValues v;
            size_t p = 0;
            while (p <= args.size()) {
                size_t q = args.find(L':', p);
                std::wstring kv = args.substr(p, q == std::wstring::npos ? std::wstring::npos : q - p);
                size_t eqp = kv.find(L'=');
                if (eqp != std::wstring::npos) {
                    std::wstring k = kv.substr(0, eqp);
                    double val = std::wcstod(kv.c_str() + eqp + 1, nullptr);
                    if (k == L"brightness") v.b = val;
                    else if (k == L"contrast") v.c = val;
                    else if (k == L"saturation") v.s = val;
                    else if (k == L"gamma") v.g = val;
                }
                if (q == std::wstring::npos) break;
                p = q + 1;
            }
            return v;
        }
        i++;
    }
    return std::nullopt;
}

struct Dec {
    std::string type;   // "" = software
    bool keep = false;
    std::string Label() const { return type.empty() ? "software" : type + (keep ? "(keep)" : "(download)"); }
};

struct Pres {
    std::string name;
    PresenterCap cap;
};

enum class SK { Ivtc, Deint, Transpose, HFlip, VFlip, Eq, Setpts };
struct Stage { SK kind; PipelineOp op; int dir = 0; };

struct Path {
    FrameDomain dom = FrameDomain::System;
    PlanCost cost;
    std::vector<PlanStep> steps;
    std::string uploadDev, filterHwDevice;
    PresenterParams pp;
    bool rotAbsorbed = false;
    std::vector<SK> absorbed; // validate against the final presenter input domain after later filter transitions
    std::vector<std::string> notes;   // notes caused by the choices of this path (e.g. residual-combing fallback unavailable)
};

struct Ctx {
    Ctx(const CapabilitySet& c_, const PipelineTable& t_, const PlanRequest& r_) : caps(c_), table(t_), req(r_) {}
    const CapabilitySet& caps;
    const PipelineTable& table;
    const PlanRequest& req;
    FilterSettings s;
    IvtcTuning tun;
    std::wstring fmt;
    std::wstring raw;
    bool rawOpaque = false;
    std::vector<std::wstring> tokens;
    std::vector<Stage> stages;
    std::vector<std::string> notes;
    ContentInfo content;
    bool Black(const std::string& k) const { return req.blacklist.count(k) != 0; }
    bool BlackFilter(const std::string& name) const {
        if (Black("filter:" + name)) return true;
        size_t p = 0;
        while (p <= name.size()) {
            size_t q = name.find(',', p);
            std::string part = name.substr(p, q == std::string::npos ? std::string::npos : q - p);
            if (Black("filter:" + part)) return true;
            if (q == std::string::npos) break;
            p = q + 1;
        }
        return false;
    }
};

PlanStep MakeStep(PlanStep::Kind k, const std::string& name, const std::wstring& text, FrameDomain in, FrameDomain out,
                  CopyKind copy, int copies, const std::string& reason) {
    PlanStep s;
    s.kind = k; s.name = name; s.text = text; s.in = in; s.out = out; s.copy = copy; s.copies = copies; s.reason = reason;
    return s;
}

bool Upload(const Ctx& c, Path& p, FrameDomain to, bool withFormat) {
    std::string dev = DeviceTypeOfDomain(to);
    if (dev.empty() || !c.caps.DeviceOk(dev)) return false;
    if (!p.uploadDev.empty() && p.uploadDev != dev) return false;
    p.uploadDev = dev;
    p.filterHwDevice = dev;
    p.steps.push_back(MakeStep(PlanStep::Kind::Transfer, "hwupload", withFormat ? L"format=" + c.fmt + L",hwupload" : L"hwupload",
                               p.dom, to, CopyKind::Host, 1, "upload to " + dev));
    p.cost.hostCopies += 1;
    p.cost.nodes += withFormat ? 2 : 1;
    p.dom = to;
    return true;
}

void Download(const Ctx& c, Path& p) {
    p.steps.push_back(MakeStep(PlanStep::Kind::Transfer, "hwdownload", L"hwdownload,format=" + c.fmt, p.dom, FrameDomain::System,
                               CopyKind::Host, 1, "download from " + std::string(DomainName(p.dom))));
    p.cost.hostCopies += 1;
    p.cost.nodes += 2;
    p.dom = FrameDomain::System;
}

bool Go(const Ctx& c, Path& p, FrameDomain to) {
    if (to == FrameDomain::Any || p.dom == to) return true;
    if (to == FrameDomain::System) { Download(c, p); return true; }
    if (p.dom == FrameDomain::System) return Upload(c, p, to, true);
    std::string key = std::string("interop:") + DomainName(p.dom) + ">" + DomainName(to);
    if (c.caps.InteropOk(p.dom, to, "map") && !c.Black(key)) {
        std::string dev = DeviceTypeOfDomain(to);
        p.steps.push_back(MakeStep(PlanStep::Kind::Transfer, "hwmap", L"hwmap=derive_device=" + W(dev), p.dom, to, CopyKind::Map, 0,
                                   "map " + std::string(DomainName(p.dom)) + " to " + DomainName(to)));
        p.cost.nodes += 1;
        p.dom = to;
        return true;
    }
    Path q = p;
    Download(c, q);
    if (!Upload(c, q, to, false)) return false;
    p = std::move(q);
    return true;
}

constexpr const char* kCpuIvtc = "fieldmatch,bwdif,decimate";

/// bwdif used as the residual-combing cleanup after / inside an ivtc: one frame per frame, only frames flagged interlaced.
std::wstring ResidualDeintText(const FilterVariant& bwdif) {
    std::wstring name = bwdif.filter == kCpuIvtc ? L"bwdif" : W(bwdif.filter);
    std::wstring opts;
    auto add = [&](const char* key, const wchar_t* o) {
        if (bwdif.filter != kCpuIvtc && !bwdif.HasOption(key)) return;
        if (!opts.empty()) opts += L':';
        opts += o;
    };
    add("mode", L"mode=send_frame");
    add("parity", L"parity=auto");
    add("deint", L"deint=interlaced");
    return opts.empty() ? name : name + L"=" + opts;
}
std::wstring StageText(const Ctx& c, const Stage& st, const FilterVariant& v) {
    const FilterSettings& s = c.s;
    std::wstring name = W(v.filter);
    switch (st.kind) {
    case SK::Ivtc: {
        // CPU recipe (FFmpeg's recommended order, decimate last): fieldmatch, bwdif for residual combing, decimate.
        if (v.filter == kCpuIvtc) return L"fieldmatch," + ResidualDeintText(v) + L",decimate";
        if (v.filter.find(',') != std::string::npos) return name;
        std::wstring opts;
        auto add = [&](const char* key, const std::wstring& o) {
            if (!v.HasOption(key)) return;
            if (!opts.empty()) opts += L':';
            opts += o;
        };
        if (c.tun.fieldOrder != -1) add("tff", L"tff=" + std::to_wstring(c.tun.fieldOrder));
        if (c.tun.combThreshold != 9.0) add("cthresh", L"cthresh=" + FilterManager::Num(c.tun.combThreshold));
        if (c.tun.dupThreshold != 1.1) add("dupthresh", L"dupthresh=" + FilterManager::Num(c.tun.dupThreshold));
        if (c.tun.sceneThreshold != 15.0) add("scthresh", L"scthresh=" + FilterManager::Num(c.tun.sceneThreshold));
        return opts.empty() ? name : name + L"=" + opts;
    }
    case SK::Deint: {
        std::wstring opts;
        auto add = [&](const char* key, const std::wstring& o) {
            if (!v.HasOption(key)) return;
            if (!opts.empty()) opts += L':';
            opts += o;
        };
        add("mode", std::wstring(L"mode=") + (s.deinterlaceRate == DeinterlaceRate::Field ? L"send_field" : L"send_frame"));
        add("parity", L"parity=auto");
        add("deint", std::wstring(L"deint=") + (s.deinterlaceOnlyInterlaced ? L"interlaced" : L"all"));
        return opts.empty() ? name : name + L"=" + opts;
    }
    case SK::Transpose: {
        std::wstring d = std::to_wstring(st.dir);
        if (v.domain != FrameDomain::System && v.HasOption("dir") && v.optionsKnown) return name + L"=dir=" + d;
        if (v.domain != FrameDomain::System && !v.optionsKnown) return name + L"=dir=" + d;
        return name + L"=" + d;
    }
    case SK::HFlip: case SK::VFlip: return name;
    case SK::Eq: {
        std::wstring args = L"brightness=" + FilterManager::Num(s.brightness) + L":contrast=" + FilterManager::Num(s.contrast) +
                            L":saturation=" + FilterManager::Num(s.saturation) + L":gamma=" + FilterManager::Num(s.gamma);
        return (v.domain == FrameDomain::System ? name + L"@color=" : name + L"=") + args;
    }
    case SK::Setpts: return L"setpts=PTS/" + FilterManager::Num(s.speed);
    }
    return name;
}

FilterVariant FallbackVariant(PipelineOp op) {
    FilterVariant v;
    v.op = op;
    v.domain = FrameDomain::System;
    v.cpuLoad = 1;
    v.discoveredBy = "default";
    switch (op) {
    case PipelineOp::Ivtc: v.filter = kCpuIvtc; break;
    case PipelineOp::Bwdif: v.filter = "bwdif"; break;
    case PipelineOp::Yadif: v.filter = "yadif"; break;
    case PipelineOp::Transpose: v.filter = "transpose"; break;
    case PipelineOp::HFlip: v.filter = "hflip"; break;
    case PipelineOp::VFlip: v.filter = "vflip"; break;
    case PipelineOp::Eq: v.filter = "eq"; break;
    default: v.filter = "null"; break;
    }
    return v;
}

const char* PriorityName(DeinterlacePriority p) { return p == DeinterlacePriority::Quality ? "quality" : "efficiency"; }

/// Deinterlace On: every GPU bwdif; a GPU yadif only in a domain the build has no bwdif for; on the CPU yadif (Efficiency) or
/// bwdif (Quality).
std::vector<FilterVariant> DeintCandidates(const Ctx& c) {
    const bool quality = c.s.deinterlacePriority == DeinterlacePriority::Quality;
    const std::vector<FilterVariant> bw = c.table.VariantsFor(PipelineOp::Bwdif), ya = c.table.VariantsFor(PipelineOp::Yadif);
    auto bwdifIn = [&](FrameDomain d) {
        return std::any_of(bw.begin(), bw.end(), [&](const FilterVariant& b) { return b.domain == d; });
    };
    std::vector<FilterVariant> out;
    bool cpu = false;
    for (const FilterVariant& b : bw)
        if (IsHardware(b.domain) || quality) { out.push_back(b); cpu = cpu || !IsHardware(b.domain); }
    for (const FilterVariant& y : ya)
        if (IsHardware(y.domain) ? !bwdifIn(y.domain) : !quality) { out.push_back(y); cpu = cpu || !IsHardware(y.domain); }
    if (!cpu) out.push_back(FallbackVariant(quality ? PipelineOp::Bwdif : PipelineOp::Yadif));
    return out;
}

std::string DeintReason(const Ctx& c, const FilterVariant& v) {
    std::string r = std::string("priority ") + PriorityName(c.s.deinterlacePriority) + ": ";
    if (IsHardware(v.domain)) return r + "GPU deinterlacer -> " + v.filter;
    bool gpu = false;
    for (const FilterVariant& o : DeintCandidates(c))
        if (IsHardware(o.domain) && !c.BlackFilter(o.filter) && c.caps.DeviceOk(DeviceTypeOfDomain(o.domain))) gpu = true;
    return r + (gpu ? "GPU deinterlacer not on the cheapest path -> " : "no GPU deinterlacer -> ") + v.filter;
}

struct Sink {
    bool haveBest = false;
    Plan best;
    std::string bestKey;
    std::map<std::string, Plan> perPair;
    std::vector<std::string> pairOrder;
};

bool CanAbsorb(const PresenterCap& pc, FrameDomain input, const FilterSettings& s, SK k) {
    // CUDA imports always run through the CUDA shader, independently of the VP and of the
    // `shader` cap (which describes D3D11/system inputs only).
    const bool shader = ((input == FrameDomain::D3D11 || input == FrameDomain::System) && pc.shader) ||
                        (input == FrameDomain::Cuda && pc.importCuda);
    switch (k) {
    case SK::Transpose: return pc.vpRotate || shader;
    case SK::HFlip: case SK::VFlip: return pc.vpMirror || shader;
    case SK::Eq:
        if (shader) return true;
        return pc.vpProcamp && s.gamma == 1 && s.brightness * 100 >= pc.vpBrightnessMin && s.brightness * 100 <= pc.vpBrightnessMax &&
               s.contrast >= pc.vpContrastMin && s.contrast <= pc.vpContrastMax && s.saturation >= pc.vpSaturationMin &&
               s.saturation <= pc.vpSaturationMax;
    default: return false;
    }
}

bool ApplyRaw(const Ctx& c, Path& p) {
    if (c.raw.empty()) return true;
    if (c.rawOpaque) {
        if (!Go(c, p, FrameDomain::System)) return false;
        p.steps.push_back(MakeStep(PlanStep::Kind::Filter, "raw", c.raw, FrameDomain::System, FrameDomain::System, CopyKind::None, 0,
                                   "opaque custom graph (system frames)"));
        p.cost.nodes += c.tokens.empty() ? 1 : static_cast<int>(c.tokens.size());
        return true;
    }
    std::vector<size_t> tokIdx;
    int mid = 0;
    for (size_t i = 0; i < c.tokens.size(); i++) {
        const std::wstring& tok = c.tokens[i];
        std::string name = PipelinePlanner::TokenFilterName(tok);
        FrameDomain dom = c.table.DomainOf(name);
        if (dom != FrameDomain::Any && dom != p.dom) {
            size_t before = p.steps.size();
            if (!Go(c, p, dom)) return false;
            if (i > 0 && p.steps.size() > before) mid++;
        }
        tokIdx.push_back(p.steps.size());
        p.steps.push_back(MakeStep(PlanStep::Kind::Filter, name, tok, p.dom, p.dom, CopyKind::None, 0, "custom filter"));
        p.cost.nodes += 1;
    }
    if (mid == 0 && !tokIdx.empty()) {
        // no transition inside the raw chain: keep the user's text verbatim
        FrameDomain d = p.dom;
        p.steps.erase(p.steps.begin() + static_cast<std::ptrdiff_t>(tokIdx.front()), p.steps.end());
        p.steps.push_back(MakeStep(PlanStep::Kind::Filter, "raw", c.raw, d, d, CopyKind::None, 0, "custom chain, verbatim"));
    }
    return true;
}

Plan BuildPlan(const Ctx& c, const Dec& d, const Pres& pr, Path&& p, const std::string& presentReason) {
    Plan out;
    out.ok = true;
    out.hwaccel = d.type;
    out.hwframesKeep = d.keep && !d.type.empty();
    out.presenter = pr.name;
    out.presenterParams = p.pp;
    out.filterHwDevice = p.filterHwDevice;
    out.cost = p.cost;
    std::wstring vf;
    for (const PlanStep& s : p.steps) Join(vf, s.text);
    out.videoFilter = vf;
    out.vfStructure = StructureOf(vf);
    out.steps = std::move(p.steps);
    out.notes = std::move(p.notes);
    (void)presentReason;
    (void)c;
    return out;
}

void Finish(const Ctx& c, const Dec& d, const Pres& pr, Path p, Sink& sink) {
    if (!ApplyRaw(c, p)) return;
    const PresenterCap& pc = pr.cap;
    auto accepts = [&](FrameDomain dom) { return std::find(pc.inputs.begin(), pc.inputs.end(), dom) != pc.inputs.end(); };
    FrameDomain in;
    CopyKind copy = CopyKind::None;
    int copies = 0;
    std::string reason;
    if (p.dom == FrameDomain::System) {
        if (!accepts(FrameDomain::System)) return;
        in = FrameDomain::System; copy = CopyKind::Host; copies = 1; reason = "upload of system frames";
        p.cost.hostCopies += 1;
    } else if (p.dom == FrameDomain::Cuda && pc.importCuda && !c.Black("interop:cuda>d3d11")) {
        in = p.dom;
        copy = pc.importCudaZeroCopy ? CopyKind::None : CopyKind::Device;
        copies = pc.importCudaZeroCopy ? 0 : 1;
        reason = pc.importCudaZeroCopy ? "cuda zero-copy (registered pool)" : "per-plane import";
        p.cost.deviceCopies += copies;
    } else if (accepts(p.dom) && !(p.dom == FrameDomain::Cuda && pc.importCuda)) {
        in = p.dom; reason = "hw frames presented directly";
    } else {
        if (!accepts(FrameDomain::System)) return;
        Download(c, p);
        in = FrameDomain::System; copy = CopyKind::Host; copies = 1; reason = "upload after download";
        p.cost.hostCopies += 1;
    }
    // A later filter or forced fallback may have changed the actual presenter input domain.
    for (SK k : p.absorbed) if (!CanAbsorb(pc, in, c.s, k)) return;
    p.steps.push_back(MakeStep(PlanStep::Kind::Present, "present:" + pr.name, L"", in, in, copy, copies, reason));
    Plan plan = BuildPlan(c, d, pr, std::move(p), reason);
    std::string key = d.Label() + "|" + pr.name;
    if (!sink.haveBest || plan.cost < sink.best.cost) { sink.best = plan; sink.bestKey = key; sink.haveBest = true; }
    auto it = sink.perPair.find(key);
    if (it == sink.perPair.end()) { sink.pairOrder.push_back(key); sink.perPair.emplace(key, std::move(plan)); }
    else if (plan.cost < it->second.cost) it->second = std::move(plan);
}

void Search(const Ctx& c, const Dec& d, const Pres& pr, size_t i, const Path& p, Sink& sink) {
    if (i == c.stages.size()) { Finish(c, d, pr, p, sink); return; }
    const Stage& st = c.stages[i];
    const FilterSettings& s = c.s;

    bool absorbBlack = (st.kind == SK::Transpose && c.Black("absorb:rotate")) || (st.kind == SK::Eq && c.Black("absorb:eq")) ||
                       ((st.kind == SK::HFlip || st.kind == SK::VFlip) && c.Black("absorb:flip"));
    if (pr.name == "d3d11" && c.raw.empty() && !absorbBlack && CanAbsorb(pr.cap, p.dom, s, st.kind)) {
        Path q = p;
        std::string what;
        switch (st.kind) {
        case SK::Transpose: q.pp.rotation = st.dir == 1 ? 90 : 270; q.rotAbsorbed = true; what = "transpose"; break;
        case SK::HFlip: q.pp.flipH = true; what = "hflip"; break;
        case SK::VFlip: q.pp.flipV = true; what = "vflip"; break;
        case SK::Eq:
            q.pp.eq = true; q.pp.brightness = s.brightness; q.pp.contrast = s.contrast; q.pp.saturation = s.saturation;
            q.pp.gamma = s.gamma; what = "eq"; break;
        default: break;
        }
        if (!what.empty()) {
            q.absorbed.push_back(st.kind);
            q.steps.push_back(MakeStep(PlanStep::Kind::Filter, "absorbed:" + what, L"", q.dom, q.dom, CopyKind::None, 0,
                                       "absorbed by presenter"));
            Search(c, d, pr, i + 1, q, sink);
        }
    }
    // a flip filter would run before an absorbed rotation: not allowed
    if ((st.kind == SK::HFlip || st.kind == SK::VFlip) && p.rotAbsorbed) return;

    if (st.kind == SK::Setpts) {
        Path q = p;
        q.steps.push_back(MakeStep(PlanStep::Kind::Filter, "setpts", StageText(c, st, FilterVariant{}), q.dom, q.dom, CopyKind::None, 0,
                                   "speed"));
        q.cost.nodes += 1;
        Search(c, d, pr, i + 1, q, sink);
        return;
    }
    std::vector<FilterVariant> vars = st.kind == SK::Deint ? DeintCandidates(c) : c.table.VariantsFor(st.op);
    if (vars.empty()) vars.push_back(FallbackVariant(st.op));
    for (const FilterVariant& v : vars) {
        if (c.BlackFilter(v.filter)) continue;
        Path q = p;
        if (v.domain != FrameDomain::Any && !Go(c, q, v.domain)) continue;
        q.cost.cpuLoad += v.cpuLoad;
        q.cost.nodes += CountNodes(v.filter);
        q.steps.push_back(MakeStep(PlanStep::Kind::Filter, v.filter, StageText(c, st, v), q.dom, q.dom, CopyKind::None, 0,
                                   st.kind == SK::Deint ? DeintReason(c, v) : std::string(OpName(st.op)) + " via " + v.filter));
        if (st.kind == SK::Ivtc && v.filter != kCpuIvtc && v.domain != FrameDomain::System && v.domain != FrameDomain::Any) {
            // Hardware ivtc clears AV_FRAME_FLAG_INTERLACED on its output: a following deint=interlaced would do nothing and
            // deint=all would degrade every frame. Only a variant that flags residual-combed frames gets a cleanup, in its own domain.
            if (v.flagsCombed) {
                const FilterVariant* same = nullptr;
                std::vector<FilterVariant> bw = c.table.VariantsFor(PipelineOp::Bwdif);
                for (const FilterVariant& b : bw)
                    if (b.domain == v.domain && !c.BlackFilter(b.filter)) { same = &b; break; }
                if (same) {
                    q.cost.cpuLoad += same->cpuLoad;
                    q.cost.nodes += 1;
                    q.steps.push_back(MakeStep(PlanStep::Kind::Filter, same->filter, ResidualDeintText(*same), q.dom, q.dom,
                                               CopyKind::None, 0, "residual-combing cleanup (frames flagged interlaced)"));
                } else {
                    q.notes.push_back(std::string("residual-combing fallback: unavailable (no ") + DomainName(v.domain) + " bwdif)");
                }
            } else {
                q.notes.push_back("residual-combing fallback: unavailable (GPU ivtc)");
            }
        }
        Search(c, d, pr, i + 1, q, sink);
    }
}

std::string WhyWorse(const PlanCost& a, const PlanCost& best) {
    if (a.Weighted() != best.Weighted()) {
        int n = a.Weighted() - best.Weighted();
        return std::to_string(n) + " more host cop" + (n == 1 ? "y" : "ies") + (a.decodePenalty != best.decodePenalty ? " (incl. software-decode penalty)" : "");
    }
    if (a.deviceCopies != best.deviceCopies)
        return std::to_string(a.deviceCopies - best.deviceCopies) + " more device cop" + (a.deviceCopies - best.deviceCopies == 1 ? "y" : "ies");
    if (std::fabs(a.cpuLoad - best.cpuLoad) > 1e-9) return "higher CPU load";
    if (a.nodes != best.nodes) return std::to_string(a.nodes - best.nodes) + " more filter nodes";
    return "tie, later in candidate order";
}

std::string DeviceNote(const std::string& t) { return "forced decoder " + t + " has no usable device"; }

Plan MakeLegacy(const PlanRequest& req) {
    FilterManager fm;
    fm.SetIvtcTuning(req.ivtc);
    fm.SetContent(req.content);
    fm.Apply(req.settings);
    Plan p;
    p.ok = true;
    p.hwaccel = "";
    p.presenter = "sdl";
    p.videoFilter = fm.VideoFilter();
    p.vfStructure = StructureOf(p.videoFilter);
    p.steps.push_back(MakeStep(PlanStep::Kind::Decode, "decode:legacy", L"", FrameDomain::System, FrameDomain::System, CopyKind::None, 0,
                               "decoder chosen by the user setting"));
    if (!p.videoFilter.empty())
        p.steps.push_back(MakeStep(PlanStep::Kind::Filter, "legacy-vf", p.videoFilter, FrameDomain::System, FrameDomain::System,
                                   CopyKind::None, 0, "FilterManager composition"));
    p.steps.push_back(MakeStep(PlanStep::Kind::Present, "present:sdl", L"", FrameDomain::System, FrameDomain::System, CopyKind::Host, 1,
                               "upload of system frames"));
    p.cost.hostCopies = 1;
    if (!p.videoFilter.empty()) {
        std::optional<std::vector<std::wstring>> t = PipelinePlanner::TokenizeChain(p.videoFilter);
        p.cost.nodes = t ? static_cast<int>(t->size()) : 1;
    }
    p.openOptions = PipelinePlanner::OpenOptionsFor(p, PlanPolicy::Legacy);
    return p;
}

}

const char* ChromaName(Chroma c) {
    switch (c) {
    case Chroma::Yuv422: return "4:2:2";
    case Chroma::Yuv444: return "4:4:4";
    default: return "4:2:0";
    }
}

const char* CopyKindName(CopyKind k) {
    switch (k) {
    case CopyKind::None: return "none";
    case CopyKind::Host: return "host";
    case CopyKind::Device: return "device";
    case CopyKind::Map: return "map";
    }
    return "none";
}

bool PlanCost::operator<(const PlanCost& o) const {
    if (Weighted() != o.Weighted()) return Weighted() < o.Weighted();
    if (deviceCopies != o.deviceCopies) return deviceCopies < o.deviceCopies;
    if (std::fabs(cpuLoad - o.cpuLoad) > 1e-9) return cpuLoad < o.cpuLoad;
    return nodes < o.nodes;
}

bool PlanCost::operator==(const PlanCost& o) const {
    return hostCopies == o.hostCopies && decodePenalty == o.decodePenalty && deviceCopies == o.deviceCopies && std::fabs(cpuLoad - o.cpuLoad) <= 1e-9 && nodes == o.nodes;
}

std::string Plan::Summary() const {
    std::string s = hwaccel.empty() ? "sw" : hwaccel + (hwframesKeep ? "(keep)" : "");
    for (const PlanStep& st : steps) {
        if (st.kind == PlanStep::Kind::Decode) continue;
        s += " > " + st.name;
        if (st.kind == PlanStep::Kind::Present && (st.copy == CopyKind::Device || st.reason == "cuda zero-copy (registered pool)"))
            s += std::string("(import ") + DomainName(st.in) + ")";
    }
    s += " [host " + std::to_string(cost.hostCopies);
    if (cost.decodePenalty) s += " (+" + std::to_string(cost.decodePenalty) + " sw-decode)";
    s += ", device " + std::to_string(cost.deviceCopies) + "]";
    return s;
}

PipelinePlanner::PipelinePlanner(CapabilitySet caps) : caps_(std::move(caps)), table_(PipelineTable::Discover(caps_)) {}

double PipelinePlanner::DecodeLoad(const ContentInfo& c) {
    double w = 1;
    if (c.codec == "mpeg2video") w = 0.5;
    else if (c.codec == "h264") w = 1;
    else if (c.codec == "hevc" || c.codec == "vp9") w = 2;
    else if (c.codec == "av1") w = 3;
    if (c.bitDepth > 8) w *= 1.25;
    if (c.width > 0 && c.height > 0) w *= static_cast<double>(c.width) * c.height / (1920.0 * 1080.0);
    return w;
}

std::optional<std::vector<std::wstring>> PipelinePlanner::TokenizeChain(const std::wstring& chain) {
    std::vector<std::wstring> out;
    std::wstring cur;
    bool quote = false;
    auto push = [&] {
        std::wstring t = Trim(cur);
        if (!t.empty()) out.push_back(t);
        cur.clear();
    };
    for (size_t i = 0; i < chain.size(); i++) {
        wchar_t ch = chain[i];
        if (quote) { cur += ch; if (ch == L'\'') quote = false; continue; }
        if (ch == L'\\') { cur += ch; if (i + 1 < chain.size()) cur += chain[++i]; continue; }
        if (ch == L'\'') { quote = true; cur += ch; continue; }
        if (ch == L';' || ch == L'[' || ch == L']') return std::nullopt;
        if (ch == L',') { push(); continue; }
        cur += ch;
    }
    push();
    return out;
}

std::string PipelinePlanner::TokenFilterName(const std::wstring& token) {
    size_t p = token.find_first_of(L"=@");
    return N(Trim(token.substr(0, p)));
}

std::wstring PresenterParamsJson(const PresenterParams& pp) {
    std::wstring j = L"{\"rotation\":" + std::to_wstring(pp.rotation) + L",\"flip_h\":" + (pp.flipH ? L"true" : L"false") +
                     L",\"flip_v\":" + (pp.flipV ? L"true" : L"false") + L",\"eq\":";
    if (!pp.eq) j += L"null";
    else j += L"{\"brightness\":" + FilterManager::Num(pp.brightness) + L",\"contrast\":" + FilterManager::Num(pp.contrast) +
              L",\"saturation\":" + FilterManager::Num(pp.saturation) + L",\"gamma\":" + FilterManager::Num(pp.gamma) + L"}";
    return j + L"}";
}

OpenOptions PipelinePlanner::OpenOptionsFor(const Plan& p, PlanPolicy policy) {
    OpenOptions o;
    if (policy == PlanPolicy::Cost) {
        if (!p.hwaccel.empty()) o.emplace_back(L"hwaccel", W(p.hwaccel));
        if (p.hwframesKeep) o.emplace_back(L"hwframes", L"keep");
        else if (!p.hwaccel.empty() && p.presenter == "d3d11") o.emplace_back(L"hwframes", L"download");
        if (p.presenter != "sdl") o.emplace_back(L"presenter", W(p.presenter));
        if (p.presenter != "sdl" && !p.presenterParams.IsIdentity()) o.emplace_back(L"presenter_params", PresenterParamsJson(p.presenterParams));
    }
    if (!p.videoFilter.empty()) o.emplace_back(L"vf", p.videoFilter);
    if (!p.filterHwDevice.empty()) o.emplace_back(L"filter_hw_device", W(p.filterHwDevice));
    return o;
}

PlanDelta PipelinePlanner::Diff(const Plan& before, const Plan& after) {
    PlanDelta d;
    bool reopen = before.hwaccel != after.hwaccel || before.hwframesKeep != after.hwframesKeep || before.presenter != after.presenter ||
                  before.filterHwDevice != after.filterHwDevice;
    bool rebuild = before.vfStructure != after.vfStructure;
    auto be = ParseEq(before.videoFilter), ae = ParseEq(after.videoFilter);
    if (be && ae) {
        auto cmd = [&](const wchar_t* name, double now, double was) {
            if (now != was) d.commands.push_back({ FilterManager::kEqInstance, name, FilterManager::Num(now) });
        };
        cmd(L"brightness", ae->b, be->b);
        cmd(L"contrast", ae->c, be->c);
        cmd(L"saturation", ae->s, be->s);
        cmd(L"gamma", ae->g, be->g);
    }
    bool paramsDiffer = !(before.presenterParams == after.presenterParams);
    if (paramsDiffer) d.params = after.presenterParams;
    if (reopen) { d.kind = PlanDeltaKind::Reopen; d.reason = "decoder, presenter or filter device changed"; }
    else if (rebuild) { d.kind = PlanDeltaKind::GraphRebuild; d.reason = "filter graph structure changed"; }
    else if (!d.commands.empty()) { d.kind = PlanDeltaKind::FilterCommands; d.reason = "eq values changed"; }
    else if (paramsDiffer) { d.kind = PlanDeltaKind::PresenterParams; d.reason = "presenter parameters changed"; }
    return d;
}

Plan PipelinePlanner::Make(const PlanRequest& req) const {
    if (req.policy == PlanPolicy::Legacy) return MakeLegacy(req);

    Ctx c(caps_, table_, req);
    c.content = req.content;
    c.s = FilterManager::Sanitize(req.settings);
    {
        FilterManager fm;
        fm.SetIvtcTuning(req.ivtc);
        c.tun = fm.Ivtc();
    }
    c.fmt = req.content.bitDepth > 8 ? L"p010le" : FilterManager::Escape(c.tun.pixelFormat);
    const FilterSettings& s = c.s;

    if (!s.advancedMode) {
        if (s.deinterlace == DeinterlaceMode::Ivtc) c.stages.push_back({ SK::Ivtc, PipelineOp::Ivtc, 0 });
        else if (s.deinterlace == DeinterlaceMode::On) c.stages.push_back({ SK::Deint, PipelineOp::Bwdif, 0 });
        bool h = s.flipHorizontal, v = s.flipVertical;
        if (s.rotation == Rotation::Rotate180) { h = !h; v = !v; }
        if (s.rotation == Rotation::Cw90) c.stages.push_back({ SK::Transpose, PipelineOp::Transpose, 1 });
        else if (s.rotation == Rotation::Ccw90) c.stages.push_back({ SK::Transpose, PipelineOp::Transpose, 2 });
        if (h) c.stages.push_back({ SK::HFlip, PipelineOp::HFlip, 0 });
        if (v) c.stages.push_back({ SK::VFlip, PipelineOp::VFlip, 0 });
        if (s.brightness != 0 || s.contrast != 1 || s.saturation != 1 || s.gamma != 1) c.stages.push_back({ SK::Eq, PipelineOp::Eq, 0 });
        if (s.speed != 1) c.stages.push_back({ SK::Setpts, PipelineOp::Setpts, 0 });
    }
    c.raw = s.customVideoFilter;
    if (!c.raw.empty()) {
        auto toks = TokenizeChain(c.raw);
        if (!toks || toks->empty()) c.rawOpaque = true;
        else {
            c.tokens = *toks;
            for (const std::wstring& t : c.tokens) {
                std::string n = TokenFilterName(t);
                if (n == "hwupload" || n == "hwdownload" || n == "hwmap" || n == "format") c.rawOpaque = true;
            }
        }
    }

    std::vector<std::string> notes;
    if (!req.content.Known()) notes.push_back("content unknown: planned for 1080p 8-bit");
    const bool chroma420 = req.content.chroma == Chroma::Yuv420;
    if (!chroma420) notes.push_back(std::string("chroma ") + ChromaName(req.content.chroma) + ": hw frames are downloaded (no hwframes=keep)");

    std::vector<Dec> decs;
    const bool keepOk = caps_.HasFeature("hwframes_keep") && !c.Black("hwframes_keep") && chroma420;
    auto addType = [&](const std::string& type) {
        if (!DomainOfDeviceType(type)) return;
        decs.push_back({ type, false });
        if (keepOk) decs.push_back({ type, true });
    };
    auto usable = [&](const std::string& type) {
        if (!caps_.DeviceOk(type) || c.Black("hwaccel:" + type)) return false;
        if (!req.content.Known()) return true;
        const DecoderCap* dc = caps_.Decoder(req.content.codec, type);
        if (!dc || !dc->ok) return false;
        if (!dc->bitDepths.empty() && std::find(dc->bitDepths.begin(), dc->bitDepths.end(), req.content.bitDepth) == dc->bitDepths.end())
            return false;
        if (dc->maxWidth > 0 && req.content.width > dc->maxWidth) return false;
        if (dc->maxHeight > 0 && req.content.height > dc->maxHeight) return false;
        return true;
    };
    static const char* const kOrder[] = { "d3d11va", "cuda", "d3d12va", "dxva2" };
    bool software = false;
    DecoderPref pref = req.decoder;
    if (pref == DecoderPref::Forced) {
        if (req.forcedDecoder.empty() || !DomainOfDeviceType(req.forcedDecoder)) {
            notes.push_back("forced decoder unknown: using automatic choice");
            pref = DecoderPref::Auto;
        } else if (c.Black("hwaccel:" + req.forcedDecoder)) {
            notes.push_back("forced decoder " + req.forcedDecoder + " is blacklisted: software decoding");
            software = true;
        } else {
            if (!caps_.DeviceOk(req.forcedDecoder)) notes.push_back(DeviceNote(req.forcedDecoder));
            addType(req.forcedDecoder);
        }
    }
    if (pref == DecoderPref::Auto || pref == DecoderPref::PreferGpu) {
        for (const char* t : kOrder) if (usable(t)) addType(t);
        if (pref == DecoderPref::Auto) software = true;
        else if (decs.empty()) { software = true; notes.push_back("no GPU decoder available: software decoding"); }
    } else if (pref == DecoderPref::Software) {
        software = true;
    }
    if (software) decs.push_back({ "", false });

    std::vector<Pres> press;
    {
        const PresenterCap* d3 = caps_.Presenter("d3d11");
        const PresenterCap* sd = caps_.Presenter("sdl");
        bool d3ok = d3 && d3->ok && !c.Black("presenter:d3d11") && req.presenter != PresenterPref::Sdl;
        bool sdok = !(sd && !sd->ok) && !c.Black("presenter:sdl") && req.presenter != PresenterPref::D3D11;
        if (req.presenter == PresenterPref::D3D11 && !d3ok) {
            notes.push_back("presenter d3d11 unavailable: using sdl");
            sdok = !(sd && !sd->ok) && !c.Black("presenter:sdl");
        }
        if (d3ok) press.push_back({ "d3d11", *d3 });
        if (sdok) {
            PresenterCap cap;
            if (sd) cap = *sd;
            else { cap.name = "sdl"; cap.ok = true; cap.inputs = { FrameDomain::System }; }
            press.push_back({ "sdl", cap });
        }
    }

    Plan result;
    result.notes = notes;
    if (press.empty() || decs.empty()) {
        result.ok = false;
        result.error = press.empty() ? "no presenter available" : "no decoder available";
        return result;
    }

    Sink sink;
    double load = DecodeLoad(req.content);
    for (const Dec& d : decs) {
        Path p;
        if (d.type.empty()) {
            int pen = static_cast<int>(std::floor(load + 1e-9));
            p.cost.decodePenalty = pen;
            p.cost.cpuLoad = load;
            p.steps.push_back(MakeStep(PlanStep::Kind::Decode, "decode:software", L"", FrameDomain::System, FrameDomain::System,
                                       CopyKind::None, 0, "software decoding (load " + std::to_string(load).substr(0, 5) + ")"));
        } else if (d.keep) {
            auto dom = DomainOfDeviceType(d.type);
            p.dom = *dom;
            p.steps.push_back(MakeStep(PlanStep::Kind::Decode, "decode:" + d.type, L"", FrameDomain::System, *dom, CopyKind::None, 0,
                                       "hwframes=keep"));
        } else {
            p.cost.hostCopies = 1;
            p.steps.push_back(MakeStep(PlanStep::Kind::Decode, "decode:" + d.type, L"", FrameDomain::System, FrameDomain::System,
                                       CopyKind::Host, 1, "hwframes=download"));
        }
        for (const Pres& pr : press) Search(c, d, pr, 0, p, sink);
    }
    if (!sink.haveBest) {
        result.ok = false;
        result.error = "no valid pipeline for this request";
        return result;
    }

    result = sink.best;
    {
        std::vector<std::string> pathNotes = std::move(result.notes);
        result.notes = notes;
        for (std::string& n : pathNotes) result.notes.push_back(std::move(n));
    }
    std::vector<RejectedAlternative> rej;
    {
        std::vector<const Plan*> others;
        for (const std::string& k : sink.pairOrder) if (k != sink.bestKey) others.push_back(&sink.perPair.at(k));
        std::stable_sort(others.begin(), others.end(), [](const Plan* a, const Plan* b) { return a->cost < b->cost; });
        for (const Plan* o : others) rej.push_back({ o->Summary(), o->cost, WhyWorse(o->cost, result.cost) });
    }
    std::vector<RejectedAlternative> blacklisted;
    for (const std::string& k : req.blacklist) {
        bool relevant = true;
        if (k.rfind("hwaccel:", 0) == 0) relevant = caps_.DeviceOk(k.substr(8));
        else if (k.rfind("filter:", 0) == 0) relevant = caps_.Filter(k.substr(7)) != nullptr;
        else if (k.rfind("presenter:", 0) == 0) relevant = caps_.Presenter(k.substr(10)) != nullptr;
        if (relevant) blacklisted.push_back({ k, PlanCost{}, "blacklisted: " + k });
    }
    if (blacklisted.size() > 3) blacklisted.resize(3);
    if (rej.size() + blacklisted.size() > 6) rej.resize(6 - blacklisted.size());
    rej.insert(rej.end(), blacklisted.begin(), blacklisted.end());
    result.rejected = std::move(rej);
    result.openOptions = OpenOptionsFor(result, PlanPolicy::Cost);
    return result;
}

}
