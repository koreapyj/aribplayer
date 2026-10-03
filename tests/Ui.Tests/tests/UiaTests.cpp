// UIA provider layer tests (headless: no window, providers are called directly; no UIA client is used).
#include <wrl/client.h>

#include <string>
#include <vector>

#include "pui/Uia.h"
#include "wvptest.h"

using namespace pui;
using Microsoft::WRL::ComPtr;

namespace {

struct ComInit {
    ComInit() { CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); }
};
ComInit g_comInit;

struct Probe : Widget {
    explicit Probe(Role r = Role::Custom) : Widget(r) {}
    bool inv = false, tog = false, rng = false, val = false, ec = false, sel = false, selItem = false, scr = false;
    int invokes = 0, toggles = 0, toggleState = 0;
    RangeInfo ri;
    double lastSet = -1e9;
    std::wstring value;
    bool valueRO = false, expanded = false, selectedFlag = false, multi = false;
    std::vector<Widget*> selection;
    double scrollPct = 0, viewSize = 25;

    bool SupportsInvoke() const override { return inv; }
    void Invoke() override { invokes++; }
    bool SupportsToggle() const override { return tog; }
    int ToggleState() const override { return toggleState; }
    void Toggle() override { toggles++; toggleState = toggleState == 1 ? 0 : 1; }
    bool SupportsRangeValue() const override { return rng; }
    RangeInfo GetRange() const override { return ri; }
    void SetRangeValue(double v) override { lastSet = v; ri.value = v; }
    bool SupportsValue() const override { return val; }
    std::wstring GetValue() const override { return value; }
    void SetValue(const std::wstring& v) override { value = v; }
    bool ValueReadOnly() const override { return valueRO; }
    bool SupportsExpandCollapse() const override { return ec; }
    bool IsExpanded() const override { return expanded; }
    void Expand() override { expanded = true; }
    void Collapse() override { expanded = false; }
    bool SupportsSelection() const override { return sel; }
    bool CanSelectMultiple() const override { return multi; }
    std::vector<Widget*> SelectedChildren() const override { return selection; }
    bool SupportsSelectionItem() const override { return selItem; }
    bool IsSelected() const override { return selectedFlag; }
    void SelectItem() override { selectedFlag = true; }
    void RemoveFromSelection() override { selectedFlag = false; }
    bool SupportsScroll() const override { return scr; }
    double VerticalScrollPercent() const override { return scrollPct; }
    double VerticalViewSize() const override { return viewSize; }
    void SetVerticalScrollPercent(double p) override { scrollPct = p; }
};

VARIANT Prop(IRawElementProviderSimple* p, PROPERTYID id) {
    VARIANT v;
    VariantInit(&v);
    p->GetPropertyValue(id, &v);
    return v;
}
int PropInt(IRawElementProviderSimple* p, PROPERTYID id) {
    VARIANT v = Prop(p, id);
    int r = v.vt == VT_I4 ? v.lVal : -999;
    VariantClear(&v);
    return r;
}
bool PropBool(IRawElementProviderSimple* p, PROPERTYID id) {
    VARIANT v = Prop(p, id);
    bool r = v.vt == VT_BOOL && v.boolVal != VARIANT_FALSE;
    VariantClear(&v);
    return r;
}
std::wstring PropStr(IRawElementProviderSimple* p, PROPERTYID id) {
    VARIANT v = Prop(p, id);
    std::wstring r = v.vt == VT_BSTR && v.bstrVal ? v.bstrVal : L"<none>";
    VariantClear(&v);
    return r;
}
std::wstring NameOf(IUnknown* u) {
    if (!u) return L"<null>";
    ComPtr<IRawElementProviderSimple> s;
    u->QueryInterface(IID_PPV_ARGS(&s));
    return PropStr(s.Get(), UIA_NamePropertyId);
}
bool Same(IUnknown* a, IUnknown* b) {
    if (!a || !b) return a == b;
    ComPtr<IUnknown> x, y;
    a->QueryInterface(IID_PPV_ARGS(&x));
    b->QueryInterface(IID_PPV_ARGS(&y));
    return x.Get() == y.Get();
}
ComPtr<IRawElementProviderFragment> Frag(IRawElementProviderSimple* s) {
    ComPtr<IRawElementProviderFragment> f;
    if (s) s->QueryInterface(IID_PPV_ARGS(&f));
    return f;
}
ComPtr<IRawElementProviderFragment> Nav(IUnknown* from, NavigateDirection d) {
    ComPtr<IRawElementProviderFragment> f, out;
    from->QueryInterface(IID_PPV_ARGS(&f));
    f->Navigate(d, &out);
    return out;
}
template <class I>
ComPtr<I> Pat(IRawElementProviderSimple* p, PATTERNID id) {
    ComPtr<IUnknown> u;
    p->GetPatternProvider(id, &u);
    ComPtr<I> r;
    if (u) u.As(&r);
    return r;
}

struct Fixture {
    UiHost host;
    Widget* root = nullptr;
    Fixture() {
        host.SetSize({800, 600});
        root = host.SetRoot(std::make_unique<Widget>(Role::Pane));
        root->SetName(L"Main");
    }
};
float g_scale = 1.f;
float ScaleFn() { return g_scale; }

}

