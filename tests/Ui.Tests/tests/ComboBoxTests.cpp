#include "pui/ComboBox.h"
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
void ClickCenter(UiHost& h, Widget* w) { Click(h, w->Bounds().cx(), w->Bounds().cy()); }

ComboBox* MakeCombo(Env& e, int n = 5, std::vector<int>* log = nullptr) {
    auto* c = e.root->Add<ComboBox>();
    for (int i = 0; i < n; i++) c->AddItem(L"Item " + std::to_wstring(i));
    c->SetBounds({100, 100, 300, 132});
    c->SetName(L"Speed");
    if (log) c->onSelectionChanged = [log](int i) { log->push_back(i); };
    return c;
}
}

TEST_CASE(ComboPlacement_SelectedItemOverlaysBox) {
    RectF box{100, 100, 300, 132};
    SizeF client{800, 600};
    RectF r = ComputeComboPopupRect(box, 5, 2, 32, 2, 0, client);
    CHECK_EQ(r.l, 100.f);
    CHECK_EQ(r.w(), 200.f);
    CHECK_EQ(r.h(), 5 * 32.f + 4.f);
    float rowCentre = r.t + 2 + 2 * 32.f + 16.f;
    CHECK_EQ(rowCentre, box.cy());
    RectF r0 = ComputeComboPopupRect(box, 5, -1, 32, 2, 0, client);
    CHECK_EQ(r0.t + 2 + 16.f, box.cy());
    CHECK_EQ(ComputeComboPopupRect(box, 5, 0, 32, 2, 260, client).w(), 260.f);
}

TEST_CASE(ComboPlacement_ClampsInsideClient) {
    SizeF client{800, 600};
    RectF top = ComputeComboPopupRect({100, 10, 300, 42}, 6, 5, 32, 2, 0, client);
    CHECK_EQ(top.t, 4.f);
    RectF bot = ComputeComboPopupRect({100, 560, 300, 592}, 5, 0, 32, 2, 0, client);
    CHECK_EQ(bot.b, 596.f);
    RectF right = ComputeComboPopupRect({700, 100, 900, 132}, 3, 1, 32, 2, 0, client);
    CHECK_EQ(right.r, 796.f);
    RectF left = ComputeComboPopupRect({-20, 100, 180, 132}, 3, 1, 32, 2, 0, client);
    CHECK_EQ(left.l, 4.f);
    RectF tall = ComputeComboPopupRect({100, 300, 300, 332}, 40, 20, 32, 2, 0, client);
    CHECK_EQ(tall.h(), 592.f);
    CHECK_EQ(tall.t, 4.f);
    RectF mid = ComputeComboPopupRect({100, 300, 300, 332}, 3, 1, 32, 2, 0, client);
    CHECK_EQ(mid.t, 300.f - 2 - 32.f);
}

TEST_CASE(ComboBox_DesiredSizeAndPaint) {
    Env e;
    Theme t = Theme::Make(false);
    auto* c = MakeCombo(e);
    CHECK_EQ(c->DesiredSize(nullptr).h, 32.f);
    CHECK_EQ(c->DesiredSize(nullptr).w, 6 * 7.f + 50.f);               // "Item 0" 6 chars
    CHECK(c->GetRole() == Role::ComboBox);
    CHECK(c->Focusable());
    RecordingRenderer rr;
    c->Paint(rr, t);
    CHECK(rr.HasText(std::wstring(1, glyph::ChevronDown)));
    CHECK(rr.calls[0].color == t.ctlDefault);
    c->SetSelectedIndex(3);
    rr.Clear(); c->Paint(rr, t);
    CHECK(rr.HasText(L"Item 3"));
    c->SetEnabled(false);
    rr.Clear(); c->Paint(rr, t);
    CHECK(rr.calls[0].color == t.ctlDisabled);
}

