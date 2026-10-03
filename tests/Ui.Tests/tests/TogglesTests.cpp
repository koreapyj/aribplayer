#include "pui/Toggles.h"
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
}

TEST_CASE(ToggleSwitch_LayoutPaintAndToggle) {
    Env e;
    Theme t = Theme::Make(false);
    auto* s = e.root->Add<ToggleSwitch>();
    s->SetBounds({10, 10, 120, 42});
    RectF tr = s->TrackRect();
    CHECK_EQ(tr.w(), 40.f);
    CHECK_EQ(tr.h(), 20.f);
    CHECK_EQ(tr.cy(), 26.f);
    CHECK_EQ(s->DesiredSize(nullptr).w, 40.f + 12.f + 21.f);        // widest label "Off" = 3 chars * 7
    CHECK_EQ(s->DesiredSize(nullptr).h, 32.f);
    CHECK(s->GetRole() == Role::ToggleButton);
    CHECK(s->SupportsToggle());
    CHECK_EQ(s->ToggleState(), 0);
    std::vector<bool> seen;
    s->onToggled = [&](bool v) { seen.push_back(v); };
    RecordingRenderer rr;
    s->Paint(rr, t);
    CHECK(rr.HasText(L"Off"));
    CHECK(rr.calls[0].color == t.ctlAltSecondary);                  // off: alt fill + stroke
    CHECK_EQ(rr.Count(RecordingRenderer::Op::StrokeRoundRect), 1);
    CHECK_EQ(rr.calls[2].rect.cx(), tr.l + 10.f);
    ClickCenter(e.h, s);
    CHECK(s->IsOn());
    CHECK_EQ(s->ToggleState(), 1);
    CHECK_EQ(e.Count(s, UiEvent::ToggleChanged), 1);
    rr.Clear(); s->Paint(rr, t);
    CHECK(rr.HasText(L"On"));
    CHECK(rr.calls[0].color != t.ctlAltSecondary);                  // accent fill (hover variant)
    CHECK_EQ(rr.calls[1].rect.cx(), tr.r - 10.f);
    e.h.KeyDown(key::Space, 0);
    CHECK(s->IsOn());
    e.h.KeyUp(key::Space, 0);
    CHECK(!s->IsOn());
    e.h.KeyDown(key::Enter, 0);
    CHECK(s->IsOn());
    s->Toggle();
    CHECK(!s->IsOn());
    REQUIRE(seen.size() == 4);
    CHECK(seen[0] && !seen[1] && seen[2] && !seen[3]);
    s->SetEnabled(false);
    s->Toggle();
    CHECK(!s->IsOn());
    ClickCenter(e.h, s);
    CHECK(!s->IsOn());
}

TEST_CASE(CheckBox_ToggleAndAutomation) {
    Env e;
    Theme t = Theme::Make(false);
    auto* c = e.root->Add<CheckBox>(L"Loop");
    c->SetBounds({0, 0, 120, 32});
    CHECK(c->GetRole() == Role::CheckBox);
    CHECK_EQ(c->Name(), std::wstring(L"Loop"));
    CHECK_EQ(c->DesiredSize(nullptr).w, 20.f + 8.f + 28.f);
    bool last = false;
    c->onToggled = [&](bool v) { last = v; };
    RecordingRenderer rr;
    c->Paint(rr, t);
    CHECK(!rr.HasText(std::wstring(1, glyph::CheckMark)));
    ClickCenter(e.h, c);
    CHECK(c->Checked());
    CHECK(last);
    CHECK_EQ(c->ToggleState(), 1);
    CHECK_EQ(e.Count(c, UiEvent::ToggleChanged), 1);
    rr.Clear(); c->Paint(rr, t);
    CHECK(rr.HasText(std::wstring(1, glyph::CheckMark)));
    CHECK(rr.HasText(L"Loop"));
    c->Toggle();
    CHECK(!c->Checked());
    e.h.KeyDown(key::Tab, 0);
    CHECK(e.h.Focused() == c);
    e.h.KeyDown(key::Space, 0); e.h.KeyUp(key::Space, 0);
    CHECK(c->Checked());
    c->SetChecked(true);
    CHECK_EQ(e.Count(c, UiEvent::ToggleChanged), 3);
    c->SetEnabled(false);
    ClickCenter(e.h, c);
    c->Toggle();
    CHECK(c->Checked());
}

namespace {
RadioGroup* MakeGroup(Env& e, int n = 3) {
    auto* g = e.root->Add<RadioGroup>();
    for (int i = 0; i < n; i++) g->AddOption(L"Option " + std::to_wstring(i + 1));
    g->SetBounds({20, 20, 220, 20 + 32.f * n});
    return g;
}
int FocusableCount(RadioGroup* g) {
    int n = 0;
    for (int i = 0; i < g->OptionCount(); i++) if (g->Option(i)->Focusable()) n++;
    return n;
}
}

TEST_CASE(RadioGroup_LayoutAndAutomation) {
    Env e;
    auto* g = MakeGroup(e);
    CHECK(g->GetRole() == Role::Group);
    CHECK(g->SupportsSelection());
    CHECK(!g->CanSelectMultiple());
    CHECK_EQ(g->OptionCount(), 3);
    CHECK_EQ(g->Option(1)->Bounds().t, 52.f);                        // stacked 32 DIP rows
    CHECK_EQ(g->Option(2)->Bounds().t, 84.f);
    CHECK_EQ(g->Option(0)->Bounds().w(), 200.f);
    CHECK_EQ(g->DesiredSize(nullptr).h, 96.f);
    CHECK(g->Option(0)->GetRole() == Role::RadioButton);
    CHECK_EQ(g->Option(2)->Name(), std::wstring(L"Option 3"));
    CHECK(g->Option(0)->SupportsSelectionItem());
    CHECK(g->Option(0)->SelectionContainer() == g);
    CHECK(g->SelectedChildren().empty());
    CHECK_EQ(g->SelectedIndex(), -1);
    g->Option(1)->SelectItem();
    CHECK_EQ(g->SelectedIndex(), 1);
    CHECK(g->Option(1)->IsSelected());
    CHECK(!g->Option(0)->IsSelected());
    REQUIRE(g->SelectedChildren().size() == 1);
    CHECK(g->SelectedChildren()[0] == g->Option(1));
    CHECK_EQ(e.Count(g, UiEvent::SelectionChanged), 1);
    CHECK_EQ(e.Count(g->Option(1), UiEvent::SelectionChanged), 1);
    g->Option(1)->RemoveFromSelection();
    CHECK_EQ(g->SelectedIndex(), -1);
}