TEST_CASE(Uia_ControlTypeMapping) {
    struct { Role r; int ct; } map[] = {
        {Role::Window, UIA_WindowControlTypeId},     {Role::Pane, UIA_PaneControlTypeId},
        {Role::Group, UIA_GroupControlTypeId},       {Role::Text, UIA_TextControlTypeId},
        {Role::Image, UIA_ImageControlTypeId},       {Role::Button, UIA_ButtonControlTypeId},
        {Role::ToggleButton, UIA_ButtonControlTypeId}, {Role::CheckBox, UIA_CheckBoxControlTypeId},
        {Role::RadioButton, UIA_RadioButtonControlTypeId}, {Role::Slider, UIA_SliderControlTypeId},
        {Role::ComboBox, UIA_ComboBoxControlTypeId}, {Role::List, UIA_ListControlTypeId},
        {Role::ListItem, UIA_ListItemControlTypeId}, {Role::Menu, UIA_MenuControlTypeId},
        {Role::MenuItem, UIA_MenuItemControlTypeId}, {Role::ToolTip, UIA_ToolTipControlTypeId},
        {Role::ScrollBar, UIA_ScrollBarControlTypeId}, {Role::Edit, UIA_EditControlTypeId},
        {Role::Tree, UIA_TreeControlTypeId},         {Role::TreeItem, UIA_TreeItemControlTypeId},
        {Role::Tab, UIA_TabControlTypeId},           {Role::TabItem, UIA_TabItemControlTypeId},
        {Role::ProgressBar, UIA_ProgressBarControlTypeId}, {Role::StatusBar, UIA_StatusBarControlTypeId},
        {Role::Hyperlink, UIA_HyperlinkControlTypeId}, {Role::Separator, UIA_SeparatorControlTypeId},
        {Role::Custom, UIA_CustomControlTypeId}};
    CHECK_EQ(sizeof(map) / sizeof(map[0]), (size_t)((int)Role::Custom + 1));
    Fixture f;
    UiaBridge b(nullptr, f.host, ScaleFn);
    for (auto& m : map) {
        CHECK_EQ(UiaControlTypeFor(m.r), m.ct);
        auto* w = f.root->Add<Probe>(m.r);
        auto* p = b.ProviderFor(w);
        REQUIRE(p != nullptr);
        CHECK_EQ(PropInt(p, UIA_ControlTypePropertyId), m.ct);
        CHECK_EQ(PropStr(p, UIA_ClassNamePropertyId), UiaClassNameFor(m.r));
        CHECK_EQ(PropStr(p, UIA_FrameworkIdPropertyId), std::wstring(L"PlayerUi"));
    }
    CHECK_EQ(UiaClassNameFor(Role::Slider), std::wstring(L"PlayerUi.Slider"));
}

TEST_CASE(Uia_Properties) {
    Fixture f;
    UiaBridge b(nullptr, f.host, ScaleFn);
    auto* w = f.root->Add<Probe>(Role::Button);
    w->SetName(L"Play");
    w->SetAutomationId(L"btnPlay");
    w->SetHelpText(L"Starts playback");
    w->SetAcceleratorKey(L"Ctrl+P");
    w->SetFocusable(true);
    w->SetBounds({10, 20, 110, 60});
    auto* p = b.ProviderFor(w);
    CHECK_EQ(PropStr(p, UIA_NamePropertyId), std::wstring(L"Play"));
    CHECK_EQ(PropStr(p, UIA_AutomationIdPropertyId), std::wstring(L"btnPlay"));
    CHECK_EQ(PropStr(p, UIA_HelpTextPropertyId), std::wstring(L"Starts playback"));
    CHECK_EQ(PropStr(p, UIA_AcceleratorKeyPropertyId), std::wstring(L"Ctrl+P"));
    CHECK(PropBool(p, UIA_IsEnabledPropertyId));
    CHECK(PropBool(p, UIA_IsKeyboardFocusablePropertyId));
    CHECK(!PropBool(p, UIA_HasKeyboardFocusPropertyId));
    CHECK(!PropBool(p, UIA_IsOffscreenPropertyId));
    CHECK(PropBool(p, UIA_IsControlElementPropertyId));
    CHECK(PropBool(p, UIA_IsContentElementPropertyId));
    CHECK_EQ(PropInt(p, UIA_ProcessIdPropertyId), (int)GetCurrentProcessId());

    w->Focus();
    CHECK(PropBool(p, UIA_HasKeyboardFocusPropertyId));
    w->SetEnabled(false);
    CHECK(!PropBool(p, UIA_IsEnabledPropertyId));
    CHECK(!PropBool(p, UIA_IsKeyboardFocusablePropertyId));
    w->SetEnabled(true);
    w->SetVisible(false);
    CHECK(PropBool(p, UIA_IsOffscreenPropertyId));
    w->SetVisible(true);
    w->SetBounds({2000, 2000, 2100, 2040});
    CHECK(PropBool(p, UIA_IsOffscreenPropertyId));   // outside the client area

    auto* tb = f.root->Add<Probe>(Role::ToggleButton);
    CHECK_EQ(PropStr(b.ProviderFor(tb), UIA_LocalizedControlTypePropertyId), std::wstring(L"toggle button"));
    auto* pane = f.root->Add<Probe>(Role::Pane);
    CHECK(!PropBool(b.ProviderFor(pane), UIA_IsControlElementPropertyId));
    CHECK(!PropBool(b.ProviderFor(pane), UIA_IsContentElementPropertyId));
    pane->SetName(L"Toolbar");
    CHECK(PropBool(b.ProviderFor(pane), UIA_IsControlElementPropertyId));

    // Bounding rectangle: screen px = origin + DIP * scale.
    g_scale = 2.f;
    b.SetClientOrigin(100, 50);
    w->SetBounds({10, 20, 110, 60});
    UiaRect r{};
    CHECK_EQ((long)Frag(p)->get_BoundingRectangle(&r), (long)S_OK);
    CHECK_EQ(r.left, 120.0);
    CHECK_EQ(r.top, 90.0);
    CHECK_EQ(r.width, 200.0);
    CHECK_EQ(r.height, 80.0);
    g_scale = 1.f;

    ProviderOptions opt{};
    p->get_ProviderOptions(&opt);
    CHECK((opt & ProviderOptions_ServerSideProvider) != 0);
    CHECK((opt & ProviderOptions_UseComThreading) != 0);
}

