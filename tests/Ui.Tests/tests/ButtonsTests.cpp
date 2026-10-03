#include "pui/Buttons.h"
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
Theme Dark() { return Theme::Make(false); }
}

TEST_CASE(Button_DesiredSizeAndAutomationName) {
    RecordingRenderer rr;
    Button b(L"OK");
    SizeF d = b.DesiredSize(&rr);
    CHECK_EQ(d.h, 32.f);
    CHECK_EQ(d.w, 14.f + 22.f);                 // 2 chars * 7 + 22 padding
    CHECK(b.GetRole() == Role::Button);
    CHECK_EQ(b.Name(), std::wstring(L"OK"));
    b.SetText(L"Save");
    CHECK_EQ(b.Name(), std::wstring(L"Save"));
    b.SetName(L"Custom");
    b.SetText(L"Other");
    CHECK_EQ(b.Name(), std::wstring(L"Custom"));
    CHECK(b.Focusable());
    CHECK(b.SupportsInvoke());
    CHECK(!b.SupportsToggle());
    Button g(L"Add");
    g.SetGlyph(glyph::Add);
    CHECK_EQ(g.DesiredSize(&rr).w, 16.f + 8.f + 21.f + 22.f);
}

TEST_CASE(Button_PointerInvoke) {
    Env e;
    auto* b = e.root->Add<Button>(L"Go");
    b->SetBounds({10, 10, 110, 42});
    int clicks = 0;
    b->onClick = [&] { clicks++; };
    ClickCenter(e.h, b);
    CHECK_EQ(clicks, 1);
    CHECK_EQ(e.Count(b, UiEvent::Invoked), 1);
    CHECK(e.h.Focused() == b);
    CHECK(!e.h.FocusVisible());
    auto d = Pt(50, 20);
    e.h.PointerMove(d); e.h.PointerDown(d);
    CHECK(b->IsPressed());
    e.h.PointerUp(Pt(300, 300));
    CHECK_EQ(clicks, 1);
    CHECK(!b->IsPressed());
    PointerEvent r = Pt(50, 20); r.button = PointerButton::Right;
    e.h.PointerDown(r); e.h.PointerUp(r);
    CHECK_EQ(clicks, 1);
}

TEST_CASE(Button_Keyboard) {
    Env e;
    auto* a = e.root->Add<Button>(L"A");
    auto* b = e.root->Add<Button>(L"B");
    a->SetBounds({0, 0, 50, 32}); b->SetBounds({60, 0, 110, 32});
    int ca = 0, cb = 0;
    a->onClick = [&] { ca++; };
    b->onClick = [&] { cb++; };
    e.h.KeyDown(key::Tab, 0);
    CHECK(e.h.Focused() == a);
    CHECK(e.h.FocusVisible());
    CHECK(e.h.KeyDown(key::Enter, 0));
    CHECK_EQ(ca, 1);
    CHECK(e.h.KeyDown(key::Space, 0));
    CHECK_EQ(ca, 1);                             // Space activates on key up
    CHECK(a->IsFocused());
    e.h.KeyUp(key::Space, 0);
    CHECK_EQ(ca, 2);
    e.h.KeyDown(key::Tab, 0);
    CHECK(e.h.Focused() == b);
    e.h.KeyUp(key::Space, 0);
    CHECK_EQ(cb, 0);
    e.h.KeyDown(key::Space, 0);
    e.h.KeyDown(key::Tab, 0);
    e.h.KeyUp(key::Space, 0);
    CHECK_EQ(cb, 0);
}

