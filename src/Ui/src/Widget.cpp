#include "pui/Widget.h"

#include <algorithm>
#include <atomic>
#include <chrono>

namespace pui {


int Widget::NextId() {
    static std::atomic<int> next{1};
    return next++;
}

Widget::~Widget() {
    // RemoveChild/ClearChildren/ClosePopup notify the host before destruction.
    children_.clear();
}

Widget* Widget::AddChild(std::unique_ptr<Widget> child, int index) {
    if (!child) return nullptr;
    Widget* raw = child.get();
    raw->parent_ = this;
    if (index < 0 || index >= (int)children_.size()) children_.push_back(std::move(child));
    else children_.insert(children_.begin() + index, std::move(child));
    Raise(UiEvent::StructureChanged);
    Invalidate();
    return raw;
}

std::unique_ptr<Widget> Widget::RemoveChild(Widget* child) {
    for (auto it = children_.begin(); it != children_.end(); ++it) {
        if (it->get() != child) continue;
        if (UiHost* h = Host()) h->OnWidgetRemoved(child);
        std::unique_ptr<Widget> out = std::move(*it);
        children_.erase(it);
        out->parent_ = nullptr;
        Raise(UiEvent::StructureChanged);
        Invalidate();
        return out;
    }
    return nullptr;
}

void Widget::ClearChildren() {
    if (children_.empty()) return;
    if (UiHost* h = Host())
        for (auto& c : children_) h->OnWidgetRemoved(c.get());
    children_.clear();
    Raise(UiEvent::StructureChanged);
    Invalidate();
}

int Widget::IndexInParent() const {
    if (!parent_) return -1;
    for (size_t i = 0; i < parent_->children_.size(); i++)
        if (parent_->children_[i].get() == this) return (int)i;
    return -1;
}

UiHost* Widget::Host() const {
    const Widget* w = this;
    while (w->parent_) w = w->parent_;
    return w->host_;
}

bool Widget::IsAncestorOf(const Widget* w) const {
    for (; w; w = w->parent_)
        if (w == this) return true;
    return false;
}

void Widget::SetBounds(const RectF& r) {
    if (r == bounds_) return;
    bounds_ = r;
    OnLayout();
    Invalidate();
}

void Widget::SetVisible(bool v) {
    if (v == visible_) return;
    visible_ = v;
    UiHost* h = Host();
    if (!v && h) h->OnWidgetRemoved(this);
    Raise(UiEvent::VisibilityChanged);
    if (h) h->Invalidate();
}

bool Widget::IsEffectivelyVisible() const {
    for (const Widget* w = this; w; w = w->parent_)
        if (!w->visible_) return false;
    return true;
}

void Widget::SetEnabled(bool e) {
    if (e == enabled_) return;
    enabled_ = e;
    if (!e) {
        if (UiHost* h = Host()) {
            if (h->Focused() && IsAncestorOf(h->Focused())) h->SetFocus(nullptr);
            if (h->Captured() && IsAncestorOf(h->Captured())) h->ReleaseCapture();
        }
        pressed_ = false;
    }
    Raise(UiEvent::EnabledChanged);
    Invalidate();
}

bool Widget::IsEffectivelyEnabled() const {
    for (const Widget* w = this; w; w = w->parent_)
        if (!w->enabled_) return false;
    return true;
}

bool Widget::IsFocused() const {
    UiHost* h = Host();
    return h && h->Focused() == this;
}

bool Widget::Focus() {
    UiHost* h = Host();
    return h && h->SetFocus(this);
}

bool Widget::ShowFocusVisual() const {
    UiHost* h = Host();
    return h && h->Focused() == this && h->FocusVisible();
}

void Widget::SetName(std::wstring n) {
    if (n == name_) return;
    name_ = std::move(n);
    Raise(UiEvent::NameChanged);
    Invalidate();
}

void Widget::PaintTree(IRenderer& r, const Theme& t) {
    if (!visible_) return;
    Paint(r, t);
    if (children_.empty()) return;
    if (clipChildren_) r.PushClip(bounds_);
    for (auto& c : children_) c->PaintTree(r, t);
    if (clipChildren_) r.PopClip();
}

void Widget::Invalidate() {
    if (UiHost* h = Host()) h->Invalidate();
}

void Widget::PaintFocusVisual(IRenderer& r, const Theme& t, const RectF& rect, float radius) const {
    if (!ShowFocusVisual()) return;
    r.StrokeRoundRect(rect.Inflate(2), radius + 2, t.focusStrokeOuter, 2);
    r.StrokeRoundRect(rect.Inflate(0.5f), radius, t.focusStrokeInner, 1);
}

Widget* Widget::HitTest(PointF p) {
    if (!visible_ || !bounds_.Contains(p)) return nullptr;
    for (auto it = children_.rbegin(); it != children_.rend(); ++it)
        if (Widget* w = (*it)->HitTest(p)) return w;
    return hitTestVisible_ ? this : nullptr;
}

void Widget::Raise(UiEvent ev) {
    if (UiHost* h = Host()) h->RaiseEvent(this, ev);
}

void Widget::SetPressedState(bool p) {
    if (p == pressed_) return;
    pressed_ = p;
    Invalidate();
}


UiHost::UiHost() = default;

UiHost::~UiHost() {
    listeners_.clear();
    onInvalidate = nullptr;
    focused_ = hovered_ = capture_ = tipCandidate_ = nullptr;
    animating_.clear();
    popups_.clear();
    graveyard_.clear();
    root_.reset();
}

Widget* UiHost::SetRoot(std::unique_ptr<Widget> root) {
    if (root_) {
        OnWidgetRemoved(root_.get());
        root_->host_ = nullptr;
        graveyard_.push_back(std::move(root_));
    }
    root_ = std::move(root);
    if (root_) {
        root_->host_ = this;
        root_->parent_ = nullptr;
        root_->SetBounds({0, 0, size_.w, size_.h});
        RaiseEvent(root_.get(), UiEvent::StructureChanged);
    }
    Invalidate();
    return root_.get();
}

void UiHost::SetSize(SizeF dip) {
    size_ = dip;
    if (root_) root_->SetBounds({0, 0, dip.w, dip.h});
    Invalidate();
}

void UiHost::SetTheme(const Theme& t) {
    theme_ = t;
    Invalidate();
}


Widget* UiHost::OpenPopup(std::unique_ptr<Widget> popup, PopupOptions opts) {
    if (!popup) return nullptr;
    HideToolTip();
    Widget* raw = popup.get();
    raw->parent_ = nullptr;
    raw->host_ = this;
    Popup p;
    p.widget = std::move(popup);
    p.prevFocus = focused_;
    if (focused_) p.prevFocusLife = focused_->LifeToken();
    if (opts.anchor) { p.prevFocus = opts.anchor; p.prevFocusLife = opts.anchor->LifeToken(); }
    bool takeFocus = opts.takeFocus;
    p.opts = std::move(opts);
    popups_.push_back(std::move(p));
    if (capture_) capture_ = nullptr;
    if (takeFocus) {
        std::vector<Widget*> order;
        CollectFocusable(raw, order);
        if (!order.empty()) SetFocus(order.front(), focusVisible_);
    }
    RaiseEvent(raw, UiEvent::PopupOpened);
    Invalidate();
    return raw;
}

void UiHost::ClosePopup(Widget* popup) {
    auto it = std::find_if(popups_.begin(), popups_.end(), [&](const Popup& p) { return p.widget.get() == popup; });
    if (it == popups_.end()) return;
    size_t idx = (size_t)(it - popups_.begin());
    while (popups_.size() > idx) {
        Popup p = std::move(popups_.back());
        popups_.pop_back();
        Widget* w = p.widget.get();
        bool focusInside = focused_ && w->IsAncestorOf(focused_);
        OnWidgetRemoved(w);
        RaiseEvent(w, UiEvent::PopupClosed);
        w->host_ = nullptr;
        if (focusInside || !focused_) {
            if (p.prevFocus && !p.prevFocusLife.expired() && p.prevFocus->Host() == this && p.prevFocus->CanFocus())
                SetFocus(p.prevFocus, focusVisible_);
        }
        auto cb = std::move(p.opts.onClosed);
        graveyard_.push_back(std::move(p.widget));
        if (cb) cb();
    }
    Invalidate();
}

void UiHost::CloseAllPopups() {
    if (!popups_.empty()) ClosePopup(popups_.front().widget.get());
}

Widget* UiHost::TopPopup() const {
    return popups_.empty() ? nullptr : popups_.back().widget.get();
}

std::vector<Widget*> UiHost::Popups() const {
    std::vector<Widget*> out;
    for (auto& p : popups_) out.push_back(p.widget.get());
    return out;
}

bool UiHost::HasModalPopup() const {
    for (const auto& p : popups_)
        if (p.opts.modal) return true;
    return false;
}

std::vector<PopupBackdropSurface> UiHost::AcrylicPopupSurfaces() const {
    std::vector<PopupBackdropSurface> out;
    bool opaquePopupBelow = false;
    for (const auto& p : popups_)
        if (p.opts.modal) return out;  // an opaque dialog backdrop separates its contents from the video
    for (const auto& p : popups_) {
        if (p.opts.acrylicSurface && opaquePopupBelow) return {}; // the Acrylic popup would sample a solid popup, not video
        if (!p.opts.acrylicSurface) opaquePopupBelow = true;
        const Widget* root = p.widget.get();
        if (!p.opts.acrylicSurface || !root || !root->IsEffectivelyVisible() || root->Bounds().Empty())
            continue;
        const float radius = p.opts.acrylicRadius >= 0 ? p.opts.acrylicRadius : theme_.overlayRadius;
        out.push_back({root->Bounds(), radius});
    }
    return out;
}

bool UiHost::IsPopupOpen(const Widget* popup) const {
    for (auto& p : popups_)
        if (p.widget.get() == popup) return true;
    return false;
}


Widget* UiHost::HitTest(PointF p) const {
    for (auto it = popups_.rbegin(); it != popups_.rend(); ++it) {
        if (Widget* w = it->widget->HitTest(p)) return w;
        if (it->opts.modal || it->opts.lightDismiss) return nullptr;   // blocks everything below
    }
    return root_ ? root_->HitTest(p) : nullptr;
}

bool UiHost::Bubble(Widget* w, const std::function<bool(Widget*)>& fn) {
    for (; w; w = w->Parent()) {
        if (!w->IsEffectivelyEnabled()) return true;   // disabled widgets swallow input
        if (fn(w)) return true;
    }
    return false;
}

void UiHost::SetHovered(Widget* w) {
    if (w == hovered_) return;
    Widget* old = hovered_;
    hovered_ = w;
    if (old) {
        old->hovered_ = false;
        old->OnPointerLeave();
        old->Invalidate();
    }
    if (w) {
        w->hovered_ = true;
        w->OnPointerEnter();
        w->Invalidate();
    }
}

void UiHost::UpdateToolTipHover(Widget* w, PointF p) {
    tipPoint_ = p;
    Widget* cand = w;
    while (cand && cand->ToolTip().empty()) cand = cand->Parent();
    if (cand != tipCandidate_) {
        tipCandidate_ = cand;
        tipSince_ = Now();
        tipSuppressed_ = false;
        if (tipVisible_) {
            tipVisible_ = false;
            Invalidate();
        }
    }
}

bool UiHost::PointerDown(const PointerEvent& e) {
    FlushGraveyard();
    lastPointer_ = e.pt;
    HideToolTip();
    tipSuppressed_ = true;
    focusVisible_ = false;
    if (!popups_.empty()) {
        bool insideAny = false;
        for (auto it = popups_.rbegin(); it != popups_.rend(); ++it) {
            if (it->widget->HitTest(e.pt)) { insideAny = true; break; }
            if (it->opts.modal || !it->opts.lightDismiss) break;
        }
        if (!insideAny) {
            Popup& top = popups_.back();
            if (top.opts.modal) return true;
            if (top.opts.lightDismiss) {
                while (!popups_.empty()) {
                    Popup& p = popups_.back();
                    if (p.opts.modal || !p.opts.lightDismiss || p.widget->HitTest(e.pt)) break;
                    ClosePopup(p.widget.get());
                }
                return true;
            }
        }
    }
    Widget* target = HitTest(e.pt);
    if (!target) return false;
    for (Widget* f = target; f; f = f->Parent())
        if (f->CanFocus()) { if (!SetFocus(f, false)) SetFocus(nullptr); break; }
    Widget* handler = nullptr;
    bool handled = Bubble(target, [&](Widget* w) {
        if (w->OnPointerDown(e)) { handler = w; return true; }
        return false;
    });
    if (handler && e.button == PointerButton::Left && handler->Host() == this) {
        capture_ = handler;
        handler->SetPressedState(true);
    }
    if (!handled && e.button == PointerButton::Right) {
        Bubble(target, [&](Widget* w) { return w->OnContextMenu(e.pt, false); });
        handled = true;
    }
    return handled;
}

bool UiHost::PointerMove(const PointerEvent& e) {
    lastPointer_ = e.pt;
    if (capture_) {
        SetHovered(capture_->Bounds().Contains(e.pt) ? capture_ : nullptr);
        capture_->OnPointerMove(e);
        return true;
    }
    Widget* target = HitTest(e.pt);
    SetHovered(target);
    UpdateToolTipHover(target, e.pt);
    if (!target) return false;
    return Bubble(target, [&](Widget* w) { return w->OnPointerMove(e); });
}

bool UiHost::PointerUp(const PointerEvent& e) {
    FlushGraveyard();
    lastPointer_ = e.pt;
    if (capture_) {
        Widget* c = capture_;
        capture_ = nullptr;
        std::weak_ptr<void> life = c->LifeToken();
        c->OnPointerUp(e);
        if (!life.expired()) c->SetPressedState(false);
        Widget* target = HitTest(e.pt);
        SetHovered(target);
        return true;
    }
    Widget* target = HitTest(e.pt);
    if (!target) return false;
    bool handled = Bubble(target, [&](Widget* w) { return w->OnPointerUp(e); });
    if (!handled && e.button == PointerButton::Right) {
        handled = Bubble(target, [&](Widget* w) { return w->OnContextMenu(e.pt, false); });
    }
    return handled;
}

bool UiHost::PointerWheel(const PointerEvent& e) {
    HideToolTip();
    Widget* target = HitTest(e.pt);
    if (!target) return false;
    return Bubble(target, [&](Widget* w) { return w->OnPointerWheel(e); });
}

void UiHost::PointerLeave() {
    if (capture_) return;
    SetHovered(nullptr);
    tipCandidate_ = nullptr;
    HideToolTip();
}


void UiHost::CancelCapture() {
    if (!capture_) return;
    capture_->pressed_ = false;
    capture_->Invalidate();
    capture_ = nullptr;
}

bool UiHost::KeyDown(int vk, unsigned mods) {
    FlushGraveyard();
    HideToolTip();
    tipSuppressed_ = true;
    switch (vk) {
    case key::Tab: case key::Left: case key::Right: case key::Up: case key::Down:
    case key::Home: case key::End: case key::PageUp: case key::PageDown: case key::F6:
        focusVisible_ = true;
        break;
    default: break;
    }
    Widget* target = focused_;
    if (!target) target = TopPopup() ? TopPopup() : root_.get();
    bool handled = target && Bubble(target, [&](Widget* w) { return w->OnKeyDown(vk, mods); });
    if (handled) { Invalidate(); return true; }
    if (vk == key::Tab && !(mods & (ModCtrl | ModAlt))) {
        MoveFocus(!(mods & ModShift));
        return true;
    }
    if (vk == key::Escape && !popups_.empty()) {
        Popup& top = popups_.back();
        if (top.opts.lightDismiss) {
            ClosePopup(top.widget.get());
            return true;
        }
    }
    if ((vk == key::F10 && (mods & ModShift)) || vk == key::Apps) return ContextMenuKey();
    return false;
}

bool UiHost::KeyUp(int vk, unsigned mods) {
    Widget* target = focused_;
    if (!target) return false;
    return Bubble(target, [&](Widget* w) { return w->OnKeyUp(vk, mods); });
}

bool UiHost::Char(wchar_t ch) {
    Widget* target = focused_;
    if (!target) return false;
    return Bubble(target, [&](Widget* w) { return w->OnChar(ch); });
}

bool UiHost::ContextMenuKey() {
    Widget* target = focused_;
    if (!target) return false;
    PointF c{target->Bounds().cx(), target->Bounds().cy()};
    return Bubble(target, [&](Widget* w) { return w->OnContextMenu(c, true); });
}


bool UiHost::SetFocus(Widget* w, bool keyboard) {
    if (w && (!w->CanFocus() || w->Host() != this || (focusFilter && !focusFilter(w)))) return false;
    if (keyboard) focusVisible_ = true;
    if (w == focused_) return true;
    Widget* old = focused_;
    focused_ = w;
    if (old) {
        old->OnFocusChanged(false);
        old->Invalidate();
    }
    if (w) {
        w->OnFocusChanged(true);
        w->Invalidate();
    }
    RaiseEvent(w, UiEvent::FocusChanged);
    Invalidate();
    return true;
}

void UiHost::CollectFocusable(Widget* w, std::vector<Widget*>& out) const {
    if (!w || !w->Visible() || !w->Enabled()) return;
    if (w->CanFocus()) out.push_back(w);
    for (auto& c : w->Children()) CollectFocusable(c.get(), out);
}

std::vector<Widget*> UiHost::FocusOrder() const {
    std::vector<Widget*> out;
    Widget* scope = root_.get();
    if (!popups_.empty()) {
        const Popup& top = popups_.back();
        if (top.opts.modal || top.opts.lightDismiss) scope = top.widget.get();
    }
    CollectFocusable(scope, out);
    return out;
}

bool UiHost::MoveFocus(bool forward) {
    focusVisible_ = true;
    auto order = FocusOrder();
    if (order.empty()) return false;
    auto it = std::find(order.begin(), order.end(), focused_);
    Widget* next;
    if (it == order.end()) next = forward ? order.front() : order.back();
    else {
        int i = (int)(it - order.begin());
        int n = (int)order.size();
        next = order[(size_t)((i + (forward ? 1 : n - 1)) % n)];
    }
    return SetFocus(next, true);
}


void UiHost::HideToolTip() {
    if (tipVisible_) {
        tipVisible_ = false;
        Invalidate();
    }
}

uint64_t UiHost::Now() const {
    if (clock_) return clock_();
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void UiHost::StartAnimating(Widget* w) {
    if (w && std::find(animating_.begin(), animating_.end(), w) == animating_.end()) animating_.push_back(w);
}

void UiHost::StopAnimating(Widget* w) {
    animating_.erase(std::remove(animating_.begin(), animating_.end(), w), animating_.end());
}

bool UiHost::Tick() {
    FlushGraveyard();
    uint64_t now = Now();
    if (!animating_.empty()) {
        auto list = animating_;
        for (Widget* w : list) {
            if (std::find(animating_.begin(), animating_.end(), w) == animating_.end()) continue;
            if (!w->OnTick(now)) StopAnimating(w);
            w->Invalidate();
        }
    }
    if (tipCandidate_ && !tipVisible_ && !tipSuppressed_ && now - tipSince_ >= kToolTipDelayMs && !capture_) {
        tipVisible_ = true;
        tipText_ = tipCandidate_->ToolTip();
        tipAnchor_ = tipCandidate_->Bounds();
        Invalidate();
    }
    return NeedsTick();
}

bool UiHost::NeedsTick() const {
    return !animating_.empty() || (tipCandidate_ && !tipVisible_ && !tipSuppressed_);
}


void UiHost::Paint(IRenderer& r) {
    if (root_) root_->PaintTree(r, theme_);
    for (auto& p : popups_) {
        if (p.opts.modal) r.FillRect({0, 0, size_.w, size_.h}, theme_.smokeFill);
        p.widget->PaintTree(r, theme_);
    }
    if (tipVisible_ && !tipText_.empty()) {
        if (paintToolTip) paintToolTip(r, theme_, tipText_, tipAnchor_, tipPoint_, size_);
        else {
            TextStyle st;
            st.size = 12;
            st.ellipsis = false;
            SizeF ts = r.MeasureText(tipText_, st, 320);
            float w = ts.w + 18, h = ts.h + 12;
            float x = std::clamp(tipPoint_.x - w * 0.5f, 4.f, std::max(4.f, size_.w - w - 4));
            float y = tipAnchor_.t - h - 8;
            if (y < 4) y = tipAnchor_.b + 8;
            RectF box = RectF::XYWH(x, y, w, h);
            r.FillRoundRect(box, 4, theme_.flyoutSurface);
            r.StrokeRoundRect(box, 4, theme_.flyoutStroke, 1);
            r.Text(tipText_, box.Deflate(9, 6, 9, 6), st, theme_.textPrimary);
        }
    }
}

void UiHost::Invalidate() {
    if (dirty_) return;
    dirty_ = true;
    if (onInvalidate) onInvalidate();
}


int UiHost::AddListener(UiEventListener l) {
    int t = nextListener_++;
    listeners_.emplace_back(t, std::move(l));
    return t;
}

void UiHost::RemoveListener(int token) {
    listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(), [&](auto& p) { return p.first == token; }),
                     listeners_.end());
}

void UiHost::RaiseEvent(Widget* w, UiEvent ev) {
    if (listeners_.empty()) return;
    auto copy = listeners_;
    for (auto& [t, l] : copy)
        if (l) l(w, ev);
}

void UiHost::OnWidgetRemoved(Widget* w) {
    if (!w) return;
    if (focused_ && w->IsAncestorOf(focused_)) {
        Widget* old = focused_;
        focused_ = nullptr;
        old->OnFocusChanged(false);
        RaiseEvent(nullptr, UiEvent::FocusChanged);
    }
    if (hovered_ && w->IsAncestorOf(hovered_)) {
        hovered_->hovered_ = false;
        hovered_ = nullptr;
    }
    if (capture_ && w->IsAncestorOf(capture_)) {
        capture_->pressed_ = false;
        capture_ = nullptr;
    }
    if (tipCandidate_ && w->IsAncestorOf(tipCandidate_)) {
        tipCandidate_ = nullptr;
        HideToolTip();
    }
    animating_.erase(std::remove_if(animating_.begin(), animating_.end(), [&](Widget* a) { return w->IsAncestorOf(a); }),
                     animating_.end());
}

}