TEST_CASE(Uia_RootProvider) {
    Fixture f;
    UiaBridge b(nullptr, f.host, ScaleFn);
    auto* root = b.Root();
    REQUIRE(root != nullptr);
    CHECK(Same(root, b.ProviderFor(f.root)));
    ComPtr<IRawElementProviderSimple> s;
    root->QueryInterface(IID_PPV_ARGS(&s));
    CHECK_EQ(PropInt(s.Get(), UIA_ControlTypePropertyId), (int)UIA_WindowControlTypeId);
    CHECK_EQ(PropStr(s.Get(), UIA_NamePropertyId), std::wstring(L"Main"));
    ComPtr<IRawElementProviderSimple> host;
    CHECK_EQ((long)s->get_HostRawElementProvider(&host), (long)S_OK);
    CHECK(host == nullptr);
    CHECK(!Nav(s.Get(), NavigateDirection_Parent));
    CHECK(!Nav(s.Get(), NavigateDirection_NextSibling));
    ComPtr<IRawElementProviderFragmentRoot> fr;
    Frag(s.Get())->get_FragmentRoot(&fr);
    CHECK(Same(fr.Get(), root));
    CHECK(Nav(s.Get(), NavigateDirection_FirstChild) == nullptr);
    UiaRect r{};
    Frag(s.Get())->get_BoundingRectangle(&r);
    CHECK_EQ(r.width, 800.0);
    CHECK_EQ(r.height, 600.0);
    // no hwnd: HandleGetObject refuses
    LRESULT lr = 0;
    CHECK(!b.HandleGetObject(0, UiaRootObjectId, &lr));
}

TEST_CASE(Uia_Navigation) {
    Fixture f;
    auto* a = f.root->Add<Probe>(Role::Button);
    a->SetName(L"A");
    auto* hidden = f.root->Add<Probe>(Role::Button);
    hidden->SetName(L"Hidden");
    hidden->SetVisible(false);
    auto* c = f.root->Add<Probe>(Role::Group);
    c->SetName(L"C");
    auto* d1 = c->Add<Probe>(Role::ListItem);
    d1->SetName(L"D1");
    auto* d2 = c->Add<Probe>(Role::ListItem);
    d2->SetName(L"D2");
    UiaBridge b(nullptr, f.host, ScaleFn);

    auto first = Nav(b.Root(), NavigateDirection_FirstChild);
    REQUIRE(first != nullptr);
    CHECK_EQ(NameOf(first.Get()), std::wstring(L"A"));
    auto next = Nav(first.Get(), NavigateDirection_NextSibling);
    CHECK_EQ(NameOf(next.Get()), std::wstring(L"C"));
    CHECK(Nav(next.Get(), NavigateDirection_NextSibling) == nullptr);
    CHECK_EQ(NameOf(Nav(next.Get(), NavigateDirection_PreviousSibling).Get()), std::wstring(L"A"));
    CHECK(Nav(first.Get(), NavigateDirection_PreviousSibling) == nullptr);
    CHECK(Same(Nav(first.Get(), NavigateDirection_Parent).Get(), b.Root()));
    CHECK_EQ(NameOf(Nav(b.Root(), NavigateDirection_LastChild).Get()), std::wstring(L"C"));
    CHECK_EQ(NameOf(Nav(next.Get(), NavigateDirection_FirstChild).Get()), std::wstring(L"D1"));
    CHECK_EQ(NameOf(Nav(next.Get(), NavigateDirection_LastChild).Get()), std::wstring(L"D2"));
    auto d2p = Nav(next.Get(), NavigateDirection_LastChild);
    CHECK(Same(Nav(d2p.Get(), NavigateDirection_Parent).Get(), next.Get()));
    CHECK(Nav(d2p.Get(), NavigateDirection_FirstChild) == nullptr);
    CHECK_EQ(NameOf(Nav(d2p.Get(), NavigateDirection_PreviousSibling).Get()), std::wstring(L"D1"));

    // popups hang under the fragment root after the visible children (bottom..top)
    auto* pop1 = f.host.OpenPopup(std::make_unique<Probe>(Role::Menu));
    pop1->SetName(L"P1");
    auto* pop2 = f.host.OpenPopup(std::make_unique<Probe>(Role::Menu));
    pop2->SetName(L"P2");
    auto last = Nav(b.Root(), NavigateDirection_LastChild);
    CHECK_EQ(NameOf(last.Get()), std::wstring(L"P2"));
    CHECK_EQ(NameOf(Nav(last.Get(), NavigateDirection_PreviousSibling).Get()), std::wstring(L"P1"));
    CHECK_EQ(NameOf(Nav(Nav(last.Get(), NavigateDirection_PreviousSibling).Get(), NavigateDirection_PreviousSibling).Get()),
             std::wstring(L"C"));
    CHECK(Same(Nav(last.Get(), NavigateDirection_Parent).Get(), b.Root()));
    CHECK(Nav(last.Get(), NavigateDirection_NextSibling) == nullptr);
    f.host.ClosePopup(pop2);
    CHECK_EQ(NameOf(Nav(b.Root(), NavigateDirection_LastChild).Get()), std::wstring(L"P1"));
    f.host.CloseAllPopups();
    CHECK_EQ(NameOf(Nav(b.Root(), NavigateDirection_LastChild).Get()), std::wstring(L"C"));
}

