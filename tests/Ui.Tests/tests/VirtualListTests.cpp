// No window; synthetic events through UiHost and RecordingRenderer.
#include <set>

#include "pui/VirtualList.h"
#include "wvptest.h"

using namespace pui;

namespace {
uint64_t g_vlNow = 10000;

struct Src : IListSource {
    int n = 0;
    int cur = -1;
    int Count() const override { return n; }
    std::wstring Text(int r, int c) const override {
        return c == 0 ? L"Item " + std::to_wstring(r) : L"col" + std::to_wstring(c) + L"/" + std::to_wstring(r);
    }
    std::wstring Glyph(int r) const override { return r == cur ? L"" : L""; }
    bool IsCurrent(int r) const override { return r == cur; }
};

struct Fx {
    UiHost h;
    Src src;
    VirtualList* list = nullptr;
    explicit Fx(int n, SelectionMode mode = SelectionMode::Single, float height = 300) {
        g_vlNow = 10000;
        src.n = n;
        h.SetClock([] { return g_vlNow; });
        h.SetSize({400, height});
        auto* root = h.SetRoot(std::make_unique<Widget>(Role::Pane));
        list = root->Add<VirtualList>();
        list->SetSelectionMode(mode);
        list->SetBounds({0, 0, 400, height});
        list->SetSource(&src);
    }
    static float Y(int row) { return (float)row * 48 + 24; }
    void Down(float x, float y, unsigned mods = 0, int clicks = 1, PointerButton b = PointerButton::Left) {
        PointerEvent e; e.pt = {x, y}; e.mods = mods; e.clickCount = clicks; e.button = b;
        h.PointerDown(e);
    }
    void Up(float x, float y, PointerButton b = PointerButton::Left) {
        PointerEvent e; e.pt = {x, y}; e.button = b;
        h.PointerUp(e);
    }
    void Move(float x, float y) {
        PointerEvent e; e.pt = {x, y};
        h.PointerMove(e);
    }
    void Click(int row, unsigned mods = 0) { Down(100, Y(row), mods); Up(100, Y(row)); }
    void FocusList() { h.SetFocus(list, true); }
    bool Key(int vk, unsigned mods = 0) { return h.KeyDown(vk, mods); }
};
using V = std::vector<int>;
}


TEST_CASE(VL_LayoutColumns) {
    std::vector<ListColumn> cols(3);
    cols[0].fraction = 2;
    cols[1].fraction = 1;
    cols[2].width = 80;
    auto s = vl::LayoutColumns(cols, 400, 10);
    // remaining = 400 - 80 - 20 = 300 -> 200 / 100
    REQUIRE(s.size() == 3);
    CHECK_EQ(s[0].x, 0.f); CHECK_EQ(s[0].w, 200.f);
    CHECK_EQ(s[1].x, 210.f); CHECK_EQ(s[1].w, 100.f);
    CHECK_EQ(s[2].x, 320.f); CHECK_EQ(s[2].w, 80.f);
    auto one = vl::LayoutColumns({ListColumn{}}, 300, 12);
    CHECK_EQ(one[0].w, 300.f);
    cols[2].width = 500;
    auto big = vl::LayoutColumns(cols, 400, 10);
    CHECK_EQ(big[0].w, 0.f);
    cols[0].minWidth = 50;
    CHECK_EQ(vl::LayoutColumns(cols, 400, 10)[0].w, 50.f);
}

TEST_CASE(VL_InsertionIndexMath) {
    CHECK_EQ(vl::InsertionIndexForY(0, 48, 10), 0);
    CHECK_EQ(vl::InsertionIndexForY(23, 48, 10), 0);
    CHECK_EQ(vl::InsertionIndexForY(24, 48, 10), 1);
    CHECK_EQ(vl::InsertionIndexForY(200, 48, 10), 4);
    CHECK_EQ(vl::InsertionIndexForY(10000, 48, 10), 10);
    CHECK_EQ(vl::InsertionIndexForY(-100, 48, 10), 0);
    CHECK_EQ(vl::InsertionIndexForY(100, 48, 0), 0);
}

TEST_CASE(VL_AutoScrollVelocity) {
    CHECK_EQ(vl::AutoScrollVelocity(150, 0, 300), 0.f);
    CHECK_EQ(vl::AutoScrollVelocity(-50, 0, 300), -900.f);
    CHECK_EQ(vl::AutoScrollVelocity(0, 0, 300), -900.f);
    CHECK_EQ(vl::AutoScrollVelocity(16, 0, 300), -450.f);
    CHECK_EQ(vl::AutoScrollVelocity(350, 0, 300), 900.f);
    CHECK(vl::AutoScrollVelocity(284, 0, 300) > 440.f && vl::AutoScrollVelocity(284, 0, 300) < 460.f);
    CHECK_EQ(vl::AutoScrollVelocity(10, 0, 0), 0.f);
}

