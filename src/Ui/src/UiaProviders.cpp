#include "UiaInternal.h"

namespace pui {

std::atomic<long> g_uiaLiveProviders{0};

int UiaControlTypeFor(Role r) {
    switch (r) {
        case Role::Window: return UIA_WindowControlTypeId;
        case Role::Pane: return UIA_PaneControlTypeId;
        case Role::Group: return UIA_GroupControlTypeId;
        case Role::Text: return UIA_TextControlTypeId;
        case Role::Image: return UIA_ImageControlTypeId;
        case Role::Button: return UIA_ButtonControlTypeId;
        case Role::ToggleButton: return UIA_ButtonControlTypeId;
        case Role::CheckBox: return UIA_CheckBoxControlTypeId;
        case Role::RadioButton: return UIA_RadioButtonControlTypeId;
        case Role::Slider: return UIA_SliderControlTypeId;
        case Role::ComboBox: return UIA_ComboBoxControlTypeId;
        case Role::List: return UIA_ListControlTypeId;
        case Role::ListItem: return UIA_ListItemControlTypeId;
        case Role::Menu: return UIA_MenuControlTypeId;
        case Role::MenuItem: return UIA_MenuItemControlTypeId;
        case Role::ToolTip: return UIA_ToolTipControlTypeId;
        case Role::ScrollBar: return UIA_ScrollBarControlTypeId;
        case Role::Edit: return UIA_EditControlTypeId;
        case Role::Tree: return UIA_TreeControlTypeId;
        case Role::TreeItem: return UIA_TreeItemControlTypeId;
        case Role::Tab: return UIA_TabControlTypeId;
        case Role::TabItem: return UIA_TabItemControlTypeId;
        case Role::ProgressBar: return UIA_ProgressBarControlTypeId;
        case Role::StatusBar: return UIA_StatusBarControlTypeId;
        case Role::Hyperlink: return UIA_HyperlinkControlTypeId;
        case Role::Separator: return UIA_SeparatorControlTypeId;
        case Role::Custom: return UIA_CustomControlTypeId;
    }
    return UIA_CustomControlTypeId;
}

const wchar_t* UiaRoleName(Role r) {
    switch (r) {
        case Role::Window: return L"Window";
        case Role::Pane: return L"Pane";
        case Role::Group: return L"Group";
        case Role::Text: return L"Text";
        case Role::Image: return L"Image";
        case Role::Button: return L"Button";
        case Role::ToggleButton: return L"ToggleButton";
        case Role::CheckBox: return L"CheckBox";
        case Role::RadioButton: return L"RadioButton";
        case Role::Slider: return L"Slider";
        case Role::ComboBox: return L"ComboBox";
        case Role::List: return L"List";
        case Role::ListItem: return L"ListItem";
        case Role::Menu: return L"Menu";
        case Role::MenuItem: return L"MenuItem";
        case Role::ToolTip: return L"ToolTip";
        case Role::ScrollBar: return L"ScrollBar";
        case Role::Edit: return L"Edit";
        case Role::Tree: return L"Tree";
        case Role::TreeItem: return L"TreeItem";
        case Role::Tab: return L"Tab";
        case Role::TabItem: return L"TabItem";
        case Role::ProgressBar: return L"ProgressBar";
        case Role::StatusBar: return L"StatusBar";
        case Role::Hyperlink: return L"Hyperlink";
        case Role::Separator: return L"Separator";
        case Role::Custom: return L"Custom";
    }
    return L"Custom";
}

std::wstring UiaClassNameFor(Role r) { return std::wstring(L"PlayerUi.") + UiaRoleName(r); }

const wchar_t* UiaControlTypeName(int id) {
    switch (id) {
        case UIA_ButtonControlTypeId: return L"Button";
        case UIA_CheckBoxControlTypeId: return L"CheckBox";
        case UIA_ComboBoxControlTypeId: return L"ComboBox";
        case UIA_EditControlTypeId: return L"Edit";
        case UIA_HyperlinkControlTypeId: return L"Hyperlink";
        case UIA_ImageControlTypeId: return L"Image";
        case UIA_ListItemControlTypeId: return L"ListItem";
        case UIA_ListControlTypeId: return L"List";
        case UIA_MenuControlTypeId: return L"Menu";
        case UIA_MenuItemControlTypeId: return L"MenuItem";
        case UIA_ProgressBarControlTypeId: return L"ProgressBar";
        case UIA_RadioButtonControlTypeId: return L"RadioButton";
        case UIA_ScrollBarControlTypeId: return L"ScrollBar";
        case UIA_SliderControlTypeId: return L"Slider";
        case UIA_StatusBarControlTypeId: return L"StatusBar";
        case UIA_TabControlTypeId: return L"Tab";
        case UIA_TabItemControlTypeId: return L"TabItem";
        case UIA_TextControlTypeId: return L"Text";
        case UIA_ToolTipControlTypeId: return L"ToolTip";
        case UIA_TreeControlTypeId: return L"Tree";
        case UIA_TreeItemControlTypeId: return L"TreeItem";
        case UIA_CustomControlTypeId: return L"Custom";
        case UIA_GroupControlTypeId: return L"Group";
        case UIA_WindowControlTypeId: return L"Window";
        case UIA_PaneControlTypeId: return L"Pane";
        case UIA_SeparatorControlTypeId: return L"Separator";
    }
    return L"Unknown";
}

bool UiaIsPopupRoot(const UiaState& s, Widget* w) {
    return w && w->Parent() == nullptr && w != s.host->Root() && s.host->IsPopupOpen(w);
}

std::vector<Widget*> UiaChildrenOf(const UiaState& s, Widget* w) {
    std::vector<Widget*> v;
    for (auto& c : w->Children())
        if (c->Visible()) v.push_back(c.get());
    if (w == s.host->Root()) {
        for (Widget* p : s.host->Popups())
            if (std::find(v.begin(), v.end(), p) == v.end()) v.push_back(p);
    }
    return v;
}

IRawElementProviderSimple* UiaProviderOf(UiaState& s, Widget* w) {
    if (!w || !s.alive || !s.host) return nullptr;
    if (w == s.host->Root()) return static_cast<IRawElementProviderSimple*>(s.root);
    if (w->uiaSlot) return static_cast<IRawElementProviderSimple*>(static_cast<WidgetProvider*>(w->uiaSlot));
    auto self = s.self.lock();
    if (!self) return nullptr;
    return static_cast<IRawElementProviderSimple*>(new WidgetProvider(std::move(self), w));
}

namespace {
HRESULT MakeRuntimeId(int widgetId, SAFEARRAY** pp) {
    int ids[2] = {UiaAppendRuntimeId, widgetId};
    SAFEARRAY* sa = SafeArrayCreateVector(VT_I4, 0, 2);
    if (!sa) return E_OUTOFMEMORY;
    for (LONG i = 0; i < 2; i++) SafeArrayPutElement(sa, &i, &ids[i]);
    *pp = sa;
    return S_OK;
}

HRESULT FragmentOut(UiaState& s, Widget* target, IRawElementProviderFragment** pp) {
    if (!target) return S_OK;
    IRawElementProviderSimple* p = UiaProviderOf(s, target);
    if (!p) return S_OK;
    return p->QueryInterface(__uuidof(IRawElementProviderFragment), reinterpret_cast<void**>(pp));
}

constexpr ProviderOptions kOptions = (ProviderOptions)(ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading);
}

HRESULT RootProvider::QueryInterface(REFIID iid, void** pp) {
    if (!pp) return E_POINTER;
    *pp = nullptr;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IRawElementProviderSimple))
        *pp = static_cast<IRawElementProviderSimple*>(this);
    else if (iid == __uuidof(IRawElementProviderFragment))
        *pp = static_cast<IRawElementProviderFragment*>(this);
    else if (iid == __uuidof(IRawElementProviderFragmentRoot))
        *pp = static_cast<IRawElementProviderFragmentRoot*>(this);
    else
        return E_NOINTERFACE;
    exposed_ = true;
    AddRef();
    return S_OK;
}

