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

// cuda decode (frames kept) > ivtc_cuda > d3d11 presenter importing cuda.
wvp::Plan CudaPlan() {
    wvp::Plan p;
    p.ok = true;
    p.hwaccel = "cuda";
    p.hwframesKeep = true;
    p.presenter = "d3d11";
    p.cost.hostCopies = 0;
    p.cost.deviceCopies = 1;
    wvp::PlanStep dec;
    dec.kind = wvp::PlanStep::Kind::Decode; dec.name = "decode:cuda"; dec.in = wvp::FrameDomain::System; dec.out = wvp::FrameDomain::Cuda;
    dec.reason = "cheapest: 0 host copies";
    wvp::PlanStep ivtc;
    ivtc.name = "ivtc_cuda"; ivtc.in = wvp::FrameDomain::Cuda; ivtc.out = wvp::FrameDomain::Cuda; ivtc.reason = "hw filter";
    wvp::PlanStep pr;
    pr.kind = wvp::PlanStep::Kind::Present; pr.name = "present:d3d11"; pr.in = wvp::FrameDomain::Cuda; pr.out = wvp::FrameDomain::D3D11;
    pr.copy = wvp::CopyKind::Device; pr.copies = 1; pr.reason = "import cuda";
    p.steps = {dec, ivtc, pr};
    return p;
}