TEST_CASE(VL_ReorderMath) {
    CHECK(vl::ReorderOrder(6, {3}, 5) == V({0, 1, 2, 4, 3, 5}));
    CHECK(vl::ReorderOrder(6, {3}, 2) == V({0, 1, 3, 2, 4, 5}));
    CHECK(vl::ReorderOrder(6, {1, 2}, 6) == V({0, 3, 4, 5, 1, 2}));
    CHECK(vl::ReorderOrder(6, {4, 1}, 0) == V({1, 4, 0, 2, 3, 5}));
    CHECK(vl::ReorderOrder(4, {2}, 99) == V({0, 1, 3, 2}));
    CHECK(vl::MovedRowsNewIndices({3}, 5) == V({4}));
    CHECK(vl::MovedRowsNewIndices({3}, 2) == V({2}));
    CHECK(vl::MovedRowsNewIndices({1, 2}, 6) == V({4, 5}));
    CHECK(vl::MovedRowsNewIndices({1, 4}, 3) == V({2, 3}));
    CHECK(vl::IsNoOpReorder(6, {3}, 3));
    CHECK(vl::IsNoOpReorder(6, {3}, 4));
    CHECK(!vl::IsNoOpReorder(6, {3}, 5));
    CHECK(vl::IsNoOpReorder(6, {2, 3}, 2));
    CHECK(vl::IsNoOpReorder(6, {2, 3}, 4));
    CHECK(!vl::IsNoOpReorder(6, {2, 4}, 3));
}

TEST_CASE(VL_RealizedRangeAndPageTarget) {
    int f, l;
    vl::RealizedRange(0, 300, 48, 10000, 2, f, l);
    CHECK_EQ(f, 0); CHECK_EQ(l, 8);
    vl::RealizedRange(480, 288, 48, 10000, 2, f, l);   // rows 10..15 visible
    CHECK_EQ(f, 8); CHECK_EQ(l, 17);
    vl::RealizedRange(0, 300, 48, 3, 2, f, l);
    CHECK_EQ(l, 2);
    vl::RealizedRange(0, 300, 48, 0, 2, f, l);
    CHECK(l < f);
    vl::RealizedRange(0, 0, 48, 5, 2, f, l);
    CHECK(l < f);
    // PageDown: edge of the page first, then a full page
    CHECK_EQ(vl::PageTarget(0, true, 0, 5, 6, 100), 5);
    CHECK_EQ(vl::PageTarget(5, true, 0, 5, 6, 100), 11);
    CHECK_EQ(vl::PageTarget(98, true, 90, 95, 6, 100), 99);
    CHECK_EQ(vl::PageTarget(11, false, 6, 11, 6, 100), 6);
    CHECK_EQ(vl::PageTarget(6, false, 6, 11, 6, 100), 0);
    CHECK_EQ(vl::PageTarget(3, false, 0, 5, 6, 100), 0);
    CHECK_EQ(vl::PageTarget(-1, true, 0, 5, 6, 100), 5);
    CHECK_EQ(vl::PageTarget(0, true, 0, 5, 6, 0), -1);
}


TEST_CASE(VL_VirtualizesTenThousandItems) {
    Fx f(10000);
    auto* L = f.list;
    CHECK_EQ(L->RealizedRowCount(), 9);   // rows 0..6 visible + 2 overscan
    CHECK_EQ(L->Children().size(), (size_t)10);   // + the overlay (scrollbar / drag visuals)
    CHECK(L->GetRole() == Role::List);
    CHECK_EQ(L->MaxOffset(), 10000.f * 48 - 300);
    std::set<Widget*> everSeen;
    int maxRealized = 0;
    for (float off = 0; off < 10000.f * 48; off += 1234.5f) {
        L->SetOffset(off);
        maxRealized = std::max(maxRealized, L->RealizedRowCount());
        for (auto& c : L->Children()) everSeen.insert(c.get());
    }
    L->SetOffset(L->MaxOffset());
    maxRealized = std::max(maxRealized, L->RealizedRowCount());
    CHECK(maxRealized <= 12);   // up to 8 partially visible rows + 2 overscan on each side
    CHECK(L->Children().size() <= 13);
    CHECK(everSeen.size() < 40);   // rows are recycled, not reallocated per scroll step
    CHECK_EQ(L->LastVisibleRow(), 9999);
    CHECK(L->RowWidget(9999) != nullptr);
    CHECK(L->RowWidget(0) == nullptr);
}

TEST_CASE(VL_RecyclingKeepsNameInSync) {
    Fx f(10000);
    auto* L = f.list;
    L->SetOffset(48.f * 5000 + 10);
    CHECK_EQ(L->FirstVisibleRow(), 5000);
    for (auto& c : L->Children()) {
        auto* row = dynamic_cast<VirtualListRow*>(c.get());
        if (!row) continue;
        CHECK(row->GetRole() == Role::ListItem);
        CHECK(row->Name() == L"Item " + std::to_wstring(row->Row()));
        CHECK_EQ(row->Bounds().t, L->RowRect(row->Row()).t);
    }
    CHECK(L->RowWidget(5000) && L->RowWidget(5000)->Name() == L"Item 5000");
    CHECK(L->RowWidget(5006) != nullptr);
    CHECK(L->RowWidget(4998) != nullptr);   // overscan
    CHECK(L->RowWidget(4997) == nullptr);
    Widget* before = L->RowWidget(4998);
    L->ScrollBy(48);
    CHECK(L->RowWidget(4998) == nullptr);
    CHECK(L->RowWidget(5009) == before);
    CHECK(before->Name() == L"Item 5009");
}

