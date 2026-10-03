#include "pui/Slider.h"
#include "wvptest.h"

using namespace pui;

namespace {
struct Env {
    UiHost h;
    Widget* root = nullptr;
    std::vector<std::pair<Widget*, UiEvent>> ev;
    Env() {
        h.SetSize({800, 600});
        root = h.SetRoot(std::make_unique<Widget>(Role::Pane));
        h.AddListener([this](Widget* w, UiEvent e) { ev.push_back({w, e}); });
    }
    int Count(Widget* w, UiEvent e) const {
        int n = 0;
        for (auto& p : ev) if (p.first == w && p.second == e) n++;
        return n;
    }
};
PointerEvent Pt(float x, float y) { PointerEvent e; e.pt = {x, y}; return e; }
struct Change { double v; bool dragging; };
Slider* MakeSlider(Env& e, std::vector<Change>* log) {
    auto* s = e.root->Add<Slider>();
    s->SetBounds({0, 0, 220, 32});             // track x 10..210, y centre 16
    s->SetName(L"Seek");
    if (log) s->onChanged = [log](double v, bool d) { log->push_back({v, d}); };
    return s;
}
}

TEST_CASE(Slider_PureMapping) {
    CHECK_EQ(SliderValueToX(0, 0, 100, 10, 110), 10.f);
    CHECK_EQ(SliderValueToX(50, 0, 100, 10, 110), 60.f);
    CHECK_EQ(SliderValueToX(100, 0, 100, 10, 110), 110.f);
    CHECK_EQ(SliderValueToX(150, 0, 100, 10, 110), 110.f);
    CHECK_EQ(SliderValueToX(5, 10, 10, 10, 110), 10.f);             // degenerate range
    CHECK_EQ(SliderXToValue(60, 0, 100, 10, 110, 0), 50.0);
    CHECK_EQ(SliderXToValue(-50, 0, 100, 10, 110, 0), 0.0);
    CHECK_EQ(SliderXToValue(500, 0, 100, 10, 110, 0), 100.0);
    CHECK_EQ(SliderXToValue(63, 0, 100, 10, 110, 10), 50.0);
    CHECK_EQ(SliderXToValue(66, 0, 100, 10, 110, 10), 60.0);
    CHECK_EQ(SliderSnap(7.6, 5, 20, 5), 10.0);                      // grid starts at min
    CHECK_EQ(SliderSnap(99, 0, 50, 5), 50.0);
    CHECK_EQ(SliderSnap(-3, 0, 50, 0), 0.0);
    CHECK_EQ(SliderXToValue(10, 0, 1, 10, 10, 0), 0.0);              // zero-width track
}

TEST_CASE(Slider_LayoutGeometry) {
    Env e;
    auto* s = MakeSlider(e, nullptr);
    RectF tr = s->TrackRect();
    CHECK_EQ(tr.l, 10.f); CHECK_EQ(tr.r, 210.f);
    CHECK_EQ(tr.h(), 4.f);
    CHECK_EQ(tr.cy(), 16.f);
    CHECK_EQ(s->DesiredSize(nullptr).h, 32.f);
    s->SetValue(50);
    RectF th = s->ThumbRect();
    CHECK_EQ(th.w(), 20.f);
    CHECK_EQ(th.cx(), 110.f);
    CHECK_EQ(th.cy(), 16.f);
    CHECK(s->GetRole() == Role::Slider);
    CHECK(s->Focusable());
}

TEST_CASE(Slider_ClickJumpsAndReportsDragEnd) {
    Env e;
    std::vector<Change> log;
    auto* s = MakeSlider(e, &log);
    e.h.PointerMove(Pt(110, 16));
    CHECK(e.h.PointerDown(Pt(110, 16)));
    CHECK(s->IsDragging());
    CHECK_EQ(s->Value(), 50.0);
    CHECK(e.h.Captured() == s);
    e.h.PointerUp(Pt(110, 16));
    CHECK(!s->IsDragging());
    REQUIRE(log.size() == 2);
    CHECK(log[0].v == 50.0 && log[0].dragging);
    CHECK(log[1].v == 50.0 && !log[1].dragging);
    CHECK_EQ(e.Count(s, UiEvent::ValueChanged), 1);
}