TEST_CASE(Button_DisabledBehaviour) {
    Env e;
    auto* b = e.root->Add<Button>(L"X");
    b->SetBounds({0, 0, 80, 32});
    int clicks = 0;
    b->onClick = [&] { clicks++; };
    b->SetEnabled(false);
    ClickCenter(e.h, b);
    CHECK_EQ(clicks, 0);
    b->Invoke();
    CHECK_EQ(clicks, 0);
    CHECK(!b->CanFocus());
    CHECK(e.h.FocusOrder().empty());
    RecordingRenderer rr;
    b->Paint(rr, Dark());
    CHECK(rr.calls[0].op == RecordingRenderer::Op::FillRoundRect);
    CHECK(rr.calls[0].color == Dark().ctlDisabled);
    b->SetEnabled(true);
    b->Invoke();
    CHECK_EQ(clicks, 1);
}

TEST_CASE(Button_PaintStatesAndFocusVisual) {
    Env e;
    Theme t = Dark();
    auto* b = e.root->Add<Button>(L"Hi");
    b->SetBounds({0, 0, 80, 32});
    RecordingRenderer rr;
    b->Paint(rr, t);
    CHECK(rr.calls[0].color == t.ctlDefault);
    CHECK_EQ(rr.calls[0].value, t.controlRadius);
    CHECK(rr.HasText(L"Hi"));
    CHECK_EQ(rr.Count(RecordingRenderer::Op::StrokeRoundRect), 1);   // border only, no focus ring
    e.h.PointerMove(Pt(10, 10));
    rr.Clear(); b->Paint(rr, t);
    CHECK(rr.calls[0].color == t.ctlSecondary);
    e.h.PointerDown(Pt(10, 10));
    rr.Clear(); b->Paint(rr, t);
    CHECK(rr.calls[0].color == t.ctlTertiary);
    e.h.PointerUp(Pt(10, 10));
    e.h.KeyDown(key::Tab, 0);
    e.h.KeyDown(key::Tab, 0);
    CHECK(b->ShowFocusVisual());
    rr.Clear(); b->Paint(rr, t);
    CHECK_EQ(rr.Count(RecordingRenderer::Op::StrokeRoundRect), 3);   // border + 2-ring focus visual
}

TEST_CASE(Button_AccentStyle) {
    Env e;
    Theme t = Dark();
    auto* b = e.root->Add<Button>(L"Play", ButtonStyle::Accent);
    b->SetBounds({0, 0, 80, 32});
    RecordingRenderer rr;
    b->Paint(rr, t);
    CHECK(rr.calls[0].color == t.accent);
    CHECK(rr.calls[1].color == t.textOnAccent);                      // text (no border stroke for accent)
    e.h.PointerMove(Pt(10, 10));
    rr.Clear(); b->Paint(rr, t);
    CHECK(rr.calls[0].color == t.accentHover);
    e.h.PointerDown(Pt(10, 10));
    rr.Clear(); b->Paint(rr, t);
    CHECK(rr.calls[0].color == t.accentPressed);
}

TEST_CASE(IconButton_LayoutPaintAndInvoke) {
    Env e;
    Theme t = Dark();
    auto* b = e.root->Add<IconButton>(glyph::Add, L"More options");
    CHECK_EQ(b->DesiredSize(nullptr).w, 36.f);
    CHECK_EQ(b->DesiredSize(nullptr).h, 36.f);
    b->SetBounds({100, 100, 136, 136});
    CHECK_EQ(b->Name(), std::wstring(L"More options"));
    CHECK_EQ(b->ToolTip(), std::wstring(L"More options"));
    int clicks = 0;
    b->onClick = [&] { clicks++; };
    RecordingRenderer rr;
    b->Paint(rr, t);
    CHECK_EQ(rr.Count(RecordingRenderer::Op::FillRoundRect), 0);
    REQUIRE(rr.calls.size() == 1);
    CHECK(rr.calls[0].style.font == FontKind::Icon);
    CHECK_EQ(rr.calls[0].style.size, 16.f);
    CHECK(rr.calls[0].text == std::wstring(1, glyph::Add));
    CHECK(rr.calls[0].color == t.textPrimary);
    e.h.PointerMove(Pt(118, 118));
    rr.Clear(); b->Paint(rr, t);
    CHECK(rr.calls[0].color == t.subtleSecondary);
    CHECK_EQ(rr.calls[0].value, 18.f);
    e.h.PointerDown(Pt(118, 118));
    rr.Clear(); b->Paint(rr, t);
    CHECK(rr.calls[0].color == t.subtleTertiary);
    CHECK(rr.calls[1].color == t.textSecondary);
    e.h.PointerUp(Pt(118, 118));
    CHECK_EQ(clicks, 1);
    CHECK_EQ(e.Count(b, UiEvent::Invoked), 1);
    b->SetEnabled(false);
    rr.Clear(); b->Paint(rr, t);
    CHECK(rr.calls.back().color == t.textDisabled);
}