TEST_CASE(VL_ChildrenOrderAscendingBeforeOverlay) {
    Fx f(100);
    f.list->SetOffset(48 * 10 + 5);
    int last = -1;
    size_t n = 0;
    for (auto& c : f.list->Children()) {
        auto* row = dynamic_cast<VirtualListRow*>(c.get());
        if (!row) { CHECK(c.get() == f.list->Children().back().get()); continue; }
        CHECK(row->Row() > last);
        last = row->Row();
        n++;
    }
    CHECK_EQ(n, (size_t)f.list->RealizedRowCount());
    CHECK(dynamic_cast<VirtualListRow*>(f.list->Children().back().get()) == nullptr);   // overlay stays last (painted on top)
}

TEST_CASE(VL_StructureChangedOnReRealization) {
    Fx f(1000);
    int events = 0;
    Widget* who = nullptr;
    f.h.AddListener([&](Widget* w, UiEvent ev) { if (ev == UiEvent::StructureChanged) { events++; who = w; } });
    f.list->SetOffset(10);    // still the same realized range
    CHECK_EQ(events, 0);
    f.list->SetOffset(48 * 20);
    CHECK(events > 0);
    CHECK(who == f.list);
    events = 0;
    f.src.n = 3;
    f.list->SourceChanged();
    CHECK(events > 0);
    CHECK_EQ(f.list->RealizedRowCount(), 3);
}

TEST_CASE(VL_RowAtAndRowRect) {
    Fx f(100);
    auto* L = f.list;
    CHECK_EQ(L->RowAt({100, 0}), 0);
    CHECK_EQ(L->RowAt({100, 47.9f}), 0);
    CHECK_EQ(L->RowAt({100, 48}), 1);
    CHECK_EQ(L->RowAt({100, 299}), 6);
    CHECK_EQ(L->RowAt({100, 300}), -1);
    CHECK_EQ(L->RowAt({-1, 100}), -1);
    L->SetOffset(100);
    CHECK_EQ(L->RowAt({100, 0}), 2);   // (0 + 100) / 48
    CHECK_EQ(L->FirstVisibleRow(), 2);
    CHECK_EQ(L->LastVisibleRow(), 8);
    RectF r = L->RowRect(3);
    CHECK_EQ(r.t, 3 * 48.f - 100); CHECK_EQ(r.h(), 48.f); CHECK_EQ(r.l, 0.f); CHECK_EQ(r.r, 400.f);
    CHECK(L->RowRect(-1).Empty());
    CHECK(L->RowRect(100).Empty());
    L->SetHeaderText(L"Playing next");
    L->SetOffset(0);
    CHECK_EQ(L->Viewport().t, 32.f);
    CHECK_EQ(L->RowRect(0).t, 32.f);
    CHECK_EQ(L->RowAt({100, 10}), -1);
    CHECK_EQ(L->RowAt({100, 32}), 0);
    L->SetHeaderText(L"");
    L->SetRowHeight(32);
    CHECK_EQ(L->RowAt({100, 100}), 3);
    CHECK_EQ(L->RealizedRowCount(), 12);   // ceil(300/32)=10 rows + 2 overscan
}

TEST_CASE(VL_OffsetClampAndWheel) {
    Fx f(100);
    auto* L = f.list;
    L->SetOffset(-10);
    CHECK_EQ(L->Offset(), 0.f);
    L->SetOffset(1e9f);
    CHECK_EQ(L->Offset(), 100.f * 48 - 300);
    PointerEvent e;
    e.pt = {100, 100};
    e.wheelDelta = 1;
    CHECK(f.h.PointerWheel(e));
    CHECK_EQ(L->Offset(), 100.f * 48 - 300 - 48);
    L->SetOffset(0);
    e.wheelDelta = -1;
    f.h.PointerWheel(e);
    CHECK_EQ(L->Offset(), 48.f);
    L->SetWheelStep(20);
    f.h.PointerWheel(e);
    CHECK_EQ(L->Offset(), 68.f);
    e.mods = ModCtrl;
    CHECK(!f.h.PointerWheel(e));
    Fx few(3);   // everything fits: nothing to scroll
    e.mods = 0;
    CHECK(!few.h.PointerWheel(e));
    CHECK_EQ(few.list->MaxOffset(), 0.f);
}


TEST_CASE(VL_SingleModeClick) {
    Fx f(20);
    int changes = 0;
    f.list->onSelectionChanged = [&] { changes++; };
    f.Click(2);
    CHECK(f.list->Selection() == V({2}));
    CHECK_EQ(f.list->FocusedRow(), 2);
    CHECK(f.h.Focused() == f.list);
    CHECK(!f.h.FocusVisible());
    f.Click(4, ModCtrl);
    CHECK(f.list->Selection() == V({4}));
    f.Click(4);
    CHECK_EQ(changes, 2);
    CHECK(!f.list->CanSelectMultiple());
    f.list->SetSelection({1, 2, 3});
    CHECK(f.list->Selection() == V({1}));
    CHECK_EQ(changes, 2);
}

TEST_CASE(VL_ExtendedClickModes) {
    Fx f(20, SelectionMode::Extended, 600);
    int changes = 0;
    f.list->onSelectionChanged = [&] { changes++; };
    f.Click(2);
    CHECK(f.list->Selection() == V({2}));
    f.Click(5, ModCtrl);
    CHECK(f.list->Selection() == V({2, 5}));
    f.Click(2, ModCtrl);
    CHECK(f.list->Selection() == V({5}));
    f.Click(1, ModShift);  // anchor was 2 (last Ctrl+click) -> range 1..2
    CHECK(f.list->Selection() == V({1, 2}));
    f.Click(4, ModShift);  // anchor stays 2
    CHECK(f.list->Selection() == V({2, 3, 4}));
    f.Click(8, ModShift | ModCtrl);
    CHECK(f.list->Selection() == V({2, 3, 4, 5, 6, 7, 8}));
    f.Click(0);
    CHECK(f.list->Selection() == V({0}));
    CHECK(changes >= 6);
    CHECK(f.list->CanSelectMultiple());
}

