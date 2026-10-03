#include "pui/Menu.h"
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
void Click(UiHost& h, float x, float y) {
    auto e = Pt(x, y);
    h.PointerMove(e); h.PointerDown(e); h.PointerUp(e);
}

struct SizedPopupContent : Widget {
    SizeF DesiredSize(IRenderer*) override { return {80, 40}; }
};

}

TEST_CASE(Theme_AcrylicTokensMatchWinUIInAppDefaults) {
    Theme dark = Theme::Make(false), light = Theme::Make(true);
    CHECK_EQ(dark.flyoutSurface.v, 0xFF2C2C2Cu);
    CHECK_EQ(dark.acrylicFallbackColor.v, 0xFF2C2C2Cu);
    CHECK_EQ(dark.acrylicTintColor.v, 0xFF2C2C2Cu);
    CHECK_EQ(dark.acrylicLuminosityColor.v, 0xFF2C2C2Cu);
    CHECK_EQ(dark.acrylicTintOpacity, 0.15f);
    CHECK_EQ(dark.acrylicLuminosityOpacity, 0.96f);
    CHECK_EQ(light.flyoutSurface.v, 0xFFF9F9F9u);
    CHECK_EQ(light.acrylicFallbackColor.v, 0xFFF9F9F9u);
    CHECK_EQ(light.acrylicTintColor.v, 0xFFFCFCFCu);
    CHECK_EQ(light.acrylicLuminosityColor.v, 0xFFFCFCFCu);
    CHECK_EQ(light.acrylicTintOpacity, 0.0f);
    CHECK_EQ(light.acrylicLuminosityOpacity, 0.85f);
    CHECK(!dark.acrylicEnabled && !light.acrylicEnabled);
}

TEST_CASE(Flyout_ReportsItsClampedAcrylicRect) {
    Env e;
    auto popup = std::make_unique<Flyout>();
    popup->SetContent(std::make_unique<SizedPopupContent>());
    Flyout* raw = Flyout::ShowAt(e.h, std::move(popup), RectF{760, 560, 796, 596}, Placement::Bottom);
    REQUIRE(raw != nullptr);
    auto surfaces = e.h.AcrylicPopupSurfaces();
    REQUIRE(surfaces.size() == 1);
    CHECK_EQ(surfaces[0].bounds.l, raw->Bounds().l);
    CHECK_EQ(surfaces[0].bounds.t, raw->Bounds().t);
    CHECK_EQ(surfaces[0].bounds.r, raw->Bounds().r);
    CHECK_EQ(surfaces[0].bounds.b, raw->Bounds().b);
}

TEST_CASE(PlaceFlyout_AutoPrefersBottomThenTop) {
    SizeF client{800, 600};
    Placement used = Placement::Auto;
    RectF r = PlaceFlyout({200, 100}, {100, 100, 136, 136}, Placement::Auto, client, 8, 4, &used);
    CHECK(used == Placement::Bottom);
    CHECK_EQ(r.t, 144.f);
    CHECK_EQ(r.l, 18.f);                                               // centred on the anchor
    r = PlaceFlyout({200, 100}, {100, 500, 136, 536}, Placement::Auto, client, 8, 4, &used);
    CHECK(used == Placement::Top);
    CHECK_EQ(r.t, 392.f);
    CHECK_EQ(r.b, 492.f);
}

TEST_CASE(PlaceFlyout_FlipsWhenPreferredSideDoesNotFit) {
    SizeF client{800, 600};
    Placement used;
    RectF r = PlaceFlyout({200, 100}, {100, 20, 136, 56}, Placement::Top, client, 8, 4, &used);
    CHECK(used == Placement::Bottom);
    CHECK_EQ(r.t, 64.f);
    r = PlaceFlyout({200, 100}, {100, 540, 136, 576}, Placement::Bottom, client, 8, 4, &used);
    CHECK(used == Placement::Top);
    CHECK_EQ(r.b, 532.f);
    r = PlaceFlyout({200, 100}, {700, 300, 736, 336}, Placement::Right, client, 8, 4, &used);
    CHECK(used == Placement::Left);
    CHECK_EQ(r.r, 692.f);
    CHECK_EQ(r.cy(), 318.f);
    r = PlaceFlyout({200, 100}, {10, 300, 46, 336}, Placement::Left, client, 8, 4, &used);
    CHECK(used == Placement::Right);
    CHECK_EQ(r.l, 54.f);
    r = PlaceFlyout({200, 100}, {400, 300, 436, 336}, Placement::Right, client, 8, 4, &used);
    CHECK(used == Placement::Right);
    CHECK_EQ(r.l, 444.f);
}