HRESULT RootProvider::get_ProviderOptions(ProviderOptions* o) {
    if (!o) return E_POINTER;
    *o = kOptions;
    return S_OK;
}

HRESULT RootProvider::GetPatternProvider(PATTERNID, IUnknown** pp) {
    if (!pp) return E_POINTER;
    *pp = nullptr;
    return Live() ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
}

HRESULT RootProvider::GetPropertyValue(PROPERTYID id, VARIANT* v) {
    if (!v) return E_POINTER;
    *v = VEmpty();
    Widget* root = Live();
    if (!root) return UIA_E_ELEMENTNOTAVAILABLE;
    bool hasHwnd = s_->hwnd != nullptr;   // the HWND host provider supplies the window's own properties
    switch (id) {
        case UIA_ControlTypePropertyId: if (!hasHwnd) *v = VInt(UIA_WindowControlTypeId); break;
        case UIA_NamePropertyId: if (!hasHwnd || !root->Name().empty()) *v = VStr(root->Name()); break;
        case UIA_AutomationIdPropertyId: if (!root->AutomationId().empty()) *v = VStr(root->AutomationId()); break;
        case UIA_ClassNamePropertyId: if (!hasHwnd) *v = VStr(L"PlayerUi.Window"); break;
        case UIA_FrameworkIdPropertyId: *v = VStr(L"PlayerUi"); break;
        case UIA_ProcessIdPropertyId: *v = VInt((int)GetCurrentProcessId()); break;
        case UIA_IsEnabledPropertyId:
        case UIA_IsControlElementPropertyId:
        case UIA_IsContentElementPropertyId: if (!hasHwnd) *v = VBool(true); break;
        default: break;
    }
    return S_OK;
}