TEST_CASE(VL_PlainClickOnSelectedRowCollapsesOnRelease) {
    Fx f(20, SelectionMode::Extended);
    f.Click(1);
    f.Click(3, ModCtrl);
    f.Click(5, ModCtrl);
    CHECK(f.list->Selection() == V({1, 3, 5}));
    f.Down(100, Fx::Y(3));
    CHECK(f.list->Selection() == V({1, 3, 5}));   // kept while pressed (a drag may start)
    f.Up(100, Fx::Y(3));
    CHECK(f.list->Selection() == V({3}));
}

TEST_CASE(VL_ExtendedKeyboardSelection) {
    Fx f(20, SelectionMode::Extended);
    f.FocusList();
    CHECK_EQ(f.list->FocusedRow(), 0);
    CHECK(f.list->Selection().empty());
    CHECK(f.Key(key::Down, ModShift));
    CHECK(f.list->Selection() == V({0, 1}));
    f.Key(key::Down, ModShift);
    CHECK(f.list->Selection() == V({0, 1, 2}));
    f.Key(key::Up, ModShift);
    CHECK(f.list->Selection() == V({0, 1}));
    CHECK_EQ(f.list->FocusedRow(), 1);
    f.Key(key::Down);
    CHECK(f.list->Selection() == V({2}));
    f.Key(key::Down, ModCtrl);
    f.Key(key::Down, ModCtrl);
    CHECK_EQ(f.list->FocusedRow(), 4);
    CHECK(f.list->Selection() == V({2}));
    CHECK(f.Key(key::Space));
    CHECK(f.list->Selection() == V({2, 4}));
    f.Key(key::Space);
    CHECK(f.list->Selection() == V({2}));
    CHECK(f.Key(key::A, ModCtrl));
    CHECK_EQ(f.list->Selection().size(), (size_t)20);
    f.Click(5);
    f.FocusList();
    f.Key(key::End, ModShift);
    CHECK_EQ(f.list->Selection().size(), (size_t)15);
    CHECK_EQ(f.list->FocusedRow(), 19);
    f.Key(key::Home, ModShift);
    CHECK(f.list->Selection() == V({0, 1, 2, 3, 4, 5}));
}

TEST_CASE(VL_SingleModeKeyboard) {
    Fx f(20);
    f.FocusList();
    f.Key(key::Down);
    CHECK(f.list->Selection() == V({1}));
    f.Key(key::Down, ModShift);
    CHECK(f.list->Selection() == V({2}));
    CHECK(!f.Key(key::A, ModCtrl));
    f.Key(key::Space);
    CHECK(f.list->Selection() == V({2}));
}

TEST_CASE(VL_KeyboardNavigationKeepsFocusRowVisible) {
    Fx f(100);
    f.FocusList();
    CHECK(f.h.FocusVisible());
    for (int i = 0; i < 7; i++) f.Key(key::Down);
    CHECK_EQ(f.list->FocusedRow(), 7);
    CHECK_EQ(f.list->Offset(), 84.f);   // row 7 bottom (384) aligned with the viewport bottom (300)
    CHECK(f.list->Selection() == V({7}));
    f.Key(key::Up);
    CHECK_EQ(f.list->FocusedRow(), 6);
    CHECK_EQ(f.list->Offset(), 84.f);
    f.Key(key::End);
    CHECK_EQ(f.list->FocusedRow(), 99);
    CHECK_EQ(f.list->Offset(), 100.f * 48 - 300);
    f.Key(key::Down);
    CHECK_EQ(f.list->FocusedRow(), 99);
    f.Key(key::Home);
    CHECK_EQ(f.list->FocusedRow(), 0);
    CHECK_EQ(f.list->Offset(), 0.f);
    f.list->ScrollIntoViewRow(50);
    CHECK_EQ(f.list->Offset(), 50 * 48.f + 48 - 300);
    f.list->ScrollIntoViewRow(10);
    CHECK_EQ(f.list->Offset(), 480.f);
    f.list->SetFocusedRow(60);
    CHECK_EQ(f.list->FocusedRow(), 60);
    CHECK(f.list->Selection() == V({0}));
    CHECK(f.list->FirstVisibleRow() <= 60 && f.list->LastVisibleRow() >= 60);
}

TEST_CASE(VL_PageUpPageDown) {
    Fx f(100);
    f.FocusList();
    f.Key(key::PageDown);
    CHECK_EQ(f.list->FocusedRow(), 5);   // last fully visible row
    f.Key(key::PageDown);
    CHECK_EQ(f.list->FocusedRow(), 11);
    CHECK_EQ(f.list->Offset(), 12 * 48.f - 300);
    f.Key(key::PageUp);
    CHECK_EQ(f.list->FocusedRow(), 6);   // first fully visible row
    f.Key(key::PageUp);
    CHECK_EQ(f.list->FocusedRow(), 0);
    CHECK_EQ(f.list->Offset(), 0.f);
}

