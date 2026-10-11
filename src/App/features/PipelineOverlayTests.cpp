// --logictest cases for the pipeline section of the debug overlay (PipelineOverlay): plans are built by hand.
#include <string>

#include "LogicTest.h"
#include "features/DebugOverlayModel.h"
#include "features/PipelineOverlay.h"

namespace {
using namespace app::dbg;

bool Has(const std::vector<OverlayLine>& v, const std::string& needle, LineKind* kind = nullptr) {
    for (const auto& l : v)
        if (l.text.find(needle) != std::string::npos) { if (kind) *kind = l.kind; return true; }
    return false;
}
int CountKind(const std::vector<OverlayLine>& v, LineKind k) {
    int n = 0;
    for (const auto& l : v) n += l.kind == k;
    return n;
}

// d3d11va decode (frames kept) > ivtc_d3d11 > d3d11 presenter taking the frames directly.
wvp::Plan D3D11Plan() {
    wvp::Plan p;
    p.ok = true;
    p.hwaccel = "d3d11va";
    p.hwframesKeep = true;
    p.cost.hostCopies = 0;
    p.cost.deviceCopies = 0;
    wvp::PlanStep dec;
    dec.kind = wvp::PlanStep::Kind::Decode; dec.name = "decode:d3d11va"; dec.in = wvp::FrameDomain::System; dec.out = wvp::FrameDomain::D3D11;
    dec.reason = "cheapest: 0 host copies";
    wvp::PlanStep ivtc;
    ivtc.name = "ivtc_d3d11"; ivtc.in = wvp::FrameDomain::D3D11; ivtc.out = wvp::FrameDomain::D3D11; ivtc.reason = "hw filter";
    wvp::PlanStep pr;
    pr.kind = wvp::PlanStep::Kind::Present; pr.name = "present:d3d11"; pr.in = wvp::FrameDomain::D3D11; pr.out = wvp::FrameDomain::D3D11;
    pr.reason = "hw frames presented directly";
    p.steps = {dec, ivtc, pr};
    return p;
}

wvp::DebugInfo Info(long long direct, long long download, long long software, const char* dev) {
    wvp::DebugInfo d;
    d.Valid = true;
    d.HasVideo = true;
    d.Video.HwaccelDevice = dev;
    d.Video.FramesHwDirect = direct;
    d.Video.FramesHwDownload = download;
    d.Video.FramesSoftware = software;
    return d;
}
}

LOGIC_TEST(pipeline_overlay_measure) {
    wvp::DebugInfo d;
    LT_CHECK(!Measure(d).known);
    d.Video.FramesHwDownload = 7;
    MeasuredCopies m = Measure(d);
    LT_CHECK(m.known);
    LT_CHECK_EQ(m.hwDownload, 7LL);
    LT_CHECK_EQ(m.hwDirect, 0LL);
}

LOGIC_TEST(pipeline_overlay_lines_d3d11_ivtc) {
    wvp::Plan p = D3D11Plan();
    wvp::DebugInfo d = Info(100, 0, 0, "d3d11va");
    auto L = BuildPlanLines(p, d);
    LT_CHECK(!CheckPlan(p, d).mismatch);
    LT_CHECK_EQ(L[0].text, "pipeline  " + p.Summary());
    LT_CHECK(L[0].kind == LineKind::Normal);
    LT_CHECK(Has(L, "  decode:d3d11va [system->d3d11] - cheapest: 0 host copies"));
    LT_CHECK(Has(L, "  ivtc_d3d11 [d3d11->d3d11] - hw filter"));
    LT_CHECK(Has(L, "  present:d3d11 [d3d11->d3d11] - hw frames presented directly"));
    LT_CHECK(Has(L, "copies    planned host 0, device 0 | measured hw_direct 100, hw_download 0, software 0"));
    LT_CHECK_EQ(CountKind(L, LineKind::Warn), 0);
}