TEST_CASE(ComboBox_ClickOpensPopupAndPicksItem) {
    Env e;
    std::vector<int> log;
    auto* c = MakeCombo(e, 5, &log);
    c->SetSelectedIndex(2);
    ClickCenter(e.h, c);
    CHECK(c->IsOpen());
    CHECK(c->IsExpanded());
    CHECK_EQ(e.h.PopupCount(), (size_t)1);
    CHECK_EQ(e.Count(c, UiEvent::ExpandCollapseChanged), 1);
    Widget* pop = c->PopupWidget();
    REQUIRE(pop != nullptr);
    CHECK(pop->GetRole() == Role::List);
    CHECK(pop->SupportsSelection());
    CHECK_EQ(pop->Children().size(), (size_t)5);
    CHECK_EQ(pop->Children()[2]->Bounds().cy(), c->Bounds().cy());
    CHECK(pop->Children()[2]->GetRole() == Role::ListItem);
    CHECK(pop->Children()[2]->IsSelected());
    CHECK(!pop->Children()[1]->IsSelected());
    CHECK_EQ(pop->Children()[4]->Name(), std::wstring(L"Item 4"));
    REQUIRE(pop->SelectedChildren().size() == 1);
    CHECK(pop->SelectedChildren()[0] == pop->Children()[2].get());
    CHECK(e.h.Focused() == pop);
    ClickCenter(e.h, pop->Children()[4].get());
    CHECK(!c->IsOpen());
    CHECK_EQ(e.h.PopupCount(), (size_t)0);
    CHECK_EQ(c->SelectedIndex(), 4);
    REQUIRE(log.size() == 1);
    CHECK_EQ(log[0], 4);
    CHECK_EQ(c->GetValue(), std::wstring(L"Item 4"));
    CHECK(e.h.Focused() == c);
    CHECK_EQ(e.Count(c, UiEvent::SelectionChanged), 2);                 // programmatic + popup
    CHECK_EQ(e.Count(c, UiEvent::ExpandCollapseChanged), 2);
}

TEST_CASE(ComboBox_DropdownReportsItsClampedAcrylicRect) {
    Env e;
    auto* c = MakeCombo(e, 40);
    e.h.SetSize({500, 374});
    c->SetBounds({430, 330, 494, 362});
    c->Open();
    Widget* pop = c->PopupWidget();
    REQUIRE(pop != nullptr);
    auto surfaces = e.h.AcrylicPopupSurfaces();
    REQUIRE(surfaces.size() == 1);
    CHECK_EQ(surfaces[0].bounds.l, pop->Bounds().l);
    CHECK_EQ(surfaces[0].bounds.t, pop->Bounds().t);
    CHECK_EQ(surfaces[0].bounds.r, pop->Bounds().r);
    CHECK_EQ(surfaces[0].bounds.b, pop->Bounds().b);
    CHECK_EQ(surfaces[0].cornerRadius, e.h.GetTheme().overlayRadius);
}

TEST_CASE(ComboBox_AcrylicDoesNotBlurThroughSolidPopupBelowIt) {
    Env e;
    auto panel = std::make_unique<Widget>(Role::Pane);
    panel->SetBounds({40, 40, 360, 300});
    e.h.OpenPopup(std::move(panel));  // a custom solid popup (for example a non-Acrylic panel)
    auto* c = MakeCombo(e, 5);
    c->Open();
    CHECK(c->IsOpen());
    CHECK(e.h.AcrylicPopupSurfaces().empty());
}

TEST_CASE(ComboBox_LightDismissKeepsSelection) {
    Env e;
    std::vector<int> log;
    auto* c = MakeCombo(e, 5, &log);
    c->SetSelectedIndex(1);
    c->Open();
    CHECK(c->IsOpen());
    Click(e.h, 700, 500);
    CHECK(!c->IsOpen());
    CHECK_EQ(c->SelectedIndex(), 1);
    CHECK(log.empty());
    c->Open();
    Widget* pop = c->PopupWidget();
    ClickCenter(e.h, pop->Children()[1].get());
    CHECK(log.empty());
    CHECK(!c->IsOpen());
}