TEST_CASE(VL_InvokeDeleteAndKeys) {
    Fx f(10, SelectionMode::Extended, 600);
    int invoked = -1, calls = 0;
    std::vector<int> deleted;
    f.list->onInvoke = [&](int r) { invoked = r; calls++; };
    f.list->onDelete = [&](std::vector<int> rows) { deleted = rows; };
    f.FocusList();
    CHECK(!f.Key(key::Delete));
    f.Click(3);
    f.Click(5, ModCtrl);
    CHECK(f.Key(key::Delete));
    CHECK(deleted == V({3, 5}));
    CHECK(f.Key(key::Enter));
    CHECK_EQ(invoked, 5);
    CHECK_EQ(calls, 1);
    f.Down(100, Fx::Y(7), 0, 2);
    f.Up(100, Fx::Y(7));
    CHECK_EQ(invoked, 7);
    CHECK_EQ(calls, 2);
    auto* row = f.list->RowWidget(2);
    REQUIRE(row != nullptr);
    CHECK(row->SupportsInvoke());
    row->Invoke();
    CHECK_EQ(invoked, 2);
    f.list->onDelete = nullptr;
    CHECK(!f.Key(key::Delete));
}

TEST_CASE(VL_ContextMenu) {
    Fx f(10, SelectionMode::Extended, 600);
    int calls = 0, row = -2;
    bool kb = true;
    PointF pt{};
    f.list->onContextMenu = [&](int r, PointF p, bool k) { calls++; row = r; pt = p; kb = k; };
    f.Click(1);
    f.Click(2, ModCtrl);
    f.Down(100, Fx::Y(6), 0, 1, PointerButton::Right);
    f.Up(100, Fx::Y(6), PointerButton::Right);
    CHECK_EQ(calls, 1);
    CHECK_EQ(row, 6);
    CHECK(!kb);
    CHECK_EQ(pt.y, Fx::Y(6));
    CHECK(f.list->Selection() == V({6}));
    f.Click(1);
    f.Click(2, ModCtrl);
    f.Down(100, Fx::Y(2), 0, 1, PointerButton::Right);
    f.Up(100, Fx::Y(2), PointerButton::Right);
    CHECK_EQ(calls, 2);
    CHECK_EQ(row, 2);
    CHECK(f.list->Selection() == V({1, 2}));
    // Use a short list to provide empty space below the rows.
    Fx g(2);
    int grow = -2;
    g.list->onContextMenu = [&](int r, PointF, bool) { grow = r; };
    g.Down(100, 250, 0, 1, PointerButton::Right);
    g.Up(100, 250, PointerButton::Right);
    CHECK_EQ(grow, -1);
    f.FocusList();
    f.list->SetFocusedRow(3);
    CHECK(f.Key(key::F10, ModShift));
    CHECK_EQ(calls, 3);
    CHECK_EQ(row, 3);
    CHECK(kb);
    CHECK_EQ(pt.y, Fx::Y(3));
    CHECK(f.Key(key::Apps));
    CHECK_EQ(calls, 4);
}


TEST_CASE(VL_MouseDragReorder) {
    Fx f(10);
    f.list->SetReorderEnabled(true);
    std::vector<int> rows;
    int before = -1, calls = 0;
    f.list->onReorder = [&](std::vector<int> r, int b) { rows = r; before = b; calls++; };
    f.Down(100, Fx::Y(1));
    f.Move(100, Fx::Y(1) + 3);   // below the 4 DIP threshold
    CHECK(!f.list->IsDragging());
    f.Move(100, 200);
    CHECK(f.list->IsDragging());
    CHECK_EQ(f.list->DropIndex(), 4);
    f.Move(100, 80);
    CHECK_EQ(f.list->DropIndex(), 2);
    f.Move(100, 200);
    RecordingRenderer r;
    f.h.Paint(r);
    bool line = false;
    for (auto& c : r.calls)
        if (c.op == RecordingRenderer::Op::Line && c.color == f.h.GetTheme().accent && c.value == 2.f && c.rect.t == 4 * 48.f) line = true;
    CHECK(line);
    CHECK(r.Count(RecordingRenderer::Op::PushOpacity) >= 1);
    f.Up(100, 200);
    CHECK(!f.list->IsDragging());
    CHECK_EQ(calls, 1);
    CHECK(rows == V({1}));
    CHECK_EQ(before, 4);
    CHECK(f.list->Selection() == V({3}));
    CHECK_EQ(f.list->FocusedRow(), 3);
}

TEST_CASE(VL_DragNoOpAndDisabled) {
    Fx f(10);
    int calls = 0;
    f.list->onReorder = [&](std::vector<int>, int) { calls++; };
    f.Down(100, Fx::Y(1));
    f.Move(100, 200);
    CHECK(!f.list->IsDragging());
    f.Up(100, 200);
    CHECK_EQ(calls, 0);
    f.list->SetReorderEnabled(true);
    f.Down(100, Fx::Y(1));
    f.Move(100, Fx::Y(1) + 20);
    CHECK(f.list->IsDragging());
    CHECK_EQ(f.list->DropIndex(), 2);   // row 1 dropped before row 2 = where it already is
    f.Up(100, Fx::Y(1) + 20);
    CHECK_EQ(calls, 0);
    Fx g(10, SelectionMode::Extended);
    g.list->SetReorderEnabled(true);
    g.Click(1);
    g.Click(1, ModCtrl);
    g.Down(100, Fx::Y(1), ModCtrl);
    g.Up(100, Fx::Y(1));
    CHECK(g.list->Selection() == V({1}));
}

