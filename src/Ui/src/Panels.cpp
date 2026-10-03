#include "pui/Panels.h"

#include <algorithm>

namespace pui {


TextBlock::TextBlock(std::wstring text, TextKind kind) : Widget(Role::Text) {
    SetHitTestVisible(false);
    SetKind(kind);
    SetText(std::move(text));
}

void TextBlock::SetKind(TextKind k) {
    switch (k) {
    case TextKind::Body: size_ = 14; weight_ = 400; break;
    case TextKind::BodyStrong: size_ = 14; weight_ = 600; break;
    case TextKind::Caption: size_ = 12; weight_ = 400; break;
    case TextKind::Subtitle: size_ = 20; weight_ = 600; break;
    case TextKind::Title: size_ = 28; weight_ = 600; break;
    }
    Changed();
}

void TextBlock::SetText(std::wstring t) {
    if (t == text_) return;
    std::wstring old = std::move(text_);
    text_ = std::move(t);
    SyncAutoName(*this, old, text_);
    Changed();
}

TextStyle TextBlock::Style() const {
    TextStyle s = BodyTextStyle(size_, weight_, align_);
    s.wrap = wrap_;
    s.ellipsis = !wrap_;
    s.valign = wrap_ ? ParaAlign::Top : ParaAlign::Center;
    return s;
}

SizeF TextBlock::DesiredSize(IRenderer* m) {
    float maxW = (wrap_ && Bounds().w() > 0) ? Bounds().w() : 0;
    return MeasurerOr(m).MeasureText(text_, Style(), maxW);
}

float TextBlock::HeightForWidth(IRenderer* m, float width) {
    return MeasurerOr(m).MeasureText(text_, Style(), wrap_ ? width : 0).h;
}

void TextBlock::Paint(IRenderer& r, const Theme& t) {
    Color c = t.textPrimary;
    switch (color_) {
    case TextColorRole::Primary: c = t.textPrimary; break;
    case TextColorRole::Secondary: c = t.textSecondary; break;
    case TextColorRole::Tertiary: c = t.textTertiary; break;
    case TextColorRole::Accent: c = t.accent; break;
    case TextColorRole::Disabled: c = t.textDisabled; break;
    }
    if (!IsEffectivelyEnabled()) c = t.textDisabled;
    if (!text_.empty()) r.Text(text_, Bounds(), Style(), c);
}


void Divider::Paint(IRenderer& r, const Theme& t) {
    RectF b = Bounds();
    if (vertical_) r.Line({b.cx(), b.t}, {b.cx(), b.b}, t.dividerStroke, 1);
    else r.Line({b.l, b.cy()}, {b.r, b.cy()}, t.dividerStroke, 1);
}


StackPanel::StackPanel(Orientation o, float spacing, Role role) : Widget(role), orient_(o), spacing_(spacing) {}

namespace {
bool IsFlexSpacer(Widget* w) {
    auto* s = dynamic_cast<Spacer*>(w);
    return s && s->Flex();
}
}

void StackPanel::OnLayout() {
    RectF b = Bounds();
    RectF in = b.Deflate(pad_.l, pad_.t, pad_.r, pad_.b);
    bool vert = orient_ == Orientation::Vertical;
    IRenderer* m = nullptr;
    struct Item { Widget* w; SizeF d; bool flex; };
    std::vector<Item> items;
    float fixedMain = 0;
    int flexCount = 0;
    for (auto& c : Children()) {
        if (!c->Visible()) continue;
        Item it{c.get(), {}, IsFlexSpacer(c.get())};
        if (it.flex) flexCount++;
        else {
            it.d = c->DesiredSize(m);
            if (vert && cross_ == CrossAlign::Stretch) {
                if (auto* hw = dynamic_cast<IHeightForWidth*>(c.get())) it.d.h = hw->HeightForWidth(m, in.w());
            }
            fixedMain += vert ? it.d.h : it.d.w;
        }
        items.push_back(it);
    }
    float gaps = items.empty() ? 0 : spacing_ * (float)(items.size() - 1);
    float avail = vert ? in.h() : in.w();
    float flexShare = flexCount > 0 ? std::max(0.f, avail - fixedMain - gaps) / (float)flexCount : 0;
    float pos = vert ? in.t : in.l;
    for (auto& it : items) {
        float main = it.flex ? flexShare : (vert ? it.d.h : it.d.w);
        float crossAvail = vert ? in.w() : in.h();
        float crossSize = it.flex ? crossAvail : (vert ? it.d.w : it.d.h);
        if (cross_ == CrossAlign::Stretch) crossSize = crossAvail;
        float crossStart = vert ? in.l : in.t;
        if (cross_ == CrossAlign::Center) crossStart += (crossAvail - crossSize) * 0.5f;
        else if (cross_ == CrossAlign::End) crossStart += crossAvail - crossSize;
        RectF r = vert ? RectF::XYWH(crossStart, pos, crossSize, main) : RectF::XYWH(pos, crossStart, main, crossSize);
        it.w->SetBounds(r);
        pos += main + spacing_;
    }
}

SizeF StackPanel::DesiredSize(IRenderer* m) {
    bool vert = orient_ == Orientation::Vertical;
    float main = 0, cross = 0;
    int n = 0;
    for (auto& c : Children()) {
        if (!c->Visible()) continue;
        SizeF d = c->DesiredSize(m);
        main += vert ? d.h : d.w;
        cross = std::max(cross, vert ? d.w : d.h);
        n++;
    }
    if (n > 1) main += spacing_ * (float)(n - 1);
    return vert ? SizeF{cross + pad_.Horizontal(), main + pad_.Vertical()} : SizeF{main + pad_.Horizontal(), cross + pad_.Vertical()};
}

float StackPanel::HeightForWidth(IRenderer* m, float width) {
    float inW = width - pad_.Horizontal();
    float h = 0, maxH = 0;
    int n = 0;
    for (auto& c : Children()) {
        if (!c->Visible()) continue;
        float ch;
        if (auto* hw = dynamic_cast<IHeightForWidth*>(c.get())) ch = hw->HeightForWidth(m, inW);
        else ch = c->DesiredSize(m).h;
        h += ch;
        maxH = std::max(maxH, ch);
        n++;
    }
    if (orient_ == Orientation::Vertical) return h + (n > 1 ? spacing_ * (float)(n - 1) : 0.f) + pad_.Vertical();
    return maxH + pad_.Vertical();
}


Card::Card(Orientation o, float spacing) : StackPanel(o, spacing) {
    SetPadding(Thickness::All(16));
}

void Card::Paint(IRenderer& r, const Theme& t) {
    r.FillRoundRect(Bounds(), t.overlayRadius, t.cardFill);
    r.StrokeRoundRect(Bounds().Deflate(0.5f, 0.5f, 0.5f, 0.5f), t.overlayRadius, t.cardStroke, 1);
}

}
