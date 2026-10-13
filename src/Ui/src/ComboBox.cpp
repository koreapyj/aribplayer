#include "pui/ComboBox.h"

#include <algorithm>
#include <cmath>
#include <cwctype>

namespace pui {

RectF ComputeComboPopupRect(const RectF& box, int n, int sel, float itemH, float padV, float minWidth, SizeF client, float margin) {
    float w = std::max({box.w(), minWidth, 0.f});
    float fullH = (float)std::max(n, 0) * itemH + 2 * padV;
    float maxH = std::max(itemH, client.h - 2 * margin);
    float h = std::min(fullH, maxH);
    float idx = (float)std::max(sel, 0);
    float y = box.t + (box.h() - itemH) * 0.5f - padV - idx * itemH;
    float x = box.l;
    float maxX = client.w - margin - w, maxY = client.h - margin - h;
    x = std::max(margin, std::min(x, maxX));
    y = std::max(margin, std::min(y, maxY));
    return RectF::XYWH(x, y, w, h);
}

namespace {
bool StartsWithNoCase(const std::wstring& s, const std::wstring& prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); i++)
        if (std::towlower(s[i]) != std::towlower(prefix[i])) return false;
    return true;
}
}

int TypeAhead::Next(wchar_t ch, uint64_t now, const std::vector<std::wstring>& items, int current) {
    if (now - last_ > kTimeoutMs) prefix_.clear();
    last_ = now;
    prefix_ += ch;
    const int n = (int)items.size();
    if (n == 0) return -1;
    const bool repeated = std::all_of(prefix_.begin(), prefix_.end(), [&](wchar_t c) { return std::towlower(c) == std::towlower(prefix_[0]); });
    const std::wstring key = repeated ? prefix_.substr(0, 1) : prefix_;
    const int start = std::max(current, 0) + (repeated ? 1 : 0);
    for (int k = 0; k < n; k++) {
        int i = (start + k) % n;
        if (StartsWithNoCase(items[(size_t)i], key)) return i;
    }
    return -1;
}

class ComboItem : public Widget {
public:
    ComboItem(class ComboPopup* p, int idx, std::wstring text);
    void Paint(IRenderer& r, const Theme& t) override;
    bool SupportsSelectionItem() const override { return true; }
    bool IsSelected() const override;
    void SelectItem() override;
    int index;
private:
    class ComboPopup* popup_;
};

class ComboPopup : public Widget {
public:
    ComboPopup(ComboBox* owner, const std::vector<std::wstring>& items, int selected)
        : Widget(Role::List), owner_(owner), ownerLife_(owner->LifeToken()), selected_(selected), highlight_(std::max(selected, 0)) {
        SetFocusable(true);
        SetClipChildren(true);
        for (size_t i = 0; i < items.size(); i++) Add<ComboItem>(this, (int)i, items[i]);
    }

    void OnLayout() override {
        RectF b = Bounds();
        ClampScroll();
        for (auto& c : Children()) {
            auto* it = static_cast<ComboItem*>(c.get());
            float top = b.t + ComboBox::kPopupPad - scroll_ + (float)it->index * ComboBox::kItemHeight;
            it->SetBounds(RectF::XYWH(b.l + 4, top, b.w() - 8, ComboBox::kItemHeight));
        }
    }

    void Paint(IRenderer& r, const Theme& t) override {
        r.FillRoundRect(Bounds(), t.overlayRadius, t.acrylicEnabled ? Color(0x00000000) : t.acrylicFallbackColor);
        r.StrokeRoundRect(Bounds().Deflate(0.5f, 0.5f, 0.5f, 0.5f), t.overlayRadius, t.flyoutStroke, 1);
    }