TEST_CASE(VL_MultiRowDragAndEscapeCancel) {
    Fx f(10, SelectionMode::Extended);
    f.list->SetReorderEnabled(true);
    std::vector<int> rows;
    int before = -1, calls = 0;
    f.list->onReorder = [&](std::vector<int> r, int b) { rows = r; before = b; calls++; };
    f.Click(1);
    f.Click(2, ModCtrl);
    f.Down(100, Fx::Y(2));
    CHECK(f.list->Selection() == V({1, 2}));
    f.Move(100, 250);
    CHECK(f.list->IsDragging());
    CHECK_EQ(f.list->DropIndex(), 5);
    CHECK(f.Key(key::Escape));
    CHECK(!f.list->IsDragging());
    CHECK_EQ(f.list->DropIndex(), -1);
    f.Up(100, 250);
    CHECK_EQ(calls, 0);
    CHECK(f.list->Selection() == V({1, 2}));
    f.Down(100, Fx::Y(1));
    f.Move(100, 330);
    f.Up(100, 330);
    CHECK_EQ(calls, 1);
    CHECK(rows == V({1, 2}));
    CHECK_EQ(before, 7);
    CHECK(f.list->Selection() == V({5, 6}));
}

TEST_CASE(VL_DragAutoScrollNearEdges) {
    Fx f(100);
    f.list->SetReorderEnabled(true);
    f.Down(100, Fx::Y(1));
    f.Move(100, 295);   // 27 DIP into the 32 DIP bottom zone
    CHECK(f.list->IsDragging());
    CHECK(f.h.NeedsTick());
    int idx0 = f.list->DropIndex();
    float off0 = f.list->Offset();
    g_vlNow += 100;
    f.h.Tick();
    float off1 = f.list->Offset();
    // velocity = 900 * (295-268)/32 = 759 DIP/s -> ~76 DIP in 100 ms
    CHECK(off1 > off0 + 70 && off1 < off0 + 80);
    CHECK(f.list->DropIndex() >= idx0);
    g_vlNow += 100;
    f.h.Tick();
    CHECK(f.list->Offset() > off1 + 70);
    CHECK(f.list->DropIndex() > idx0);
    // pointer in the middle: no more scrolling, but ticks continue while dragging
    f.Move(100, 150);
    float off2 = f.list->Offset();
    g_vlNow += 100;
    CHECK(f.h.Tick());
    CHECK_EQ(f.list->Offset(), off2);
    f.Move(100, 2);
    g_vlNow += 100;
    f.h.Tick();
    CHECK(f.list->Offset() < off2);
    for (int i = 0; i < 30; i++) { g_vlNow += 100; f.h.Tick(); }
    CHECK_EQ(f.list->Offset(), 0.f);
    CHECK_EQ(f.list->DropIndex(), 0);
    f.Key(key::Escape);
    f.Up(100, 2);
}

TEST_CASE(VL_AltArrowKeyboardReorder) {
    Fx f(10, SelectionMode::Extended);
    std::vector<int> rows;
    int before = -1, calls = 0;
    f.list->onReorder = [&](std::vector<int> r, int b) { rows = r; before = b; calls++; };
    f.FocusList();
    f.Click(3);
    f.FocusList();
    CHECK(!f.Key(key::Down, ModAlt));
    f.list->SetReorderEnabled(true);
    CHECK(f.Key(key::Down, ModAlt));
    CHECK_EQ(calls, 1);
    CHECK(rows == V({3}));
    CHECK_EQ(before, 5);
    CHECK(f.list->Selection() == V({4}));
    CHECK_EQ(f.list->FocusedRow(), 4);
    CHECK(f.Key(key::Up, ModAlt));
    CHECK(rows == V({4}));
    CHECK_EQ(before, 3);
    CHECK(f.list->Selection() == V({3}));
    f.Click(4, ModCtrl);
    f.Key(key::Up, ModAlt);
    CHECK(rows == V({3, 4}));
    CHECK_EQ(before, 2);
    CHECK(f.list->Selection() == V({2, 3}));
    f.list->SetSelection({0});
    calls = 0;
    CHECK(f.Key(key::Up, ModAlt));
    CHECK_EQ(calls, 0);
    f.list->SetSelection({9});
    CHECK(f.Key(key::Down, ModAlt));
    CHECK_EQ(calls, 0);
    f.list->SetSelection({8});
    f.Key(key::Down, ModAlt);
    CHECK_EQ(before, 10);
    CHECK(f.list->Selection() == V({9}));
}