TEST_CASE(RadioGroup_RovingFocusTabStops) {
    Env e;
    auto* before = e.root->Add<CheckBox>(L"before");
    before->SetBounds({0, 0, 50, 20});
    auto* g = MakeGroup(e);
    auto* after = e.root->Add<CheckBox>(L"after");
    after->SetBounds({0, 300, 50, 320});
    CHECK_EQ(FocusableCount(g), 1);
    CHECK(g->Option(0)->Focusable());
    auto order = e.h.FocusOrder();
    REQUIRE(order.size() == 3);
    CHECK(order[1] == g->Option(0));
    g->SetSelectedIndex(2);
    CHECK_EQ(FocusableCount(g), 1);
    CHECK(g->Option(2)->Focusable());
    order = e.h.FocusOrder();
    REQUIRE(order.size() == 3);
    CHECK(order[1] == g->Option(2));
    e.h.KeyDown(key::Tab, 0); CHECK(e.h.Focused() == before);
    e.h.KeyDown(key::Tab, 0); CHECK(e.h.Focused() == g->Option(2));
    e.h.KeyDown(key::Tab, 0); CHECK(e.h.Focused() == after);
    g->SetSelectedIndex(-1);
    g->Option(0)->SetEnabled(false);
    g->UpdateRoving();
    CHECK(g->Option(1)->Focusable());
    CHECK(!g->Option(2)->Focusable());
}

TEST_CASE(RadioGroup_ArrowKeysMoveSelectionAndFocus) {
    Env e;
    auto* g = MakeGroup(e);
    std::vector<int> sel;
    g->onSelectionChanged = [&](int i) { sel.push_back(i); };
    g->SetSelectedIndex(0);
    CHECK(sel.size() == 1);
    e.h.SetFocus(g->Option(0), true);
    CHECK(e.h.KeyDown(key::Down, 0));
    CHECK_EQ(g->SelectedIndex(), 1);
    CHECK(e.h.Focused() == g->Option(1));
    CHECK(e.h.KeyDown(key::Right, 0));
    CHECK_EQ(g->SelectedIndex(), 2);
    CHECK(e.h.Focused() == g->Option(2));
    CHECK(e.h.KeyDown(key::Down, 0));
    CHECK_EQ(g->SelectedIndex(), 0);
    CHECK(e.h.Focused() == g->Option(0));
    CHECK(e.h.KeyDown(key::Up, 0));
    CHECK_EQ(g->SelectedIndex(), 2);
    CHECK(e.h.Focused() == g->Option(2));
    CHECK(e.h.FocusVisible());
    CHECK_EQ(FocusableCount(g), 1);
    g->Option(0)->SetEnabled(false);
    CHECK(e.h.KeyDown(key::Down, 0));
    CHECK_EQ(g->SelectedIndex(), 1);
    CHECK(e.h.Focused() == g->Option(1));
    REQUIRE(sel.size() == 6);
    CHECK_EQ(sel[1], 1);
    CHECK_EQ(sel[5], 1);
}

TEST_CASE(RadioGroup_PointerSelectsAndFocuses) {
    Env e;
    auto* g = MakeGroup(e);
    ClickCenter(e.h, g->Option(1));
    CHECK_EQ(g->SelectedIndex(), 1);
    CHECK(e.h.Focused() == g->Option(1));
    CHECK(!e.h.FocusVisible());
    ClickCenter(e.h, g->Option(2));
    CHECK_EQ(g->SelectedIndex(), 2);
    CHECK_EQ(FocusableCount(g), 1);
    g->Option(0)->SetEnabled(false);
    ClickCenter(e.h, g->Option(0));
    CHECK_EQ(g->SelectedIndex(), 2);
    g->Option(0)->SetEnabled(true);
    e.h.SetFocus(g->Option(2), true);
    e.h.KeyDown(key::Space, 0);
    e.h.KeyUp(key::Space, 0);
    CHECK_EQ(g->SelectedIndex(), 2);
    RecordingRenderer rr;
    Theme t = Theme::Make(false);
    g->Option(2)->Paint(rr, t);
    CHECK_EQ(rr.Count(RecordingRenderer::Op::FillEllipse), 2);         // accent disc + inner dot
    rr.Clear();
    g->Option(1)->Paint(rr, t);
    CHECK_EQ(rr.Count(RecordingRenderer::Op::FillEllipse), 1);         // off: fill + stroke ring only
    CHECK_EQ(rr.Count(RecordingRenderer::Op::StrokeEllipse), 1);
}

TEST_CASE(RadioGroup_HorizontalLayout) {
    Env e;
    auto* g = MakeGroup(e, 2);
    g->SetHorizontal(true);
    g->SetBounds({0, 0, 400, 32});
    g->RequestLayout();
    CHECK(g->Option(1)->Bounds().l > g->Option(0)->Bounds().r);
    CHECK_EQ(g->Option(1)->Bounds().t, g->Option(0)->Bounds().t);
}