HRESULT RootProvider::get_HostRawElementProvider(IRawElementProviderSimple** pp) {
    if (!pp) return E_POINTER;
    *pp = nullptr;
    if (!Live()) return UIA_E_ELEMENTNOTAVAILABLE;
    if (s_->hwnd) return UiaHostProviderFromHwnd(s_->hwnd, pp);
    return S_OK;
}

HRESULT RootProvider::Navigate(NavigateDirection dir, IRawElementProviderFragment** pp) {
    if (!pp) return E_POINTER;
    *pp = nullptr;
    Widget* root = Live();
    if (!root) return UIA_E_ELEMENTNOTAVAILABLE;
    if (dir == NavigateDirection_FirstChild || dir == NavigateDirection_LastChild) {
        auto kids = UiaChildrenOf(*s_, root);
        if (!kids.empty()) return FragmentOut(*s_, dir == NavigateDirection_FirstChild ? kids.front() : kids.back(), pp);
    } else if (dir != NavigateDirection_Parent && dir != NavigateDirection_NextSibling &&
               dir != NavigateDirection_PreviousSibling) {
        return E_INVALIDARG;
    }
    return S_OK;
}

HRESULT RootProvider::GetRuntimeId(SAFEARRAY** pp) {
    if (!pp) return E_POINTER;
    *pp = nullptr;
    Widget* root = Live();
    if (!root) return UIA_E_ELEMENTNOTAVAILABLE;
    if (s_->hwnd) return S_OK;   // the HWND host provider supplies it
    return MakeRuntimeId(root->Id(), pp);
}

HRESULT RootProvider::get_BoundingRectangle(UiaRect* r) {
    if (!r) return E_POINTER;
    *r = {};
    if (!Live()) return UIA_E_ELEMENTNOTAVAILABLE;
    float sc = s_->Scale();
    POINT o = s_->ClientOrigin();
    SizeF sz = s_->host->Size();
    r->left = o.x;
    r->top = o.y;
    r->width = std::round(sz.w * sc);
    r->height = std::round(sz.h * sc);
    return S_OK;
}

HRESULT RootProvider::GetEmbeddedFragmentRoots(SAFEARRAY** pp) {
    if (!pp) return E_POINTER;
    *pp = nullptr;
    return S_OK;
}

HRESULT RootProvider::SetFocus() { return Live() ? S_OK : UIA_E_ELEMENTNOTAVAILABLE; }

HRESULT RootProvider::get_FragmentRoot(IRawElementProviderFragmentRoot** pp) {
    if (!pp) return E_POINTER;
    *pp = nullptr;
    if (!Live()) return UIA_E_ELEMENTNOTAVAILABLE;
    *pp = static_cast<IRawElementProviderFragmentRoot*>(this);
    AddRef();
    exposed_ = true;
    return S_OK;
}