wvp::DebugInfo Info(long long direct, long long download, long long software, const char* dev, const char* renderer) {
    wvp::DebugInfo d;
    d.Valid = true;
    d.HasVideo = true;
    d.Renderer = renderer;
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

LOGIC_TEST(pipeline_overlay_lines_cuda_ivtc_d3d11) {
    wvp::Plan p = CudaPlan();
    wvp::DebugInfo d = Info(100, 0, 0, "cuda", "d3d11");
    auto L = BuildPlanLines(p, d);
    LT_CHECK(!CheckPlan(p, d).mismatch);
    LT_CHECK_EQ(L[0].text, "pipeline  " + p.Summary());
    LT_CHECK(L[0].kind == LineKind::Normal);
    LT_CHECK(Has(L, "  decode:cuda [system->cuda] - cheapest: 0 host copies"));
    LT_CHECK(Has(L, "  ivtc_cuda [cuda->cuda] - hw filter"));
    LT_CHECK(Has(L, "  present:d3d11 [cuda->d3d11] devicex1 - import cuda"));
    LT_CHECK(Has(L, "copies    planned host 0, device 1 | measured hw_direct 100, hw_download 0, software 0"));
    LT_CHECK_EQ(CountKind(L, LineKind::Warn), 0);
}

LOGIC_TEST(pipeline_overlay_warmup_and_decoder_mismatch) {
    wvp::Plan p = CudaPlan();
    // below warm-up: nothing is compared
    LT_CHECK(!CheckPlan(p, Info(10, 0, 0, "none", "d3d11")).mismatch);
    // after warm-up: planned cuda, actual none
    wvp::DebugInfo d = Info(0, 0, 50, "none", "d3d11");
    PlanCheck c = CheckPlan(p, d);
    LT_CHECK(c.mismatch);
    LT_CHECK_EQ(c.problems.size(), (size_t)1);
    LT_CHECK_EQ(c.problems[0], std::string("planned decoder cuda, actual none"));
    auto L = BuildPlanLines(p, d);
    LT_CHECK(L[0].kind == LineKind::Warn);
    LT_CHECK(Has(L, "planned decoder cuda, actual none"));
    LT_CHECK(CountKind(L, LineKind::Warn) >= 3);
}

LOGIC_TEST(pipeline_overlay_keep_but_download) {
    wvp::Plan p = CudaPlan();
    wvp::DebugInfo d = Info(0, 60, 0, "cuda", "d3d11");
    PlanCheck c = CheckPlan(p, d);
    LT_CHECK(c.mismatch);
    LT_CHECK_EQ(c.problems[0], std::string("planned 0 decode copies, measured hw_download 60"));
}

LOGIC_TEST(pipeline_overlay_download_plan_direct_frames_and_software_plan) {
    wvp::Plan p = CudaPlan();
    p.hwframesKeep = false;
    LT_CHECK(CheckPlan(p, Info(50, 0, 0, "cuda", "d3d11")).mismatch);   // informational problem
    LT_CHECK(!CheckPlan(p, Info(0, 50, 0, "cuda", "d3d11")).mismatch);  // downloading as planned
    wvp::Plan s = CudaPlan();
    s.hwaccel.clear();
    s.hwframesKeep = false;
    s.presenter = "sdl";
    LT_CHECK(!CheckPlan(s, Info(0, 0, 50, "none", "sdl:direct3d11")).mismatch);
    LT_CHECK(CheckPlan(s, Info(0, 20, 30, "none", "sdl:direct3d11")).mismatch);
}

LOGIC_TEST(pipeline_overlay_renderer_mismatch) {
    wvp::Plan p = CudaPlan();   // d3d11 presenter
    LT_CHECK(CheckPlan(p, Info(0, 0, 0, "cuda", "sdl:direct3d11")).mismatch);
    wvp::DebugInfo noRenderer = Info(0, 0, 0, "cuda", "");
    LT_CHECK(!CheckPlan(p, noRenderer).mismatch);
    wvp::Plan s = CudaPlan();
    s.presenter = "sdl";
    LT_CHECK(!CheckPlan(s, Info(0, 0, 0, "cuda", "sdl:direct3d11")).mismatch);
    LT_CHECK(CheckPlan(s, Info(0, 0, 0, "cuda", "d3d11")).mismatch);
    LT_CHECK(CheckPlan(s, Info(0, 0, 0, "cuda", "vulkan")).mismatch);
}

LOGIC_TEST(pipeline_overlay_old_dll_measured_unknown) {
    wvp::Plan p = CudaPlan();
    wvp::DebugInfo d;
    d.Valid = true;
    d.Video.HwaccelDevice = "none";   // would mismatch, but no counters: no warm-up evidence
    auto L = BuildPlanLines(p, d);
    LT_CHECK(Has(L, "planned host 0, device 1 | measured ?"));
    LT_CHECK_EQ(CountKind(L, LineKind::Warn), 0);
}

LOGIC_TEST(pipeline_overlay_presenter_params_notes_rejected) {
    wvp::Plan p = CudaPlan();
    p.presenterParams.rotation = 90;
    p.presenterParams.flipH = true;
    p.presenterParams.eq = true;
    p.presenterParams.brightness = 0.1;
    p.presenterParams.contrast = 1.2;
    p.notes = {"content unknown: planned for 1080p"};
    for (int i = 0; i < 6; i++) {
        wvp::RejectedAlternative r;
        r.summary = "alt" + std::to_string(i);
        r.cost.hostCopies = i + 1;
        r.reason = "more copies";
        p.rejected.push_back(r);
    }
    auto L = BuildPlanLines(p, Info(100, 0, 0, "cuda", "d3d11"));
    LT_CHECK(Has(L, "presenter rotate 90, hflip, eq b=0.1 c=1.2 s=1 g=1"));
    LineKind k = LineKind::Normal;
    LT_CHECK(Has(L, "content unknown: planned for 1080p", &k));
    LT_CHECK(k == LineKind::Dim);
    LT_CHECK(Has(L, "  rejected alt0 [host 1, device 0] - more copies", &k));
    LT_CHECK(k == LineKind::Dim);
    LT_CHECK(Has(L, "rejected alt3"));
    LT_CHECK(!Has(L, "rejected alt4"));
    LT_CHECK(!Has(BuildPlanLines(p, Info(100, 0, 0, "cuda", "d3d11"), false), "rejected"));
    // identity params: no presenter line
    LT_CHECK(!Has(BuildPlanLines(CudaPlan(), Info(100, 0, 0, "cuda", "d3d11")), "presenter "));
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
    c.info = Info(100, 0, 0, "cuda", "d3d11");
    c.mode = "now playing";
    auto base = BuildOverlayLines(c);
    LT_CHECK(!Has(base, "pipeline"));
    c.plan = CudaPlan();
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
    d.Presenter.Requested = "d3d11";
    d.Presenter.Error = error;
    d.Presenter.FramesDirect = direct;
    d.Presenter.FramesUploaded = uploaded;
    return d;
}
}

LOGIC_TEST(pipeline_overlay_presenter_line_direct_and_uploaded) {
    wvp::Plan p = CudaPlan();
    p.cost.deviceCopies = 0;
    p.steps.back().copy = wvp::CopyKind::None;
    wvp::DebugInfo d = WithPresenter(Info(100, 0, 0, "cuda", "d3d11:videoprocessor"), "d3d11", 99, 0);
    auto L = BuildPlanLines(p, d);
    LT_CHECK(!CheckPlan(p, d).mismatch);                    // "d3d11" plan vs "d3d11:videoprocessor" renderer matches
    LT_CHECK(Has(L, "presenter d3d11 direct 99 uploaded 0"));
    LT_CHECK_EQ(CountKind(L, LineKind::Warn), 0);
    d.Presenter.Params = "{\"rotation\":90}";
    LT_CHECK(Has(BuildPlanLines(p, d), "params {\"rotation\":90}"));
}