TEST_CASE(IconButton_KeyboardAndSize) {
    Env e;
    auto* b = e.root->Add<IconButton>(glyph::Play, L"Play");
    b->SetSize(50);
    b->SetGlyphSize(22);
    CHECK_EQ(b->DesiredSize(nullptr).w, 50.f);
    b->SetBounds({0, 0, 50, 50});
    int clicks = 0;
    b->onClick = [&] { clicks++; };
    e.h.KeyDown(key::Tab, 0);
    e.h.KeyDown(key::Enter, 0);
    CHECK_EQ(clicks, 1);
    e.h.KeyDown(key::Space, 0);
    e.h.KeyUp(key::Space, 0);
    CHECK_EQ(clicks, 2);
    b->SetControlFill(true);
    RecordingRenderer rr;
    b->Paint(rr, Dark());
    CHECK(rr.calls[0].color == Dark().ctlDefault);
}

TEST_CASE(ToggleButton_TogglePatternAndEvents) {
    Env e;
    auto* b = e.root->Add<ToggleButton>(L"Mute");
    b->SetBounds({0, 0, 80, 32});
    std::vector<bool> seen;
    b->onToggled = [&](bool v) { seen.push_back(v); };
    CHECK(b->GetRole() == Role::ToggleButton);
    CHECK(b->SupportsToggle());
    CHECK(!b->SupportsInvoke());
    CHECK_EQ(b->ToggleState(), 0);
    ClickCenter(e.h, b);
    CHECK(b->Checked());
    CHECK_EQ(b->ToggleState(), 1);
    CHECK_EQ(e.Count(b, UiEvent::ToggleChanged), 1);
    b->Toggle();
    CHECK(!b->Checked());
    b->SetChecked(false);
    CHECK_EQ(e.Count(b, UiEvent::ToggleChanged), 2);
    REQUIRE(seen.size() == 2);
    CHECK(seen[0] && !seen[1]);
    b->SetChecked(true);
    RecordingRenderer rr;
    b->Paint(rr, Dark());
    CHECK(rr.calls[0].color == Dark().accentHover);                 // checked uses the accent look (pointer still hovering)
    b->SetEnabled(false);
    b->Toggle();
    CHECK(b->Checked());
}

TEST_CASE(HyperlinkButton_InvokeAndPaint) {
    Env e;
    Theme t = Dark();
    auto* b = e.root->Add<HyperlinkButton>(L"Learn more");
    CHECK(b->GetRole() == Role::Hyperlink);
    CHECK_EQ(b->Name(), std::wstring(L"Learn more"));
    b->SetBounds({0, 0, 120, 32});
    int clicks = 0;
    b->onClick = [&] { clicks++; };
    RecordingRenderer rr;
    b->Paint(rr, t);
    CHECK(rr.calls[0].color == t.accent);
    CHECK_EQ(rr.Count(RecordingRenderer::Op::Line), 0);
    e.h.PointerMove(Pt(10, 10));
    rr.Clear(); b->Paint(rr, t);
    CHECK_EQ(rr.Count(RecordingRenderer::Op::Line), 1);
    ClickCenter(e.h, b);
    CHECK_EQ(clicks, 1);
    CHECK(b->SupportsInvoke());
    b->Invoke();
    CHECK_EQ(clicks, 2);
}