HRESULT RootProvider::ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** pp) {
    if (!pp) return E_POINTER;
    *pp = nullptr;
    Widget* root = Live();
    if (!root) return UIA_E_ELEMENTNOTAVAILABLE;
    float sc = s_->Scale();
    POINT o = s_->ClientOrigin();
    PointF dip{(float)((x - o.x) / sc), (float)((y - o.y) / sc)};
    Widget* hit = s_->host->HitTest(dip);
    if (!hit || hit == root) return S_OK;   // the window itself
    if (UiaIsOffscreen(*s_, hit)) return S_OK;   // auto-hidden bar/title: not hit-testable
    return FragmentOut(*s_, hit, pp);
}

HRESULT RootProvider::GetFocus(IRawElementProviderFragment** pp) {
    if (!pp) return E_POINTER;
    *pp = nullptr;
    if (!Live()) return UIA_E_ELEMENTNOTAVAILABLE;
    Widget* f = s_->host->Focused();
    if (!f || f == s_->host->Root()) return S_OK;
    return FragmentOut(*s_, f, pp);
}

HRESULT WidgetProvider::QueryInterface(REFIID iid, void** pp) {
    if (!pp) return E_POINTER;
    *pp = nullptr;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IRawElementProviderSimple))
        *pp = static_cast<IRawElementProviderSimple*>(this);
    else if (iid == __uuidof(IRawElementProviderFragment)) *pp = static_cast<IRawElementProviderFragment*>(this);
    else if (iid == __uuidof(IInvokeProvider)) *pp = static_cast<IInvokeProvider*>(this);
    else if (iid == __uuidof(IToggleProvider)) *pp = static_cast<IToggleProvider*>(this);
    else if (iid == __uuidof(IRangeValueProvider)) *pp = static_cast<IRangeValueProvider*>(this);
    else if (iid == __uuidof(IValueProvider)) *pp = static_cast<IValueProvider*>(this);
    else if (iid == __uuidof(IExpandCollapseProvider)) *pp = static_cast<IExpandCollapseProvider*>(this);
    else if (iid == __uuidof(ISelectionProvider)) *pp = static_cast<ISelectionProvider*>(this);
    else if (iid == __uuidof(ISelectionItemProvider)) *pp = static_cast<ISelectionItemProvider*>(this);
    else if (iid == __uuidof(IScrollProvider)) *pp = static_cast<IScrollProvider*>(this);
    else return E_NOINTERFACE;
    exposed_ = true;
    AddRef();
    return S_OK;
}

HRESULT WidgetProvider::get_ProviderOptions(ProviderOptions* o) {
    if (!o) return E_POINTER;
    *o = kOptions;
    return S_OK;
}

HRESULT WidgetProvider::GetPatternProvider(PATTERNID id, IUnknown** pp) {
    if (!pp) return E_POINTER;
    *pp = nullptr;
    Widget* w = Live();
    if (!w) return UIA_E_ELEMENTNOTAVAILABLE;
    IUnknown* out = nullptr;
    switch (id) {
        case UIA_InvokePatternId: if (w->SupportsInvoke()) out = static_cast<IInvokeProvider*>(this); break;
        case UIA_TogglePatternId: if (w->SupportsToggle()) out = static_cast<IToggleProvider*>(this); break;
        case UIA_RangeValuePatternId: if (w->SupportsRangeValue()) out = static_cast<IRangeValueProvider*>(this); break;
        case UIA_ValuePatternId: if (w->SupportsValue()) out = static_cast<IValueProvider*>(this); break;
        case UIA_ExpandCollapsePatternId: if (w->SupportsExpandCollapse()) out = static_cast<IExpandCollapseProvider*>(this); break;
        case UIA_SelectionPatternId: if (w->SupportsSelection()) out = static_cast<ISelectionProvider*>(this); break;
        case UIA_SelectionItemPatternId: if (w->SupportsSelectionItem()) out = static_cast<ISelectionItemProvider*>(this); break;
        case UIA_ScrollPatternId: if (w->SupportsScroll()) out = static_cast<IScrollProvider*>(this); break;
        default: break;
    }
    if (out) {
        exposed_ = true;
        out->AddRef();
        *pp = out;
    }
    return S_OK;
}