LOGIC_TEST(pipeline_overlay_cuda_copy_mismatch_and_informational_zero_copy) {
    wvp::Plan zero = CudaPlan();
    zero.cost.deviceCopies = 0;
    zero.steps.back().copy = wvp::CopyKind::None;
    zero.steps.back().copies = 0;
    auto d = WithPresenter(Info(100, 0, 0, "cuda", "d3d11:videoprocessor"), "d3d11", 0, 0);
    d.Presenter.FramesCudaCopied = 91;
    auto bad = CheckPlan(zero, d);
    LT_CHECK(bad.mismatch);
    LT_CHECK(Has(BuildPlanLines(zero, d), "MISMATCH planned 0 device copies, measured cuda copied 91"));
    LineKind badKind = LineKind::Normal;
    LT_CHECK(Has(BuildPlanLines(zero, d), "cuda zero-copy 0, copied 91", &badKind));
    LT_CHECK(badKind == LineKind::Warn);
    d.Presenter.FramesCudaCopied = 0;
    d.Presenter.FramesCudaZeroCopy = 90;
    LT_CHECK(!CheckPlan(zero, d).mismatch);
    LT_CHECK(Has(BuildPlanLines(zero, d), "cuda zero-copy 90, copied 0"));
    wvp::Plan copied = CudaPlan();
    auto note = CheckPlan(copied, d);
    LT_CHECK(!note.mismatch);
    LT_CHECK_EQ(note.notes.size(), (size_t)1);
    LineKind kind = LineKind::Normal;
    LT_CHECK(Has(BuildPlanLines(copied, d), "NOTE cuda import zero-copy measured despite planned device copy", &kind));
    LT_CHECK(kind == LineKind::Dim);
    LT_CHECK_EQ(CountKind(BuildPlanLines(copied, d), LineKind::Warn), 0);
    d.Presenter.FramesCudaCopied = 5; // mixed per-plane/zero-copy still warrants a note, not a warning
    LT_CHECK(!CheckPlan(copied, d).mismatch);
    LT_CHECK_EQ(CheckPlan(copied, d).notes.size(), (size_t)1);
}

LOGIC_TEST(pipeline_overlay_presenter_name_mismatch_after_warmup_and_error_warn) {
    wvp::Plan p = CudaPlan();
    // before the warm-up a different presenter name is not judged
    wvp::DebugInfo early = WithPresenter(Info(5, 0, 0, "cuda", "d3d11:videoprocessor"), "sdl", 0, 0);
    LT_CHECK(!CheckPlan(p, early).mismatch);
    wvp::DebugInfo warm = WithPresenter(Info(100, 0, 0, "cuda", "d3d11:videoprocessor"), "sdl", 0, 0);
    PlanCheck c = CheckPlan(p, warm);
    LT_CHECK(c.mismatch);
    LT_CHECK(c.problems.size() == 1 && c.problems[0].find("presenter in use sdl") != std::string::npos);
    LT_CHECK(Has(BuildPlanLines(p, warm), "MISMATCH planned presenter d3d11, presenter in use sdl"));
    // an error is a problem at any time, even before the warm-up, and is printed
    wvp::DebugInfo err = WithPresenter(Info(5, 0, 0, "cuda", "sdl:direct3d11"), "sdl", 0, 0, "Present failed");
    PlanCheck ce = CheckPlan(p, err);
    LT_CHECK(ce.mismatch);
    auto L = BuildPlanLines(p, err);
    LT_CHECK(Has(L, "error: Present failed"));
    LT_CHECK(CountKind(L, LineKind::Warn) >= 2);
}

LOGIC_TEST(pipeline_overlay_planned_zero_host_copies_but_uploads_is_a_mismatch) {
    wvp::Plan p = CudaPlan();
    p.cost.deviceCopies = 0;
    p.hwaccel = "d3d11va";
    wvp::DebugInfo ok = WithPresenter(Info(100, 0, 0, "d3d11va", "d3d11:videoprocessor"), "d3d11", 99, 0);
    LT_CHECK(!CheckPlan(p, ok).mismatch);
    wvp::DebugInfo bad = WithPresenter(Info(100, 0, 0, "d3d11va", "d3d11:videoprocessor"), "d3d11", 0, 98);
    PlanCheck c = CheckPlan(p, bad);
    LT_CHECK(c.mismatch);
    LT_CHECK(c.problems.size() == 1 && c.problems[0].find("measured presenter uploads 98") != std::string::npos);
    // planned host copy 1 (upload) with uploads measured: fine
    p.cost.hostCopies = 1;
    LT_CHECK(!CheckPlan(p, bad).mismatch);
    // the renderer rule: a d3d11 plan with an SDL renderer is still a mismatch
    p.cost.hostCopies = 0;
    LT_CHECK(CheckPlan(p, Info(100, 0, 0, "d3d11va", "sdl:direct3d11")).mismatch);
}
