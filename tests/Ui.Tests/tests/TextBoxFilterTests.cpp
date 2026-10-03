#include "pui/TextBox.h"
#include "pui/VirtualList.h"
#include "wvptest.h"

using namespace pui;

namespace {
struct Src : IListSource {
    int n = 10;
    int Count() const override { return n; }
    std::wstring Text(int r, int) const override { return L"Item " + std::to_wstring(r); }
};

struct Fx {
    UiHost h;
    Src src;
    VirtualList* list = nullptr;
    Fx() {
        h.SetSize({400, 300});
        auto* root = h.SetRoot(std::make_unique<Widget>(Role::Pane));
        list = root->Add<VirtualList>();
        list->SetSelectionMode(SelectionMode::Extended);
        list->SetRowHeight(42);
        list->SetBounds({0, 0, 400, 300});
        list->SetSource(&src);
    }
    void Click(float y) {
        PointerEvent e; e.pt = {100, y};
        h.PointerDown(e);
        h.PointerUp(e);
    }
};
}

TEST_CASE(VL_RowPainterAndFilterReportSourceRows) {
    Fx f;
    int painted = 0, selectedPainted = 0;
    f.list->SetRowPainter([&](IRenderer& r, const Theme& t, const RowPaintArgs& a) {
        painted++;
        if (a.selected) selectedPainted++;
        r.Text(L"row" + std::to_wstring(a.row), a.rect, TextStyle{}, t.textPrimary);
    });
    f.Click(21);
    RecordingRenderer rr;
    f.h.Paint(rr);
    CHECK(painted > 0);
    CHECK_EQ(selectedPainted, 1);
    CHECK(rr.HasText(L"row0"));
    CHECK(!rr.HasText(L"Item 0"));   // default look not used

    f.list->SetFilter([](int s) { return s % 2 == 1; });   // 1,3,5,7,9
    CHECK_EQ(f.list->ViewCount(), 5);
    std::vector<int> invoked;
    f.list->onInvoke = [&](int r) { invoked.push_back(r); };
    f.Click(42 + 21);   // view row 1 -> source 3
    CHECK(f.list->Selection() == std::vector<int>{3});
    f.list->RowInvoke(2);
    CHECK(invoked == std::vector<int>{5});
    f.list->SetReorderEnabled(true);
    CHECK(f.list->IsFiltered());
    f.list->SetFilter(nullptr);
    CHECK_EQ(f.list->ViewCount(), 10);
    CHECK(f.list->Selection() == std::vector<int>{3});
}

TEST_CASE(TextBox_InsertBackspaceSelect) {
    UiHost h;
    h.SetSize({300, 100});
    auto* root = h.SetRoot(std::make_unique<Widget>(Role::Pane));
    auto* tb = root->Add<TextBox>();
    tb->SetBounds({0, 0, 200, 32});
    std::wstring last;
    tb->onChanged = [&](const std::wstring& s) { last = s; };
    h.SetFocus(tb, true);
    h.Char(L'a'); h.Char(L'b'); h.Char(L'c');
    CHECK(tb->Text() == L"abc");
    h.KeyDown(key::Back, 0);
    CHECK(tb->Text() == L"ab");
    h.KeyDown(key::Left, 0);
    h.Char(L'X');
    CHECK(tb->Text() == L"aXb");
    CHECK(last == L"aXb");
    h.KeyDown(key::A, ModCtrl);
    h.Char(L'z');
    CHECK(tb->Text() == L"z");
    h.KeyDown(key::Escape, 0);
    CHECK(tb->Text().empty());
    RecordingRenderer rr;
    tb->Paint(rr, h.GetTheme());
    CHECK(TextBox::IsTextInputFocused(h));
}