bool UiaIsOffscreen(const UiaState& s, Widget* w) {
    if (!w->IsEffectivelyVisible()) return true;
    if (s.offscreen)
        for (Widget* x = w; x; x = x->Parent())
            if (s.offscreen(x)) return true;
    SizeF sz = s.host->Size();
    return sz.w > 0 && sz.h > 0 && !w->Bounds().Intersects(RectF{0, 0, sz.w, sz.h});
}

bool WidgetProvider::Offscreen(Widget* w) const { return UiaIsOffscreen(*s_, w); }

HRESULT WidgetProvider::GetPropertyValue(PROPERTYID id, VARIANT* v) {
    if (!v) return E_POINTER;
    *v = VEmpty();
    Widget* w = Live();
    if (!w) return UIA_E_ELEMENTNOTAVAILABLE;
    Role r = w->GetRole();
    // Unnamed layout containers and unnamed Custom helper widgets (e.g. a scroll viewer's overlay) are not control elements.
    bool layout = (r == Role::Pane || r == Role::Group || (r == Role::Custom && !w->SupportsInvoke() && !w->SupportsToggle() &&
                   !w->SupportsRangeValue() && !w->SupportsValue() && !w->SupportsSelection() && !w->SupportsSelectionItem() &&
                   !w->SupportsExpandCollapse() && !w->SupportsScroll())) && w->Name().empty() && w->AutomationId().empty();
    switch (id) {
        case UIA_ControlTypePropertyId: *v = VInt(UiaControlTypeFor(r)); break;
        case UIA_NamePropertyId: *v = VStr(w->Name()); break;
        case UIA_AutomationIdPropertyId: if (!w->AutomationId().empty()) *v = VStr(w->AutomationId()); break;
        case UIA_HelpTextPropertyId:
            if (!w->HelpText().empty()) *v = VStr(w->HelpText());
            else if (!w->ToolTip().empty()) *v = VStr(w->ToolTip());
            break;
        case UIA_AcceleratorKeyPropertyId: if (!w->AcceleratorKey().empty()) *v = VStr(w->AcceleratorKey()); break;
        case UIA_ClassNamePropertyId: *v = VStr(UiaClassNameFor(r)); break;
        case UIA_FrameworkIdPropertyId: *v = VStr(L"PlayerUi"); break;
        case UIA_IsEnabledPropertyId: *v = VBool(w->IsEffectivelyEnabled()); break;
        case UIA_IsKeyboardFocusablePropertyId: *v = VBool(w->CanFocus()); break;
        case UIA_HasKeyboardFocusPropertyId: *v = VBool(w->IsFocused()); break;
        case UIA_IsOffscreenPropertyId: *v = VBool(Offscreen(w)); break;
        case UIA_IsControlElementPropertyId: *v = VBool(!layout); break;
        case UIA_IsContentElementPropertyId:
            *v = VBool(!layout && r != Role::Pane && r != Role::Group && r != Role::Separator);
            break;
        case UIA_IsPasswordPropertyId: *v = VBool(false); break;
        case UIA_ProcessIdPropertyId: *v = VInt((int)GetCurrentProcessId()); break;
        case UIA_LocalizedControlTypePropertyId:
            if (r == Role::ToggleButton) *v = VStr(L"toggle button");
            break;
        default: break;
    }
    return S_OK;
}

HRESULT WidgetProvider::get_HostRawElementProvider(IRawElementProviderSimple** pp) {
    if (!pp) return E_POINTER;
    *pp = nullptr;
    return Live() ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
}

HRESULT WidgetProvider::Navigate(NavigateDirection dir, IRawElementProviderFragment** pp) {
    if (!pp) return E_POINTER;
    *pp = nullptr;
    Widget* w = Live();
    if (!w) return UIA_E_ELEMENTNOTAVAILABLE;
    UiaState& s = *s_;
    Widget* target = nullptr;
    switch (dir) {
        case NavigateDirection_Parent:
            if (w == s.host->Root()) target = nullptr;
            else if (w->Parent()) target = w->Parent();
            else if (UiaIsPopupRoot(s, w)) target = s.host->Root();
            break;
        case NavigateDirection_FirstChild:
        case NavigateDirection_LastChild: {
            auto kids = UiaChildrenOf(s, w);
            if (!kids.empty()) target = dir == NavigateDirection_FirstChild ? kids.front() : kids.back();
            break;
        }
        case NavigateDirection_NextSibling:
        case NavigateDirection_PreviousSibling: {
            Widget* parent = w->Parent();
            if (!parent && UiaIsPopupRoot(s, w)) parent = s.host->Root();
            if (!parent) break;
            auto kids = UiaChildrenOf(s, parent);
            auto it = std::find(kids.begin(), kids.end(), w);
            if (it == kids.end()) break;
            if (dir == NavigateDirection_NextSibling) {
                if (++it != kids.end()) target = *it;
            } else if (it != kids.begin()) {
                target = *--it;
            }
            break;
        }
        default: return E_INVALIDARG;
    }
    return FragmentOut(s, target, pp);
}

