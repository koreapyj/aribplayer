#pragma once
// Window-independent: everything here is testable without an HWND (drive UiHost with synthetic events in tests).
//  * Bounds are absolute DIPs in client space. Containers position children in OnLayout() (called by SetBounds
//    when the rect changes, or by RequestLayout()).
//  * Painting: UiHost::Paint(renderer) paints root, then popups (bottom..top), then the tooltip.
//    Widget::PaintTree paints Paint() then visible children in order (optionally clipped).
//  * Input: UiHost routes pointer events to the hit widget (or the capture widget); unhandled events bubble to
//    parents. Keyboard goes to the focused widget and bubbles; Tab/Shift+Tab move focus when unhandled;
//    Escape closes the top light-dismiss popup when unhandled.
//  * Automation: each widget exposes Role/Name and optional patterns via virtuals. The UIA layer (pui/Uia.h)
//    wraps widgets in IRawElementProviderFragment objects and listens to UiHost UI events.
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Geometry.h"
#include "Renderer.h"
#include "Theme.h"

namespace pui {

class UiHost;

enum class Role {
    Window, Pane, Group, Text, Image, Button, ToggleButton, CheckBox, RadioButton, Slider, ComboBox, List, ListItem,
    Menu, MenuItem, ToolTip, ScrollBar, Edit, Tree, TreeItem, Tab, TabItem, ProgressBar, StatusBar, Hyperlink,
    Separator, Custom
};

enum class PointerButton { None, Left, Right };

struct PointerEvent {
    PointF pt{};
    PointerButton button = PointerButton::Left;
    int clickCount = 1;        // 2 = double click
    unsigned mods = ModNone;
    float wheelDelta = 0;      // +1.0 per notch (WHEEL_DELTA), positive = away from the user
};

enum class UiEvent {
    FocusChanged,              // widget = newly focused (may be nullptr)
    StructureChanged,          // widget = parent whose children changed
    NameChanged, ValueChanged, ToggleChanged, ExpandCollapseChanged, SelectionChanged, EnabledChanged,
    VisibilityChanged, Invoked, PopupOpened, PopupClosed, MenuOpened, MenuClosed
};

struct RangeInfo {
    double minimum = 0, maximum = 100, value = 0, smallChange = 1, largeChange = 10;
    bool readOnly = false;
};

class Widget {
public:
    Widget() = default;
    explicit Widget(Role r) : role_(r) {}
    virtual ~Widget();
    Widget(const Widget&) = delete;
    Widget& operator=(const Widget&) = delete;

    Widget* Parent() const { return parent_; }
    const std::vector<std::unique_ptr<Widget>>& Children() const { return children_; }
    Widget* AddChild(std::unique_ptr<Widget> child, int index = -1);   // raises StructureChanged
    template <class T, class... A> T* Add(A&&... args) {
        auto p = std::make_unique<T>(std::forward<A>(args)...);
        T* raw = p.get();
        AddChild(std::move(p));
        return raw;
    }
    std::unique_ptr<Widget> RemoveChild(Widget* child);              // fixes focus/hover/capture; raises StructureChanged
    void ClearChildren();
    int IndexInParent() const;
    UiHost* Host() const;                                             // root's host (nullptr when detached)
    bool IsAncestorOf(const Widget* w) const;                         // true when w is this or a descendant

    const RectF& Bounds() const { return bounds_; }
    void SetBounds(const RectF& r);                                   // calls OnLayout when changed
    void RequestLayout() { OnLayout(); Invalidate(); }
    virtual void OnLayout() {}
    /// Preferred size for auto-sizing containers (DIP). Default = current bounds size.
    virtual SizeF DesiredSize(IRenderer* measurer) { (void)measurer; return {bounds_.w(), bounds_.h()}; }

    bool Visible() const { return visible_; }
    void SetVisible(bool v);
    bool IsEffectivelyVisible() const;                                // this and all ancestors visible
    bool Enabled() const { return enabled_; }
    void SetEnabled(bool e);
    bool IsEffectivelyEnabled() const;
    bool Focusable() const { return focusable_; }
    void SetFocusable(bool f) { focusable_ = f; }
    bool CanFocus() const { return focusable_ && IsEffectivelyVisible() && IsEffectivelyEnabled(); }
    bool IsFocused() const;
    bool Focus();
    bool IsHovered() const { return hovered_; }
    bool IsPressed() const { return pressed_; }
    /// True when the focus visual should be drawn (focused via keyboard, not pointer).
    bool ShowFocusVisual() const;
    bool HitTestVisible() const { return hitTestVisible_; }
    void SetHitTestVisible(bool v) { hitTestVisible_ = v; }
    bool ClipChildren() const { return clipChildren_; }
    void SetClipChildren(bool c) { clipChildren_ = c; }

