// No window; synthetic events through UiHost.
#include "pui/ScrollViewer.h"
#include "wvptest.h"

using namespace pui;

namespace {
uint64_t g_svNow = 5000;

struct Item : Widget {
    Item() : Widget(Role::Button) { SetFocusable(true); }
};

struct SvFx {
    UiHost h;
    ScrollViewer* sv = nullptr;
    Widget* content = nullptr;
    Item* item = nullptr;
    explicit SvFx(float extent = 1000, float viewH = 200) {
        g_svNow = 5000;
        h.SetClock([] { return g_svNow; });
        h.SetSize({400, 400});
        auto* root = h.SetRoot(std::make_unique<Widget>(Role::Pane));
        sv = root->Add<ScrollViewer>();
        sv->SetBounds({10, 20, 310, 20 + viewH});
        content = sv->SetContent(std::make_unique<Widget>(Role::Pane));
        item = content->Add<Item>();
        sv->SetExtentHeight(extent);
        item->SetBounds({10, 20, 100, 60});
    }
};
}

TEST_CASE(SV_OffsetClampAndContentArrangement) {
    SvFx f;
    CHECK_EQ(f.sv->Offset(), 0.f);
    CHECK_EQ(f.content->Bounds().t, 20.f);
    CHECK_EQ(f.content->Bounds().h(), 1000.f);
    CHECK_EQ(f.content->Bounds().w(), 300.f);
    f.sv->SetOffset(-5);
    CHECK_EQ(f.sv->Offset(), 0.f);
    f.sv->SetOffset(300);
    CHECK_EQ(f.sv->Offset(), 300.f);
    CHECK_EQ(f.content->Bounds().t, 20.f - 300.f);
    f.sv->SetOffset(99999);
    CHECK_EQ(f.sv->Offset(), 800.f);
    CHECK_EQ(f.sv->MaxOffset(), 800.f);
    CHECK_EQ(f.content->Bounds().b, 20.f - 800.f + 1000.f);
    CHECK(f.sv->ClipChildren());
    f.sv->SetExtentHeight(500);
    CHECK_EQ(f.sv->Offset(), 300.f);
    f.sv->SetExtentHeight(100);
    CHECK_EQ(f.sv->Offset(), 0.f);
    CHECK_EQ(f.content->Bounds().h(), 200.f);
}

TEST_CASE(SV_ExtentFromContentDesiredSize) {
    UiHost h;
    h.SetSize({400, 400});
    auto* root = h.SetRoot(std::make_unique<Widget>(Role::Pane));
    auto* sv = root->Add<ScrollViewer>();
    sv->SetBounds({0, 0, 300, 200});
    auto c = std::make_unique<Widget>(Role::Pane);
    c->SetBounds({0, 0, 300, 640});   // default DesiredSize = current bounds size
    sv->SetContent(std::move(c));
    CHECK_EQ(sv->MaxOffset(), 440.f);
}

TEST_CASE(SV_WheelStep) {
    SvFx f;
    PointerEvent e;
    e.pt = {100, 100};
    e.wheelDelta = -1;   // towards the user = scroll down
    CHECK(f.h.PointerWheel(e));
    CHECK_EQ(f.sv->Offset(), 48.f);
    f.h.PointerWheel(e);
    CHECK_EQ(f.sv->Offset(), 96.f);
    e.wheelDelta = 1;
    f.h.PointerWheel(e);
    CHECK_EQ(f.sv->Offset(), 48.f);
    f.sv->SetWheelStep(30);
    f.h.PointerWheel(e);
    CHECK_EQ(f.sv->Offset(), 18.f);
    f.h.PointerWheel(e);
    CHECK_EQ(f.sv->Offset(), 0.f);
    e.mods = ModCtrl;
    e.wheelDelta = -1;
    CHECK(!f.h.PointerWheel(e));
    f.sv->SetExtentHeight(100);
    e.mods = 0;
    CHECK(!f.h.PointerWheel(e));
}

TEST_CASE(SV_KeyboardPagingFromFocusedChild) {
    SvFx f;
    f.h.SetFocus(f.item, true);
    CHECK(f.h.KeyDown(key::PageDown, 0));
    CHECK_EQ(f.sv->Offset(), 200.f);
    CHECK(f.h.KeyDown(key::PageDown, 0));
    CHECK_EQ(f.sv->Offset(), 400.f);
    CHECK(f.h.KeyDown(key::PageUp, 0));
    CHECK_EQ(f.sv->Offset(), 200.f);
    CHECK(f.h.KeyDown(key::End, 0));
    CHECK_EQ(f.sv->Offset(), 800.f);
    CHECK(f.h.KeyDown(key::Home, 0));
    CHECK_EQ(f.sv->Offset(), 0.f);
    CHECK(!f.h.KeyDown(key::Down, 0));
    CHECK(!f.h.KeyDown(key::PageDown, ModCtrl));
}

