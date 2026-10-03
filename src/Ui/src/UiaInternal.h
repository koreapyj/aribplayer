#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

#include "pui/Uia.h"

namespace pui {

class WidgetProvider;
class RootProvider;

extern std::atomic<long> g_uiaLiveProviders;

struct UiaBridgeState {
    HWND hwnd = nullptr;
    UiHost* host = nullptr;
    std::function<float()> scale;
    bool alive = true;
    bool alwaysRaise = false;
    POINT origin{0, 0};
    int listener = 0;
    unsigned handled = 0, raised = 0;
    std::function<void(const wchar_t*, Widget*)> hook;
    std::function<bool(Widget*)> offscreen;   // app-defined "treated as off-screen" (e.g. the auto-hidden bar)
    std::vector<WidgetProvider*> providers;   // registry (raw; each holds a cache reference owned by the bridge)
    RootProvider* root = nullptr;             // one reference owned by the bridge
    std::weak_ptr<UiaBridgeState> self;

    float Scale() const {
        float s = scale ? scale() : 1.f;
        return s > 0 ? s : 1.f;
    }
    POINT ClientOrigin() const {
        if (hwnd) {
            POINT p{0, 0};
            ClientToScreen(hwnd, &p);
            return p;
        }
        return origin;
    }
    bool ClientsListening() const { return alwaysRaise || UiaClientsAreListening(); }
};

using UiaState = UiaBridgeState;

const wchar_t* UiaRoleName(Role r);
/// Widget is not visible, is excluded by the app predicate, or lies outside the host area.
bool UiaIsOffscreen(const UiaState& s, Widget* w);
const wchar_t* UiaControlTypeName(int id);

inline VARIANT VEmpty() { VARIANT v; VariantInit(&v); return v; }
inline VARIANT VInt(int x) { VARIANT v; VariantInit(&v); v.vt = VT_I4; v.lVal = x; return v; }
inline VARIANT VBool(bool b) { VARIANT v; VariantInit(&v); v.vt = VT_BOOL; v.boolVal = b ? VARIANT_TRUE : VARIANT_FALSE; return v; }
inline VARIANT VStr(const std::wstring& s) { VARIANT v; VariantInit(&v); v.vt = VT_BSTR; v.bstrVal = SysAllocStringLen(s.c_str(), (UINT)s.size()); return v; }
inline VARIANT VDouble(double d) { VARIANT v; VariantInit(&v); v.vt = VT_R8; v.dblVal = d; return v; }

bool UiaIsPopupRoot(const UiaState& s, Widget* w);
std::vector<Widget*> UiaChildrenOf(const UiaState& s, Widget* w);
/// Borrowed provider for a widget (root widget -> fragment root). Creates it on demand. nullptr when shut down.
IRawElementProviderSimple* UiaProviderOf(UiaState& s, Widget* w);

class RootProvider final : public IRawElementProviderSimple,
                           public IRawElementProviderFragment,
                           public IRawElementProviderFragmentRoot {
public:
    explicit RootProvider(std::shared_ptr<UiaState> s) : s_(std::move(s)) { g_uiaLiveProviders++; }
    ~RootProvider() { g_uiaLiveProviders--; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** pp) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        long r = --ref_;
        if (r == 0) delete this;
        return (ULONG)r;
    }
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* o) override;
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID, IUnknown** pp) override;
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID id, VARIANT* v) override;
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** pp) override;
    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection dir, IRawElementProviderFragment** pp) override;
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** pp) override;
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* r) override;
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** pp) override;
    HRESULT STDMETHODCALLTYPE SetFocus() override;
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** pp) override;
    HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** pp) override;
    HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** pp) override;

    Widget* Live() const { return s_->alive && s_->host ? s_->host->Root() : nullptr; }
    bool exposed_ = false;

private:
    std::shared_ptr<UiaState> s_;
    std::atomic<long> ref_{1};
};