TEST_CASE(Slider_DragWithCaptureClampsOutsideBounds) {
    Env e;
    std::vector<Change> log;
    auto* s = MakeSlider(e, &log);
    e.h.PointerDown(Pt(10, 16));
    CHECK_EQ(s->Value(), 0.0);
    e.h.PointerMove(Pt(160, 300));                  // far below: still captured
    CHECK_EQ(s->Value(), 75.0);
    e.h.PointerMove(Pt(900, 16));
    CHECK_EQ(s->Value(), 100.0);
    e.h.PointerMove(Pt(-40, 16));
    CHECK_EQ(s->Value(), 0.0);
    e.h.PointerMove(Pt(60, 16));
    e.h.PointerUp(Pt(60, 16));
    CHECK_EQ(s->Value(), 25.0);
    REQUIRE(!log.empty());
    CHECK(!log.back().dragging);
    CHECK_EQ(log.back().v, 25.0);
    for (size_t i = 0; i + 1 < log.size(); i++) CHECK(log[i].dragging);
}

TEST_CASE(Slider_HoverPreviewValue) {
    Env e;
    auto* s = MakeSlider(e, nullptr);
    std::vector<std::optional<double>> hov;
    s->onHoverValue = [&](std::optional<double> v) { hov.push_back(v); };
    s->valueFormatter = [](double v) { return std::to_wstring((int)v) + L"%"; };
    e.h.PointerMove(Pt(60, 16));
    REQUIRE(hov.size() == 1);
    CHECK(hov[0].has_value());
    CHECK_EQ(*hov[0], 25.0);
    CHECK_EQ(s->ToolTip(), std::wstring(L"25%"));
    e.h.PointerMove(Pt(110, 16));
    CHECK_EQ(*hov.back(), 50.0);
    CHECK_EQ(s->ToolTip(), std::wstring(L"50%"));
    e.h.PointerMove(Pt(110, 200));
    CHECK(!hov.back().has_value());
    e.h.PointerMove(Pt(60, 16));
    size_t before = hov.size();
    e.h.PointerDown(Pt(60, 16));
    size_t afterDown = hov.size();
    CHECK(!hov.back().has_value());
    e.h.PointerMove(Pt(100, 16));
    CHECK_EQ(hov.size(), afterDown);
    CHECK(afterDown > before);
}

TEST_CASE(Slider_Keyboard) {
    Env e;
    std::vector<Change> log;
    auto* s = MakeSlider(e, &log);
    s->SetRange(0, 100);
    s->SetStep(1);
    s->SetLargeStep(10);
    s->SetValue(50);
    e.h.SetFocus(s, true);
    e.h.KeyDown(key::Right, 0);  CHECK_EQ(s->Value(), 51.0);
    e.h.KeyDown(key::Up, 0);     CHECK_EQ(s->Value(), 52.0);
    e.h.KeyDown(key::Left, 0);   CHECK_EQ(s->Value(), 51.0);
    e.h.KeyDown(key::Down, 0);   CHECK_EQ(s->Value(), 50.0);
    e.h.KeyDown(key::PageUp, 0); CHECK_EQ(s->Value(), 60.0);
    e.h.KeyDown(key::PageDown, 0); CHECK_EQ(s->Value(), 50.0);
    e.h.KeyDown(key::End, 0);    CHECK_EQ(s->Value(), 100.0);
    size_t n = log.size();
    e.h.KeyDown(key::Right, 0);
    CHECK_EQ(log.size(), n);
    e.h.KeyDown(key::Home, 0);   CHECK_EQ(s->Value(), 0.0);
    CHECK(log.back().v == 0.0 && !log.back().dragging);
    CHECK(!e.h.KeyDown(key::Escape, 0));
}

TEST_CASE(Slider_Wheel) {
    Env e;
    std::vector<Change> log;
    auto* s = MakeSlider(e, &log);
    s->SetValue(50);
    PointerEvent w = Pt(110, 16);
    w.wheelDelta = 1;
    CHECK(e.h.PointerWheel(w));
    CHECK_EQ(s->Value(), 51.0);
    w.wheelDelta = -1;
    e.h.PointerWheel(w); e.h.PointerWheel(w);
    CHECK_EQ(s->Value(), 49.0);
    CHECK_EQ(log.size(), (size_t)3);
}

