#include "pui/InfoBar.h"
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
bool Near(float a, float b) { return std::fabs(a - b) < 0.01f; }
TextStyle WrapStyle(int weight) {
    TextStyle s; s.size = 14; s.weight = weight; s.wrap = true; s.ellipsis = false; s.valign = ParaAlign::Top; return s;
}
}

TEST_CASE(InfoBar_AutomationShape) {
    Env e;
    auto* b = e.root->Add<InfoBar>();
    b->SetTitle(L"Playback error");
    b->SetMessage(L"The file could not be opened.");
    CHECK_EQ(b->Name(), std::wstring(L"Playback error"));
    CHECK_EQ(b->HelpText(), std::wstring(L"The file could not be opened."));
    CHECK(b->GetRole() == Role::Group);
    CHECK(b->IsOpen());
    REQUIRE(b->CloseButton() != nullptr);
    CHECK_EQ(b->CloseButton()->Name(), std::wstring(L"Close"));
    CHECK(b->CloseButton()->Focusable());
    CHECK(!b->ActionButton()->Visible());
}

TEST_CASE(InfoBar_HeightGrowsWithWrappedMessage) {
    RecordingRenderer rr;
    Env e;
    auto* b = e.root->Add<InfoBar>();
    b->SetTitle(L"Error");
    CHECK_EQ(b->HeightForWidth(&rr, 480), 48.f);
    std::wstring longMsg(200, L'x');
    b->SetMessage(longMsg);
    float textW = 480 - 44 - (4 + 32 + 8);                              // icon column and close button reserved
    float expect = 28 + rr.MeasureText(L"Error", WrapStyle(600), textW).h + rr.MeasureText(longMsg, WrapStyle(400), textW).h;
    CHECK(Near(b->HeightForWidth(&rr, 480), expect));
    CHECK(b->HeightForWidth(&rr, 480) > 48.f);
    CHECK(b->HeightForWidth(&rr, 300) > b->HeightForWidth(&rr, 480));
    b->SetBounds({0, 0, 480, 100});
    SizeF d = b->DesiredSize(&rr);
    CHECK_EQ(d.w, 480.f);
    CHECK(Near(d.h, expect));
    float before = b->HeightForWidth(&rr, 480);
    b->SetActionText(L"Undo");
    CHECK(b->HeightForWidth(&rr, 480) >= before);
    b->SetActionText(L"");
    b->SetClosable(false);
    CHECK(b->HeightForWidth(&rr, 480) <= before);
}

TEST_CASE(InfoBar_LayoutCloseAndActionButtons) {
    Env e;
    auto* b = e.root->Add<InfoBar>();
    b->SetTitle(L"Update");
    b->SetMessage(L"Short");
    b->SetActionText(L"Undo");
    b->SetBounds({0, 0, 480, 48});
    RectF cb = b->CloseButton()->Bounds();
    CHECK_EQ(cb.w(), 32.f);
    CHECK_EQ(cb.r, 476.f);
    CHECK_EQ(cb.t, 8.f);
    CHECK(b->ActionButton()->Visible());
    CHECK_EQ(b->ActionButton()->Text(), std::wstring(L"Undo"));
    CHECK(b->ActionButton()->Bounds().r <= cb.l);
    int actions = 0;
    b->onAction = [&] { actions++; };
    ClickCenter(e.h, b->ActionButton());
    CHECK_EQ(actions, 1);
    CHECK(b->IsOpen());
}

TEST_CASE(InfoBar_CloseButtonClosesAndRaises) {
    Env e;
    auto* b = e.root->Add<InfoBar>();
    b->SetTitle(L"Done");
    b->SetBounds({0, 0, 480, 48});
    int closed = 0;
    b->onClosed = [&] { closed++; };
    ClickCenter(e.h, b->CloseButton());
    CHECK_EQ(closed, 1);
    CHECK(!b->IsOpen());
    CHECK(!b->Visible());
    CHECK_EQ(e.Count(b, UiEvent::VisibilityChanged), 1);
    b->SetOpen(true);
    CHECK(b->Visible());
    CHECK_EQ(e.Count(b, UiEvent::VisibilityChanged), 2);
    b->SetOpen(false);
    CHECK_EQ(closed, 1);
    b->SetOpen(true);
    b->SetClosable(false);
    CHECK(!b->CloseButton()->Visible());
    CHECK(!b->CloseButton()->CanFocus());
}

TEST_CASE(InfoBar_SeverityColoursAndGlyphs) {
    Env e;
    Theme dark = Theme::Make(false), light = Theme::Make(true);
    auto* b = e.root->Add<InfoBar>();
    b->SetTitle(L"T");
    b->SetBounds({0, 0, 480, 48});
    struct Case { InfoBarSeverity s; Color Theme::*bg; Color Theme::*fg; wchar_t g; };
    const Case cases[] = {
        {InfoBarSeverity::Informational, &Theme::attentionBg, &Theme::accent, glyph::Info},
        {InfoBarSeverity::Success, &Theme::successBg, &Theme::success, glyph::Success},
        {InfoBarSeverity::Warning, &Theme::cautionBg, &Theme::caution, glyph::Warning},
        {InfoBarSeverity::Error, &Theme::criticalBg, &Theme::critical, glyph::Error},
    };
    for (const Theme* th : {&dark, &light}) {
        for (auto& c : cases) {
            b->SetSeverity(c.s);
            CHECK_EQ((int)InfoBar::SeverityGlyph(c.s), (int)c.g);
            CHECK(b->BackgroundColor(*th) == (*th).*c.bg);
            CHECK(b->SeverityColor(*th) == (*th).*c.fg);
            RecordingRenderer rr;
            b->Paint(rr, *th);
            CHECK(rr.calls[0].op == RecordingRenderer::Op::FillRoundRect);
            CHECK(rr.calls[0].color == (*th).*c.bg);
            bool icon = false, badge = false;
            for (auto& k : rr.calls) {
                if (k.op == RecordingRenderer::Op::Text && k.text == std::wstring(1, c.g) && k.style.font == FontKind::Icon) icon = true;
                if (k.op == RecordingRenderer::Op::FillEllipse && k.color == (*th).*c.fg) badge = true;
            }
            CHECK(icon);
            CHECK(badge);
            CHECK(rr.HasText(L"T"));
        }
    }
}

TEST_CASE(InfoBar_PaintsTitleAndMessageOnSeparateLines) {
    Env e;
    Theme t = Theme::Make(false);
    auto* b = e.root->Add<InfoBar>();
    b->SetTitle(L"Title");
    b->SetMessage(L"Message body");
    b->SetBounds({0, 0, 480, 64});
    RecordingRenderer rr;
    b->Paint(rr, t);
    float ty = -1, my = -1;
    int tw = 0, mw = 0;
    for (auto& k : rr.calls) {
        if (k.op != RecordingRenderer::Op::Text) continue;
        if (k.text == L"Title") { ty = k.rect.t; tw = k.style.weight; }
        if (k.text == L"Message body") { my = k.rect.t; mw = k.style.weight; }
    }
    CHECK(ty >= 0 && my > ty);
    CHECK_EQ(tw, 600);
    CHECK_EQ(mw, 400);
}