TEST_CASE(Uia_ElementFromPointAndFocus) {
    Fixture f;
    auto* a = f.root->Add<Probe>(Role::Button);
    a->SetName(L"A");
    a->SetFocusable(true);
    a->SetBounds({0, 0, 100, 30});
    auto* g = f.root->Add<Probe>(Role::Group);
    g->SetName(L"G");
    g->SetBounds({0, 100, 200, 200});
    auto* inner = g->Add<Probe>(Role::Button);
    inner->SetName(L"Inner");
    inner->SetBounds({10, 110, 60, 140});
    g_scale = 2.f;
    UiaBridge b(nullptr, f.host, ScaleFn);
    b.SetClientOrigin(100, 50);
    ComPtr<IRawElementProviderFragment> hit;
    CHECK_EQ((long)b.Root()->ElementProviderFromPoint(100 + 20, 50 + 20, &hit), (long)S_OK);   // DIP (10,10)
    CHECK_EQ(NameOf(hit.Get()), std::wstring(L"A"));
    hit.Reset();
    b.Root()->ElementProviderFromPoint(100 + 2 * 20, 50 + 2 * 120, &hit);   // DIP (20,120) -> inner
    CHECK_EQ(NameOf(hit.Get()), std::wstring(L"Inner"));
    hit.Reset();
    b.Root()->ElementProviderFromPoint(100 + 2 * 700, 50 + 2 * 500, &hit);
    CHECK(hit == nullptr);
    g_scale = 1.f;

    ComPtr<IRawElementProviderFragment> foc;
    CHECK_EQ((long)b.Root()->GetFocus(&foc), (long)S_OK);
    CHECK(foc == nullptr);
    a->Focus();
    CHECK_EQ((long)b.Root()->GetFocus(&foc), (long)S_OK);
    CHECK_EQ(NameOf(foc.Get()), std::wstring(L"A"));
    CHECK(Same(foc.Get(), b.ProviderFor(a)));
}

TEST_CASE(Uia_InvokeAndToggle) {
    Fixture f;
    UiaBridge b(nullptr, f.host, ScaleFn);
    auto* w = f.root->Add<Probe>(Role::ToggleButton);
    auto* p = b.ProviderFor(w);
    CHECK(Pat<IInvokeProvider>(p, UIA_InvokePatternId) == nullptr);
    CHECK(Pat<IToggleProvider>(p, UIA_TogglePatternId) == nullptr);
    w->inv = w->tog = true;
    auto inv = Pat<IInvokeProvider>(p, UIA_InvokePatternId);
    auto tog = Pat<IToggleProvider>(p, UIA_TogglePatternId);
    REQUIRE(inv != nullptr);
    REQUIRE(tog != nullptr);
    CHECK_EQ((long)inv->Invoke(), (long)S_OK);
    CHECK_EQ(w->invokes, 1);
    ToggleState st{};
    CHECK_EQ((long)tog->get_ToggleState(&st), (long)S_OK);
    CHECK_EQ((int)st, (int)ToggleState_Off);
    CHECK_EQ((long)tog->Toggle(), (long)S_OK);
    CHECK_EQ(w->toggles, 1);
    tog->get_ToggleState(&st);
    CHECK_EQ((int)st, (int)ToggleState_On);
    w->toggleState = 2;
    tog->get_ToggleState(&st);
    CHECK_EQ((int)st, (int)ToggleState_Indeterminate);
    w->SetEnabled(false);
    CHECK_EQ((long)inv->Invoke(), (long)UIA_E_ELEMENTNOTENABLED);
    CHECK_EQ((long)tog->Toggle(), (long)UIA_E_ELEMENTNOTENABLED);
    CHECK_EQ(w->invokes, 1);
    CHECK_EQ(w->toggles, 1);
}

TEST_CASE(Uia_RangeValueAndValue) {
    Fixture f;
    UiaBridge b(nullptr, f.host, ScaleFn);
    auto* w = f.root->Add<Probe>(Role::Slider);
    w->rng = true;
    w->ri.minimum = 0;
    w->ri.maximum = 10;
    w->ri.value = 3;
    w->ri.smallChange = 1;
    w->ri.largeChange = 4;
    auto rv = Pat<IRangeValueProvider>(b.ProviderFor(w), UIA_RangeValuePatternId);
    REQUIRE(rv != nullptr);
    double d = -1;
    rv->get_Value(&d); CHECK_EQ(d, 3.0);
    rv->get_Minimum(&d); CHECK_EQ(d, 0.0);
    rv->get_Maximum(&d); CHECK_EQ(d, 10.0);
    rv->get_SmallChange(&d); CHECK_EQ(d, 1.0);
    rv->get_LargeChange(&d); CHECK_EQ(d, 4.0);
    CHECK_EQ((long)rv->SetValue(7.0), (long)S_OK);
    CHECK_EQ(w->lastSet, 7.0);
    rv->SetValue(99.0);
    CHECK_EQ(w->lastSet, 10.0);
    rv->SetValue(-5.0);
    CHECK_EQ(w->lastSet, 0.0);
    CHECK_EQ((long)rv->SetValue(std::nan("")), (long)E_INVALIDARG);
    BOOL ro = TRUE;
    rv->get_IsReadOnly(&ro);
    CHECK(!ro);
    w->ri.readOnly = true;
    w->lastSet = -1e9;
    CHECK_EQ((long)rv->SetValue(2.0), (long)UIA_E_NOTSUPPORTED);
    CHECK_EQ(w->lastSet, -1e9);

    auto* e = f.root->Add<Probe>(Role::Edit);
    e->val = true;
    e->value = L"hello";
    auto vp = Pat<IValueProvider>(b.ProviderFor(e), UIA_ValuePatternId);
    REQUIRE(vp != nullptr);
    BSTR s = nullptr;
    vp->get_Value(&s);
    CHECK_EQ(std::wstring(s ? s : L""), std::wstring(L"hello"));
    SysFreeString(s);
    CHECK_EQ((long)vp->SetValue(L"world"), (long)S_OK);
    CHECK_EQ(e->value, std::wstring(L"world"));
    vp->get_IsReadOnly(&ro);
    CHECK(!ro);
    e->valueRO = true;
    vp->get_IsReadOnly(&ro);
    CHECK(ro);
    CHECK_EQ((long)vp->SetValue(L"x"), (long)UIA_E_NOTSUPPORTED);
    CHECK_EQ(e->value, std::wstring(L"world"));
}

