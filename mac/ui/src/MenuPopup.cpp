#include "puimac/MenuPopup.h"

#include <algorithm>

#include "pui/Buttons.h"
#include "pui/Glyphs.h"

using namespace pui;

namespace puimac {

namespace {
constexpr uint64_t kSubMenuDelayMs = 180;
constexpr float kAccelGap = 24, kSubChevronW = 20;

TextStyle MenuText() { return BodyTextStyle(13); }
TextStyle AccelText() { TextStyle s = BodyTextStyle(13); s.align = TextAlign::Trailing; return s; }

// Light-dismiss popup options shared by every level of a menu chain.
PopupOptions MenuOptions(Widget* focusReturn) {
    PopupOptions o;
    o.lightDismiss = true;
    o.takeFocus = true;
    o.anchor = focusReturn;
    return o;
}
}

MenuPopup::MenuPopup(std::vector<MenuItemDesc> items, MenuPopup* parentMenu) : Widget(Role::Menu), items_(std::move(items)), parent_(parentMenu) {
    SetFocusable(true);
}

SizeF MenuPopup::DesiredSize(IRenderer* m) {
    IRenderer& mr = MeasurerOr(m);
    float textW = 0, accelW = 0, h = 2 * kPadV;
    bool anySub = false;
    for (auto& it : items_) {
        if (it.kind == MenuItemKind::Separator) { h += kSepH; continue; }
        h += kRowH;
        textW = std::max(textW, mr.MeasureText(it.text, MenuText()).w);
        if (!it.accelerator.empty()) accelW = std::max(accelW, mr.MeasureText(it.accelerator, AccelText()).w);
        if (it.kind == MenuItemKind::SubMenu) anySub = true;
    }
    float w = kPadH + kTextL + textW + (accelW > 0 ? kAccelGap + accelW : 0) + (anySub ? kSubChevronW : 12) + kPadH;
    return {std::max(kMinW, w), h};
}

RectF MenuPopup::RowRect(int i) const {
    RectF b = Bounds();
    float y = b.t + kPadV;
    for (int k = 0; k < (int)items_.size(); k++) {
        float rh = items_[(size_t)k].kind == MenuItemKind::Separator ? kSepH : kRowH;
        if (k == i) return {b.l + kPadH, y, b.r - kPadH, y + rh};
        y += rh;
    }
    return {};
}

int MenuPopup::IndexAt(PointF p) const {
    for (int i = 0; i < (int)items_.size(); i++)
        if (RowRect(i).Contains(p)) return i;
    return -1;
}

void MenuPopup::Paint(IRenderer& r, const Theme& t) {
    RectF b = Bounds();
    r.FillRoundRect(b, t.overlayRadius, t.flyoutSurface);
    r.StrokeRoundRect(b.Deflate(0.5f, 0.5f, 0.5f, 0.5f), t.overlayRadius, t.flyoutStroke, 1);
    for (int i = 0; i < (int)items_.size(); i++) {
        const MenuItemDesc& it = items_[(size_t)i];
        RectF row = RowRect(i);
        if (it.kind == MenuItemKind::Separator) {
            float y = pui::SnapDip(row.cy(), 1.f / std::max(t.hairline, 0.01f)) ;
            r.FillRect({row.l + 4, y - t.hairline * 0.5f, row.r - 4, y + t.hairline * 0.5f}, t.dividerStroke);
            continue;
        }
        bool hi = i == highlight_ && it.enabled;
        Color fg = !it.enabled ? t.textDisabled : hi ? t.textOnAccent : t.textPrimary;
        if (hi) r.FillRoundRect(row, 5, t.accent);
        // Mark column: check / radio dot / glyph.
        RectF mark = {row.l, row.t, row.l + kTextL, row.b};
        if (it.kind == MenuItemKind::Check && it.checked)
            r.Text(glyph::Str(glyph::CheckMark), mark, IconTextStyle(11), fg);
        else if (it.kind == MenuItemKind::Radio && it.checked)
            r.FillEllipse({mark.cx(), mark.cy()}, 3, 3, fg);
        else if (it.glyph)
            r.Text(glyph::Str(it.glyph), mark, IconTextStyle(13), fg);
        float textR = row.r - 8;
        if (it.kind == MenuItemKind::SubMenu) {
            r.Text(glyph::Str(glyph::ChevronRightSmall), {row.r - kSubChevronW, row.t, row.r - 6, row.b}, IconTextStyle(9), fg);
            textR = row.r - kSubChevronW;
        } else if (!it.accelerator.empty()) {
            float aw = r.MeasureText(it.accelerator, AccelText()).w;
            r.Text(it.accelerator, {row.r - 8 - aw, row.t, row.r - 8, row.b}, AccelText(), hi ? fg : t.textSecondary);
            textR = row.r - 8 - aw - 8;
        }
        r.Text(it.text, {row.l + kTextL, row.t, std::max(row.l + kTextL, textR), row.b}, MenuText(), fg);
    }
}

bool MenuPopup::IsSubMenuItem(int i) const {
    return i >= 0 && i < (int)items_.size() && items_[(size_t)i].kind == MenuItemKind::SubMenu && items_[(size_t)i].enabled;
}

MenuPopup* MenuPopup::SubMenu() const { return static_cast<MenuPopup*>(sub_); }

void MenuPopup::SetHighlight(int i) {
    if (i != highlight_) {
        highlight_ = i;
        Invalidate();
    }
    pending_ = false;
    pendingSub_ = -1;
    if (sub_ && highlight_ != subIndex_) CloseSubMenu();
}

void MenuPopup::HoverItem(int i) {
    if (i != highlight_) {
        highlight_ = i;
        Invalidate();
    }
    if (sub_ && i == subIndex_) {   // back on the open submenu's item: keep it
        pending_ = false;
        pendingSub_ = -1;
        return;
    }
    if (!sub_ && !IsSubMenuItem(i)) {
        pending_ = false;
        pendingSub_ = -1;
        return;
    }
    if (pending_ && pendingSub_ == i) return;   // the delay runs from the first move onto the item
    // Another item while a submenu is open (switch / close it), or a submenu item: after the delay, so that crossing
    // other rows on the way into the open submenu does not close it (native menus do the same).
    pending_ = true;
    pendingSub_ = i;
    if (UiHost* h = Host()) { pendingSince_ = h->Now(); h->StartAnimating(this); }
}

void MenuPopup::KeepSubMenu() {
    pending_ = false;
    pendingSub_ = -1;
    if (sub_ && highlight_ != subIndex_) {
        highlight_ = subIndex_;
        Invalidate();
    }
}

bool MenuPopup::OnTick(uint64_t now) {
    if (!pending_) return false;
    if (now - pendingSince_ < kSubMenuDelayMs) return true;
    const int i = pendingSub_;
    pending_ = false;
    pendingSub_ = -1;
    if (i != highlight_) return false;
    if (sub_ && subIndex_ != i) CloseSubMenu();
    if (IsSubMenuItem(i) && !sub_) OpenSubMenu(i);
    return false;
}

void MenuPopup::OpenSubMenu(int i) {
    UiHost* h = Host();
    if (!h || i < 0 || i >= (int)items_.size()) return;
    CloseSubMenu();
    pending_ = false;
    pendingSub_ = -1;
    auto sub = std::make_unique<MenuPopup>(items_[(size_t)i].sub, this);
    MenuPopup* raw = sub.get();
    SizeF sz = sub->DesiredSize(nullptr);
    RectF row = RowRect(i);
    SizeF client = h->Size();
    float x = Bounds().r - 4, y = row.t - kPadV;
    if (x + sz.w > client.w - 4) x = std::max(4.f, Bounds().l - sz.w + 4);
    y = std::clamp(y, 4.f, std::max(4.f, client.h - sz.h - 4));
    sub->SetBounds(RectF::XYWH(x, y, sz.w, sz.h));
    PopupOptions o = MenuOptions(this);
    o.onClosed = [this, raw] { if (sub_ == raw) { sub_ = nullptr; subIndex_ = -1; Invalidate(); } };
    sub_ = h->OpenPopup(std::move(sub), std::move(o));
    subIndex_ = i;
    if (highlight_ != i) { highlight_ = i; Invalidate(); }
    h->SetFocus(this);
    Raise(UiEvent::MenuOpened);
}

void MenuPopup::CloseSubMenu() {
    if (!sub_) return;
    Widget* s = sub_;
    sub_ = nullptr;
    subIndex_ = -1;
    if (UiHost* h = Host()) h->ClosePopup(s);
}

void MenuPopup::CloseChain() {
    MenuPopup* root = this;
    while (root->parent_) root = root->parent_;
    if (UiHost* h = root->Host()) h->ClosePopup(root);
}

void MenuPopup::FocusSubMenuFirst() {
    MenuPopup* s = SubMenu();
    UiHost* h = Host();
    if (!s || !h) return;
    h->SetFocus(s, true);
    if (s->highlight_ < 0) s->MoveHighlight(1);
}

void MenuPopup::Activate(int i) {
    if (i < 0 || i >= (int)items_.size()) return;
    MenuItemDesc& it = items_[(size_t)i];
    if (!it.enabled || it.kind == MenuItemKind::Separator) return;
    if (it.kind == MenuItemKind::SubMenu) {
        if (!sub_ || subIndex_ != i) OpenSubMenu(i);   // a click on the open submenu's item keeps it
        if (sub_) if (UiHost* h = Host()) h->SetFocus(sub_, h->FocusVisible());
        return;
    }
    // Copy the callbacks: closing the chain destroys this widget (graveyard) before they run.
    auto onInvoke = it.onInvoke;
    auto onToggled = it.onToggled;
    bool nowChecked = it.kind == MenuItemKind::Check ? !it.checked : true;
    MenuItemKind kind = it.kind;
    CloseChain();
    if (kind == MenuItemKind::Check && onToggled) onToggled(nowChecked);
    if (onInvoke) onInvoke();
}

Widget* MenuPopup::HitTest(PointF p) {
    if (Widget* w = Widget::HitTest(p)) return w;
    // The open submenu is the host's top popup and light dismiss stops its hit test there: answer for the menus below
    // it in the chain so the pointer still reaches them (hover, click), as in a native menu.
    for (MenuPopup* m = parent_; m; m = m->parent_)
        if (m->Visible() && m->Bounds().Contains(p)) return m;
    return nullptr;
}

bool MenuPopup::OnPointerDown(const PointerEvent& e) { return e.button == PointerButton::Left; }

bool MenuPopup::OnPointerMove(const PointerEvent& e) {
    UiHost* h = Host();
    if (h && h->Focused() != this) h->SetFocus(this, h->FocusVisible());   // keys go to the menu under the pointer
    if (parent_) parent_->KeepSubMenu();
    int i = IndexAt(e.pt);
    if (i >= 0 && items_[(size_t)i].kind == MenuItemKind::Separator) i = -1;
    if (i >= 0) HoverItem(i);
    else if (!sub_ && highlight_ >= 0) SetHighlight(-1);
    return true;
}

bool MenuPopup::OnPointerUp(const PointerEvent& e) {
    if (e.button != PointerButton::Left) return false;
    int i = IndexAt(e.pt);
    if (i >= 0) Activate(i);
    return true;
}

void MenuPopup::OnPointerLeave() {
    if (sub_) KeepSubMenu();             // into the submenu or out of the chain: the open submenu's item stays lit
    else if (highlight_ >= 0) SetHighlight(-1);
}

void MenuPopup::MoveHighlight(int dir) {
    int n = (int)items_.size();
    if (n == 0) return;
    int i = highlight_;
    for (int k = 0; k < n; k++) {
        i = (i + dir + n) % n;
        if (i < 0) i = dir > 0 ? 0 : n - 1;
        const MenuItemDesc& it = items_[(size_t)i];
        if (it.kind != MenuItemKind::Separator && it.enabled) { SetHighlight(i); return; }
    }
}

bool MenuPopup::OnKeyDown(int vk, unsigned mods) {
    (void)mods;
    switch (vk) {
    case key::Down: MoveHighlight(1); return true;
    case key::Up: if (highlight_ < 0) highlight_ = 0; MoveHighlight(-1); return true;
    case key::Home: highlight_ = -1; MoveHighlight(1); return true;
    case key::End: highlight_ = 0; MoveHighlight(-1); return true;
    case key::Right:
        if (IsSubMenuItem(highlight_)) {
            if (!sub_ || subIndex_ != highlight_) OpenSubMenu(highlight_);
            FocusSubMenuFirst();
            return true;
        }
        return false;
    case key::Left:   // one level up (the root menu has none)
        if (parent_) { if (UiHost* h = Host()) h->ClosePopup(this); return true; }
        return false;
    case key::Escape:   // one level up; on the root menu: close it
        if (parent_) { if (UiHost* h = Host()) h->ClosePopup(this); }
        else CloseChain();
        return true;
    case key::Enter: case key::Space:
        if (IsSubMenuItem(highlight_)) {
            if (!sub_ || subIndex_ != highlight_) OpenSubMenu(highlight_);
            FocusSubMenuFirst();
            return true;
        }
        Activate(highlight_);
        return true;
    default: return false;
    }
}

MenuPopup* ShowMenu(UiHost& host, std::vector<MenuItemDesc> items, PointF at, IRenderer* measurer, Widget* focusReturn) {
    auto m = std::make_unique<MenuPopup>(std::move(items));
    MenuPopup* raw = m.get();
    SizeF sz = m->DesiredSize(measurer);
    SizeF client = host.Size();
    float x = std::clamp(at.x, 4.f, std::max(4.f, client.w - sz.w - 4));
    float y = std::clamp(at.y, 4.f, std::max(4.f, client.h - sz.h - 4));
    m->SetBounds(RectF::XYWH(x, y, sz.w, sz.h));
    host.OpenPopup(std::move(m), MenuOptions(focusReturn));
    raw->Raise(UiEvent::MenuOpened);
    return raw;
}

MenuPopup* ShowMenu(UiHost& host, std::vector<MenuItemDesc> items, Widget* anchor, IRenderer* measurer) {
    auto m = std::make_unique<MenuPopup>(std::move(items));
    MenuPopup* raw = m.get();
    SizeF sz = m->DesiredSize(measurer);
    RectF a = anchor ? anchor->Bounds() : RectF{};
    SizeF client = host.Size();
    float y = a.b + 4;
    if (y + sz.h > client.h - 4 && a.t - 4 - sz.h >= 4) y = a.t - 4 - sz.h;
    float x = std::clamp(a.l, 4.f, std::max(4.f, client.w - sz.w - 4));
    y = std::clamp(y, 4.f, std::max(4.f, client.h - sz.h - 4));
    m->SetBounds(RectF::XYWH(x, y, sz.w, sz.h));
    host.OpenPopup(std::move(m), MenuOptions(anchor));
    raw->Raise(UiEvent::MenuOpened);
    return raw;
}

}