HRESULT WidgetProvider::GetRuntimeId(SAFEARRAY** pp) {
    if (!pp) return E_POINTER;
    *pp = nullptr;
    Widget* w = Live();
    if (!w) return UIA_E_ELEMENTNOTAVAILABLE;
    return MakeRuntimeId(w->Id(), pp);
}

HRESULT WidgetProvider::get_BoundingRectangle(UiaRect* r) {
    if (!r) return E_POINTER;
    *r = {};
    Widget* w = Live();
    if (!w) return UIA_E_ELEMENTNOTAVAILABLE;
    float sc = s_->Scale();
    POINT o = s_->ClientOrigin();
    const RectF& b = w->Bounds();
    r->left = o.x + std::round(b.l * sc);
    r->top = o.y + std::round(b.t * sc);
    r->width = std::round(b.w() * sc);
    r->height = std::round(b.h() * sc);
    return S_OK;
}

HRESULT WidgetProvider::GetEmbeddedFragmentRoots(SAFEARRAY** pp) {
    if (!pp) return E_POINTER;
    *pp = nullptr;
    return S_OK;
}

HRESULT WidgetProvider::SetFocus() {
    Widget* w = Live();
    if (!w) return UIA_E_ELEMENTNOTAVAILABLE;
    w->Focus();
    return S_OK;
}

HRESULT WidgetProvider::get_FragmentRoot(IRawElementProviderFragmentRoot** pp) {
    if (!pp) return E_POINTER;
    *pp = nullptr;
    if (!Live()) return UIA_E_ELEMENTNOTAVAILABLE;
    return s_->root->QueryInterface(__uuidof(IRawElementProviderFragmentRoot), reinterpret_cast<void**>(pp));
}

Widget* WidgetProvider::Mutable(HRESULT& hr) const {
    Widget* w = Live();
    if (!w) { hr = UIA_E_ELEMENTNOTAVAILABLE; return nullptr; }
    if (!w->IsEffectivelyEnabled()) { hr = UIA_E_ELEMENTNOTENABLED; return nullptr; }
    hr = S_OK;
    return w;
}

#define PUI_MUTABLE(w)  \
    HRESULT hr_ = S_OK; \
    Widget* w = Mutable(hr_); \
    if (!w) return hr_;
#define PUI_LIVE(w) \
    Widget* w = Live(); \
    if (!w) return UIA_E_ELEMENTNOTAVAILABLE;

HRESULT WidgetProvider::Invoke() {
    PUI_MUTABLE(w)
    if (!w->SupportsInvoke()) return UIA_E_NOTSUPPORTED;
    w->Invoke();
    return S_OK;
}

HRESULT WidgetProvider::Toggle() {
    PUI_MUTABLE(w)
    if (!w->SupportsToggle()) return UIA_E_NOTSUPPORTED;
    w->Toggle();
    return S_OK;
}

HRESULT WidgetProvider::get_ToggleState(ToggleState* st) {
    if (!st) return E_POINTER;
    PUI_LIVE(w)
    int s = w->ToggleState();
    *st = s == 1 ? ToggleState_On : s == 2 ? ToggleState_Indeterminate : ToggleState_Off;
    return S_OK;
}

HRESULT WidgetProvider::SetValue(double v) {
    PUI_MUTABLE(w)
    if (!w->SupportsRangeValue()) return UIA_E_NOTSUPPORTED;
    RangeInfo ri = w->GetRange();
    if (ri.readOnly) return UIA_E_NOTSUPPORTED;
    if (std::isnan(v)) return E_INVALIDARG;
    w->SetRangeValue(std::clamp(v, ri.minimum, ri.maximum));
    return S_OK;
}