TEST_CASE(Uia_ExpandCollapse) {
    Fixture f;
    UiaBridge b(nullptr, f.host, ScaleFn);
    auto* w = f.root->Add<Probe>(Role::ComboBox);
    CHECK(Pat<IExpandCollapseProvider>(b.ProviderFor(w), UIA_ExpandCollapsePatternId) == nullptr);
    w->ec = true;
    auto ec = Pat<IExpandCollapseProvider>(b.ProviderFor(w), UIA_ExpandCollapsePatternId);
    REQUIRE(ec != nullptr);
    ExpandCollapseState st{};
    ec->get_ExpandCollapseState(&st);
    CHECK_EQ((int)st, (int)ExpandCollapseState_Collapsed);
    CHECK_EQ((long)ec->Expand(), (long)S_OK);
    CHECK(w->expanded);
    ec->get_ExpandCollapseState(&st);
    CHECK_EQ((int)st, (int)ExpandCollapseState_Expanded);
    CHECK_EQ((long)ec->Collapse(), (long)S_OK);
    CHECK(!w->expanded);
}

TEST_CASE(Uia_SelectionContainerAndItems) {
    Fixture f;
    UiaBridge b(nullptr, f.host, ScaleFn);
    auto* list = f.root->Add<Probe>(Role::List);
    list->sel = true;
    list->multi = true;
    auto* i1 = list->Add<Probe>(Role::ListItem);
    auto* i2 = list->Add<Probe>(Role::ListItem);
    i1->SetName(L"I1");
    i2->SetName(L"I2");
    i1->selItem = i2->selItem = true;
    i2->selectedFlag = true;
    list->selection = {i2};

    auto sp = Pat<ISelectionProvider>(b.ProviderFor(list), UIA_SelectionPatternId);
    REQUIRE(sp != nullptr);
    BOOL m = FALSE, req = TRUE;
    sp->get_CanSelectMultiple(&m);
    sp->get_IsSelectionRequired(&req);
    CHECK(m);
    CHECK(!req);
    SAFEARRAY* sa = nullptr;
    CHECK_EQ((long)sp->GetSelection(&sa), (long)S_OK);
    REQUIRE(sa != nullptr);
    LONG lo = 0, hi = -1;
    SafeArrayGetLBound(sa, 1, &lo);
    SafeArrayGetUBound(sa, 1, &hi);
    CHECK_EQ(hi - lo + 1, 1);
    IUnknown* u = nullptr;
    LONG idx = 0;
    SafeArrayGetElement(sa, &idx, &u);
    CHECK_EQ(NameOf(u), std::wstring(L"I2"));
    if (u) u->Release();
    SafeArrayDestroy(sa);

    auto it = Pat<ISelectionItemProvider>(b.ProviderFor(i1), UIA_SelectionItemPatternId);
    REQUIRE(it != nullptr);
    BOOL sel = TRUE;
    it->get_IsSelected(&sel);
    CHECK(!sel);
    CHECK_EQ((long)it->Select(), (long)S_OK);
    it->get_IsSelected(&sel);
    CHECK(sel);
    CHECK_EQ((long)it->RemoveFromSelection(), (long)S_OK);
    CHECK(!i1->selectedFlag);
    CHECK_EQ((long)it->AddToSelection(), (long)S_OK);
    CHECK(i1->selectedFlag);
    ComPtr<IRawElementProviderSimple> cont;
    CHECK_EQ((long)it->get_SelectionContainer(&cont), (long)S_OK);
    CHECK(Same(cont.Get(), b.ProviderFor(list)));
    CHECK(Pat<ISelectionItemProvider>(b.ProviderFor(list), UIA_SelectionItemPatternId) == nullptr);
}