class WidgetProvider final : public IRawElementProviderSimple,
                             public IRawElementProviderFragment,
                             public IInvokeProvider,
                             public IToggleProvider,
                             public IRangeValueProvider,
                             public IValueProvider,
                             public IExpandCollapseProvider,
                             public ISelectionProvider,
                             public ISelectionItemProvider,
                             public IScrollProvider {
public:
    WidgetProvider(std::shared_ptr<UiaState> s, Widget* w) : s_(std::move(s)), w_(w), life_(w->LifeToken()) {
        g_uiaLiveProviders++;
        w->uiaSlot = this;
        s_->providers.push_back(this);
    }
    ~WidgetProvider() {
        if (!life_.expired() && w_->uiaSlot == this) w_->uiaSlot = nullptr;
        auto& v = s_->providers;
        v.erase(std::remove(v.begin(), v.end(), this), v.end());
        g_uiaLiveProviders--;
    }

    bool exposed_ = false;
    bool dead_ = false;
    bool cacheRef_ = true;   // the bridge's reference (the refcount starts at 1)

    void Kill() {
        dead_ = true;
        if (!life_.expired() && w_->uiaSlot == this) w_->uiaSlot = nullptr;
    }
    void DropCacheRef() {
        if (cacheRef_) {
            cacheRef_ = false;
            Release();
        }
    }
    bool WidgetGone() const { return life_.expired() || (s_->host && w_->Host() != s_->host); }
    Widget* Live() const {
        if (dead_ || !s_->alive || !s_->host || WidgetGone()) return nullptr;
        return w_;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** pp) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        long r = --ref_;
        if (r == 0) delete this;
        return (ULONG)r;
    }
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* o) override;
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID id, IUnknown** pp) override;
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID id, VARIANT* v) override;
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** pp) override;
    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection dir, IRawElementProviderFragment** pp) override;
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** pp) override;
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* r) override;
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** pp) override;
    HRESULT STDMETHODCALLTYPE SetFocus() override;
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** pp) override;
    HRESULT STDMETHODCALLTYPE Invoke() override;
    HRESULT STDMETHODCALLTYPE Toggle() override;
    HRESULT STDMETHODCALLTYPE get_ToggleState(ToggleState* st) override;
    HRESULT STDMETHODCALLTYPE SetValue(double v) override;
    HRESULT STDMETHODCALLTYPE get_Value(double* p) override;
    HRESULT STDMETHODCALLTYPE get_Minimum(double* p) override;
    HRESULT STDMETHODCALLTYPE get_Maximum(double* p) override;
    HRESULT STDMETHODCALLTYPE get_LargeChange(double* p) override;
    HRESULT STDMETHODCALLTYPE get_SmallChange(double* p) override;
    // Value (get_IsReadOnly is shared with RangeValue)
    HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR v) override;
    HRESULT STDMETHODCALLTYPE get_Value(BSTR* p) override;
    HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* p) override;
    HRESULT STDMETHODCALLTYPE Expand() override;
    HRESULT STDMETHODCALLTYPE Collapse() override;
    HRESULT STDMETHODCALLTYPE get_ExpandCollapseState(ExpandCollapseState* p) override;
    HRESULT STDMETHODCALLTYPE GetSelection(SAFEARRAY** pp) override;
    HRESULT STDMETHODCALLTYPE get_CanSelectMultiple(BOOL* p) override;
    HRESULT STDMETHODCALLTYPE get_IsSelectionRequired(BOOL* p) override;
    HRESULT STDMETHODCALLTYPE Select() override;
    HRESULT STDMETHODCALLTYPE AddToSelection() override;
    HRESULT STDMETHODCALLTYPE RemoveFromSelection() override;
    HRESULT STDMETHODCALLTYPE get_IsSelected(BOOL* p) override;
    HRESULT STDMETHODCALLTYPE get_SelectionContainer(IRawElementProviderSimple** pp) override;
    HRESULT STDMETHODCALLTYPE Scroll(ScrollAmount h, ScrollAmount v) override;
    HRESULT STDMETHODCALLTYPE SetScrollPercent(double h, double v) override;
    HRESULT STDMETHODCALLTYPE get_HorizontalScrollPercent(double* p) override;
    HRESULT STDMETHODCALLTYPE get_VerticalScrollPercent(double* p) override;
    HRESULT STDMETHODCALLTYPE get_HorizontalViewSize(double* p) override;
    HRESULT STDMETHODCALLTYPE get_VerticalViewSize(double* p) override;
    HRESULT STDMETHODCALLTYPE get_HorizontallyScrollable(BOOL* p) override;
    HRESULT STDMETHODCALLTYPE get_VerticallyScrollable(BOOL* p) override;

private:
    Widget* Mutable(HRESULT& hr) const;
    bool Offscreen(Widget* w) const;

    std::shared_ptr<UiaState> s_;
    Widget* w_;
    std::weak_ptr<void> life_;
    std::atomic<long> ref_{1};
};

}