TEST_CASE(VL_SourceChangedClampsAndReRealizes) {
    Fx f(100, SelectionMode::Extended);
    auto* L = f.list;
    L->SetSelection({3, 40, 90});
    L->SetFocusedRow(90);
    CHECK_EQ(L->Offset(), 91 * 48.f - 300);
    f.src.n = 50;
    L->SourceChanged();
    CHECK(L->Selection() == V({3, 40}));
    CHECK_EQ(L->FocusedRow(), 49);
    CHECK_EQ(L->Offset(), 50 * 48.f - 300);
    CHECK_EQ(L->RealizedRowCount(), 9);
    f.src.n = 2;
    L->SourceChanged();
    CHECK(L->Selection() == V({}));
    CHECK_EQ(L->FocusedRow(), 1);
    CHECK_EQ(L->Offset(), 0.f);
    CHECK_EQ(L->RealizedRowCount(), 2);
    f.src.n = 0;
    L->SourceChanged();
    CHECK_EQ(L->FocusedRow(), -1);
    CHECK_EQ(L->RealizedRowCount(), 0);
    CHECK_EQ(L->FirstVisibleRow(), -1);
    f.src.n = 20;
    L->SourceChanged();
    CHECK_EQ(L->RealizedRowCount(), 9);
    CHECK(L->RowWidget(0)->Name() == L"Item 0");
    L->SetSelection({15});
    int selEvents = 0;
    f.h.AddListener([&](Widget*, UiEvent ev) { if (ev == UiEvent::SelectionChanged) selEvents++; });
    f.src.n = 10;
    L->SourceChanged();
    CHECK(selEvents >= 1);
    CHECK(L->Selection().empty());
}

TEST_CASE(VL_NameChangedWhenSourceTextChanges) {
    struct Dyn : IListSource {
        std::wstring t = L"a";
        int Count() const override { return 3; }
        std::wstring Text(int r, int) const override { return t + std::to_wstring(r); }
    } dyn;
    UiHost h;
    h.SetSize({400, 300});
    auto* root = h.SetRoot(std::make_unique<Widget>(Role::Pane));
    auto* L = root->Add<VirtualList>();
    L->SetBounds({0, 0, 400, 300});
    L->SetSource(&dyn);
    int names = 0;
    h.AddListener([&](Widget*, UiEvent ev) { if (ev == UiEvent::NameChanged) names++; });
    dyn.t = L"b";
    L->SourceChanged();
    CHECK_EQ(names, 3);
    CHECK(L->RowWidget(1)->Name() == L"b1");
}


TEST_CASE(VL_ScrollbarThumbGeometryAndDrag) {
    Fx f(100);
    auto* L = f.list;
    const auto& sb = L->ScrollBar();
    CHECK(sb.Needed());
    RectF th = sb.ThumbRect();
    CHECK_EQ(th.t, 12.f);                       // below the up arrow
    CHECK_EQ(th.h(), ScrollBarController::kMinThumb);   // 276 * 300/4800 = 17 -> minimum 24
    CHECK_EQ(th.r, 398.f);
    L->SetOffset(L->MaxOffset());
    CHECK(std::fabs(sb.ThumbRect().b - 288.f) < 0.01f);
    L->SetOffset(0);
    // hit-testing: the right 12 DIP belong to the list, not to a row
    CHECK(f.h.HitTest({395, 100}) == L);
    CHECK(f.h.HitTest({300, 100}) == L->RowWidget(2));
    // drag the thumb by half of its travel (252): offset = half of max
    f.Down(395, 20);
    CHECK(f.h.Captured() == L);
    CHECK(sb.Dragging());
    f.Move(395, 20 + 126);
    CHECK(std::fabs(L->Offset() - L->MaxOffset() * 0.5f) < 1.f);
    f.Move(395, 5000);
    CHECK_EQ(L->Offset(), L->MaxOffset());
    f.Up(395, 5000);
    CHECK(!sb.Dragging());
    CHECK(L->Selection().empty());
    L->SetOffset(0);
    f.Down(395, 200);
    f.Up(395, 200);
    CHECK_EQ(L->Offset(), 300.f);
    f.Down(395, 295);
    f.Up(395, 295);
    CHECK_EQ(L->Offset(), 324.f);
    f.Down(395, 5);
    f.Up(395, 5);
    CHECK_EQ(L->Offset(), 300.f);
    Fx g(3);
    CHECK(!g.list->ScrollBar().Needed());
    CHECK(g.h.HitTest({395, 24}) == g.list->RowWidget(0));
}

TEST_CASE(VL_ScrollbarFadesAfterScroll) {
    Fx f(100);
    auto* L = f.list;
    CHECK_EQ(L->ScrollBar().Opacity(g_vlNow), 0.f);
    PointerEvent e;
    e.pt = {100, 100};
    e.wheelDelta = -1;
    f.h.PointerWheel(e);
    CHECK_EQ(L->ScrollBar().Opacity(g_vlNow), 1.f);
    CHECK(f.h.NeedsTick());
    g_vlNow += 2000;
    CHECK(!f.h.Tick());
    CHECK_EQ(L->ScrollBar().Opacity(g_vlNow), 0.f);
    RecordingRenderer r;
    f.h.Paint(r);
    bool thumb = false;
    for (auto& c : r.calls)
        if (c.op == RecordingRenderer::Op::FillRoundRect && c.rect.w() == 2.f) thumb = true;
    CHECK(!thumb);
}