TEST_CASE(Uia_Scroll) {
    Fixture f;
    UiaBridge b(nullptr, f.host, ScaleFn);
    auto* w = f.root->Add<Probe>(Role::List);
    CHECK(Pat<IScrollProvider>(b.ProviderFor(w), UIA_ScrollPatternId) == nullptr);
    w->scr = true;
    w->viewSize = 25;
    w->scrollPct = 0;
    auto sp = Pat<IScrollProvider>(b.ProviderFor(w), UIA_ScrollPatternId);
    REQUIRE(sp != nullptr);
    BOOL h = TRUE, v = FALSE;
    sp->get_HorizontallyScrollable(&h);
    sp->get_VerticallyScrollable(&v);
    CHECK(!h);
    CHECK(v);
    double d = 0;
    sp->get_VerticalViewSize(&d);
    CHECK_EQ(d, 25.0);
    sp->get_HorizontalScrollPercent(&d);
    CHECK_EQ(d, (double)UIA_ScrollPatternNoScroll);
    CHECK_EQ((long)sp->SetScrollPercent(UIA_ScrollPatternNoScroll, 40.0), (long)S_OK);
    CHECK_EQ(w->scrollPct, 40.0);
    sp->get_VerticalScrollPercent(&d);
    CHECK_EQ(d, 40.0);
    CHECK_EQ((long)sp->SetScrollPercent(UIA_ScrollPatternNoScroll, 120.0), (long)E_INVALIDARG);
    CHECK_EQ((long)sp->SetScrollPercent(10.0, 10.0), (long)UIA_E_NOTSUPPORTED);
    // large increment = one view: 25 / (100-25) = 33.33%
    w->scrollPct = 0;
    CHECK_EQ((long)sp->Scroll(ScrollAmount_NoAmount, ScrollAmount_LargeIncrement), (long)S_OK);
    CHECK(std::abs(w->scrollPct - 100.0 / 3.0) < 1e-6);
    sp->Scroll(ScrollAmount_NoAmount, ScrollAmount_LargeIncrement);
    sp->Scroll(ScrollAmount_NoAmount, ScrollAmount_LargeIncrement);
    sp->Scroll(ScrollAmount_NoAmount, ScrollAmount_LargeIncrement);
    CHECK_EQ(w->scrollPct, 100.0);
    sp->Scroll(ScrollAmount_NoAmount, ScrollAmount_SmallDecrement);
    CHECK(w->scrollPct < 100.0 && w->scrollPct > 90.0);
    CHECK_EQ((long)sp->Scroll(ScrollAmount_LargeIncrement, ScrollAmount_NoAmount), (long)UIA_E_NOTSUPPORTED);
    w->scrollPct = -1;   // no scroll
    sp->get_VerticallyScrollable(&v);
    CHECK(!v);
}

TEST_CASE(Uia_RuntimeIdUnique) {
    Fixture f;
    UiaBridge b(nullptr, f.host, ScaleFn);
    std::vector<int> ids;
    auto take = [&](IRawElementProviderSimple* p) {
        SAFEARRAY* sa = nullptr;
        REQUIRE(SUCCEEDED(Frag(p)->GetRuntimeId(&sa)));
        REQUIRE(sa != nullptr);
        LONG i = 0;
        int a = 0, c = 0;
        SafeArrayGetElement(sa, &i, &a);
        i = 1;
        SafeArrayGetElement(sa, &i, &c);
        SafeArrayDestroy(sa);
        CHECK_EQ(a, (int)UiaAppendRuntimeId);
        ids.push_back(c);
    };
    take(b.ProviderFor(f.root));
    for (int n = 0; n < 5; n++) take(b.ProviderFor(f.root->Add<Probe>(Role::Button)));
    std::sort(ids.begin(), ids.end());
    CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
    CHECK_EQ(ids.size(), (size_t)6);
}

TEST_CASE(Uia_StaleProviderAndSlot) {
    long before = UiaBridge::LiveProviderCount();
    {
        Fixture f;
        UiaBridge b(nullptr, f.host, ScaleFn);
        auto* w = f.root->Add<Probe>(Role::Button);
        w->SetName(L"Gone");
        w->inv = true;
        auto* p = b.ProviderFor(w);
        CHECK(w->uiaSlot != nullptr);
        CHECK(b.ProviderFor(w) == p);
        ComPtr<IRawElementProviderSimple> held(p);   // a UIA client keeps its own reference
        ComPtr<IInvokeProvider> inv = Pat<IInvokeProvider>(p, UIA_InvokePatternId);
        CHECK_EQ((long)inv->Invoke(), (long)S_OK);

        // Detached but alive: unavailable, slot cleared by the structure-change purge.
        auto detached = f.root->RemoveChild(w);
        REQUIRE(detached != nullptr);
        VARIANT v;
        CHECK_EQ((long)held->GetPropertyValue(UIA_NamePropertyId, &v), (long)UIA_E_ELEMENTNOTAVAILABLE);
        CHECK_EQ((long)inv->Invoke(), (long)UIA_E_ELEMENTNOTAVAILABLE);
        CHECK(detached->uiaSlot == nullptr);
        CHECK_EQ(b.CachedProviderCount(), (size_t)0);
        detached.reset();

        auto* w2 = f.root->Add<Probe>(Role::Button);
        w2->SetName(L"Gone2");
        w2->inv = true;
        ComPtr<IRawElementProviderSimple> held2(b.ProviderFor(w2));
        ComPtr<IRawElementProviderFragment> frag2 = Frag(held2.Get());
        f.root->ClearChildren();
        CHECK_EQ((long)held2->GetPropertyValue(UIA_NamePropertyId, &v), (long)UIA_E_ELEMENTNOTAVAILABLE);
        ComPtr<IUnknown> pat;
        CHECK_EQ((long)held2->GetPatternProvider(UIA_InvokePatternId, &pat), (long)UIA_E_ELEMENTNOTAVAILABLE);
        ComPtr<IRawElementProviderFragment> out;
        CHECK_EQ((long)frag2->Navigate(NavigateDirection_Parent, &out), (long)UIA_E_ELEMENTNOTAVAILABLE);
        UiaRect r;
        CHECK_EQ((long)frag2->get_BoundingRectangle(&r), (long)UIA_E_ELEMENTNOTAVAILABLE);
        SAFEARRAY* sa = nullptr;
        CHECK_EQ((long)frag2->GetRuntimeId(&sa), (long)UIA_E_ELEMENTNOTAVAILABLE);
        CHECK(!Nav(b.Root(), NavigateDirection_FirstChild));
        CHECK_EQ(b.CachedProviderCount(), (size_t)0);

        // uiaSlot is cleared when the provider is released (bridge shutdown drops the cache references).
        auto* w3 = f.root->Add<Probe>(Role::Button);
        b.ProviderFor(w3);
        CHECK(w3->uiaSlot != nullptr);
        b.Shutdown();
        CHECK(w3->uiaSlot == nullptr);
        CHECK(b.ProviderFor(w3) == nullptr);
        CHECK(b.Root() == nullptr);
        CHECK_EQ((long)held->GetPropertyValue(UIA_NamePropertyId, &v), (long)UIA_E_ELEMENTNOTAVAILABLE);
    }
    CHECK_EQ(UiaBridge::LiveProviderCount(), before);
}