TEST_CASE(ComboBox_KeyboardWhenClosed) {
    Env e;
    std::vector<int> log;
    auto* c = MakeCombo(e, 4, &log);
    e.h.KeyDown(key::Tab, 0);
    CHECK(e.h.Focused() == c);
    e.h.KeyDown(key::Down, 0);  CHECK_EQ(c->SelectedIndex(), 0);
    e.h.KeyDown(key::Down, 0);  CHECK_EQ(c->SelectedIndex(), 1);
    e.h.KeyDown(key::Up, 0);    CHECK_EQ(c->SelectedIndex(), 0);
    e.h.KeyDown(key::Up, 0);    CHECK_EQ(c->SelectedIndex(), 0);
    e.h.KeyDown(key::End, 0);   CHECK_EQ(c->SelectedIndex(), 3);
    e.h.KeyDown(key::Home, 0);  CHECK_EQ(c->SelectedIndex(), 0);
    CHECK_EQ(log.size(), (size_t)5);                                    // 0,1,0,3,0 (the clamped repeat is no change)
    CHECK(!c->IsOpen());
    e.h.KeyDown(key::Down, ModAlt);  CHECK(c->IsOpen()); c->Close(); CHECK(!c->IsOpen());
    e.h.KeyDown(key::F4, 0);         CHECK(c->IsOpen()); c->Close();
    e.h.KeyDown(key::Enter, 0);      CHECK(c->IsOpen()); c->Close();
    e.h.KeyDown(key::Space, 0);      CHECK(c->IsOpen()); c->Close();
    CHECK_EQ(e.h.PopupCount(), (size_t)0);
}

TEST_CASE(ComboBox_PopupKeyboard) {
    Env e;
    std::vector<int> log;
    auto* c = MakeCombo(e, 5, &log);
    c->SetSelectedIndex(1);
    e.h.SetFocus(c, true);
    e.h.KeyDown(key::Enter, 0);
    REQUIRE(c->IsOpen());
    CHECK(e.h.Focused() == c->PopupWidget());
    e.h.KeyDown(key::Down, 0);
    e.h.KeyDown(key::Down, 0);
    CHECK(c->IsOpen());
    CHECK_EQ(c->SelectedIndex(), 1);
    e.h.KeyDown(key::Enter, 0);
    CHECK(!c->IsOpen());
    CHECK_EQ(c->SelectedIndex(), 3);
    CHECK(e.h.Focused() == c);
    e.h.KeyDown(key::F4, 0);
    e.h.KeyDown(key::End, 0);
    e.h.KeyDown(key::Down, 0);
    e.h.KeyDown(key::Space, 0);
    CHECK_EQ(c->SelectedIndex(), 4);
    e.h.KeyDown(key::F4, 0);
    e.h.KeyDown(key::Home, 0);
    e.h.KeyDown(key::Up, 0);
    e.h.KeyDown(key::Enter, 0);
    CHECK_EQ(c->SelectedIndex(), 0);
    e.h.KeyDown(key::F4, 0);
    e.h.KeyDown(key::End, 0);
    CHECK(e.h.KeyDown(key::Escape, 0));
    CHECK(!c->IsOpen());
    CHECK_EQ(c->SelectedIndex(), 0);
    CHECK(e.h.Focused() == c);
    e.h.KeyDown(key::F4, 0);
    CHECK(c->IsOpen());
    e.h.KeyDown(key::Up, ModAlt);
    CHECK(!c->IsOpen());
    REQUIRE(log.size() == 3);
    CHECK_EQ(log[0], 3); CHECK_EQ(log[1], 4); CHECK_EQ(log[2], 0);
}

TEST_CASE(ComboBox_PopupHoverHighlightAndWheelScroll) {
    Env e;
    // 30 items do not fit in a 300 DIP client: popup limited and scrollable
    e.h.SetSize({400, 300});
    auto* c = e.root->Add<ComboBox>();
    for (int i = 0; i < 30; i++) c->AddItem(L"Row " + std::to_wstring(i));
    c->SetBounds({50, 100, 250, 132});
    c->Open();
    Widget* pop = c->PopupWidget();
    REQUIRE(pop != nullptr);
    CHECK_EQ(pop->Bounds().t, 4.f);
    CHECK_EQ(pop->Bounds().h(), 292.f);
    CHECK_EQ(pop->Bounds().w(), 200.f);
    RecordingRenderer rr;
    Theme t = Theme::Make(false);
    pop->PaintTree(rr, t);
    CHECK_EQ(rr.Count(RecordingRenderer::Op::PushClip), 1);
    float y0 = pop->Children()[0]->Bounds().t;
    PointerEvent w = Pt(150, 150); w.wheelDelta = -1;
    CHECK(e.h.PointerWheel(w));
    CHECK(pop->Children()[0]->Bounds().t < y0);
    float cy = pop->Children()[3]->Bounds().cy();
    Click(e.h, 150, cy);
    CHECK(!c->IsOpen());
    CHECK(c->SelectedIndex() >= 0);
}