#define PUI_RANGE_GETTER(name, field)                         \
    HRESULT WidgetProvider::name(double* p) {                 \
        if (!p) return E_POINTER;                             \
        *p = 0;                                               \
        PUI_LIVE(w)                                           \
        if (!w->SupportsRangeValue()) return UIA_E_NOTSUPPORTED; \
        *p = w->GetRange().field;                             \
        return S_OK;                                          \
    }
PUI_RANGE_GETTER(get_Value, value)
PUI_RANGE_GETTER(get_Minimum, minimum)
PUI_RANGE_GETTER(get_Maximum, maximum)
PUI_RANGE_GETTER(get_LargeChange, largeChange)
PUI_RANGE_GETTER(get_SmallChange, smallChange)

HRESULT WidgetProvider::SetValue(LPCWSTR v) {
    PUI_MUTABLE(w)
    if (!w->SupportsValue()) return UIA_E_NOTSUPPORTED;
    if (w->ValueReadOnly()) return UIA_E_NOTSUPPORTED;
    w->SetValue(v ? v : L"");
    return S_OK;
}

HRESULT WidgetProvider::get_Value(BSTR* p) {
    if (!p) return E_POINTER;
    *p = nullptr;
    PUI_LIVE(w)
    std::wstring s = w->GetValue();
    *p = SysAllocStringLen(s.c_str(), (UINT)s.size());
    return S_OK;
}

HRESULT WidgetProvider::get_IsReadOnly(BOOL* p) {
    if (!p) return E_POINTER;
    PUI_LIVE(w)
    bool ro = (w->SupportsRangeValue() && !w->SupportsValue()) ? w->GetRange().readOnly : w->ValueReadOnly();
    *p = ro ? TRUE : FALSE;
    return S_OK;
}

HRESULT WidgetProvider::Expand() {
    PUI_MUTABLE(w)
    if (!w->SupportsExpandCollapse()) return UIA_E_NOTSUPPORTED;
    w->Expand();
    return S_OK;
}

HRESULT WidgetProvider::Collapse() {
    PUI_MUTABLE(w)
    if (!w->SupportsExpandCollapse()) return UIA_E_NOTSUPPORTED;
    w->Collapse();
    return S_OK;
}

HRESULT WidgetProvider::get_ExpandCollapseState(ExpandCollapseState* p) {
    if (!p) return E_POINTER;
    PUI_LIVE(w)
    *p = w->IsExpanded() ? ExpandCollapseState_Expanded : ExpandCollapseState_Collapsed;
    return S_OK;
}

HRESULT WidgetProvider::GetSelection(SAFEARRAY** pp) {
    if (!pp) return E_POINTER;
    *pp = nullptr;
    PUI_LIVE(w)
    std::vector<Widget*> sel = w->SelectedChildren();
    SAFEARRAY* sa = SafeArrayCreateVector(VT_UNKNOWN, 0, (ULONG)sel.size());
    if (!sa) return E_OUTOFMEMORY;
    for (LONG i = 0; i < (LONG)sel.size(); i++) {
        IRawElementProviderSimple* p = UiaProviderOf(*s_, sel[(size_t)i]);
        if (p) SafeArrayPutElement(sa, &i, p);   // AddRefs
    }
    *pp = sa;
    return S_OK;
}

HRESULT WidgetProvider::get_CanSelectMultiple(BOOL* p) {
    if (!p) return E_POINTER;
    PUI_LIVE(w)
    *p = w->CanSelectMultiple() ? TRUE : FALSE;
    return S_OK;
}

HRESULT WidgetProvider::get_IsSelectionRequired(BOOL* p) {
    if (!p) return E_POINTER;
    PUI_LIVE(w)
    *p = w->IsSelectionRequired() ? TRUE : FALSE;
    return S_OK;
}

HRESULT WidgetProvider::Select() {
    PUI_MUTABLE(w)
    if (!w->SupportsSelectionItem()) return UIA_E_NOTSUPPORTED;
    w->SelectItem();
    return S_OK;
}

HRESULT WidgetProvider::AddToSelection() {
    PUI_MUTABLE(w)
    if (!w->SupportsSelectionItem()) return UIA_E_NOTSUPPORTED;
    w->AddToSelection();
    return S_OK;
}

