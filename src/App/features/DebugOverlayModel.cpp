#include "features/DebugOverlayModel.h"

#include "features/PipelineOverlay.h"
#include "wvp/PipelineTable.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <set>

namespace app::dbg {

namespace {
using wvp::DebugInfo;

std::string Fmt(const char* f, double v) {
    char b[64];
    std::snprintf(b, sizeof b, f, v);
    return b;
}
std::string Num(const std::optional<double>& v, const char* f = "%.2f") { return v ? Fmt(f, *v) : "?"; }
std::string Int(const std::optional<int>& v) { return v ? std::to_string(*v) : "?"; }
std::string I64(const std::optional<std::int64_t>& v) { return v ? std::to_string(*v) : "?"; }
std::string S(const std::string& s, const char* dflt = "?") { return s.empty() ? dflt : s; }
// The drawing path the presenter reports ("d3d11:videoprocessor"), else its name.
std::string PresenterOutput(const wvp::DebugInfo& d) { return d.Presenter.Renderer.empty() ? d.Presenter.Name : d.Presenter.Renderer; }
std::string Size(const std::optional<int>& w, const std::optional<int>& h) { return (w && h) ? std::to_string(*w) + "x" + std::to_string(*h) : "?"; }
std::string Bytes(const std::optional<std::int64_t>& v) {
    if (!v) return "?";
    double b = (double)*v;
    if (b < 1024) return std::to_string(*v) + "B";
    if (b < 1024 * 1024) return Fmt("%.1f", b / 1024) + "KB";
    return Fmt("%.1f", b / (1024 * 1024)) + "MB";
}
std::string QueueText(char tag, const wvp::DebugQueue& q) {
    return std::string(1, tag) + " " + I64(q.Packets) + " pkt " + Bytes(q.Bytes) + " " + Num(q.Duration) + "s";
}

bool IsInfraNode(const std::string& n) { return n == "buffer" || n == "buffersink" || n == "abuffer" || n == "abuffersink"; }

void Add(std::vector<OverlayLine>& v, LineKind k, std::string t) { v.push_back({k, std::move(t)}); }

std::string Pad(const char* label) {
    std::string s = label;
    if (s.size() >= 10) return s + ' ';
    while (s.size() < 10) s += ' ';
    return s;
}

std::string JoinWith(const std::vector<std::string>& v, const char* sep) {
    std::string s;
    for (size_t i = 0; i < v.size(); i++) s += (i ? sep : "") + v[i];
    return s;
}

std::vector<std::string> ActualNames(const std::vector<std::string>& filters, const std::vector<wvp::DebugFilterNode>& nodes, const std::string& graph) {
    if (!filters.empty()) return filters;
    if (!nodes.empty()) {
        std::vector<std::string> n;
        for (auto& x : nodes) n.push_back(x.Name);
        return n;
    }
    return GraphFilterNames(graph);
}

std::string NodesText(const std::vector<wvp::DebugFilterNode>& nodes) {
    std::string s;
    for (size_t i = 0; i < nodes.size(); i++) {
        if (i) s += " > ";
        s += nodes[i].Name;
        if (!nodes[i].HwDevice.empty()) s += "[" + nodes[i].HwDevice + "]";
    }
    return s;
}

bool HasName(const std::vector<std::string>& v, const char* n) { return std::find(v.begin(), v.end(), n) != v.end(); }

}

std::vector<std::string> GraphFilterNames(const std::string& graph) {
    std::vector<std::string> names;
    std::string cur;
    int bracket = 0;
    bool quote = false, esc = false;
    auto flush = [&] {
        std::string t;
        for (char ch : cur) { if (ch == '=' || ch == '@') break; t += ch; }
        while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
        size_t b = t.find_first_not_of(" \t");
        t = b == std::string::npos ? std::string() : t.substr(b);
        if (!t.empty()) names.push_back(t);
        cur.clear();
    };
    bool inArgs = false;
    for (char ch : graph) {
        if (esc) { esc = false; if (!inArgs) cur += ch; continue; }
        if (ch == '\\') { esc = true; continue; }
        if (quote) { if (ch == '\'') quote = false; continue; }
        if (ch == '\'') { quote = true; continue; }
        if (ch == '[') { bracket++; continue; }
        if (ch == ']') { if (bracket) bracket--; continue; }
        if (bracket) continue;
        if (ch == ',' || ch == ';') { flush(); inArgs = false; continue; }
        if (ch == '=' || ch == '@') inArgs = true;
        if (!inArgs) cur += ch;
    }
    flush();
    return names;
}

GraphCompare CompareGraph(const std::string& requested, const std::vector<std::string>& actual) {
    GraphCompare r;
    r.known = !actual.empty();
    if (!r.known) return r;
    std::vector<std::string> req = GraphFilterNames(requested);
    std::vector<bool> used(actual.size(), false);
    size_t from = 0;
    for (const std::string& n : req) {
        size_t i = from;
        while (i < actual.size() && actual[i] != n) i++;
        if (i == actual.size()) { r.missing.push_back(n); continue; }
        used[i] = true;
        from = i + 1;
    }
    r.mismatch = !r.missing.empty();
    for (size_t i = 0; i < actual.size(); i++)
        if (!used[i] && !IsInfraNode(actual[i])) r.autoInserted.push_back(actual[i]);
    return r;
}

double AtempoProduct(const std::string& g, bool* any) {
    double prod = 1.0;
    bool found = false;
    size_t pos = 0;
    while ((pos = g.find("atempo", pos)) != std::string::npos) {
        size_t p = pos + 6;
        pos = p;
        if (p < g.size() && g[p] != '=' && g[p] != ',' && g[p] != '@' && g[p] != '[' && g[p] != ' ') continue;
        found = true;
        double v = 1.0;
        if (p < g.size() && g[p] == '@') while (p < g.size() && g[p] != '=' && g[p] != ',') p++;
        if (p < g.size() && g[p] == '=') {
            size_t e = g.find(',', p);
            std::string args = g.substr(p + 1, e == std::string::npos ? std::string::npos : e - p - 1);
            size_t t = args.find("tempo=");
            std::string val = t != std::string::npos ? args.substr(t + 6) : args;
            size_t colon = val.find(':');
            if (colon != std::string::npos) val = val.substr(0, colon);
            char* end = nullptr;
            double d = std::strtod(val.c_str(), &end);
            if (end != val.c_str() && d > 0) v = d;
        }
        prod *= v;
    }
    if (any) *any = found;
    return prod;
}

namespace {
using wvp::PipelineOp;
using wvp::PipelineTable;

// Deinterlacers without a pipeline operation, by base name (a hw suffix is stripped first: "deinterlace_d3d11").
bool IsOtherDeinterlacer(const std::string& base) { return base == "w3fdif" || base == "kerndeint" || base == "deinterlace"; }

std::string BaseName(const std::string& name) {
    std::string base = name;
    PipelineTable::DomainFromSuffix(name, &base);
    return base;
}

// Parts of the composite IVTC aliases (the CPU recipe) present in the graph, in recipe order; empty when none is.
std::vector<std::string> CpuIvtcParts(const std::vector<std::string>& names, bool* any) {
    *any = false;
    for (int i = 0; i < wvp::kAliasCount; i++) {
        std::string recipe = wvp::kAliases[i].filter;
        if (wvp::kAliases[i].op != PipelineOp::Ivtc || recipe.find(',') == std::string::npos) continue;
        std::vector<std::string> found;
        bool own = false;   // a part that only the recipe uses (fieldmatch, decimate), not a lone bwdif
        for (const std::string& p : GraphFilterNames(recipe)) {
            if (!HasName(names, p.c_str())) continue;
            bool residual = PipelineTable::OpOfFilterName(p) == PipelineOp::Bwdif;
            own = own || !residual;
            found.push_back(residual ? p + "(interlaced)" : p);
        }
        if (own) { *any = true; return found; }
    }
    return {};
}
}

std::string DescribeIvtcDeint(const OverlayContext& c, bool* warn) {
    if (warn) *warn = false;
    const auto& f = c.info.Filters;
    std::vector<std::string> names = ActualNames(f.VfFilters, f.VfNodes, f.VfGraph);
    bool known = !names.empty();
    auto where = [&](const std::string& n) {
        std::string base;
        auto d = PipelineTable::DomainFromSuffix(n, &base);
        if (!d) return std::string("CPU");
        for (auto& x : f.VfNodes) if (x.Name == n && !x.HwDevice.empty()) return x.HwDevice;
        return n.substr(base.size() + 1);
    };
    auto isHw = [](const std::string& n) { return PipelineTable::DomainFromSuffix(n).has_value(); };
    auto op = [](const std::string& n) { return PipelineTable::OpOfFilterName(n); };

    bool cpuIvtc = false;
    std::vector<std::string> recipe = CpuIvtcParts(names, &cpuIvtc);
    std::string gpuIvtc;
    for (auto& n : names) if (op(n) == PipelineOp::Ivtc && isHw(n)) { gpuIvtc = n; break; }

    std::string text;
    if (!gpuIvtc.empty()) {
        text = "IVTC: " + gpuIvtc + " (" + where(gpuIvtc) + ")";
        std::string resid;
        for (auto& n : names) if (op(n) == PipelineOp::Bwdif && isHw(n)) { resid = n; break; }
        if (!resid.empty()) text += " + " + resid + "(interlaced)";
        else text += ", residual-combing fallback unavailable";
    } else if (cpuIvtc) {
        text = "IVTC: " + JoinWith(recipe, "+") + " (CPU)";
    } else {
        std::vector<std::string> parts;
        for (auto& n : names)
            if (op(n) == PipelineOp::Bwdif || op(n) == PipelineOp::Yadif || IsOtherDeinterlacer(BaseName(n)))
                parts.push_back(n + " (" + where(n) + ")");
        text = JoinWith(parts, " + ");
    }

    std::string what = c.deinterlaceRequested;
    if (what.empty() || what == "off") return text.empty() ? "none" : text;
    if (!known) return what + " requested (actual graph unknown)";
    bool present = false;
    if (what == "ivtc") present = !gpuIvtc.empty() || cpuIvtc;
    else for (auto& n : names) if (op(n) == PipelineOp::Bwdif || op(n) == PipelineOp::Yadif) present = true;
    if (what == "deinterlace" && c.plan)
        for (const wvp::PlanStep& s : c.plan->steps)
            if (s.reason.rfind("priority ", 0) == 0) { text += (text.empty() ? "" : "; ") + s.reason; break; }
    if (present) return text;
    if (warn) *warn = true;
    return (text.empty() ? "" : text + "; ") + what + " requested, NOT in the actual graph";
}

namespace {
std::string HzText(double hz) {
    if (!(hz > 0) || !std::isfinite(hz)) return "?";
    char b[32];
    if (std::fabs(hz - std::round(hz)) < 0.005) std::snprintf(b, sizeof b, "%d", (int)std::lround(hz));
    else std::snprintf(b, sizeof b, "%.2f", hz);
    return b;
}
std::optional<double> Positive(const std::optional<double>& v) { return v && *v > 0 ? v : std::nullopt; }
}

bool FpsMismatch(const wvp::DebugFps& f, bool paused) {
    if (paused || !f.Present) return false;
    // The "reference" fps the display is compared with: the filter graph's output rate.
    auto ref = Positive(f.FilterOut);
    auto disp = Positive(f.Displayed);
    if (!ref || !disp) return false;
    if (f.FramesDisplayed.value_or(0) < kFpsWarmupFrames) return false;
    return std::fabs(*disp - *ref) / *ref > kFpsTolerance;
}

Cadence DescribeCadence(double fps, double refreshHz) {
    Cadence c;
    if (!(fps > 0) || !(refreshHz > 0) || !std::isfinite(fps) || !std::isfinite(refreshHz)) return c;
    double r = refreshHz / fps;
    c.framesPerRefresh = r;
    std::string ctx = " (" + HzText(refreshHz) + " Hz / " + Fmt("%.2f", fps) + " fps)";
    double nearest = std::round(r);
    if (r < 0.985) {
        c.kind = Cadence::Kind::FasterThanRefresh;
        c.text = "fps exceeds refresh: frames are skipped" + ctx;
    } else if (nearest >= 1 && std::fabs(r - nearest) / nearest < 0.015) {
        c.kind = Cadence::Kind::Even;
        c.text = "even " + std::to_string((int)nearest) + ":" + std::to_string((int)nearest) + " cadence" + ctx;
    } else {
        double fl = std::floor(r);
        if (fl >= 1 && std::fabs(r - (fl + 0.5)) / r < 0.015) {
            c.kind = Cadence::Kind::Pulldown32;
            c.text = std::to_string((int)fl + 1) + ":" + std::to_string((int)fl) + " cadence (pulldown judder)" + ctx;
        } else {
            c.kind = Cadence::Kind::Uneven;
            c.text = "uneven cadence (" + Fmt("%.2f", r) + " refreshes per frame, judder)" + ctx;
        }
    }
    return c;
}

std::vector<OverlayLine> BuildFpsLines(const wvp::DebugInfo& d, double refreshHz) {
    std::vector<OverlayLine> L;
    const wvp::DebugFps& f = d.Fps;
    std::optional<double> in = Positive(f.StreamAvg);
    if (!in) in = Positive(f.StreamR);
    if (!in) in = Positive(d.Video.FrameRate);
    std::optional<double> out = Positive(f.FilterOut);
    std::string t = Pad("fps") + "in " + Num(in) + " -> filter out " + Num(out) + " -> displayed " + Num(f.Displayed, "%.1f") + " (refresh " + HzText(refreshHz) + " Hz)";
    bool warn = FpsMismatch(f, d.Paused);
    if (warn) {
        double pct = (*f.Displayed - *out) / *out * 100.0;
        t += "  MISMATCH displayed vs filter out (" + Fmt("%+.0f", pct) + " %)";
    }
    Add(L, warn ? LineKind::Warn : LineKind::Normal, t);
    if (!f.Present) return L;
    Add(L, LineKind::Normal, Pad("") + "decoded " + Num(f.Decoded, "%.1f") + " fps | frames decoded " + I64(f.FramesDecoded) + " filtered " + I64(f.FramesFiltered) +
                                 " displayed " + I64(f.FramesDisplayed) + " | drops early " + I64(f.DropsEarly) + " late " + I64(f.DropsLate));
    if (in && out && std::fabs(*in - *out) / *in > 0.02)
        Add(L, LineKind::Normal, Pad("") + "frame rate changed by the filter graph: " + Fmt("%.2f", *in) + " -> " + Fmt("%.2f", *out) + " fps");
    // Cadence of what is actually presented: the filter-out rate (the displayed rate once it is measurable and sane).
    double basis = out ? *out : (Positive(f.Displayed) ? *f.Displayed : 0.0);
    if (basis > 0) {
        Cadence c = DescribeCadence(basis, refreshHz);
        if (c.kind != Cadence::Kind::Unknown) Add(L, c.kind == Cadence::Kind::Even ? LineKind::Dim : LineKind::Normal, Pad("") + c.text);
    }
    return L;
}

std::vector<OverlayLine> BuildOverlayLines(const OverlayContext& c) {
    std::vector<OverlayLine> L;
    Add(L, LineKind::Header, "libffplay debug" + (c.mode.empty() ? std::string() : " [" + c.mode + "]") + "   (Tab: hide)");
    if (!c.buildStamp.empty()) Add(L, LineKind::Header, c.buildStamp);
    if (!c.adapterNote.empty())
        Add(L, c.adapterWarn ? LineKind::Warn : LineKind::Normal, "adapter: " + c.adapterNote);
    if (!c.resumeDecision.empty()) Add(L, LineKind::Normal, Pad("resume") + c.resumeDecision);
    const DebugInfo& d = c.info;
    auto openLines = [&] {
        if (!c.url.empty()) Add(L, LineKind::Dim, Pad("open") + c.url);
        if (!c.openOptions.empty()) Add(L, LineKind::Normal, Pad("options") + c.openOptions);
    };
    if (!c.haveJson || !d.Valid) {
        Add(L, LineKind::Warn, "no debug info: nothing is open");
        openLines();
        return L;
    }

    {
        const auto& v = d.Video;
        int nonZero = (v.FramesHwDirect.value_or(0) > 0) + (v.FramesHwDownload.value_or(0) > 0) + (v.FramesSoftware.value_or(0) > 0);
        std::string t = Pad("output") + S(PresenterOutput(d)) + "   frame_path " + S(v.FramePath) + "  (hw_direct " + I64(v.FramesHwDirect) + " | hw_download " +
                        I64(v.FramesHwDownload) + " | software " + I64(v.FramesSoftware) + ")";
        bool warn = nonZero > 1;
        if (warn) t += "  PATH CHANGED";
        Add(L, warn ? LineKind::Warn : LineKind::Normal, t);
    }
    if (!c.acrylicFallbackReason.empty()) {
        Add(L, LineKind::Warn, "acrylic: solid fallback (" + c.acrylicFallbackReason + ")");
    } else if (d.Presenter.Acrylic.Present) {
        Add(L, LineKind::Normal, "acrylic: " + Int(d.Presenter.Acrylic.Regions) + " regions, " + Num(d.Presenter.Acrylic.BlurMs, "%.1f") + " ms");
    }

    if (d.HasVideo) {
        const auto& v = d.Video;
        Add(L, LineKind::Normal, Pad("video") + "codec " + S(v.Codec) + "  decoder " + S(v.Decoder) + "  |  " + Size(v.Width, v.Height) + " -> " + Size(v.OutWidth, v.OutHeight) +
                                     "  SAR " + S(v.Sar) + "  " + Num(v.FrameRate) + " fps");
        std::string req = v.HwaccelRequested, dev = v.HwaccelDevice;
        std::string expected = req.empty() ? "none" : req;
        // "auto" means "any hardware device": only a software fallback is a mismatch there.
        bool mismatch = !dev.empty() && (req == "auto" ? dev == "none" : dev != expected);
        std::string t = Pad("hwaccel") + "requested " + (req.empty() ? "-" : req) + " -> actual " + S(dev);
        if (mismatch) t += "  MISMATCH";
        if (!v.HwaccelError.empty()) t += "  error: " + v.HwaccelError;
        Add(L, mismatch ? LineKind::Warn : LineKind::Normal, t);
        std::string decoded = !v.PixFmtDecoded.empty() ? v.PixFmtDecoded : !v.HwPixFmt.empty() ? v.HwPixFmt : v.PixFmt;
        for (auto& fl : BuildFpsLines(d, c.displayRefreshHz)) L.push_back(std::move(fl));
        Add(L, LineKind::Normal, Pad("pix_fmt") + "decoded " + S(decoded) + " | after transfer " + S(v.PixFmtAfterTransfer, "-") + " | out " + S(v.OutPixFmt));
    }

    if (d.HasAudio) {
        const auto& a = d.Audio;
        Add(L, LineKind::Normal, Pad("audio") + "codec " + S(a.Codec) + "  decoder " + S(a.Decoder) + "  |  " + Int(a.SampleRate) + " Hz  " + Int(a.Channels) + " ch  " +
                                     S(a.ChannelLayout) + "  " + S(a.SampleFmt));
        Add(L, LineKind::Normal, Pad("audio out") + "device \"" + S(a.OutDevice) + "\"  |  " + Int(a.OutSampleRate) + " Hz  " + Int(a.OutChannels) + " ch  | hw_buf " + Int(a.HwBufSize) +
                                     "  diff_avg " + Num(a.DiffAvg, "%.4f") + "  latency " + Num(a.LatencyMs, "%.1f") + " ms");
    }

    if (d.AribAudio.Present) {
        const auto& a = d.AribAudio;
        auto role = [](const wvp::DebugAribRole& r) {
            if (!r.Present) return std::string("none") + (r.Starved ? "  (starved)" : "");
            char tag[16];
            std::snprintf(tag, sizeof tag, r.ComponentTag >= 0 ? "0x%02x" : "-", r.ComponentTag);
            return "stream #" + std::to_string(r.StreamIndex) + "  pid " + std::to_string(r.Pid) + "  tag " + tag +
                   "  buffered " + Num(r.BufferedMs, "%.0f") + " ms" + (r.Starved ? "  STARVED" : "");
        };
        Add(L, LineKind::Normal, Pad("arib audio") + "preference " + S(a.Preference) + " | playing " + S(a.Playing, "-") +
                                     " | switches " + I64(a.Switches) + " | silence " + Num(a.SilenceMs, "%.0f") + " ms | rebinds " + I64(a.Rebinds));
        Add(L, LineKind::Normal, Pad("audio 1") + role(a.Audio1));
        Add(L, LineKind::Normal, Pad("audio 2") + role(a.Audio2));
    }

    if (d.CaptionLayer.Present || d.SuperimposeLayer.Present) {
        Add(L, LineKind::Header, "captions");
        auto layerLines = [&](const char* name, const wvp::DebugSubtitleLayer& s) {
            if (!s.Present) return;
            Add(L, LineKind::Normal, Pad(name) + "stream #" + std::to_string(s.StreamIndex) + " " + S(s.Codec));
            Add(L, LineKind::Dim, Pad("decoder") + S(s.DecoderOptions));
            Add(L, LineKind::Normal, Pad("font") + S(s.ResolvedFont));
            Add(L, LineKind::Normal, Pad("cue") + "rects " + Int(s.RectCount) + " | " + Num(s.CueStart, "%.3f") + " .. " + Num(s.CueEnd, "%.3f") + " s");
            Add(L, LineKind::Normal, Pad("canvas") + Size(s.CanvasWidth, s.CanvasHeight) + " | shown " + I64(s.CuesShown) + " cleared " + I64(s.CuesCleared) +
                                            " uploaded " + I64(s.BitmapsUploaded) + " rerenders " + I64(s.Rerenders));
        };
        layerLines("caption", d.CaptionLayer);
        layerLines("superimp", d.SuperimposeLayer);
    }

    {
        const auto& f = d.Filters;
        auto graphLines = [&](const char* tag, const std::string& req, const std::string& graph, const GraphCompare& cmp) {
            Add(L, LineKind::Normal, Pad((std::string(tag) + " req").c_str()) + S(req, "(none)"));
            std::string t = Pad((std::string(tag) + " graph").c_str()) + (graph.empty() ? "?" : graph);
            if (cmp.mismatch) t += "  MISMATCH (missing: " + JoinWith(cmp.missing, ",") + ")";
            Add(L, cmp.mismatch ? LineKind::Warn : LineKind::Normal, t);
            if (!cmp.autoInserted.empty()) Add(L, LineKind::Dim, Pad("") + "auto-inserted: " + JoinWith(cmp.autoInserted, ", "));
        };
        auto vnames = ActualNames(f.VfFilters, f.VfNodes, f.VfGraph);
        auto anames = ActualNames(f.AfFilters, f.AfNodes, f.AfGraph);
        if (d.HasVideo) graphLines("vf", f.VfRequested, f.VfGraph, CompareGraph(f.VfRequested, vnames));
        if (d.HasAudio) graphLines("af", f.AfRequested, f.AfGraph, CompareGraph(f.AfRequested, anames));
        if (!f.VfNodes.empty()) Add(L, LineKind::Normal, Pad("vf nodes") + NodesText(f.VfNodes));
        if (!f.AfNodes.empty()) Add(L, LineKind::Normal, Pad("af nodes") + NodesText(f.AfNodes));
        bool warn = false;
        std::string impl = DescribeIvtcDeint(c, &warn);
        Add(L, warn ? LineKind::Warn : LineKind::Normal, Pad("deinterlace") + impl);
        auto up = std::find(vnames.begin(), vnames.end(), "hwupload");
        auto roundTrip = up == vnames.end() ? up : std::find_if(up, vnames.end(), [](const std::string& n) {
            return wvp::PipelineTable::OpOfFilterName(n) != wvp::PipelineOp::Unknown && wvp::PipelineTable::DomainFromSuffix(n).has_value();
        });
        if (roundTrip != vnames.end() && HasName(vnames, "hwdownload")) {
            const auto& v = d.Video;
            bool hw = v.FramePath == "hw_direct" || v.FramePath == "hw_download";
            auto dom = *wvp::PipelineTable::DomainFromSuffix(*roundTrip);
            std::string label = wvp::DomainName(dom);
            Add(L, LineKind::Dim, Pad("path") + (hw ? "GPU decode -> CPU download -> " : "CPU decode -> ") + label + " upload + " + *roundTrip +
                                      " -> CPU download -> " + S(PresenterOutput(d), "presenter"));
        }
        if (d.HasAudio) {
            double want = c.requestedSpeed > 0 ? c.requestedSpeed : d.Speed.value_or(1.0);
            bool any = false;
            double have = AtempoProduct(f.AfGraph, &any);
            bool known = !f.AfGraph.empty() || !anames.empty();
            bool mismatch = known && std::fabs(want - have) > 0.005;
            std::string t = Pad("speed") + "requested " + Fmt("%.2f", want) + " | atempo in graph " + (known ? (any ? Fmt("%.2f", have) : std::string("none")) : "?");
            if (mismatch) t += "  MISMATCH";
            Add(L, mismatch ? LineKind::Warn : LineKind::Normal, t);
        }
    }

    if (c.plan) for (auto& pl : BuildPlanLines(*c.plan, d)) L.push_back(std::move(pl));

    {
        const auto& s = d.Sync;
        Add(L, LineKind::Normal, Pad("sync") + "master " + S(s.Master) + " | clocks a " + Num(s.AudioClock, "%.3f") + " v " + Num(s.VideoClock, "%.3f") + " ext " + Num(s.ExternalClock, "%.3f") +
                                     " master " + Num(s.MasterClock, "%.3f") + " | A-V " + Num(s.AvDiffMs, "%.1f") + " ms");
        Add(L, LineKind::Normal, Pad("") + "drops early " + I64(s.FrameDropsEarly) + " late " + I64(s.FrameDropsLate) + " | duplicated " + I64(s.FramesDuplicated) + " | timer drift " +
                                     Num(s.FrameTimerDriftMs, "%.1f") + " ms");
        Add(L, LineKind::Normal, Pad("queues") + QueueText('a', d.AudioQueue) + " | " + QueueText('v', d.VideoQueue) + " | " + QueueText('s', d.SubtitleQueue));
        std::string st;
        auto seg = [&](const char* tag, int idx, const std::optional<double>& start, const std::string& tb) {
            if (!st.empty()) st += " | ";
            st += std::string(tag) + "#" + std::to_string(idx) + " start " + Num(start, "%.3f") + " tb " + S(tb);
        };
        if (d.HasVideo) seg("v", d.Video.StreamIndex, d.Video.StartTime, d.Video.TimeBase);
        if (d.HasAudio) seg("a", d.Audio.StreamIndex, d.Audio.StartTime, d.Audio.TimeBase);
        if (d.HasSubtitle) seg("s", d.Subtitle.StreamIndex, d.Subtitle.StartTime, d.Subtitle.TimeBase);
        if (!st.empty()) Add(L, LineKind::Normal, Pad("streams") + st);
        Add(L, LineKind::Normal, Pad("position") + Num(d.Position) + " / " + Num(d.Duration) + " s  paused " + (d.Paused ? "yes" : "no") + "  speed " + Num(d.Speed));
    }

    openLines();
    return L;
}

int Utf8Len(const std::string& s) {
    int n = 0;
    for (unsigned char ch : s) if ((ch & 0xC0) != 0x80) n++;
    return n;
}

namespace {
size_t AdvanceCols(const std::string& s, size_t from, int cols) {
    size_t i = from;
    int n = 0;
    while (i < s.size() && n < cols) {
        i++;
        while (i < s.size() && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
        n++;
    }
    return i;
}
}

std::vector<OverlayLine> WrapLines(const std::vector<OverlayLine>& lines, int maxCols, int maxContinuation) {
    std::vector<OverlayLine> out;
    if (maxCols < 8) maxCols = 8;
    for (const OverlayLine& l : lines) {
        if (Utf8Len(l.text) <= maxCols) { out.push_back(l); continue; }
        std::string rest = l.text;
        int row = 0;
        bool first = true;
        while (!rest.empty()) {
            int cap = first ? maxCols : maxCols - 2;
            if (Utf8Len(rest) <= cap) { out.push_back({l.kind, (first ? "" : "  ") + rest}); break; }
            if (row >= 1 + maxContinuation - 1) {
                size_t cut = AdvanceCols(rest, 0, cap - 3);
                out.push_back({l.kind, (first ? "" : "  ") + rest.substr(0, cut) + "..."});
                break;
            }
            size_t hard = AdvanceCols(rest, 0, cap);
            size_t brk = rest.rfind(' ', hard - 1);
            if (brk == std::string::npos || brk < hard / 2) brk = hard;   // no blank in the second half: hard break
            std::string head = rest.substr(0, brk);
            while (!head.empty() && head.back() == ' ') head.pop_back();
            out.push_back({l.kind, (first ? "" : "  ") + head});
            size_t nx = rest.find_first_not_of(' ', brk);
            rest = nx == std::string::npos ? std::string() : rest.substr(nx);
            first = false;
            row++;
        }
    }
    return out;
}

std::vector<OverlayLine> ClipRows(const std::vector<OverlayLine>& lines, int maxRows) {
    if (maxRows < 1) return {};
    if ((int)lines.size() <= maxRows) return lines;
    std::vector<OverlayLine> out(lines.begin(), lines.begin() + (maxRows - 1));
    out.push_back({LineKind::Dim, "... (" + std::to_string(lines.size() - (size_t)(maxRows - 1)) + " more rows)"});
    return out;
}

std::string PlainText(const std::vector<OverlayLine>& lines) {
    std::string s;
    for (size_t i = 0; i < lines.size(); i++) s += (i ? "\n" : "") + lines[i].text;
    return s;
}

OverlayGeometry ComputeOverlayGeometry(int cw, int ch, double scale, ShellMode mode, float charW, float lineH, int bottomReserved) {
    OverlayGeometry g;
    if (cw <= 0 || ch <= 0 || charW <= 0 || lineH <= 0) return g;
    float margin = (float)std::lround(kMarginDip * scale), pad = (float)std::lround(kPadDip * scale);
    float top = (mode == ShellMode::NowPlaying ? (float)std::lround(kStripDip * scale) : 0.f) + margin;
    float availW = (float)cw - 2 * margin, availH = (float)ch - top - (float)bottomReserved - margin;
    int cols = (int)std::floor((availW - 2 * pad) / charW);
    cols = std::min(cols, kMaxCols);
    int rows = (int)std::floor((availH - 2 * pad) / lineH);
    if (cols < 8 || rows < 2) return g;
    g.cols = cols;
    g.rows = rows;
    g.x = margin;
    g.y = top;
    g.w = std::ceil(cols * charW + 2 * pad);
    g.h = std::ceil(rows * lineH + 2 * pad);
    g.visible = true;
    return g;
}

TabAction DecideTab(bool popupOpen, bool shift) {
    if (popupOpen) return TabAction::FocusTraversal;
    return shift ? TabAction::Ignore : TabAction::ToggleOverlay;
}

}