TEST_CASE(ComboBox_AutomationPatterns) {
    Env e;
    std::vector<int> log;
    auto* c = MakeCombo(e, 3, &log);
    CHECK(c->SupportsExpandCollapse());
    CHECK(c->SupportsSelection());
    CHECK(c->SupportsValue());
    CHECK(!c->ValueReadOnly());
    CHECK(!c->IsExpanded());
    c->Expand();
    CHECK(c->IsExpanded());
    c->Expand();
    CHECK_EQ(e.h.PopupCount(), (size_t)1);
    c->Collapse();
    CHECK(!c->IsExpanded());
    CHECK_EQ(e.h.PopupCount(), (size_t)0);
    CHECK_EQ(e.Count(c, UiEvent::ExpandCollapseChanged), 2);
    c->SetValue(L"Item 2");
    CHECK_EQ(c->SelectedIndex(), 2);
    CHECK_EQ(c->GetValue(), std::wstring(L"Item 2"));
    CHECK_EQ(e.Count(c, UiEvent::ValueChanged), 1);
    REQUIRE(log.size() == 1);
    c->SetValue(L"nope");
    CHECK_EQ(c->SelectedIndex(), 2);
    c->SetEnabled(false);
    c->Expand();
    CHECK(!c->IsExpanded());
    c->SetValue(L"Item 0");
    CHECK_EQ(c->SelectedIndex(), 2);
    auto* d = e.root->Add<ComboBox>();
    d->SetBounds({0, 0, 100, 32});
    d->Open();
    CHECK(!d->IsOpen());
}

TEST_CASE(ComboBox_DestroyedWhileOpenIsSafe) {
    Env e;
    auto* c = MakeCombo(e);
    c->Open();
    CHECK_EQ(e.h.PopupCount(), (size_t)1);
    e.root->RemoveChild(c);                                              // combo destroyed, popup still open
    e.h.CloseAllPopups();                                                // onClosed must not touch the dead combo
    CHECK_EQ(e.h.PopupCount(), (size_t)0);
}

TEST_CASE(ComboBox_TypeAheadLikeExplorer) {
    std::vector<std::wstring> items{L"Arial", L"Batang", L"Gulim", L"Gungsuh", L"Malgun Gothic", L"Meiryo", L"MS Gothic"};
    TypeAhead t;
    uint64_t now = 10000;
    CHECK_EQ(t.Next(L'm', now, items, 0), 4);
    CHECK_EQ(t.Next(L'e', now += 100, items, 4), 5);                 // "me" -> Meiryo
    CHECK_EQ(t.Next(L'g', now += 2000, items, 5), 2);                // timeout: new prefix "g"
    CHECK_EQ(t.Next(L'g', now += 100, items, 2), 3);                 // "gg": cycle the items starting with g
    CHECK_EQ(t.Next(L'g', now += 100, items, 3), 2);                 // ... wrapping
    CHECK_EQ(t.Next(L'U', now += 100, items, 2), -1);                // "gggu": no longer a repeat, and no match
    CHECK_EQ(t.Next(L'x', now += 2000, items, 2), -1);

    Env e;
    uint64_t clock = 10000;
    e.h.SetClock([&clock] { return clock; });
    std::vector<int> log;
    auto* c = e.root->Add<ComboBox>();
    c->SetItems(items);
    c->SetBounds({100, 100, 300, 132});
    c->onSelectionChanged = [&log](int i) { log.push_back(i); };
    e.h.SetFocus(c, true);
    e.h.Char(L'G');
    CHECK_EQ(c->SelectedIndex(), 2);
    e.h.KeyDown(key::F4, 0);
    REQUIRE(c->IsOpen());
    clock += 2000;
    e.h.Char(L'm');
    e.h.KeyDown(key::Enter, 0);
    CHECK_EQ(c->SelectedIndex(), 4);
    REQUIRE(log.size() == 2);
}
