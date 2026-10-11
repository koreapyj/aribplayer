#include "features/PipelineOverlay.h"

#include <cstdio>

namespace app::dbg {

namespace {
std::string Pad10(const char* label) {
    std::string s = label;
    if (s.size() >= 10) return s + ' ';
    while (s.size() < 10) s += ' ';
    return s;
}
std::string G(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%g", v);
    return b;
}
std::string Ll(long long v) { return std::to_string(v); }
}

MeasuredCopies Measure(const wvp::DebugInfo& d) {
    MeasuredCopies m;
    const auto& v = d.Video;
    m.known = v.FramesHwDirect || v.FramesHwDownload || v.FramesSoftware;
    m.hwDirect = v.FramesHwDirect.value_or(0);
    m.hwDownload = v.FramesHwDownload.value_or(0);
    m.software = v.FramesSoftware.value_or(0);
    return m;
}

namespace {
// The presenter the plan targets: the "present:<name>" step.
std::string PlannedPresenter(const wvp::Plan& p) {
    for (const auto& s : p.steps)
        if (s.kind == wvp::PlanStep::Kind::Present && s.name.rfind("present:", 0) == 0) return s.name.substr(8);
    return {};
}
}

PlanCheck CheckPlan(const wvp::Plan& p, const wvp::DebugInfo& d) {
    PlanCheck r;
    if (!p.ok) return r;
    MeasuredCopies m = Measure(d);
    bool warm = m.known && m.hwDirect + m.hwDownload + m.software >= kFpsWarmupFrames;
    if (warm) {
        const bool hwPlan = !p.hwaccel.empty();
        if (hwPlan && d.Video.HwaccelDevice != p.hwaccel)
            r.problems.push_back("planned decoder " + p.hwaccel + ", actual " + (d.Video.HwaccelDevice.empty() ? std::string("none") : d.Video.HwaccelDevice));
        if (hwPlan && p.hwframesKeep && m.hwDownload > 0)
            r.problems.push_back("planned 0 decode copies, measured hw_download " + Ll(m.hwDownload));
        if (hwPlan && !p.hwframesKeep && m.hwDirect > 0 && m.hwDownload == 0)
            r.problems.push_back("planned decode download, measured hw_direct " + Ll(m.hwDirect) + " (no download)");
        if (!hwPlan && m.hwDirect + m.hwDownload > 0)
            r.problems.push_back("planned software decoding, measured hw frames " + Ll(m.hwDirect + m.hwDownload));
    }
    if (d.Presenter.Present) {
        const std::string planned = PlannedPresenter(p);
        if (!d.Presenter.Error.empty()) r.problems.push_back("presenter error: " + d.Presenter.Error);
        if (warm && !d.Presenter.Name.empty() && !planned.empty() && d.Presenter.Name != planned)
            r.problems.push_back("planned presenter " + planned + ", presenter in use " + d.Presenter.Name);
        if (warm && p.cost.hostCopies == 0 && d.Presenter.FramesUploaded.value_or(0) > 0)
            r.problems.push_back("planned 0 host copies, measured presenter uploads " + Ll(*d.Presenter.FramesUploaded));
    }
    r.mismatch = !r.problems.empty();
    return r;
}

std::vector<OverlayLine> BuildPlanLines(const wvp::Plan& p, const wvp::DebugInfo& d, bool showRejected) {
    std::vector<OverlayLine> L;
    auto add = [&](LineKind k, std::string t) { L.push_back({k, std::move(t)}); };
    PlanCheck chk = CheckPlan(p, d);
    if (!p.ok) {
        add(LineKind::Warn, Pad10("pipeline") + "no plan: " + p.error);
        for (const auto& n : p.notes) add(LineKind::Dim, Pad10("") + n);
        return L;
    }
    add(chk.mismatch ? LineKind::Warn : LineKind::Normal, Pad10("pipeline") + p.Summary());
    for (const auto& s : p.steps) {
        std::string t = std::string("  ") + s.name + " [" + wvp::DomainName(s.in) + "->" + wvp::DomainName(s.out) + "]";
        if (s.copy != wvp::CopyKind::None) t += std::string(" ") + wvp::CopyKindName(s.copy) + "x" + std::to_string(s.copies);
        if (!s.reason.empty()) t += " - " + s.reason;
        add(LineKind::Normal, t);
    }
    MeasuredCopies m = Measure(d);
    std::string c = Pad10("copies") + "planned host " + std::to_string(p.cost.hostCopies) + ", device " + std::to_string(p.cost.deviceCopies) + " | measured ";
    c += m.known ? "hw_direct " + Ll(m.hwDirect) + ", hw_download " + Ll(m.hwDownload) + ", software " + Ll(m.software) : std::string("?");
    add(chk.mismatch ? LineKind::Warn : LineKind::Normal, c);
    for (const auto& pr : chk.problems) add(LineKind::Warn, Pad10("") + "MISMATCH " + pr);
    for (const auto& note : chk.notes) add(LineKind::Dim, Pad10("") + "NOTE " + note);
    if (d.Presenter.Present) {
        std::string t = Pad10("presenter") + d.Presenter.Name;
        t += " direct " + Ll(d.Presenter.FramesDirect.value_or(0)) + " uploaded " + Ll(d.Presenter.FramesUploaded.value_or(0));
        bool bad = false;
        for (const auto& pr : chk.problems)
            if (pr.find("presenter") != std::string::npos) bad = true;
        add(bad ? LineKind::Warn : LineKind::Normal, t);
        if (!d.Presenter.Error.empty()) add(LineKind::Warn, Pad10("") + "error: " + d.Presenter.Error);
        if (!d.Presenter.Route.empty())
            add(LineKind::Normal, Pad10("") + "route " + d.Presenter.Route + " (shader " + Ll(d.Presenter.FramesShader.value_or(0)) +
                                      ", d3d11 copied " + Ll(d.Presenter.FramesD3d11Copied.value_or(0)) + ")");
        if (!d.Presenter.AdapterName.empty() || !d.Presenter.AdapterLuid.empty()) {
            char v[16];
            std::snprintf(v, sizeof v, "0x%04x", d.Presenter.AdapterVendor.value_or(0));
            add(LineKind::Normal, Pad10("") + "adapter " + d.Presenter.AdapterName + " (vendor " + v + ", luid " + d.Presenter.AdapterLuid + ")");
        }
        if (!d.Presenter.Fallbacks.empty()) {
            std::string f;
            for (const auto& x : d.Presenter.Fallbacks) f += (f.empty() ? "" : ", ") + x;
            add(LineKind::Warn, Pad10("") + "fallbacks " + f);
        }
        if (d.Presenter.DebugLayer)
            add(d.Presenter.DebugMessages.value_or(0) > 0 ? LineKind::Warn : LineKind::Normal,
                Pad10("") + "d3d11 debug layer " + (*d.Presenter.DebugLayer ? "on" : "unavailable") + ", " +
                    Ll(d.Presenter.DebugMessages.value_or(0)) + " messages");
    }
    for (const auto& n : p.notes) add(LineKind::Dim, Pad10("") + n);
    if (showRejected) {
        size_t n = 0;
        for (const auto& r : p.rejected) {
            if (n++ >= 4) break;
            add(LineKind::Dim, "  rejected " + r.summary + " [host " + std::to_string(r.cost.hostCopies) + ", device " + std::to_string(r.cost.deviceCopies) + "] - " + r.reason);
        }
    }
    return L;
}

}