HRESULT WidgetProvider::RemoveFromSelection() {
    PUI_MUTABLE(w)
    if (!w->SupportsSelectionItem()) return UIA_E_NOTSUPPORTED;
    w->RemoveFromSelection();
    return S_OK;
}

HRESULT WidgetProvider::get_IsSelected(BOOL* p) {
    if (!p) return E_POINTER;
    PUI_LIVE(w)
    *p = w->IsSelected() ? TRUE : FALSE;
    return S_OK;
}

HRESULT WidgetProvider::get_SelectionContainer(IRawElementProviderSimple** pp) {
    if (!pp) return E_POINTER;
    *pp = nullptr;
    PUI_LIVE(w)
    Widget* c = w->SelectionContainer();
    if (!c) return S_OK;
    IRawElementProviderSimple* p = UiaProviderOf(*s_, c);
    if (p) {
        exposed_ = true;
        p->AddRef();
        *pp = p;
    }
    return S_OK;
}

HRESULT WidgetProvider::Scroll(ScrollAmount h, ScrollAmount v) {
    PUI_MUTABLE(w)
    if (!w->SupportsScroll()) return UIA_E_NOTSUPPORTED;
    if (h != ScrollAmount_NoAmount) return UIA_E_NOTSUPPORTED;   // no horizontal scrolling
    if (v == ScrollAmount_NoAmount) return S_OK;
    double cur = w->VerticalScrollPercent();
    if (cur < 0) return UIA_E_NOTSUPPORTED;
    double view = std::clamp(w->VerticalViewSize(), 0.0, 100.0);
    double scrollable = 100.0 - view;
    if (scrollable <= 0) return S_OK;
    double large = view / scrollable * 100.0, smallStep = large * 0.1;
    double d = 0;
    switch (v) {
        case ScrollAmount_LargeDecrement: d = -large; break;
        case ScrollAmount_SmallDecrement: d = -smallStep; break;
        case ScrollAmount_LargeIncrement: d = large; break;
        case ScrollAmount_SmallIncrement: d = smallStep; break;
        default: break;
    }
    w->SetVerticalScrollPercent(std::clamp(cur + d, 0.0, 100.0));
    return S_OK;
}

HRESULT WidgetProvider::SetScrollPercent(double h, double v) {
    PUI_MUTABLE(w)
    if (!w->SupportsScroll()) return UIA_E_NOTSUPPORTED;
    if (h != UIA_ScrollPatternNoScroll) return UIA_E_NOTSUPPORTED;
    if (v == UIA_ScrollPatternNoScroll) return S_OK;
    if (std::isnan(v) || v < 0 || v > 100) return E_INVALIDARG;
    if (w->VerticalScrollPercent() < 0) return UIA_E_NOTSUPPORTED;
    w->SetVerticalScrollPercent(v);
    return S_OK;
}

HRESULT WidgetProvider::get_HorizontalScrollPercent(double* p) {
    if (!p) return E_POINTER;
    PUI_LIVE(w)
    (void)w;
    *p = UIA_ScrollPatternNoScroll;
    return S_OK;
}
HRESULT WidgetProvider::get_VerticalScrollPercent(double* p) {
    if (!p) return E_POINTER;
    PUI_LIVE(w)
    *p = w->VerticalScrollPercent();
    return S_OK;
}
HRESULT WidgetProvider::get_HorizontalViewSize(double* p) {
    if (!p) return E_POINTER;
    PUI_LIVE(w)
    (void)w;
    *p = 100.0;
    return S_OK;
}
HRESULT WidgetProvider::get_VerticalViewSize(double* p) {
    if (!p) return E_POINTER;
    PUI_LIVE(w)
    *p = w->VerticalViewSize();
    return S_OK;
}
HRESULT WidgetProvider::get_HorizontallyScrollable(BOOL* p) {
    if (!p) return E_POINTER;
    PUI_LIVE(w)
    (void)w;
    *p = FALSE;
    return S_OK;
}
HRESULT WidgetProvider::get_VerticallyScrollable(BOOL* p) {
    if (!p) return E_POINTER;
    PUI_LIVE(w)
    *p = (w->SupportsScroll() && w->VerticalScrollPercent() >= 0) ? TRUE : FALSE;
    return S_OK;
}

}