LOGIC_TEST(pipeline_overlay_warmup_and_decoder_mismatch) {
    wvp::Plan p = D3D11Plan();
    // below warm-up: nothing is compared
    LT_CHECK(!CheckPlan(p, Info(10, 0, 0, "none")).mismatch);
    // after warm-up: planned d3d11va, actual none
    wvp::DebugInfo d = Info(0, 0, 50, "none");
    PlanCheck c = CheckPlan(p, d);
    LT_CHECK(c.mismatch);
    LT_CHECK_EQ(c.problems.size(), (size_t)1);
    LT_CHECK_EQ(c.problems[0], std::string("planned decoder d3d11va, actual none"));
    auto L = BuildPlanLines(p, d);
    LT_CHECK(L[0].kind == LineKind::Warn);
    LT_CHECK(Has(L, "planned decoder d3d11va, actual none"));
    LT_CHECK(CountKind(L, LineKind::Warn) >= 3);
}

LOGIC_TEST(pipeline_overlay_keep_but_download) {
    wvp::Plan p = D3D11Plan();
    wvp::DebugInfo d = Info(0, 60, 0, "d3d11va");
    PlanCheck c = CheckPlan(p, d);
    LT_CHECK(c.mismatch);
    LT_CHECK_EQ(c.problems[0], std::string("planned 0 decode copies, measured hw_download 60"));
}

LOGIC_TEST(pipeline_overlay_download_plan_direct_frames_and_software_plan) {
    wvp::Plan p = D3D11Plan();
    p.hwframesKeep = false;
    LT_CHECK(CheckPlan(p, Info(50, 0, 0, "d3d11va")).mismatch);   // informational problem
    LT_CHECK(!CheckPlan(p, Info(0, 50, 0, "d3d11va")).mismatch);  // downloading as planned
    wvp::Plan s = D3D11Plan();
    s.hwaccel.clear();
    s.hwframesKeep = false;
    LT_CHECK(!CheckPlan(s, Info(0, 0, 50, "none")).mismatch);
    LT_CHECK(CheckPlan(s, Info(0, 20, 30, "none")).mismatch);
}

LOGIC_TEST(pipeline_overlay_old_dll_measured_unknown) {
    wvp::Plan p = D3D11Plan();
    wvp::DebugInfo d;
    d.Valid = true;
    d.Video.HwaccelDevice = "none";   // would mismatch, but no counters: no warm-up evidence
    auto L = BuildPlanLines(p, d);
    LT_CHECK(Has(L, "planned host 0, device 0 | measured ?"));
    LT_CHECK_EQ(CountKind(L, LineKind::Warn), 0);
}

LOGIC_TEST(pipeline_overlay_notes_and_rejected) {
    wvp::Plan p = D3D11Plan();
    p.notes = {"content unknown: planned for 1080p"};
    for (int i = 0; i < 6; i++) {
        wvp::RejectedAlternative r;
        r.summary = "alt" + std::to_string(i);
        r.cost.hostCopies = i + 1;
        r.reason = "more copies";
        p.rejected.push_back(r);
    }
    auto L = BuildPlanLines(p, Info(100, 0, 0, "d3d11va"));
    LineKind k = LineKind::Normal;
    LT_CHECK(Has(L, "content unknown: planned for 1080p", &k));
    LT_CHECK(k == LineKind::Dim);
    LT_CHECK(Has(L, "  rejected alt0 [host 1, device 0] - more copies", &k));
    LT_CHECK(k == LineKind::Dim);
    LT_CHECK(Has(L, "rejected alt3"));
    LT_CHECK(!Has(L, "rejected alt4"));
    LT_CHECK(!Has(BuildPlanLines(p, Info(100, 0, 0, "d3d11va"), false), "rejected"));
    // no presenter object in the debug info: no presenter line
    LT_CHECK(!Has(BuildPlanLines(D3D11Plan(), Info(100, 0, 0, "d3d11va")), "presenter "));
}

LOGIC_TEST(pipeline_overlay_no_plan_error) {
    wvp::Plan p;
    p.ok = false;
    p.error = "no decoder for av1";
    auto L = BuildPlanLines(p, wvp::DebugInfo{});
    LT_CHECK_EQ(L.size(), (size_t)1);
    LT_CHECK(L[0].kind == LineKind::Warn);
    LT_CHECK_EQ(L[0].text, std::string("pipeline  no plan: no decoder for av1"));
}