TEST_CASE(SV_ScrollIntoViewRect) {
    SvFx f;
    f.sv->ScrollIntoView(RectF{10, 320, 100, 370});   // content y 300..350, below the viewport
    CHECK_EQ(f.sv->Offset(), 150.f);
    f.sv->ScrollIntoView(RectF{10, 20 + 120, 100, 20 + 140});   // already visible: no change
    CHECK_EQ(f.sv->Offset(), 150.f);
    f.sv->ScrollIntoView(RectF{10, -30, 100, 20});    // above the viewport (content y 100..150 -> abs -30..20)
    CHECK_EQ(f.sv->Offset(), 100.f);
    f.sv->ScrollIntoView(f.item);   // item bounds are content-relative abs coordinates
    CHECK(f.sv->Offset() >= 0.f);
}

TEST_CASE(SV_ScrollPatternVirtuals) {
    SvFx f;
    CHECK(f.sv->SupportsScroll());
    CHECK_EQ(f.sv->VerticalScrollPercent(), 0.0);
    CHECK_EQ(f.sv->VerticalViewSize(), 20.0);
    f.sv->SetVerticalScrollPercent(50);
    CHECK_EQ(f.sv->Offset(), 400.f);
    CHECK_EQ(f.sv->VerticalScrollPercent(), 50.0);
    f.sv->SetVerticalScrollPercent(250);
    CHECK_EQ(f.sv->Offset(), 800.f);
    f.sv->SetExtentHeight(100);
    CHECK_EQ(f.sv->VerticalScrollPercent(), -1.0);
    CHECK_EQ(f.sv->VerticalViewSize(), 100.0);
}

TEST_CASE(SV_ScrollbarThumbGeometry) {
    SvFx f;   // view 10..310 x 20..220, extent 1000
    const auto& sb = f.sv->ScrollBar();
    CHECK(sb.Needed());
    RectF th = sb.ThumbRect();
    // inner track = 176 DIP tall (arrows 12 + 12); thumb = 176 * 200/1000 = 35.2
    CHECK(std::fabs(th.h() - 35.2f) < 0.01f);
    CHECK_EQ(th.t, 20.f + 12.f);
    CHECK_EQ(th.w(), 2.f);   // collapsed indicator
    CHECK_EQ(th.r, 310.f - 2.f);
    f.sv->SetOffset(800);
    th = sb.ThumbRect();
    CHECK(std::fabs(th.b - (220.f - 12.f)) < 0.01f);
    f.sv->SetExtentHeight(150);
    CHECK(!sb.Needed());
    CHECK(sb.HitPart({305, 100}) == ScrollBarController::Part::None);
    f.sv->SetExtentHeight(100000);
    CHECK_EQ(sb.ThumbRect().h(), ScrollBarController::kMinThumb);
}

TEST_CASE(SV_ScrollbarDragPageAndArrows) {
    SvFx f;
    PointerEvent e;
    // thumb is at y 32..67.2; grab it at y=40
    e.pt = {305, 40};
    CHECK(f.h.HitTest(e.pt) == f.sv);   // the zone belongs to the viewer, not the content
    CHECK(f.h.PointerDown(e));
    CHECK(f.h.Captured() == f.sv);
    CHECK(f.sv->ScrollBar().Dragging());
    // drag by half of the travel (travel = 176 - 35.2 = 140.8)
    e.pt = {305, 40 + 70.4f};
    f.h.PointerMove(e);
    CHECK(std::fabs(f.sv->Offset() - 400.f) < 0.5f);
    e.pt = {305, 5000};
    f.h.PointerMove(e);
    CHECK_EQ(f.sv->Offset(), 800.f);
    e.pt = {305, -5000};
    f.h.PointerMove(e);
    CHECK_EQ(f.sv->Offset(), 0.f);
    f.h.PointerUp(e);
    CHECK(!f.sv->ScrollBar().Dragging());
    CHECK(f.h.Captured() == nullptr);
    e.pt = {305, 150};
    f.h.PointerDown(e);
    f.h.PointerUp(e);
    CHECK_EQ(f.sv->Offset(), 200.f);
    f.h.PointerDown(e);
    f.h.PointerUp(e);
    CHECK_EQ(f.sv->Offset(), 400.f);
    e.pt = {305, 25};
    f.h.PointerDown(e);
    f.h.PointerUp(e);
    CHECK_EQ(f.sv->Offset(), 376.f);
    e.pt = {305, 215};
    f.h.PointerDown(e);
    f.h.PointerUp(e);
    CHECK_EQ(f.sv->Offset(), 400.f);
}

