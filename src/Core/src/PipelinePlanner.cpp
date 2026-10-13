#include "wvp/PipelinePlanner.h"

#include "wvp/FilterManager.h"
#include "wvp/Utf8.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace wvp {

namespace {

std::string AsciiOnly(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        char32_t c = utf8::Next(s, i);
        out.push_back(c < 128 ? static_cast<char>(c) : '?');
    }
    return out;
}
bool IsAsciiSpace(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && IsAsciiSpace(s[a])) a++;
    while (b > a && IsAsciiSpace(s[b - 1])) b--;
    return s.substr(a, b - a);
}
void Join(std::string& out, const std::string& part) {
    if (part.empty()) return;
    if (!out.empty()) out += ',';
    out += part;
}
int CountNodes(const std::string& filterNames) {
    return static_cast<int>(std::count(filterNames.begin(), filterNames.end(), ',')) + 1;
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

enum class SK { Ivtc, Deint, Setpts };
struct Stage { SK kind; PipelineOp op; };

struct Path {
    FrameDomain dom = FrameDomain::System;
    PlanCost cost;
    std::vector<PlanStep> steps;
    std::string uploadDev, filterHwDevice;
    std::vector<std::string> notes;   // notes caused by the choices of this path (e.g. residual-combing fallback unavailable)
};

struct Ctx {
    Ctx(const CapabilitySet& c_, const PipelineTable& t_, const PlanRequest& r_) : caps(c_), table(t_), req(r_) {}
    const CapabilitySet& caps;
    const PipelineTable& table;
    const PlanRequest& req;
    FilterSettings s;
    IvtcTuning tun;
    std::string fmt;
    std::string raw;
    bool rawOpaque = false;
    std::vector<std::string> tokens;
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

PlanStep MakeStep(PlanStep::Kind k, const std::string& name, const std::string& text, FrameDomain in, FrameDomain out,
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
    p.steps.push_back(MakeStep(PlanStep::Kind::Transfer, "hwupload", withFormat ? "format=" + c.fmt + ",hwupload" : "hwupload",
                               p.dom, to, CopyKind::Host, 1, "upload to " + dev));
    p.cost.hostCopies += 1;
    p.cost.nodes += withFormat ? 2 : 1;
    p.dom = to;
    return true;
}

void Download(const Ctx& c, Path& p) {
    p.steps.push_back(MakeStep(PlanStep::Kind::Transfer, "hwdownload", "hwdownload,format=" + c.fmt, p.dom, FrameDomain::System,
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
    const std::string dev = DeviceTypeOfDomain(to);
    if (!dev.empty() && c.caps.InteropOk(p.dom, to, "map") && !c.Black(key)) {
        p.steps.push_back(MakeStep(PlanStep::Kind::Transfer, "hwmap", "hwmap=derive_device=" + dev, p.dom, to, CopyKind::Map, 0,
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
std::string ResidualDeintText(const FilterVariant& bwdif) {
    std::string name = bwdif.filter == kCpuIvtc ? "bwdif" : bwdif.filter;
    std::string opts;
    auto add = [&](const char* key, const char* o) {
        if (bwdif.filter != kCpuIvtc && !bwdif.HasOption(key)) return;
        if (!opts.empty()) opts += ':';
        opts += o;
    };
    add("mode", "mode=send_frame");
    add("parity", "parity=auto");
    add("deint", "deint=interlaced");
    return opts.empty() ? name : name + "=" + opts;
}
std::string StageText(const Ctx& c, const Stage& st, const FilterVariant& v) {
    const FilterSettings& s = c.s;
    std::string name = v.filter;
    switch (st.kind) {
    case SK::Ivtc: {
        // CPU recipe (FFmpeg's recommended order, decimate last): fieldmatch, bwdif for residual combing, decimate.
        if (v.filter == kCpuIvtc) return "fieldmatch," + ResidualDeintText(v) + ",decimate";
        if (v.filter.find(',') != std::string::npos) return name;
        std::string opts;
        auto add = [&](const char* key, const std::string& o) {
            if (!v.HasOption(key)) return;
            if (!opts.empty()) opts += ':';
            opts += o;
        };
        if (c.tun.fieldOrder != -1) add("tff", "tff=" + std::to_string(c.tun.fieldOrder));
        if (c.tun.combThreshold != 9.0) add("cthresh", "cthresh=" + FilterManager::Num(c.tun.combThreshold));
        if (c.tun.dupThreshold != 1.1) add("dupthresh", "dupthresh=" + FilterManager::Num(c.tun.dupThreshold));
        if (c.tun.sceneThreshold != 15.0) add("scthresh", "scthresh=" + FilterManager::Num(c.tun.sceneThreshold));
        return opts.empty() ? name : name + "=" + opts;
    }
    case SK::Deint: {
        std::string opts;
        auto add = [&](const char* key, const std::string& o) {
            if (!v.HasOption(key)) return;
            if (!opts.empty()) opts += ':';
            opts += o;
        };
        add("mode", std::string("mode=") + (s.deinterlaceRate == DeinterlaceRate::Field ? "send_field" : "send_frame"));
        add("parity", "parity=auto");
        add("deint", std::string("deint=") + (s.deinterlaceOnlyInterlaced ? "interlaced" : "all"));
        return opts.empty() ? name : name + "=" + opts;
    }
    case SK::Setpts: return "setpts=PTS/" + FilterManager::Num(s.speed);
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
        const std::string& tok = c.tokens[i];
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
    if (!d.type.empty() && c.content.Known())
        if (const DecoderCap* dc = c.caps.Decoder(c.content.codec, d.type); dc && !dc->decoder.empty() && dc->decoder != c.content.codec)
            out.decoder = dc->decoder;
    out.filterHwDevice = p.filterHwDevice;
    out.cost = p.cost;
    std::string vf;
    for (const PlanStep& s : p.steps) Join(vf, s.text);
    out.videoFilter = vf;
    out.steps = std::move(p.steps);
    out.notes = std::move(p.notes);
    (void)pr;
    (void)presentReason;
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
    } else if (accepts(p.dom)) {
        in = p.dom; reason = "hw frames presented directly";
    } else {
        if (!accepts(FrameDomain::System)) return;
        Download(c, p);
        in = FrameDomain::System; copy = CopyKind::Host; copies = 1; reason = "upload after download";
        p.cost.hostCopies += 1;
    }
    p.steps.push_back(MakeStep(PlanStep::Kind::Present, "present:" + pr.name, "", in, in, copy, copies, reason));
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
        if (st.kind == PlanStep::Kind::Present && st.copy == CopyKind::Device)
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

std::optional<std::vector<std::string>> PipelinePlanner::TokenizeChain(const std::string& chain) {
    std::vector<std::string> out;
    std::string cur;
    bool quote = false;
    auto push = [&] {
        std::string t = Trim(cur);
        if (!t.empty()) out.push_back(t);
        cur.clear();
    };
    for (size_t i = 0; i < chain.size(); i++) {
        char ch = chain[i];
        if (quote) { cur += ch; if (ch == '\'') quote = false; continue; }
        if (ch == '\\') { cur += ch; if (i + 1 < chain.size()) cur += chain[++i]; continue; }
        if (ch == '\'') { quote = true; cur += ch; continue; }
        if (ch == ';' || ch == '[' || ch == ']') return std::nullopt;
        if (ch == ',') { push(); continue; }
        cur += ch;
    }
    push();
    return out;
}

std::string PipelinePlanner::TokenFilterName(const std::string& token) {
    size_t p = token.find_first_of("=@");
    return AsciiOnly(Trim(token.substr(0, p)));
}

OpenOptions PipelinePlanner::OpenOptionsFor(const Plan& p) {
    OpenOptions o;
    if (!p.hwaccel.empty()) {
        o.emplace_back("hwaccel", p.hwaccel);
        if (!p.decoder.empty()) o.emplace_back("vcodec", p.decoder);
        o.emplace_back("hwframes", p.hwframesKeep ? "keep" : "download");
    }
    if (!p.videoFilter.empty()) o.emplace_back("vf", p.videoFilter);
    if (!p.filterHwDevice.empty()) o.emplace_back("filter_hw_device", p.filterHwDevice);
    return o;
}

PlanDelta PipelinePlanner::Diff(const Plan& before, const Plan& after) {
    PlanDelta d;
    if (before.hwaccel != after.hwaccel || before.decoder != after.decoder || before.hwframesKeep != after.hwframesKeep ||
        before.filterHwDevice != after.filterHwDevice) {
        d.kind = PlanDeltaKind::Reopen;
        d.reason = "decoder or filter device changed";
    } else if (before.videoFilter != after.videoFilter) {
        d.kind = PlanDeltaKind::GraphRebuild;
        d.reason = "filter graph changed";
    }
    return d;
}

Plan PipelinePlanner::Make(const PlanRequest& req) const {
    Ctx c(caps_, table_, req);
    c.content = req.content;
    c.s = FilterManager::Sanitize(req.settings);
    {
        FilterManager fm;
        fm.SetIvtcTuning(req.ivtc);
        c.tun = fm.Ivtc();
    }
    c.fmt = req.content.bitDepth > 8 ? "p010le" : FilterManager::Escape(c.tun.pixelFormat);
    const FilterSettings& s = c.s;

    if (!s.advancedMode) {
        if (!req.content.progressive && s.deinterlace == DeinterlaceMode::Ivtc) c.stages.push_back({ SK::Ivtc, PipelineOp::Ivtc });
        else if (!req.content.progressive && s.deinterlace == DeinterlaceMode::On) c.stages.push_back({ SK::Deint, PipelineOp::Bwdif });
        if (s.speed != 1) c.stages.push_back({ SK::Setpts, PipelineOp::Setpts });
    }
    c.raw = s.customVideoFilter;
    if (!c.raw.empty()) {
        auto toks = TokenizeChain(c.raw);
        if (!toks || toks->empty()) c.rawOpaque = true;
        else {
            c.tokens = *toks;
            for (const std::string& t : c.tokens) {
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
    static const char* const kOrder[] = { "d3d11va", "videotoolbox", "d3d12va", "dxva2" };
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

    std::optional<Pres> pres;
    for (const PresenterCap& pc : caps_.presenters)
        if (pc.ok) { pres = Pres{ pc.name, pc }; break; }

    Plan result;
    result.notes = notes;
    if (!pres || decs.empty()) {
        result.ok = false;
        result.error = !pres ? "no presenter available" : "no decoder available";
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
            p.steps.push_back(MakeStep(PlanStep::Kind::Decode, "decode:software", "", FrameDomain::System, FrameDomain::System,
                                       CopyKind::None, 0, "software decoding (load " + std::to_string(load).substr(0, 5) + ")"));
        } else if (d.keep) {
            auto dom = DomainOfDeviceType(d.type);
            p.dom = *dom;
            p.steps.push_back(MakeStep(PlanStep::Kind::Decode, "decode:" + d.type, "", FrameDomain::System, *dom, CopyKind::None, 0,
                                       "hwframes=keep"));
        } else {
            p.cost.hostCopies = 1;
            p.steps.push_back(MakeStep(PlanStep::Kind::Decode, "decode:" + d.type, "", FrameDomain::System, FrameDomain::System,
                                       CopyKind::Host, 1, "hwframes=download"));
        }
        Search(c, d, *pres, 0, p, sink);
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
        if (relevant) blacklisted.push_back({ k, PlanCost{}, "blacklisted: " + k });
    }
    if (blacklisted.size() > 3) blacklisted.resize(3);
    if (rej.size() + blacklisted.size() > 6) rej.resize(6 - blacklisted.size());
    rej.insert(rej.end(), blacklisted.begin(), blacklisted.end());
    result.rejected = std::move(rej);
    result.openOptions = OpenOptionsFor(result);
    return result;
}

}