TEST_CASE(PlaceFlyout_ClampsInsideClient) {
    SizeF client{800, 600};
    RectF r = PlaceFlyout({200, 100}, {0, 100, 36, 136}, Placement::Bottom, client);
    CHECK_EQ(r.l, 4.f);
    r = PlaceFlyout({200, 100}, {770, 100, 806, 136}, Placement::Bottom, client);
    CHECK_EQ(r.r, 796.f);
    r = PlaceFlyout({200, 580}, {100, 300, 136, 336}, Placement::Auto, client);
    CHECK(r.t >= 4.f);
    CHECK(r.b <= 596.f);
    r = PlaceFlyout({900, 100}, {100, 100, 136, 136}, Placement::Bottom, client);
    CHECK_EQ(r.l, 4.f);
}

TEST_CASE(PopupPlacement_ClampsEveryAxisToMinimumClient) {
    SizeF client{500, 374};
    auto inside = [&](const RectF& r) {
        CHECK(r.l >= 0 && r.t >= 0 && r.r <= client.w && r.b <= client.h);
        CHECK(!r.Empty());
    };
    inside(PlaceFlyout({320, 600}, {448, 314, 484, 350}, Placement::Top, client));
}


TEST_CASE(Flyout_PlacementAndContent) {
    Env e;
    auto f = std::make_unique<Flyout>();
    Widget* content = f->SetContentNew<Widget>(Role::Pane);
    content->SetBounds({0, 0, 216, 60});
    Flyout* raw = f.get();
    CHECK_EQ(raw->DesiredSize(nullptr).w, 216.f);
    RectF anchor{400, 520, 436, 556};
    Flyout* shown = Flyout::ShowAt(e.h, std::move(f), anchor, Placement::Top);
    CHECK(shown == raw);
    CHECK(e.h.IsPopupOpen(raw));
    CHECK(raw->ActualPlacement() == Placement::Top);
    CHECK_EQ(raw->Bounds().w(), 216.f);
    CHECK_EQ(raw->Bounds().b, 512.f);                                  // 8 DIP gap above the anchor
    CHECK_EQ(raw->Bounds().cx(), 418.f);                               // centred on the anchor
    CHECK(content->Bounds() == raw->Bounds());                        // no padding by default
    RecordingRenderer rr;
    Theme t = Theme::Make(false);
    e.h.Paint(rr);
    bool surface = false, stroke = false;
    for (auto& c : rr.calls) {
        if (c.op == RecordingRenderer::Op::FillRoundRect && c.color == t.flyoutSurface && c.value == 8.f) surface = true;
        if (c.op == RecordingRenderer::Op::StrokeRoundRect && c.color == t.flyoutStroke && c.value == 8.f) stroke = true;
    }
    CHECK(surface);
    CHECK(stroke);
}

TEST_CASE(Flyout_PaddingFlipAndDismiss) {
    Env e;
    auto f = std::make_unique<Flyout>();
    f->SetPadding(8);
    auto* b = f->SetContentNew<IconButton>(glyph::Volume, L"Mute");
    Flyout* raw = f.get();
    bool closed = false;
    raw->onClosed = [&] { closed = true; };
    CHECK_EQ(raw->DesiredSize(nullptr).w, 36.f + 16.f);
    Flyout::ShowAt(e.h, std::move(f), RectF{400, 10, 436, 46}, Placement::Top);
    CHECK(raw->ActualPlacement() == Placement::Bottom);
    CHECK_EQ(raw->Bounds().t, 54.f);
    CHECK_EQ(b->Bounds().l, raw->Bounds().l + 8.f);
    CHECK(e.h.Focused() == b);
    Click(e.h, 700, 500);
    CHECK(closed);
    CHECK_EQ(e.h.PopupCount(), (size_t)0);
    auto* anchor = e.root->Add<IconButton>(glyph::Volume, L"Volume");
    anchor->SetBounds({300, 540, 336, 576});
    e.h.SetFocus(anchor, true);
    auto f2 = std::make_unique<Flyout>();
    f2->SetContentNew<Widget>(Role::Pane)->SetBounds({0, 0, 100, 40});
    Flyout* r2 = Flyout::ShowAt(e.h, std::move(f2), anchor, Placement::Top);
    CHECK(r2->ActualPlacement() == Placement::Top);
    CHECK_EQ(r2->Bounds().b, 532.f);
    r2->Hide();
    CHECK_EQ(e.h.PopupCount(), (size_t)0);
    CHECK(e.h.Focused() == anchor);
}