    bool OnPointerDown(const PointerEvent& e) override { return e.button == PointerButton::Left; }
    bool OnPointerMove(const PointerEvent& e) override {
        int i = IndexAt(e.pt);
        if (i >= 0 && i != highlight_) { highlight_ = i; Invalidate(); }
        return true;
    }
    bool OnPointerUp(const PointerEvent& e) override {
        if (e.button != PointerButton::Left) return false;
        int i = IndexAt(e.pt);
        if (i >= 0) Commit(i);
        return true;
    }
    bool OnPointerWheel(const PointerEvent& e) override {
        scroll_ -= e.wheelDelta * ComboBox::kItemHeight * 2;
        OnLayout();
        Invalidate();
        return true;
    }
    bool OnKeyDown(int vk, unsigned mods) override {
        int n = (int)Children().size();
        if (n == 0) return false;
        switch (vk) {
        case key::Up:
            if (mods & ModAlt) { CloseOnly(); return true; }
            Highlight(highlight_ - 1); return true;
        case key::Down: Highlight(highlight_ + 1); return true;
        case key::Home: Highlight(0); return true;
        case key::End: Highlight(n - 1); return true;
        case key::PageUp: Highlight(highlight_ - 5); return true;
        case key::PageDown: Highlight(highlight_ + 5); return true;
        case key::Enter: case key::Space: Commit(highlight_); return true;
        case key::F4: CloseOnly(); return true;
        case key::Escape: CloseOnly(); return true;
        default: return false;
        }
    }
    bool OnChar(wchar_t ch) override {
        if (ch <= L' ' || ownerLife_.expired()) return false;
        int i = owner_->typeAhead_.Next(ch, Host() ? Host()->Now() : 0, owner_->items_, highlight_);
        if (i >= 0) Highlight(i);
        return true;
    }

    bool SupportsSelection() const override { return true; }
    bool IsSelectionRequired() const override { return false; }
    std::vector<Widget*> SelectedChildren() const override {
        std::vector<Widget*> v;
        if (selected_ >= 0 && selected_ < (int)Children().size()) v.push_back(Children()[(size_t)selected_].get());
        return v;
    }

    int highlight() const { return highlight_; }
    int selectedIndex() const { return selected_; }
    void Commit(int i) {
        std::weak_ptr<void> life = ownerLife_;
        ComboBox* o = owner_;
        UiHost* h = Host();
        if (h) h->ClosePopup(this);                   // restores focus to the combo box
        if (!life.expired()) o->UserSelect(i);
    }

private:
    int IndexAt(PointF p) const {
        for (auto& c : Children())
            if (c->Bounds().Contains(p)) return static_cast<ComboItem*>(c.get())->index;
        return -1;
    }
    void CloseOnly() { if (UiHost* h = Host()) h->ClosePopup(this); }
    void Highlight(int i) {
        i = std::clamp(i, 0, (int)Children().size() - 1);
        highlight_ = i;
        float top = ComboBox::kPopupPad + (float)i * ComboBox::kItemHeight;
        float viewH = Bounds().h();
        if (top - scroll_ < 0) scroll_ = top - ComboBox::kPopupPad;
        else if (top + ComboBox::kItemHeight - scroll_ > viewH - ComboBox::kPopupPad) scroll_ = top + ComboBox::kItemHeight - viewH + ComboBox::kPopupPad;
        OnLayout();
        Invalidate();
    }
    void ClampScroll() {
        float full = (float)Children().size() * ComboBox::kItemHeight + 2 * ComboBox::kPopupPad;
        scroll_ = std::clamp(scroll_, 0.f, std::max(0.f, full - Bounds().h()));
    }
    ComboBox* owner_;
    std::weak_ptr<void> ownerLife_;
    int selected_, highlight_;
    float scroll_ = 0;
};

ComboItem::ComboItem(ComboPopup* p, int idx, std::wstring text) : Widget(Role::ListItem), index(idx), popup_(p) {
    SetName(std::move(text));
}
bool ComboItem::IsSelected() const { return popup_->selectedIndex() == index; }
void ComboItem::SelectItem() { popup_->Commit(index); }

void ComboItem::Paint(IRenderer& r, const Theme& t) {
    RectF b = Bounds();
    bool hi = popup_->highlight() == index, sel = popup_->selectedIndex() == index;
    if (hi) r.FillRoundRect(b, t.controlRadius, t.subtleSecondary);
    else if (sel) r.FillRoundRect(b, t.controlRadius, t.subtleTertiary);
    if (sel) r.FillRoundRect(RectF::XYWH(b.l, b.cy() - 8, 3, 16), 1.5f, t.accent);
    r.Text(Name(), {b.l + 12, b.t, b.r - 8, b.b}, BodyTextStyle(14), IsEffectivelyEnabled() ? t.textPrimary : t.textDisabled);
}


ComboBox::ComboBox() : Widget(Role::ComboBox) { SetFocusable(true); }

void ComboBox::AddItem(std::wstring text) {
    items_.push_back(std::move(text));
    RelayoutAncestors(*this);
    Invalidate();
}

void ComboBox::SetItems(std::vector<std::wstring> items) {
    Close();
    items_ = std::move(items);
    if (selected_ >= (int)items_.size()) selected_ = -1;
    RelayoutAncestors(*this);
    Invalidate();
}