    Role GetRole() const { return role_; }
    void SetRole(Role r) { role_ = r; }
    const std::wstring& Name() const { return name_; }
    void SetName(std::wstring n);                                     // raises NameChanged
    const std::wstring& ToolTip() const { return tooltip_; }
    void SetToolTip(std::wstring t) { tooltip_ = std::move(t); }
    const std::wstring& AutomationId() const { return automationId_; }
    void SetAutomationId(std::wstring id) { automationId_ = std::move(id); }
    const std::wstring& HelpText() const { return helpText_; }
    void SetHelpText(std::wstring h) { helpText_ = std::move(h); }
    const std::wstring& AcceleratorKey() const { return accel_; }
    void SetAcceleratorKey(std::wstring a) { accel_ = std::move(a); }
    int Id() const { return id_; }                                    // unique per process (UIA runtime id)
    /// Lifetime token: weak_ptr observers (UIA providers) can detect destruction.
    std::weak_ptr<void> LifeToken() const { return life_; }

    void PaintTree(IRenderer& r, const Theme& t);
    virtual void Paint(IRenderer& r, const Theme& t) { (void)r; (void)t; }
    void Invalidate();
    /// Paints the standard Fluent focus rectangle (2 px outer + 1 px inner) around rect when ShowFocusVisual().
    void PaintFocusVisual(IRenderer& r, const Theme& t, const RectF& rect, float radius) const;

    virtual Widget* HitTest(PointF p);                                // deepest visible+hittable widget at p

    // Handlers return true when handled; unhandled events bubble to the parent.
    virtual bool OnPointerDown(const PointerEvent& e) { (void)e; return false; }
    virtual bool OnPointerMove(const PointerEvent& e) { (void)e; return false; }
    virtual bool OnPointerUp(const PointerEvent& e) { (void)e; return false; }
    virtual bool OnPointerWheel(const PointerEvent& e) { (void)e; return false; }
    virtual void OnPointerEnter() {}
    virtual void OnPointerLeave() {}
    virtual bool OnKeyDown(int vk, unsigned mods) { (void)vk; (void)mods; return false; }
    virtual bool OnKeyUp(int vk, unsigned mods) { (void)vk; (void)mods; return false; }
    virtual bool OnChar(wchar_t ch) { (void)ch; return false; }
    /// Context menu request (right click: keyboard=false, pt = pointer; Shift+F10/Apps: keyboard=true, pt = widget centre).
    virtual bool OnContextMenu(PointF pt, bool keyboard) { (void)pt; (void)keyboard; return false; }
    virtual void OnFocusChanged(bool focused) { (void)focused; }
    /// Animation tick (after UiHost::StartAnimating(this)); return false to stop ticking.
    virtual bool OnTick(uint64_t nowMs) { (void)nowMs; return false; }

    virtual bool SupportsInvoke() const { return false; }
    virtual void Invoke() {}
    virtual bool SupportsToggle() const { return false; }
    virtual int ToggleState() const { return 0; }                     // 0 off, 1 on, 2 indeterminate
    virtual void Toggle() {}
    virtual bool SupportsRangeValue() const { return false; }
    virtual RangeInfo GetRange() const { return {}; }
    virtual void SetRangeValue(double v) { (void)v; }
    virtual bool SupportsValue() const { return false; }
    virtual std::wstring GetValue() const { return {}; }
    virtual void SetValue(const std::wstring& v) { (void)v; }
    virtual bool ValueReadOnly() const { return true; }
    virtual bool SupportsExpandCollapse() const { return false; }
    virtual bool IsExpanded() const { return false; }
    virtual void Expand() {}
    virtual void Collapse() {}
    virtual bool SupportsSelection() const { return false; }
    virtual bool CanSelectMultiple() const { return false; }
    virtual bool IsSelectionRequired() const { return false; }
    virtual std::vector<Widget*> SelectedChildren() const { return {}; }
    virtual bool SupportsSelectionItem() const { return false; }
    virtual bool IsSelected() const { return false; }
    virtual void SelectItem() {}                                       // select only this
    virtual void AddToSelection() { SelectItem(); }
    virtual void RemoveFromSelection() {}
    virtual Widget* SelectionContainer() const { return parent_; }
    virtual bool SupportsScroll() const { return false; }
    virtual double VerticalScrollPercent() const { return -1; }       // -1 = UIA_ScrollPatternNoScroll
    virtual double VerticalViewSize() const { return 100; }
    virtual void SetVerticalScrollPercent(double pct) { (void)pct; }
    virtual void ScrollIntoView(Widget* child) { (void)child; }

