#include "pui/Widget.h"
#include "wvptest.h"

using namespace pui;

namespace {
struct Box : Widget {
    int downs = 0, ups = 0, keys = 0, ctx = 0;
    bool handleDown = true;
    explicit Box(bool focusable = true) : Widget(Role::Button) { SetFocusable(focusable); }
    bool OnPointerDown(const PointerEvent&) override { downs++; return handleDown; }
    bool OnPointerUp(const PointerEvent&) override { ups++; return true; }
    bool OnKeyDown(int vk, unsigned) override { if (vk == key::Enter) { keys++; return true; } return false; }
    bool OnContextMenu(PointF, bool) override { ctx++; return true; }
};
uint64_t g_now = 1000;
}

TEST_CASE(UiHost_HitTestAndCapture) {
    UiHost h;
    h.SetClock([] { return g_now; });
    h.SetSize({400, 300});
    auto* root = h.SetRoot(std::make_unique<Widget>(Role::Pane));
    auto* a = root->Add<Box>();
    auto* b = root->Add<Box>();
    a->SetBounds({0, 0, 100, 100});
    b->SetBounds({50, 50, 150, 150});  // overlaps a; later child wins
    CHECK(h.HitTest({75, 75}) == b);
    CHECK(h.HitTest({10, 10}) == a);
    CHECK(h.HitTest({300, 250}) == root);
    PointerEvent e; e.pt = {10, 10};
    CHECK(h.PointerDown(e));
    CHECK(h.Captured() == a);
    CHECK(a->IsPressed());
    CHECK(h.Focused() == a);
    CHECK(!h.FocusVisible());
    e.pt = {300, 250};
    h.PointerMove(e);
    CHECK(h.Hovered() == nullptr);   // captured: hover only over the capture widget
    h.PointerUp(e);
    CHECK_EQ(a->ups, 1);
    CHECK(!a->IsPressed());
    CHECK(h.Captured() == nullptr);
}

TEST_CASE(UiHost_FocusOrderAndTab) {
    UiHost h;
    h.SetSize({400, 300});
    auto* root = h.SetRoot(std::make_unique<Widget>(Role::Pane));
    auto* a = root->Add<Box>();
    auto* grp = root->Add<Widget>(Role::Group);
    auto* b = grp->Add<Box>();
    auto* c = grp->Add<Box>(false);
    auto* d = root->Add<Box>();
    (void)c;
    d->SetEnabled(false);
    auto order = h.FocusOrder();
    REQUIRE(order.size() == 2);
    CHECK(order[0] == a);
    CHECK(order[1] == b);
    CHECK(h.KeyDown(key::Tab, 0));
    CHECK(h.Focused() == a);
    CHECK(h.FocusVisible());
    h.KeyDown(key::Tab, 0);
    CHECK(h.Focused() == b);
    h.KeyDown(key::Tab, 0);
    CHECK(h.Focused() == a);
    h.KeyDown(key::Tab, ModShift);
    CHECK(h.Focused() == b);
    CHECK(h.KeyDown(key::Enter, 0));
    CHECK_EQ(b->keys, 1);
    grp->SetVisible(false);
    CHECK(h.Focused() == nullptr);
}

TEST_CASE(UiHost_PopupLightDismissAndFocusRestore) {
    UiHost h;
    h.SetSize({400, 300});
    auto* root = h.SetRoot(std::make_unique<Widget>(Role::Pane));
    auto* a = root->Add<Box>();
    a->SetBounds({0, 0, 50, 50});
    h.SetFocus(a, true);
    auto pop = std::make_unique<Widget>(Role::Menu);
    pop->SetBounds({100, 100, 200, 200});
    auto* item = pop->Add<Box>();
    item->SetBounds({100, 100, 200, 130});
    bool closed = false;
    PopupOptions o; o.onClosed = [&] { closed = true; };
    Widget* p = h.OpenPopup(std::move(pop), o);
    CHECK(h.IsPopupOpen(p));
    CHECK(h.Focused() == item);
    auto order = h.FocusOrder();
    CHECK(order.size() == 1);
    PointerEvent e; e.pt = {10, 10};
    CHECK(h.PointerDown(e));
    CHECK(closed);
    CHECK(h.PopupCount() == 0);
    CHECK(h.Focused() == a);
    CHECK_EQ(a->downs, 0);
    p = h.OpenPopup(std::make_unique<Box>(), {});
    CHECK(h.PopupCount() == 1);
    CHECK(h.KeyDown(key::Escape, 0));
    CHECK(h.PopupCount() == 0);
}

TEST_CASE(UiHost_ModalBlocksAndPaintsSmoke) {
    UiHost h;
    h.SetSize({400, 300});
    auto* root = h.SetRoot(std::make_unique<Widget>(Role::Pane));
    auto* a = root->Add<Box>();
    a->SetBounds({0, 0, 50, 50});
    PopupOptions o; o.modal = true; o.lightDismiss = false;
    auto dlg = std::make_unique<Widget>(Role::Window);
    dlg->SetBounds({100, 100, 300, 200});
    h.OpenPopup(std::move(dlg), o);
    PointerEvent e; e.pt = {10, 10};
    CHECK(h.PointerDown(e));
    CHECK_EQ(a->downs, 0);
    CHECK(h.PopupCount() == 1);
    CHECK(!h.KeyDown(key::Escape, 0));   // modal: the dialog decides
    RecordingRenderer r;
    h.Paint(r);
    bool smoke = false;
    for (auto& c : r.calls) if (c.op == RecordingRenderer::Op::FillRect && c.color == h.GetTheme().smokeFill) smoke = true;
    CHECK(smoke);
}

TEST_CASE(UiHost_ToolTipDelay) {
    UiHost h;
    g_now = 1000;
    h.SetClock([] { return g_now; });
    h.SetSize({400, 300});
    auto* root = h.SetRoot(std::make_unique<Widget>(Role::Pane));
    auto* a = root->Add<Box>();
    a->SetBounds({0, 0, 50, 50});
    a->SetToolTip(L"Play (Ctrl+P)");
    PointerEvent e; e.pt = {10, 10};
    h.PointerMove(e);
    CHECK(h.NeedsTick());
    g_now += 200; h.Tick();
    CHECK(!h.ToolTipVisible());
    g_now += 400; h.Tick();
    CHECK(h.ToolTipVisible());
    CHECK(h.ToolTipText() == L"Play (Ctrl+P)");
    RecordingRenderer r;
    h.Paint(r);
    CHECK(r.HasText(L"Play (Ctrl+P)"));
    h.PointerDown(e);
    CHECK(!h.ToolTipVisible());
}

TEST_CASE(UiHost_ContextMenuAndRemoval) {
    UiHost h;
    h.SetSize({400, 300});
    auto* root = h.SetRoot(std::make_unique<Widget>(Role::Pane));
    auto* a = root->Add<Box>();
    a->SetBounds({0, 0, 50, 50});
    a->handleDown = false;
    PointerEvent e; e.pt = {10, 10}; e.button = PointerButton::Right;
    h.PointerDown(e);
    CHECK_EQ(a->ctx, 1);
    h.SetFocus(a, true);
    CHECK(h.KeyDown(key::F10, ModShift));
    CHECK_EQ(a->ctx, 2);
    int events = 0;
    h.AddListener([&](Widget*, UiEvent ev) { if (ev == UiEvent::StructureChanged) events++; });
    auto life = a->LifeToken();
    auto owned = root->RemoveChild(a);
    CHECK(h.Focused() == nullptr);
    CHECK(events == 1);
    owned.reset();
    CHECK(life.expired());
}