TEST_CASE(VL_AutomationPatterns) {
    Fx f(100, SelectionMode::Extended);
    auto* L = f.list;
    CHECK(L->GetRole() == Role::List);
    CHECK(L->SupportsSelection());
    CHECK(L->CanSelectMultiple());
    CHECK(!L->IsSelectionRequired());
    CHECK(L->Focusable());
    f.Click(2);
    f.Click(4, ModCtrl);
    auto sel = L->SelectedChildren();
    REQUIRE(sel.size() == 2);
    CHECK(sel[0]->Name() == L"Item 2");
    CHECK(sel[1]->Name() == L"Item 4");
    auto* row = L->RowWidget(6);
    REQUIRE(row != nullptr);
    CHECK(row->GetRole() == Role::ListItem);
    CHECK(row->SupportsSelectionItem());
    CHECK(!row->IsSelected());
    CHECK(row->SelectionContainer() == L);
    int selEvents = 0;
    f.h.AddListener([&](Widget* w, UiEvent ev) { if (ev == UiEvent::SelectionChanged && w == row) selEvents++; });
    row->AddToSelection();
    CHECK(row->IsSelected());
    CHECK(L->Selection() == V({2, 4, 6}));
    CHECK_EQ(selEvents, 1);
    row->RemoveFromSelection();
    CHECK(L->Selection() == V({2, 4}));
    row->SelectItem();
    CHECK(L->Selection() == V({6}));
    CHECK_EQ(L->FocusedRow(), 6);
    CHECK(L->SupportsScroll());
    CHECK_EQ(L->VerticalScrollPercent(), 0.0);
    CHECK(std::fabs(L->VerticalViewSize() - 300.0 / 4800.0 * 100.0) < 1e-9);
    L->SetVerticalScrollPercent(50);
    CHECK(std::fabs(L->Offset() - L->MaxOffset() * 0.5f) < 0.5f);
    CHECK(std::fabs(L->VerticalScrollPercent() - 50.0) < 0.1);
    L->SetVerticalScrollPercent(100);
    CHECK_EQ(L->Offset(), L->MaxOffset());
    L->SetOffset(0);
    L->ScrollIntoView(L->RowWidget(8));   // partially below the viewport (overscan row)
    CHECK(L->Offset() > 0.f);
    Fx few(2);
    CHECK_EQ(few.list->VerticalScrollPercent(), -1.0);
    CHECK_EQ(few.list->VerticalViewSize(), 100.0);
    Fx one(10);
    one.list->RowWidget(1)->SelectItem();
    one.list->RowWidget(3)->AddToSelection();
    CHECK(one.list->Selection() == V({3}));
    CHECK(!one.list->CanSelectMultiple());
}


TEST_CASE(VL_PaintStatesAndRoles) {
    Fx f(10, SelectionMode::Extended);
    f.src.cur = 4;
    f.list->SetShowGlyphs(true);
    f.list->SetColumns({{L"Title", 0, 1.f, false}, {L"Artist", 0, 0.5f, false}, {L"Time", 60, 0, true}});
    f.Click(2);
    f.Move(100, Fx::Y(5));
    const Theme& th = f.h.GetTheme();
    RecordingRenderer r;
    f.h.Paint(r);
    using Op = RecordingRenderer::Op;
    bool pill = false, hover = false, selFill = false;
    for (auto& c : r.calls) {
        if (c.op != Op::FillRoundRect) continue;
        if (c.color == th.accent && std::fabs(c.rect.w() - 3) < 0.01f && std::fabs(c.rect.h() - 16) < 0.01f) pill = true;
        if (c.color == th.subtleSecondary && c.rect.t == 2 * 48.f + 2) selFill = true;
        if (c.color == th.subtleSecondary && c.rect.t == 5 * 48.f + 2) hover = true;
        if (c.color == th.subtleSecondary) CHECK_EQ(c.value, 4.f);
    }
    CHECK(pill);
    CHECK(selFill);
    CHECK(hover);
    bool curBold = false, glyph = false, right = false, second = false;
    for (auto& c : r.calls) {
        if (c.op != Op::Text) continue;
        if (c.text == L"Item 4" && c.style.weight == 600 && c.color == th.accent) curBold = true;
        if (c.text == L"" && c.style.font == FontKind::Icon) glyph = true;
        if (c.text == L"col2/3" && c.style.align == TextAlign::Trailing) right = true;
        if (c.text == L"col1/3") second = c.color == th.textSecondary;
    }
    CHECK(curBold);
    CHECK(glyph);
    CHECK(right);
    CHECK(second);
    CHECK(r.Count(Op::PushClip) >= 7);   // the 7 rows intersecting the viewport
    int strokes0 = r.Count(Op::StrokeRoundRect);
    f.FocusList();
    f.Key(key::Down);
    r.Clear();
    f.h.Paint(r);
    CHECK(r.Count(Op::StrokeRoundRect) >= strokes0 + 2);
}

TEST_CASE(VL_PaintEmptyStateAndHeader) {
    Fx f(0);
    f.list->SetEmptyText(L"Nothing queued");
    f.list->SetHeaderText(L"Play queue");
    RecordingRenderer r;
    f.h.Paint(r);
    CHECK(r.HasText(L"Nothing queued"));
    CHECK(r.HasText(L"Play queue"));
    CHECK_EQ(f.list->RealizedRowCount(), 0);
    f.src.n = 1;
    f.list->SourceChanged();
    r.Clear();
    f.h.Paint(r);
    CHECK(!r.HasText(L"Nothing queued"));
    CHECK(r.HasText(L"Item 0"));
}

TEST_CASE(VL_LiveSourceSwap) {
    Fx f(5);
    Src other;
    other.n = 2;
    f.list->SetSelection({4});
    f.list->SetSource(&other);
    CHECK(f.list->Selection().empty());
    CHECK_EQ(f.list->RealizedRowCount(), 2);
    f.list->SetSource(nullptr);
    CHECK_EQ(f.list->RealizedRowCount(), 0);
    CHECK_EQ(f.list->RowAt({100, 10}), -1);
}