    /// Raise a UI event to the host listeners (UIA bridge, app).
    void Raise(UiEvent ev);

    /// Opaque per-widget slot for the UIA layer (it stores its provider here). Not owned.
    void* uiaSlot = nullptr;

protected:
    void SetPressedState(bool p);
    Role role_ = Role::Custom;

private:
    friend class UiHost;
    Widget* parent_ = nullptr;
    UiHost* host_ = nullptr;   // set on root/popup roots only
    std::vector<std::unique_ptr<Widget>> children_;
    RectF bounds_{};
    bool visible_ = true, enabled_ = true, focusable_ = false, hitTestVisible_ = true, clipChildren_ = false;
    bool hovered_ = false, pressed_ = false;
    std::wstring name_, tooltip_, automationId_, helpText_, accel_;
    int id_ = NextId();
    std::shared_ptr<int> life_ = std::make_shared<int>(0);
    static int NextId();
};

struct PopupOptions {
    bool lightDismiss = true;      // click outside / Escape closes it
    bool modal = false;            // blocks input to everything below (dialogs); implies a smoke backdrop is painted by the app
    bool takeFocus = true;         // move focus to the first focusable widget inside
    bool acrylicSurface = false;   // this popup uses WinUI AcrylicInAppFillColorDefaultBrush over video
    float acrylicRadius = -1.f;    // < 0 uses Theme::overlayRadius; zero for square surfaces such as the video sidebar
    Widget* anchor = nullptr;      // widget that opened it (focus returns there on close)
    std::function<void()> onClosed;
};

struct PopupBackdropSurface {
    RectF bounds;                  // absolute client DIPs, same clamped rect the popup paints
    float cornerRadius = 0;
};

class UiHost {
public:
    UiHost();
    ~UiHost();
    UiHost(const UiHost&) = delete;
    UiHost& operator=(const UiHost&) = delete;

    Widget* SetRoot(std::unique_ptr<Widget> root);
    Widget* Root() const { return root_.get(); }
    void SetSize(SizeF dip);                                          // root bounds = {0,0,w,h}
    SizeF Size() const { return size_; }

    void SetTheme(const Theme& t);                                    // invalidates everything
    const Theme& GetTheme() const { return theme_; }

    // The host owns an open popup until it is closed.
    Widget* OpenPopup(std::unique_ptr<Widget> popup, PopupOptions opts = {});
    void ClosePopup(Widget* popup);                                    // no-op when not open
    void CloseAllPopups();
    Widget* TopPopup() const;
    size_t PopupCount() const { return popups_.size(); }
    bool HasModalPopup() const;
    bool IsPopupOpen(const Widget* popup) const;
    /// Open popups, bottom -> top (for the UIA bridge and diagnostics).
    std::vector<Widget*> Popups() const;
    /// Rects of explicitly tagged in-app Acrylic popups, in paint order. Modal surfaces suppress all underlying Acrylic.
    std::vector<PopupBackdropSurface> AcrylicPopupSurfaces() const;

    // Coordinates are DIP client space; return true when consumed by a widget or popup logic.
    bool PointerDown(const PointerEvent& e);
    bool PointerMove(const PointerEvent& e);
    bool PointerUp(const PointerEvent& e);
    bool PointerWheel(const PointerEvent& e);
    void PointerLeave();                                               // pointer left the window
    Widget* HitTest(PointF p) const;                                   // popups first (top to bottom), then root
    Widget* Hovered() const { return hovered_; }
    Widget* Captured() const { return capture_; }
    void SetCapture(Widget* w) { capture_ = w; }
    void ReleaseCapture() { capture_ = nullptr; }
    /// The pointer press was taken away (OS capture lost, window drag started): drops the capture AND the capture widget's pressed look.
    void CancelCapture();