TEST_CASE(Uia_DumpTree) {
    Fixture f;
    auto* play = f.root->Add<Probe>(Role::Button);
    play->SetName(L"Play");
    play->inv = true;
    auto* grp = f.root->Add<Probe>(Role::Group);
    grp->SetName(L"Controls");
    auto* item = grp->Add<Probe>(Role::ListItem);
    item->SetName(L"Track 1");
    item->selItem = true;
    auto* sl = grp->Add<Probe>(Role::Slider);
    sl->SetName(L"Seek");
    sl->rng = true;
    auto* hid = f.root->Add<Probe>(Role::Button);
    hid->SetName(L"Hidden");
    hid->SetVisible(false);
    UiaBridge b(nullptr, f.host, ScaleFn);
    std::wstring dump = DumpUiaTree(Frag(b.ProviderFor(f.root)).Get());
    std::wstring expect =
        L"Window \"Main\"\n"
        L"  Button \"Play\" [Invoke]\n"
        L"  Group \"Controls\"\n"
        L"    ListItem \"Track 1\" [SelectionItem]\n"
        L"    Slider \"Seek\" [RangeValue]\n";
    CHECK_EQ(dump, expect);
    std::wstring shallow = DumpUiaTree(Frag(b.ProviderFor(f.root)).Get(), 1);
    CHECK(shallow.find(L"Track 1") == std::wstring::npos);
    CHECK(shallow.find(L"Controls") != std::wstring::npos);
    CHECK_EQ(DumpUiaTree(nullptr), std::wstring());
}

TEST_CASE(Uia_EventsAndListenerRemoval) {
    Fixture f;
    auto* btn = f.root->Add<Probe>(Role::Button);
    btn->SetName(L"B");
    btn->SetFocusable(true);
    btn->inv = btn->tog = true;
    auto* sl = f.root->Add<Probe>(Role::Slider);
    sl->rng = true;
    auto* ed = f.root->Add<Probe>(Role::Edit);
    ed->val = true;
    auto* cb = f.root->Add<Probe>(Role::ComboBox);
    cb->ec = true;
    auto* li = f.root->Add<Probe>(Role::ListItem);
    li->selItem = true;
    UiaBridge b(nullptr, f.host, ScaleFn);
    b.SetAlwaysRaise(true);
    CHECK(b.ListenerToken() != 0);
    std::vector<std::wstring> log;
    b.SetRaisedHook([&](const wchar_t* what, Widget* w) {
        log.push_back(std::wstring(what) + L"@" + (w ? w->Name() : L"?"));
    });
    auto last = [&]() { return log.empty() ? std::wstring() : log.back(); };

    btn->Focus();
    CHECK_EQ(last(), std::wstring(L"Event:Focus@B"));
    btn->Raise(UiEvent::Invoked);
    CHECK_EQ(last(), std::wstring(L"Event:Invoke@B"));
    btn->Raise(UiEvent::ToggleChanged);
    CHECK_EQ(last(), std::wstring(L"Prop:ToggleState@B"));
    sl->Raise(UiEvent::ValueChanged);
    CHECK(last().find(L"Prop:RangeValue") == 0);
    ed->Raise(UiEvent::ValueChanged);
    CHECK(last().find(L"Prop:Value") == 0);
    cb->Raise(UiEvent::ExpandCollapseChanged);
    CHECK(last().find(L"Prop:ExpandCollapseState") == 0);
    li->Raise(UiEvent::SelectionChanged);
    CHECK(last().find(L"Event:ElementRemovedFromSelection") == 0);
    li->selectedFlag = true;
    li->Raise(UiEvent::SelectionChanged);
    CHECK(last().find(L"Event:ElementSelected") == 0);
    btn->SetName(L"B2");
    CHECK_EQ(last(), std::wstring(L"Prop:Name@B2"));
    btn->SetEnabled(false);
    CHECK(last().find(L"Prop:IsEnabled") == 0);
    f.root->Add<Probe>(Role::Button);
    CHECK(last().find(L"Event:Structure@Main") == 0);
    auto* menu = f.host.OpenPopup(std::make_unique<Probe>(Role::Menu));
    menu->Raise(UiEvent::MenuOpened);
    CHECK(last().find(L"Event:MenuOpened") == 0);
    menu->Raise(UiEvent::MenuClosed);
    CHECK(last().find(L"Event:MenuClosed") == 0);
    f.host.CloseAllPopups();

    unsigned raised = b.EventsRaised();
    CHECK(b.EventsHandled() > 0);
    CHECK(raised > 0);
    b.SetAlwaysRaise(false);

    b.Shutdown();
    CHECK_EQ(b.ListenerToken(), 0);
    unsigned h2 = b.EventsHandled();
    btn->SetName(L"after");
    btn->Raise(UiEvent::Invoked);
    f.root->Add<Probe>(Role::Button);
    CHECK_EQ(b.EventsHandled(), h2);
    CHECK(b.IsShutdown());
    b.Shutdown();
}