TEST_CASE(SV_ScrollbarFadeAndExpand) {
    SvFx f;
    const auto& sb = f.sv->ScrollBar();
    CHECK_EQ(sb.Opacity(g_svNow), 0.f);
    RecordingRenderer r;
    f.h.Paint(r);
    CHECK_EQ(r.Count(RecordingRenderer::Op::PushOpacity), 0);

    PointerEvent e;
    e.pt = {100, 100};
    e.wheelDelta = -1;
    f.h.PointerWheel(e);
    CHECK(f.h.NeedsTick());
    CHECK_EQ(sb.Opacity(g_svNow), 1.f);
    r.Clear();
    f.h.Paint(r);
    CHECK_EQ(r.Count(RecordingRenderer::Op::PushOpacity), 1);
    bool thumbDrawn = false;
    for (auto& c : r.calls)
        if (c.op == RecordingRenderer::Op::FillRoundRect && std::fabs(c.rect.w() - 2.f) < 0.01f) thumbDrawn = true;
    CHECK(thumbDrawn);

    g_svNow += 1000;
    CHECK(f.h.Tick());
    CHECK_EQ(sb.Opacity(g_svNow), 1.f);   // still within the delay
    g_svNow += 350;   // 1350 after activity: fading (delay 1200 + 150 of 300)
    CHECK(std::fabs(sb.Opacity(g_svNow) - 0.5f) < 0.01f);
    g_svNow += 400;
    CHECK(!f.h.Tick());   // fully faded: ticking stops
    CHECK(!f.h.NeedsTick());
    CHECK_EQ(sb.Opacity(g_svNow), 0.f);
    r.Clear();
    f.h.Paint(r);
    CHECK_EQ(r.Count(RecordingRenderer::Op::PushOpacity), 0);

    e.pt = {305, 100};
    f.h.PointerMove(e);
    CHECK(sb.Hover());
    CHECK(f.h.NeedsTick());
    for (int i = 0; i < 12; i++) { g_svNow += 50; f.h.Tick(); }
    CHECK_EQ(sb.Expand(), 1.f);
    CHECK_EQ(sb.ThumbRect().w(), 6.f);
    r.Clear();
    f.h.Paint(r);
    CHECK(r.HasText(L"") && r.HasText(L""));   // Segoe Fluent chevrons
    g_svNow += 10000;
    CHECK_EQ(sb.Opacity(g_svNow), 1.f);
    e.pt = {100, 100};
    f.h.PointerMove(e);
    CHECK(!sb.Hover());
    for (int i = 0; i < 12; i++) { g_svNow += 50; f.h.Tick(); }
    CHECK_EQ(sb.Expand(), 0.f);
}

namespace {
struct Stack : Widget {
    Item* a = nullptr;
    Stack() : Widget(Role::Pane) { a = Add<Item>(); }
    void OnLayout() override { a->SetBounds({Bounds().l + 10, Bounds().t + 10, Bounds().l + 110, Bounds().t + 50}); }
};
}

TEST_CASE(SV_ContentChildrenFollowOffsetAndClicksReachThem) {
    UiHost h;
    h.SetSize({400, 400});
    auto* root = h.SetRoot(std::make_unique<Widget>(Role::Pane));
    auto* sv = root->Add<ScrollViewer>();
    sv->SetBounds({0, 0, 300, 200});
    auto* st = sv->EmplaceContent<Stack>();
    sv->SetExtentHeight(1000);
    CHECK_EQ(st->a->Bounds().t, 10.f);
    PointerEvent e;
    e.pt = {50, 30};
    CHECK(h.HitTest(e.pt) == st->a);
    h.PointerDown(e);
    CHECK(h.Focused() == st->a);
    h.PointerUp(e);
    sv->SetOffset(100);
    CHECK_EQ(st->a->Bounds().t, -90.f);
    CHECK(h.HitTest({50, 30}) != st->a);
    sv->ScrollIntoView(st->a);
    CHECK_EQ(sv->Offset(), 10.f);
    CHECK_EQ(st->a->Bounds().t, 0.f);
}