    bool KeyDown(int vk, unsigned mods);
    bool KeyUp(int vk, unsigned mods);
    bool Char(wchar_t ch);
    bool ContextMenuKey();                                             // Shift+F10 / Apps on the focused widget

    Widget* Focused() const { return focused_; }
    bool SetFocus(Widget* w, bool keyboard = false);                   // nullptr clears focus; false if !CanFocus or focusFilter rejects w
    /// When set, only widgets it accepts can take focus (the app narrows keyboard focus per view: the player view allows only the
    /// playlist search box and popup contents). A pointer press on a rejected widget clears the focus instead of moving it.
    std::function<bool(const Widget*)> focusFilter;
    bool MoveFocus(bool forward);                                      // Tab order = depth-first tree order
    std::vector<Widget*> FocusOrder() const;                           // inside the top modal/light-dismiss popup if any
    bool FocusVisible() const { return focusVisible_; }                // last focus change came from the keyboard
    void SetFocusVisible(bool v) { focusVisible_ = v; }

    // A tooltip shows after kToolTipDelayMs of hover on a widget with ToolTip(); it hides on press/leave/key.
    static constexpr uint64_t kToolTipDelayMs = 500;
    bool ToolTipVisible() const { return tipVisible_; }
    const std::wstring& ToolTipText() const { return tipText_; }
    RectF ToolTipAnchor() const { return tipAnchor_; }
    PointF ToolTipPoint() const { return tipPoint_; }
    void HideToolTip();
    /// Tooltip painting is delegated so the app/ToolTip control can style it; default draws a simple box.
    std::function<void(IRenderer&, const Theme&, const std::wstring& text, RectF anchor, PointF pointer, SizeF client)> paintToolTip;

    void SetClock(std::function<uint64_t()> clock) { clock_ = std::move(clock); }
    uint64_t Now() const;
    void StartAnimating(Widget* w);
    void StopAnimating(Widget* w);
    /// Drives tooltips and animations. Returns true while something still needs ticks (keep a timer running).
    bool Tick();
    bool NeedsTick() const;

    void Paint(IRenderer& r);
    void Invalidate();
    bool IsDirty() const { return dirty_; }
    void ClearDirty() { dirty_ = false; }
    std::function<void()> onInvalidate;                                // e.g. schedule a redraw of the surface

    // Listeners (UIA bridge, app) are called synchronously.
    using UiEventListener = std::function<void(Widget*, UiEvent)>;
    int AddListener(UiEventListener l);
    void RemoveListener(int token);
    void RaiseEvent(Widget* w, UiEvent ev);

    void OnWidgetRemoved(Widget* w);   // w is being detached/destroyed: clears focus/hover/capture/animations pointing into w

private:
    struct Popup {
        std::unique_ptr<Widget> widget;
        PopupOptions opts;
        Widget* prevFocus = nullptr;
        std::weak_ptr<void> prevFocusLife;
    };
    void SetHovered(Widget* w);
    bool Bubble(Widget* w, const std::function<bool(Widget*)>& fn);
    void UpdateToolTipHover(Widget* w, PointF p);
    void CollectFocusable(Widget* w, std::vector<Widget*>& out) const;

    std::unique_ptr<Widget> root_;
    std::vector<Popup> popups_;
    SizeF size_{};
    Theme theme_{};
    Widget* focused_ = nullptr;
    Widget* hovered_ = nullptr;
    Widget* capture_ = nullptr;
    bool focusVisible_ = false;
    bool dirty_ = true;
    std::function<uint64_t()> clock_;
    std::vector<Widget*> animating_;
    Widget* tipCandidate_ = nullptr;
    uint64_t tipSince_ = 0;
    bool tipVisible_ = false, tipSuppressed_ = false;
    std::wstring tipText_;
    RectF tipAnchor_{};
    PointF tipPoint_{};
    std::vector<std::pair<int, UiEventListener>> listeners_;
    int nextListener_ = 1;
    std::vector<std::unique_ptr<Widget>> graveyard_;   // closed popups, destroyed at the next safe point
    PointF lastPointer_{};
    void FlushGraveyard() { graveyard_.clear(); }
};

}