TEST_CASE(Uia_ProviderRefcountSanity) {
    long before = UiaBridge::LiveProviderCount();
    {
        Fixture f;
        for (int n = 0; n < 50; n++) f.root->Add<Probe>(Role::Button);
        UiaBridge b(nullptr, f.host, ScaleFn);
        CHECK_EQ(UiaBridge::LiveProviderCount(), before + 1);   // the fragment root
        for (auto& c : f.root->Children()) b.ProviderFor(c.get());
        CHECK_EQ(UiaBridge::LiveProviderCount(), before + 51);
        CHECK_EQ(b.CachedProviderCount(), (size_t)50);
        // walking the tree via COM adds and releases references without creating extra objects
        {
            auto ch = Nav(b.Root(), NavigateDirection_FirstChild);
            int n = 0;
            while (ch) {
                n++;
                ComPtr<IRawElementProviderFragment> nx;
                ch->Navigate(NavigateDirection_NextSibling, &nx);
                ch = nx;
            }
            CHECK_EQ(n, 50);
        }
        CHECK_EQ(UiaBridge::LiveProviderCount(), before + 51);
        f.root->ClearChildren();
        CHECK_EQ(UiaBridge::LiveProviderCount(), before + 1);   // purged: nothing external holds them
    }
    CHECK_EQ(UiaBridge::LiveProviderCount(), before);
}

TEST_CASE(Uia_PopupsOpenedBeforeBridgeAreEnumerated) {
    Fixture f;
    auto* c = f.root->Add<Probe>(Role::Button);
    c->SetName(L"C");
    c->SetBounds({0, 0, 50, 20});
    auto p1 = std::make_unique<Probe>(Role::Menu);
    p1->SetName(L"P1");
    p1->SetBounds({10, 10, 60, 60});
    auto p2 = std::make_unique<Probe>(Role::Menu);
    p2->SetName(L"P2");
    p2->SetBounds({20, 20, 70, 70});
    PopupOptions po;
    po.takeFocus = false;
    f.host.OpenPopup(std::move(p1), po);
    f.host.OpenPopup(std::move(p2), po);
    CHECK_EQ((int)f.host.Popups().size(), 2);
    UiaBridge b(nullptr, f.host, ScaleFn);
    ComPtr<IRawElementProviderFragment> kid = Nav(b.Root(), NavigateDirection_FirstChild);
    std::vector<std::wstring> names;
    while (kid) {
        names.push_back(NameOf(kid.Get()));
        kid = Nav(kid.Get(), NavigateDirection_NextSibling);
    }
    CHECK_EQ((int)names.size(), 3);
    if (names.size() == 3) {
        CHECK_EQ(names[0], std::wstring(L"C"));
        CHECK_EQ(names[1], std::wstring(L"P1"));
        CHECK_EQ(names[2], std::wstring(L"P2"));
    }
    f.host.CloseAllPopups();
    CHECK_EQ(NameOf(Nav(b.Root(), NavigateDirection_LastChild).Get()), std::wstring(L"C"));
}

TEST_CASE(Uia_OffscreenPredicateHidesAutoHiddenBar) {
    Fixture f;
    auto* bar = f.root->Add<Probe>(Role::Group);
    bar->SetName(L"Bar");
    bar->SetBounds({0, 500, 800, 600});
    auto* play = bar->Add<Probe>(Role::Button);
    play->SetName(L"Play");
    play->SetBounds({380, 520, 420, 560});
    UiaBridge b(nullptr, f.host, ScaleFn);
    bool hidden = false;
    b.SetOffscreenPredicate([&](Widget* w) { return hidden && w == bar; });
    IRawElementProviderSimple* pb = b.ProviderFor(bar);
    IRawElementProviderSimple* pp = b.ProviderFor(play);
    CHECK(!PropBool(pb, UIA_IsOffscreenPropertyId));
    CHECK(!PropBool(pp, UIA_IsOffscreenPropertyId));
    ComPtr<IRawElementProviderFragment> hit;
    b.Root()->ElementProviderFromPoint(400, 540, &hit);
    CHECK_EQ(NameOf(hit.Get()), std::wstring(L"Play"));
    hidden = true;                      // auto-hidden: still in the tree with its bounds, but off-screen and not hit-testable
    CHECK(PropBool(pb, UIA_IsOffscreenPropertyId));
    CHECK(PropBool(pp, UIA_IsOffscreenPropertyId));
    hit.Reset();
    b.Root()->ElementProviderFromPoint(400, 540, &hit);
    CHECK(hit == nullptr);
    std::vector<std::wstring> raised;
    b.SetAlwaysRaise(true);
    b.SetRaisedHook([&](const wchar_t* d, Widget*) { raised.push_back(d); });
    b.NotifyOffscreenChanged(bar);
    CHECK(std::find(raised.begin(), raised.end(), std::wstring(L"Prop:IsOffscreen")) != raised.end());
    hidden = false;
    CHECK(!PropBool(pb, UIA_IsOffscreenPropertyId));
}

TEST_CASE(Uia_UnnamedCustomHelperIsNotAControlElement) {
    Fixture f;
    auto* helper = f.root->Add<Probe>(Role::Custom);          // e.g. ScrollViewer's overlay widget
    helper->SetBounds({0, 0, 10, 10});
    auto* named = f.root->Add<Probe>(Role::Custom);
    named->SetName(L"Canvas");
    named->SetBounds({0, 20, 10, 30});
    UiaBridge b(nullptr, f.host, ScaleFn);
    CHECK(!PropBool(b.ProviderFor(helper), UIA_IsControlElementPropertyId));
    CHECK(!PropBool(b.ProviderFor(helper), UIA_IsContentElementPropertyId));
    CHECK(PropBool(b.ProviderFor(named), UIA_IsControlElementPropertyId));
}
