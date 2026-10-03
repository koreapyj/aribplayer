#include <wrl/client.h>

#include "UiaInternal.h"

namespace pui {

namespace {

using Microsoft::WRL::ComPtr;

IRawElementProviderSimple* EventProvider(UiaState& s, Widget* w) {
    if (w && w->Host() == s.host) return UiaProviderOf(s, w);
    return UiaProviderOf(s, s.host->Root());
}

void RaiseAuto(UiaState& s, Widget* w, EVENTID id, const wchar_t* desc) {
    if (!s.ClientsListening()) return;
    IRawElementProviderSimple* p = EventProvider(s, w);
    if (!p) return;
    s.raised++;
    if (s.hook) s.hook(desc, w);
    UiaRaiseAutomationEvent(p, id);
}

void RaiseProp(UiaState& s, Widget* w, PROPERTYID id, VARIANT nv, const wchar_t* desc) {
    if (s.ClientsListening()) {
        if (IRawElementProviderSimple* p = EventProvider(s, w)) {
            s.raised++;
            if (s.hook) s.hook(desc, w);
            UiaRaiseAutomationPropertyChangedEvent(p, id, VEmpty(), nv);
        }
    }
    VariantClear(&nv);
}

void RaiseStructure(UiaState& s, Widget* w) {
    if (!s.ClientsListening()) return;
    IRawElementProviderSimple* p = EventProvider(s, w);
    if (!p) return;
    s.raised++;
    if (s.hook) s.hook(L"Event:Structure", w);
    UiaRaiseStructureChangedEvent(p, StructureChangeType_ChildrenInvalidated, nullptr, 0);
}

/// Drops providers whose widget died or left the host (no provider may stay reachable for a dead widget).
void Purge(UiaState& s) {
    std::vector<WidgetProvider*> stale;
    for (WidgetProvider* p : s.providers)
        if (p->cacheRef_ && p->WidgetGone()) stale.push_back(p);
    if (stale.empty()) return;
    bool real = UiaClientsAreListening();
    for (WidgetProvider* p : stale) p->AddRef();   // keep alive across disconnect/release
    for (WidgetProvider* p : stale) {
        if (real && p->exposed_) UiaDisconnectProvider(p);
        p->Kill();
        p->DropCacheRef();
    }
    for (WidgetProvider* p : stale) p->Release();
}

void HandleEvent(UiaState& s, Widget* w, UiEvent ev) {
    if (!s.alive) return;
    s.handled++;
    switch (ev) {
        case UiEvent::PopupOpened:
            if (w) {   // popups are enumerated from UiHost::Popups(), so popups opened before the bridge existed are listed too
                if (w->GetRole() == Role::ToolTip) RaiseAuto(s, w, UIA_ToolTipOpenedEventId, L"Event:ToolTipOpened");
                else RaiseStructure(s, s.host->Root());
            }
            break;
        case UiEvent::PopupClosed:
            Purge(s);
            RaiseStructure(s, s.host->Root());
            break;
        case UiEvent::StructureChanged:
            Purge(s);
            RaiseStructure(s, w ? w : s.host->Root());
            break;
        case UiEvent::FocusChanged:
            if (w) RaiseAuto(s, w, UIA_AutomationFocusChangedEventId, L"Event:Focus");
            break;
        case UiEvent::Invoked:
            if (w) RaiseAuto(s, w, UIA_Invoke_InvokedEventId, L"Event:Invoke");
            break;
        case UiEvent::ToggleChanged:
            if (w) {
                int st = w->ToggleState();
                RaiseProp(s, w, UIA_ToggleToggleStatePropertyId,
                          VInt(st == 1 ? ToggleState_On : st == 2 ? ToggleState_Indeterminate : ToggleState_Off),
                          L"Prop:ToggleState");
            }
            break;
        case UiEvent::ValueChanged:
            if (w) {
                if (w->SupportsRangeValue())
                    RaiseProp(s, w, UIA_RangeValueValuePropertyId, VDouble(w->GetRange().value), L"Prop:RangeValue");
                if (w->SupportsValue())
                    RaiseProp(s, w, UIA_ValueValuePropertyId, VStr(w->GetValue()), L"Prop:Value");
            }
            break;
        case UiEvent::ExpandCollapseChanged:
            if (w)
                RaiseProp(s, w, UIA_ExpandCollapseExpandCollapseStatePropertyId,
                          VInt(w->IsExpanded() ? ExpandCollapseState_Expanded : ExpandCollapseState_Collapsed),
                          L"Prop:ExpandCollapseState");
            break;
        case UiEvent::SelectionChanged:
            if (w) {
                if (w->SupportsSelectionItem())
                    w->IsSelected() ? RaiseAuto(s, w, UIA_SelectionItem_ElementSelectedEventId, L"Event:ElementSelected")
                                    : RaiseAuto(s, w, UIA_SelectionItem_ElementRemovedFromSelectionEventId,
                                                L"Event:ElementRemovedFromSelection");
                if (w->SupportsSelection())
                    RaiseAuto(s, w, UIA_Selection_InvalidatedEventId, L"Event:SelectionInvalidated");
            }
            break;
        case UiEvent::NameChanged:
            if (w) RaiseProp(s, w, UIA_NamePropertyId, VStr(w->Name()), L"Prop:Name");
            break;
        case UiEvent::EnabledChanged:
            if (w) RaiseProp(s, w, UIA_IsEnabledPropertyId, VBool(w->IsEffectivelyEnabled()), L"Prop:IsEnabled");
            break;
        case UiEvent::VisibilityChanged:
            if (w) {
                RaiseProp(s, w, UIA_IsOffscreenPropertyId, VBool(!w->IsEffectivelyVisible()), L"Prop:IsOffscreen");
                if (w->Parent()) RaiseStructure(s, w->Parent());
            }
            break;
        case UiEvent::MenuOpened:
            if (w) RaiseAuto(s, w, UIA_MenuOpenedEventId, L"Event:MenuOpened");
            break;
        case UiEvent::MenuClosed:
            if (w) RaiseAuto(s, w, UIA_MenuClosedEventId, L"Event:MenuClosed");
            break;
    }
}

}

UiaBridge::UiaBridge(HWND hwnd, UiHost& host, std::function<float()> scale) {
    s_ = std::make_shared<UiaBridgeState>();
    s_->self = s_;
    s_->hwnd = hwnd;
    s_->host = &host;
    s_->scale = std::move(scale);
    s_->root = new RootProvider(s_);
    std::weak_ptr<UiaBridgeState> weak = s_;
    s_->listener = host.AddListener([weak](Widget* w, UiEvent ev) {
        if (auto s = weak.lock()) HandleEvent(*s, w, ev);
    });
}

UiaBridge::~UiaBridge() { Shutdown(); }

bool UiaBridge::IsShutdown() const { return !s_->alive; }

bool UiaBridge::HandleGetObject(WPARAM wParam, LPARAM lParam, LRESULT* result) {
    if (!s_->alive || !s_->hwnd || !result) return false;
    if (static_cast<LONG>(lParam) != static_cast<LONG>(UiaRootObjectId)) return false;
    *result = UiaReturnRawElementProvider(s_->hwnd, wParam, lParam, static_cast<IRawElementProviderSimple*>(s_->root));
    s_->root->exposed_ = true;
    return true;
}

IRawElementProviderFragmentRoot* UiaBridge::Root() {
    return s_->alive && s_->root ? static_cast<IRawElementProviderFragmentRoot*>(s_->root) : nullptr;
}

IRawElementProviderSimple* UiaBridge::ProviderFor(Widget* w) { return UiaProviderOf(*s_, w); }

void UiaBridge::Shutdown() {
    UiaState& s = *s_;
    if (!s.alive) return;
    if (s.host && s.listener) s.host->RemoveListener(s.listener);
    s.listener = 0;

    std::vector<WidgetProvider*> all = s.providers;
    for (WidgetProvider* p : all) p->AddRef();
    for (WidgetProvider* p : all)
        if (p->exposed_) UiaDisconnectProvider(p);
    RootProvider* root = s.root;
    root->AddRef();
    if (root->exposed_) UiaDisconnectProvider(static_cast<IRawElementProviderSimple*>(root));
    if (s.hwnd) UiaReturnRawElementProvider(s.hwnd, 0, 0, nullptr);

    s.alive = false;
    for (WidgetProvider* p : all) {
        p->Kill();
        p->DropCacheRef();
    }
    for (WidgetProvider* p : all) p->Release();
    s.root = nullptr;
    root->Release();   // our temporary reference
    root->Release();   // the bridge's reference
}

void UiaBridge::SetOffscreenPredicate(std::function<bool(Widget*)> pred) { s_->offscreen = std::move(pred); }
void UiaBridge::NotifyOffscreenChanged(Widget* w) {
    if (!s_->alive || !w) return;
    RaiseProp(*s_, w, UIA_IsOffscreenPropertyId, VBool(UiaIsOffscreen(*s_, w)), L"Prop:IsOffscreen");
}
void UiaBridge::SetClientOrigin(int x, int y) { s_->origin = POINT{x, y}; }
void UiaBridge::SetAlwaysRaise(bool on) { s_->alwaysRaise = on; }
void UiaBridge::SetRaisedHook(std::function<void(const wchar_t*, Widget*)> h) { s_->hook = std::move(h); }
unsigned UiaBridge::EventsHandled() const { return s_->handled; }
unsigned UiaBridge::EventsRaised() const { return s_->raised; }
int UiaBridge::ListenerToken() const { return s_->listener; }
size_t UiaBridge::CachedProviderCount() const {
    return (size_t)std::count_if(s_->providers.begin(), s_->providers.end(), [](WidgetProvider* p) { return p->cacheRef_; });
}
long UiaBridge::LiveProviderCount() { return g_uiaLiveProviders.load(); }

namespace {

void DumpNode(IRawElementProviderFragment* f, int depth, int maxDepth, std::wstring& out) {
    ComPtr<IRawElementProviderSimple> simple;
    if (FAILED(f->QueryInterface(__uuidof(IRawElementProviderSimple), reinterpret_cast<void**>(simple.GetAddressOf()))))
        return;
    int ct = UIA_WindowControlTypeId;
    std::wstring name;
    VARIANT v;
    if (SUCCEEDED(simple->GetPropertyValue(UIA_ControlTypePropertyId, &v))) {
        if (v.vt == VT_I4) ct = v.lVal;
        VariantClear(&v);
    }
    if (SUCCEEDED(simple->GetPropertyValue(UIA_NamePropertyId, &v))) {
        if (v.vt == VT_BSTR && v.bstrVal) name = v.bstrVal;
        VariantClear(&v);
    }
    static const struct { PATTERNID id; const wchar_t* name; } kPatterns[] = {
        {UIA_InvokePatternId, L"Invoke"},           {UIA_TogglePatternId, L"Toggle"},
        {UIA_RangeValuePatternId, L"RangeValue"},   {UIA_ValuePatternId, L"Value"},
        {UIA_ExpandCollapsePatternId, L"ExpandCollapse"}, {UIA_SelectionPatternId, L"Selection"},
        {UIA_SelectionItemPatternId, L"SelectionItem"},   {UIA_ScrollPatternId, L"Scroll"}};
    std::wstring pats;
    for (auto& p : kPatterns) {
        ComPtr<IUnknown> u;
        if (SUCCEEDED(simple->GetPatternProvider(p.id, u.GetAddressOf())) && u) {
            if (!pats.empty()) pats += L",";
            pats += p.name;
        }
    }
    out.append((size_t)depth * 2, L' ');
    out += UiaControlTypeName(ct);
    out += L" \"" + name + L"\"";
    if (!pats.empty()) out += L" [" + pats + L"]";
    out += L"\n";
    if (depth >= maxDepth) return;
    ComPtr<IRawElementProviderFragment> child;
    f->Navigate(NavigateDirection_FirstChild, child.GetAddressOf());
    while (child) {
        DumpNode(child.Get(), depth + 1, maxDepth, out);
        ComPtr<IRawElementProviderFragment> next;
        child->Navigate(NavigateDirection_NextSibling, next.GetAddressOf());
        child = std::move(next);
    }
}

}

std::wstring DumpUiaTree(IRawElementProviderFragment* root, int maxDepth) {
    std::wstring out;
    if (root) DumpNode(root, 0, maxDepth, out);
    return out;
}

}