void ComboBox::SetSelectedIndex(int i) {
    if (i < -1 || i >= (int)items_.size()) return;
    if (i == selected_) return;
    selected_ = i;
    Raise(UiEvent::SelectionChanged);
    Raise(UiEvent::ValueChanged);
    Invalidate();
}

void ComboBox::UserSelect(int i) {
    if (i < 0 || i >= (int)items_.size() || i == selected_) return;
    SetSelectedIndex(i);
    if (onSelectionChanged) onSelectionChanged(selected_);
}

void ComboBox::SetValue(const std::wstring& v) {
    if (!IsEffectivelyEnabled()) return;
    for (int i = 0; i < (int)items_.size(); i++)
        if (items_[(size_t)i] == v) { UserSelect(i); return; }
}

void ComboBox::Open() {
    if (popup_ || items_.empty() || !IsEffectivelyEnabled()) return;
    UiHost* h = Host();
    if (!h) return;
    auto pop = std::make_unique<ComboPopup>(this, items_, selected_);
    pop->SetBounds(ComputeComboPopupRect(Bounds(), (int)items_.size(), selected_, kItemHeight, kPopupPad, 0, h->Size()));
    PopupOptions o;
    o.acrylicSurface = true;
    o.anchor = this;
    std::weak_ptr<void> life = LifeToken();
    o.onClosed = [this, life] {
        if (life.expired()) return;
        popup_ = nullptr;
        Raise(UiEvent::ExpandCollapseChanged);
        Invalidate();
    };
    popup_ = h->OpenPopup(std::move(pop), std::move(o));
    Raise(UiEvent::ExpandCollapseChanged);
    Invalidate();
}

void ComboBox::Close() {
    if (!popup_) return;
    if (UiHost* h = Host()) h->ClosePopup(popup_);
    popup_ = nullptr;
}

SizeF ComboBox::DesiredSize(IRenderer* m) {
    float w = 0;
    for (auto& s : items_) w = std::max(w, MeasurerOr(m).MeasureText(s, BodyTextStyle(14)).w);
    return {std::max(64.f, w + 50), kHeight};
}

void ComboBox::Paint(IRenderer& r, const Theme& t) {
    RectF b = Bounds();
    bool en = IsEffectivelyEnabled();
    Color fill = !en ? t.ctlDisabled : (IsPressed() || IsOpen()) ? t.ctlTertiary : IsHovered() ? t.ctlSecondary : t.ctlDefault;
    r.FillRoundRect(b, t.controlRadius, fill);
    r.StrokeRoundRect(b.Deflate(0.5f, 0.5f, 0.5f, 0.5f), t.controlRadius, t.ctlStrokeDefault, 1);
    if (selected_ >= 0)
        r.Text(items_[(size_t)selected_], {b.l + 12, b.t, b.r - 36, b.b}, BodyTextStyle(14), en ? t.textPrimary : t.textDisabled);
    r.Text(glyph::Str(glyph::ChevronDown), RectF::XYWH(b.r - 12 - 12, b.t, 12, b.h()), IconTextStyle(12), en ? t.textSecondary : t.textDisabled);
    PaintFocusVisual(r, t, b, t.controlRadius);
}

bool ComboBox::OnPointerDown(const PointerEvent& e) { return e.button == PointerButton::Left; }

bool ComboBox::OnPointerUp(const PointerEvent& e) {
    if (e.button != PointerButton::Left) return false;
    if (Bounds().Contains(e.pt) && IsEffectivelyEnabled()) {
        if (IsOpen()) Close(); else Open();
    }
    return true;
}

bool ComboBox::OnKeyDown(int vk, unsigned mods) {
    if (IsOpen()) return false;
    bool alt = (mods & ModAlt) != 0;
    if ((vk == key::Down && alt) || vk == key::F4 || vk == key::Enter || vk == key::Space) { Open(); return true; }
    if (mods & (ModCtrl | ModAlt)) return false;
    int n = ItemCount();
    if (n == 0) return false;
    switch (vk) {
    case key::Up: UserSelect(std::max(0, selected_ - 1)); return true;
    case key::Down: UserSelect(std::min(n - 1, selected_ + 1)); return true;
    case key::Home: UserSelect(0); return true;
    case key::End: UserSelect(n - 1); return true;
    default: return false;
    }
}

bool ComboBox::OnChar(wchar_t ch) {
    if (IsOpen() || ch <= L' ') return false;
    int i = typeAhead_.Next(ch, Host() ? Host()->Now() : 0, items_, selected_);
    if (i >= 0) UserSelect(i);
    return true;
}

}