LOGIC_TEST(pipeline_overlay_overlay_lines_identity_without_plan) {
    OverlayContext c;
    c.haveJson = true;
    c.info = Info(100, 0, 0, "d3d11va");
    c.mode = "now playing";
    auto base = BuildOverlayLines(c);
    LT_CHECK(!Has(base, "pipeline"));
    c.plan = D3D11Plan();
    auto withPlan = BuildOverlayLines(c);
    LT_CHECK(withPlan.size() > base.size());
    LT_CHECK(Has(withPlan, "pipeline  "));
    auto pl = BuildPlanLines(*c.plan, c.info);
    LT_CHECK_EQ(withPlan.size(), base.size() + pl.size());
    c.plan.reset();
    LT_CHECK(BuildOverlayLines(c) == base);
}

namespace {
wvp::DebugInfo WithPresenter(wvp::DebugInfo d, const char* name, long long direct, long long uploaded, const char* error = "") {
    d.Presenter.Present = true;
    d.Presenter.Name = name;
    d.Presenter.Error = error;
    d.Presenter.FramesDirect = direct;
    d.Presenter.FramesUploaded = uploaded;
    return d;
}
}

LOGIC_TEST(pipeline_overlay_presenter_line_direct_and_uploaded) {
    wvp::Plan p = D3D11Plan();
    wvp::DebugInfo d = WithPresenter(Info(100, 0, 0, "d3d11va"), "d3d11", 99, 0);
    auto L = BuildPlanLines(p, d);
    LT_CHECK(!CheckPlan(p, d).mismatch);
    LT_CHECK(Has(L, "presenter d3d11 direct 99 uploaded 0"));
    LT_CHECK_EQ(CountKind(L, LineKind::Warn), 0);
    d.Presenter.Route = "vp";
    LT_CHECK(Has(BuildPlanLines(p, d), "route vp (shader 0, d3d11 copied 0)"));
}

LOGIC_TEST(pipeline_overlay_presenter_name_mismatch_after_warmup_and_error_warn) {
    wvp::Plan p = D3D11Plan();
    // before the warm-up a different presenter name is not judged
    wvp::DebugInfo early = WithPresenter(Info(5, 0, 0, "d3d11va"), "metal", 0, 0);
    LT_CHECK(!CheckPlan(p, early).mismatch);
    wvp::DebugInfo warm = WithPresenter(Info(100, 0, 0, "d3d11va"), "metal", 0, 0);
    PlanCheck c = CheckPlan(p, warm);
    LT_CHECK(c.mismatch);
    LT_CHECK(c.problems.size() == 1 && c.problems[0].find("presenter in use metal") != std::string::npos);
    LT_CHECK(Has(BuildPlanLines(p, warm), "MISMATCH planned presenter d3d11, presenter in use metal"));
    // an error is a problem at any time, even before the warm-up, and is printed
    wvp::DebugInfo err = WithPresenter(Info(5, 0, 0, "d3d11va"), "metal", 0, 0, "Present failed");
    PlanCheck ce = CheckPlan(p, err);
    LT_CHECK(ce.mismatch);
    auto L = BuildPlanLines(p, err);
    LT_CHECK(Has(L, "error: Present failed"));
    LT_CHECK(CountKind(L, LineKind::Warn) >= 2);
}

LOGIC_TEST(pipeline_overlay_planned_zero_host_copies_but_uploads_is_a_mismatch) {
    wvp::Plan p = D3D11Plan();
    wvp::DebugInfo ok = WithPresenter(Info(100, 0, 0, "d3d11va"), "d3d11", 99, 0);
    LT_CHECK(!CheckPlan(p, ok).mismatch);
    wvp::DebugInfo bad = WithPresenter(Info(100, 0, 0, "d3d11va"), "d3d11", 0, 98);
    PlanCheck c = CheckPlan(p, bad);
    LT_CHECK(c.mismatch);
    LT_CHECK(c.problems.size() == 1 && c.problems[0].find("measured presenter uploads 98") != std::string::npos);
    // planned host copy 1 (upload) with uploads measured: fine
    p.cost.hostCopies = 1;
    LT_CHECK(!CheckPlan(p, bad).mismatch);
}
