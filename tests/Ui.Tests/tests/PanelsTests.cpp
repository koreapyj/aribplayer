#include "pui/Panels.h"
#include "pui/ToolTipStyle.h"
#include "wvptest.h"

using namespace pui;

TEST_CASE(TextBlock_SizeKindsAndAutomation) {
    RecordingRenderer rr;
    TextBlock t(L"Hello");
    CHECK(t.GetRole() == Role::Text);
    CHECK_EQ(t.Name(), std::wstring(L"Hello"));
    SizeF d = t.DesiredSize(&rr);
    CHECK_EQ(d.w, 35.f);
    CHECK(!t.HitTestVisible());
    t.SetKind(TextKind::Title);
    CHECK_EQ(t.FontSize(), 28.f);
    CHECK_EQ(t.DesiredSize(&rr).w, 70.f);
    t.SetKind(TextKind::Caption);
    CHECK_EQ(t.FontSize(), 12.f);
}

TEST_CASE(TextBlock_WrapHeightAndPaintColors) {
    RecordingRenderer rr;
    Theme th = Theme::Make(false);
    TextBlock t(std::wstring(100, L'x'));
    float one = t.HeightForWidth(&rr, 1000);
    t.SetWrap(true);
    CHECK(t.HeightForWidth(&rr, 100) > one);
    CHECK_EQ(t.HeightForWidth(&rr, 1000), one);
    t.SetBounds({0, 0, 100, 50});
    TextColorRole roles[] = {TextColorRole::Primary, TextColorRole::Secondary, TextColorRole::Tertiary, TextColorRole::Accent, TextColorRole::Disabled};
    Color want[] = {th.textPrimary, th.textSecondary, th.textTertiary, th.accent, th.textDisabled};
    for (int i = 0; i < 5; i++) {
        t.SetColorRole(roles[i]);
        rr.Clear();
        t.Paint(rr, th);
        REQUIRE(rr.calls.size() == 1);
        CHECK(rr.calls[0].color == want[i]);
        CHECK(rr.calls[0].style.wrap);
    }
}

TEST_CASE(StackPanel_VerticalLayoutSpacingPaddingStretch) {
    RecordingRenderer rr;
    UiHost h;
    h.SetSize({400, 400});
    auto* sp = h.SetRoot(std::make_unique<StackPanel>(Orientation::Vertical, 8.f));
    static_cast<StackPanel*>(sp)->SetPadding(Thickness::All(10));
    auto* a = sp->Add<TextBlock>(L"abcd");
    auto* b = sp->Add<Button>(L"OK");
    sp->RequestLayout();
    CHECK_EQ(a->Bounds().l, 10.f);
    CHECK_EQ(a->Bounds().t, 10.f);
    CHECK_EQ(a->Bounds().w(), 380.f);
    float ah = a->Bounds().h();
    CHECK_EQ(b->Bounds().t, 10.f + ah + 8.f);
    CHECK_EQ(b->Bounds().h(), 32.f);
    SizeF d = sp->DesiredSize(&rr);
    CHECK_EQ(d.h, 20.f + ah + 8.f + 32.f);
    b->SetVisible(false);
    sp->RequestLayout();
    CHECK_EQ(sp->DesiredSize(&rr).h, 20.f + ah);
}

TEST_CASE(StackPanel_HorizontalAlignAndFlexSpacer) {
    UiHost h;
    h.SetSize({400, 100});
    auto* sp = static_cast<StackPanel*>(h.SetRoot(std::make_unique<StackPanel>(Orientation::Horizontal, 4.f)));
    sp->SetCrossAlign(CrossAlign::Center);
    auto* a = sp->Add<IconButton>(glyph::Add, L"Back");
    sp->Add<Spacer>(0.f, 0.f, true);
    auto* c = sp->Add<IconButton>(glyph::Close, L"More");
    sp->RequestLayout();
    CHECK_EQ(a->Bounds().l, 0.f);
    CHECK_EQ(a->Bounds().t, 32.f);                            // centred vertically in 100
    CHECK_EQ(c->Bounds().r, 400.f);
    sp->SetCrossAlign(CrossAlign::Start);
    CHECK_EQ(a->Bounds().t, 0.f);
    sp->SetCrossAlign(CrossAlign::End);
    CHECK_EQ(a->Bounds().b, 100.f);
}

TEST_CASE(StackPanel_NestedWrapHeightForWidth) {
    RecordingRenderer rr;
    auto sp = std::make_unique<StackPanel>(Orientation::Vertical, 0.f);
    auto* t = sp->Add<TextBlock>(std::wstring(100, L'x'));
    t->SetWrap(true);
    CHECK(sp->HeightForWidth(&rr, 100) > sp->HeightForWidth(&rr, 1000));
    sp->SetBounds({0, 0, 100, 500});
    CHECK_EQ(t->Bounds().h(), t->HeightForWidth(&rr, 100));
}

TEST_CASE(Card_ChromeAndDivider) {
    Theme th = Theme::Make(false);
    Card c;
    c.SetBounds({0, 0, 200, 100});
    RecordingRenderer rr;
    c.Paint(rr, th);
    CHECK(rr.calls[0].color == th.cardFill);
    CHECK_EQ(rr.calls[0].value, 8.f);
    CHECK(rr.calls[1].color == th.cardStroke);
    CHECK_EQ(c.Padding().l, 16.f);
    Divider d;
    d.SetBounds({0, 10, 100, 11});
    CHECK(d.GetRole() == Role::Separator);
    rr.Clear();
    d.Paint(rr, th);
    CHECK(rr.calls[0].op == RecordingRenderer::Op::Line);
    CHECK(rr.calls[0].color == th.dividerStroke);
    Spacer s(5.f, 7.f);
    CHECK_EQ(s.DesiredSize(nullptr).w, 5.f);
    CHECK_EQ(s.DesiredSize(nullptr).h, 7.f);
}

TEST_CASE(ToolTipStyle_PlacementAndPaint) {
    SizeF client{800, 600};
    RectF r = PlaceToolTip({100, 30}, {100, 300, 136, 336}, {118, 318}, client);
    CHECK_EQ(r.b, 292.f);                                     // 8 above
    CHECK_EQ(r.cx(), 118.f);
    r = PlaceToolTip({100, 30}, {100, 10, 136, 46}, {118, 20}, client);
    CHECK_EQ(r.t, 50.f);
    r = PlaceToolTip({100, 30}, {0, 300, 20, 320}, {10, 310}, client);
    CHECK_EQ(r.l, 4.f);
    r = PlaceToolTip({100, 30}, {780, 300, 800, 320}, {790, 310}, client);
    CHECK_EQ(r.r, 796.f);
    UiHost h;
    InstallToolTipStyle(h);
    CHECK(static_cast<bool>(h.paintToolTip));
    RecordingRenderer rr;
    Theme th = Theme::Make(false);
    h.paintToolTip(rr, th, L"Play", {100, 300, 136, 336}, {118, 318}, client);
    CHECK(rr.HasText(L"Play"));
    CHECK(rr.calls[0].color == th.flyoutSurface);
    CHECK(rr.calls[1].color == th.flyoutStroke);
    for (auto& c : rr.calls) if (c.op == RecordingRenderer::Op::Text) CHECK_EQ(c.style.size, 12.f);
}