TEST_CASE(Slider_DisabledBehaviour) {
    Env e;
    Theme t = Theme::Make(false);
    std::vector<Change> log;
    auto* s = MakeSlider(e, &log);
    s->SetValue(30);
    s->SetEnabled(false);
    e.h.PointerDown(Pt(110, 16));
    e.h.PointerUp(Pt(110, 16));
    CHECK_EQ(s->Value(), 30.0);
    CHECK(log.empty());
    CHECK(s->GetRange().readOnly);
    s->SetRangeValue(70);
    CHECK_EQ(s->Value(), 30.0);
    RecordingRenderer rr;
    s->Paint(rr, t);
    REQUIRE(rr.calls.size() == 1);                                   // track only
    CHECK(rr.calls[0].color == t.ctlStrongFillDisabled);
    s->SetEnabled(true);
    CHECK(!s->GetRange().readOnly);
    s->SetRangeValue(70);
    CHECK_EQ(s->Value(), 70.0);
}

TEST_CASE(Slider_RangeValuePattern) {
    Env e;
    std::vector<Change> log;
    auto* s = MakeSlider(e, &log);
    s->SetRange(0, 200);
    s->SetStep(2);
    s->SetLargeStep(20);
    CHECK(s->SupportsRangeValue());
    RangeInfo ri = s->GetRange();
    CHECK_EQ(ri.minimum, 0.0); CHECK_EQ(ri.maximum, 200.0);
    CHECK_EQ(ri.smallChange, 2.0); CHECK_EQ(ri.largeChange, 20.0);
    s->SetRangeValue(500);
    CHECK_EQ(s->Value(), 200.0);
    s->SetRangeValue(-5);
    CHECK_EQ(s->Value(), 0.0);
    s->SetRangeValue(33);
    CHECK_EQ(s->Value(), 34.0);
    CHECK_EQ(e.Count(s, UiEvent::ValueChanged), 3);
    CHECK(!log.empty());
    s->SetValue(50);
    size_t n = log.size();
    s->SetValue(60);
    CHECK_EQ(log.size(), n);
    s->SetRange(0, 40);
    CHECK_EQ(s->Value(), 40.0);
}

TEST_CASE(Slider_PaintThumbStates) {
    Env e;
    Theme t = Theme::Make(false);
    auto* s = MakeSlider(e, nullptr);
    s->SetValue(50);
    RecordingRenderer rr;
    s->Paint(rr, t);
    using Op = RecordingRenderer::Op;
    CHECK_EQ(rr.Count(Op::FillRoundRect), 2);                        // track + accent fill
    CHECK_EQ(rr.Count(Op::FillEllipse), 2);                          // thumb outer + inner dot
    CHECK(rr.calls[0].color == t.ctlStrongFill);
    CHECK(rr.calls[1].color == t.accent);
    CHECK_EQ(rr.calls[1].rect.r, 110.f);                             // fill ends at the thumb centre
    CHECK(rr.calls[2].color == t.ctlSolidFill);
    CHECK_EQ(rr.calls[2].rect.w(), 20.f);
    CHECK_EQ(rr.calls[4].rect.w(), 12.f);                            // inner dot 12 at rest
    e.h.PointerMove(Pt(110, 16));
    rr.Clear(); s->Paint(rr, t);
    CHECK_EQ(rr.calls[4].rect.w(), 14.f);
    CHECK(rr.calls[4].color == MulAlpha(t.accent, 0.9f));
    e.h.PointerDown(Pt(110, 16));
    rr.Clear(); s->Paint(rr, t);
    CHECK_EQ(rr.calls[4].rect.w(), 10.f);
    CHECK(rr.calls[4].color == MulAlpha(t.accent, 0.8f));
    e.h.PointerUp(Pt(110, 16));
    e.h.KeyDown(key::Right, 0);
    rr.Clear(); s->Paint(rr, t);
    CHECK_EQ(rr.Count(Op::StrokeRoundRect), 2);                      // keyboard focus visual
    s->SetValue(0);
    rr.Clear(); s->Paint(rr, t);
    CHECK_EQ(rr.Count(Op::FillRoundRect), 1);
}
