#include "pui/Menu.h"
#include "pui/UiText.h"

#include <algorithm>

namespace pui {


namespace {
SizeF FitSize(SizeF size, SizeF client, float margin) {
    float mx = std::min(margin, std::max(0.f, client.w * 0.5f));
    float my = std::min(margin, std::max(0.f, client.h * 0.5f));
    return {std::clamp(size.w, 0.f, std::max(0.f, client.w - 2 * mx)),
            std::clamp(size.h, 0.f, std::max(0.f, client.h - 2 * my))};
}
float ClampAxis(float v, float size, float total, float margin) {
    float m = std::min(margin, std::max(0.f, total * 0.5f));
    float fittedSize = std::min(std::max(0.f, size), std::max(0.f, total - 2 * m));
    return std::clamp(v, m, std::max(m, total - m - fittedSize));
}
}

RectF PlaceFlyout(SizeF size, const RectF& a, Placement p, SizeF client, float gap, float margin, Placement* actual) {
    size = FitSize(size, client, margin);
    auto fits = [&](Placement q) {
        switch (q) {
        case Placement::Bottom: return a.b + gap + size.h <= client.h - margin;
        case Placement::Top: return a.t - gap - size.h >= margin;
        case Placement::Right: return a.r + gap + size.w <= client.w - margin;
        case Placement::Left: return a.l - gap - size.w >= margin;
        default: return false;
        }
    };
    auto opposite = [](Placement q) {
        switch (q) {
        case Placement::Bottom: return Placement::Top;
        case Placement::Top: return Placement::Bottom;
        case Placement::Left: return Placement::Right;
        default: return Placement::Left;
        }
    };
    Placement use = Placement::Bottom;
    if (p == Placement::Auto) {
        const Placement order[] = {Placement::Bottom, Placement::Top, Placement::Right, Placement::Left};
        bool found = false;
        for (Placement q : order) if (fits(q)) { use = q; found = true; break; }
        if (!found) use = Placement::Bottom;
    } else {
        use = p;
        if (!fits(use) && fits(opposite(use))) use = opposite(use);
    }
    RectF r;
    switch (use) {
    case Placement::Bottom: r = RectF::XYWH(a.cx() - size.w * 0.5f, a.b + gap, size.w, size.h); break;
    case Placement::Top: r = RectF::XYWH(a.cx() - size.w * 0.5f, a.t - gap - size.h, size.w, size.h); break;
    case Placement::Right: r = RectF::XYWH(a.r + gap, a.cy() - size.h * 0.5f, size.w, size.h); break;
    default: r = RectF::XYWH(a.l - gap - size.w, a.cy() - size.h * 0.5f, size.w, size.h); break;
    }
    float x = ClampAxis(r.l, size.w, client.w, margin), y = ClampAxis(r.t, size.h, client.h, margin);
    if (actual) *actual = use;
    return RectF::XYWH(x, y, size.w, size.h);
}


Flyout::Flyout() : Widget(Role::Group) {}

Widget* Flyout::SetContent(std::unique_ptr<Widget> content) {
    if (content_) RemoveChild(content_);
    content_ = AddChild(std::move(content));
    RequestLayout();
    return content_;
}

SizeF Flyout::DesiredSize(IRenderer* m) {
    SizeF c = content_ ? content_->DesiredSize(m) : SizeF{};
    return {c.w + 2 * padding_, c.h + 2 * padding_};
}

void Flyout::OnLayout() {
    if (content_) content_->SetBounds(Bounds().Deflate(padding_, padding_, padding_, padding_));
}

void Flyout::Paint(IRenderer& r, const Theme& t) {
    r.FillRoundRect(Bounds(), t.overlayRadius, t.acrylicEnabled ? Color(0x00000000) : t.acrylicFallbackColor);
    r.StrokeRoundRect(Bounds().Deflate(0.5f, 0.5f, 0.5f, 0.5f), t.overlayRadius, t.flyoutStroke, 1);
}

void Flyout::Hide() {
    if (UiHost* h = Host()) h->ClosePopup(this);
}

Flyout* Flyout::ShowAt(UiHost& host, std::unique_ptr<Flyout> f, Widget* anchor, Placement p, IRenderer* m) {
    return ShowAt(host, std::move(f), anchor ? anchor->Bounds() : RectF{}, p, m, anchor);
}

Flyout* Flyout::ShowAt(UiHost& host, std::unique_ptr<Flyout> f, const RectF& anchorRect, Placement p, IRenderer* m, Widget* focusReturn) {
    if (!f) return nullptr;
    Flyout* raw = f.get();
    SizeF sz = f->DesiredSize(m);
    Placement used = p;
    RectF r = PlaceFlyout(sz, anchorRect, p, host.Size(), 8, 4, &used);
    raw->actual_ = used;
    f->SetBounds(r);
    PopupOptions o;
    o.acrylicSurface = true;
    o.anchor = focusReturn;
    o.onClosed = [raw] { if (raw->onClosed) raw->onClosed(); };
    host.OpenPopup(std::move(f), std::move(o));
    return raw;
}

}
