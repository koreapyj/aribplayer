#include "pui/ScrollViewer.h"

namespace pui {


RectF ScrollBarController::ThumbRect() const {
    RectF in = InnerTrack();
    float len = std::max(0.f, in.h());
    float tl = len;
    if (Needed()) tl = std::clamp(len * viewport_ / extent_, std::min(kMinThumb, len), len);
    float travel = len - tl;
    float mo = MaxOffset();
    float frac = mo > 0 ? std::clamp(offset_ / mo, 0.f, 1.f) : 0.f;
    float top = in.t + travel * frac;
    float w = kThumbCollapsedW + (kThumbExpandedW - kThumbCollapsedW) * expand_;
    return {view_.r - kEdgeInset - w, top, view_.r - kEdgeInset, top + tl};
}

ScrollBarController::Part ScrollBarController::HitPart(PointF p) const {
    if (!Needed() || !ZoneRect().Contains(p)) return Part::None;
    if (p.y < view_.t + kArrowH) return Part::ArrowUp;
    if (p.y >= view_.b - kArrowH) return Part::ArrowDown;
    RectF th = ThumbRect();
    if (p.y < th.t) return Part::TrackUp;
    if (p.y >= th.b) return Part::TrackDown;
    return Part::Thumb;
}

bool ScrollBarController::PointerDown(PointF p, uint64_t now, float arrowStep, float& newOffset) {
    Part part = HitPart(p);
    if (part == Part::None) return false;
    Poke(now);
    hover_ = true;
    newOffset = offset_;
    switch (part) {
    case Part::Thumb: dragging_ = true; grab_ = p.y - ThumbRect().t; break;
    case Part::TrackUp: newOffset = ClampOffset(offset_ - viewport_, viewport_, extent_); break;
    case Part::TrackDown: newOffset = ClampOffset(offset_ + viewport_, viewport_, extent_); break;
    case Part::ArrowUp: newOffset = ClampOffset(offset_ - arrowStep, viewport_, extent_); break;
    case Part::ArrowDown: newOffset = ClampOffset(offset_ + arrowStep, viewport_, extent_); break;
    default: break;
    }
    return true;
}

bool ScrollBarController::PointerMove(PointF p, uint64_t now, float& newOffset) {
    if (dragging_) {
        RectF in = InnerTrack();
        RectF th = ThumbRect();
        float travel = in.h() - th.h();
        float top = p.y - grab_;
        float frac = travel > 0 ? std::clamp((top - in.t) / travel, 0.f, 1.f) : 0.f;
        newOffset = frac * MaxOffset();
        Poke(now);
        return true;
    }
    bool nh = HitPart(p) != Part::None;
    if (nh) Poke(now);
    else if (hover_) Poke(now);   // starts the fade delay when leaving the zone
    hover_ = nh;
    return false;
}

void ScrollBarController::PointerUp(uint64_t now) {
    if (dragging_) { dragging_ = false; Poke(now); }
}

void ScrollBarController::Leave(uint64_t now) {
    if (dragging_) return;
    if (hover_) Poke(now);
    hover_ = false;
}

bool ScrollBarController::Update(uint64_t now) {
    uint64_t dt = lastUpdate_ == 0 || now < lastUpdate_ ? 16 : std::min<uint64_t>(now - lastUpdate_, kExpandMs);
    lastUpdate_ = now;
    float target = (hover_ || dragging_) ? 1.f : 0.f;
    float step = (float)dt / (float)kExpandMs;
    if (expand_ < target) expand_ = std::min(target, expand_ + step);
    else if (expand_ > target) expand_ = std::max(target, expand_ - step);
    if (expand_ != target) return true;
    lastUpdate_ = 0;   // idle: next animation starts with a fresh delta
    return active_ && !hover_ && !dragging_ && Opacity(now) > 0.f;
}

float ScrollBarController::Opacity(uint64_t now) const {
    if (!active_) return 0.f;
    if (hover_ || dragging_) return 1.f;
    int64_t age = (int64_t)now - (int64_t)lastActivity_;
    if (age < (int64_t)kFadeDelayMs) return 1.f;
    return std::clamp(1.f - (float)(age - (int64_t)kFadeDelayMs) / (float)kFadeMs, 0.f, 1.f);
}

void ScrollBarController::Paint(IRenderer& r, const Theme& t, uint64_t now) const {
    float op = Opacity(now);
    if (!Needed() || op <= 0.001f) return;
    r.PushOpacity(op);
    if (expand_ > 0.01f) {
        RectF z = ZoneRect();
        r.FillRoundRect(z.Deflate(1, 2, 1, 2), 6, MulAlpha(t.flyoutSurface, 0.9f * expand_));
        TextStyle st;
        st.font = FontKind::Icon;
        st.size = 8;
        st.align = TextAlign::Center;
        st.ellipsis = false;
        r.Text(L"", {z.l, z.t, z.r, z.t + kArrowH}, st, MulAlpha(t.textSecondary, expand_));
        r.Text(L"", {z.l, z.b - kArrowH, z.r, z.b}, st, MulAlpha(t.textSecondary, expand_));
    }
    RectF th = ThumbRect();
    r.FillRoundRect(th, th.w() * 0.5f, t.ctlStrongFill);
    r.PopOpacity();
}


ScrollViewer::ScrollViewer() : Widget(Role::Pane) {
    SetClipChildren(true);
    overlay_ = Add<OverlayWidget>();
    overlay_->onPaint = [this](IRenderer& r, const Theme& t) { sb_.Paint(r, t, Now()); };
}

uint64_t ScrollViewer::Now() const {
    UiHost* h = Host();
    return h ? h->Now() : 0;
}

void ScrollViewer::Animate() {
    if (UiHost* h = Host()) h->StartAnimating(this);
}

Widget* ScrollViewer::SetContent(std::unique_ptr<Widget> content) {
    if (content_) RemoveChild(content_);
    content_ = content.get();
    if (content) AddChild(std::move(content), 0);
    OnLayout();
    return content_;
}

void ScrollViewer::SetExtentHeight(float h) {
    explicitExtent_ = h;
    OnLayout();
    Invalidate();
}

float ScrollViewer::ComputeExtent() {
    if (explicitExtent_ >= 0) return explicitExtent_;
    if (content_) return content_->DesiredSize(measurer_).h;
    return 0;
}

void ScrollViewer::OnLayout() {
    RectF view = Bounds();
    extent_ = ComputeExtent();
    offset_ = ScrollBarController::ClampOffset(offset_, view.h(), extent_);
    sb_.SetView(view);
    sb_.SetMetrics(view.h(), extent_, offset_);
    if (content_) {
        float top = view.t - offset_;
        content_->SetBounds({view.l, top, view.r, top + std::max(extent_, view.h())});
    }
    if (overlay_) overlay_->SetBounds(view);
}

void ScrollViewer::SetOffset(float o) {
    o = ScrollBarController::ClampOffset(o, Bounds().h(), extent_);
    if (o == offset_) return;
    offset_ = o;
    OnLayout();
    if (UiHost* h = Host()) { sb_.Poke(h->Now()); h->StartAnimating(this); }
    Invalidate();
}

void ScrollViewer::ScrollIntoView(const RectF& rect) {
    RectF view = Bounds();
    float top = rect.t - view.t + offset_, bottom = rect.b - view.t + offset_;
    if (top < offset_ || bottom - top > view.h()) SetOffset(top);
    else if (bottom > offset_ + view.h()) SetOffset(bottom - view.h());
}

Widget* ScrollViewer::HitTest(PointF p) {
    if (!Visible() || !Bounds().Contains(p)) return nullptr;
    if (sb_.HitPart(p) != ScrollBarController::Part::None) return this;
    return Widget::HitTest(p);
}

bool ScrollViewer::OnPointerDown(const PointerEvent& e) {
    if (e.button != PointerButton::Left) return false;
    float no = offset_;
    if (!sb_.PointerDown(e.pt, Now(), 24.f, no)) return false;
    SetOffset(no);
    Animate();
    return true;
}

bool ScrollViewer::OnPointerMove(const PointerEvent& e) {
    float no = offset_;
    bool hov = sb_.Hover();
    if (sb_.PointerMove(e.pt, Now(), no)) { SetOffset(no); Animate(); return true; }
    if (hov != sb_.Hover()) Animate();
    return false;
}

bool ScrollViewer::OnPointerUp(const PointerEvent&) {
    if (!sb_.Dragging()) return false;
    sb_.PointerUp(Now());
    Animate();
    return true;
}

void ScrollViewer::OnPointerLeave() {
    sb_.Leave(Now());
    Animate();
}

bool ScrollViewer::OnPointerWheel(const PointerEvent& e) {
    if ((e.mods & ModCtrl) || e.wheelDelta == 0 || MaxOffset() <= 0) return false;
    ScrollBy(-e.wheelDelta * wheelStep_);
    return true;
}

bool ScrollViewer::OnKeyDown(int vk, unsigned mods) {
    if ((mods & (ModCtrl | ModAlt)) || MaxOffset() <= 0) return false;
    switch (vk) {
    case key::PageUp: ScrollBy(-Bounds().h()); return true;
    case key::PageDown: ScrollBy(Bounds().h()); return true;
    case key::Home: SetOffset(0); return true;
    case key::End: SetOffset(MaxOffset()); return true;
    default: return false;
    }
}

bool ScrollViewer::OnTick(uint64_t nowMs) { return sb_.Update(nowMs); }

double ScrollViewer::VerticalScrollPercent() const {
    float mo = MaxOffset();
    return mo > 0 ? (double)offset_ / mo * 100.0 : -1.0;
}

double ScrollViewer::VerticalViewSize() const {
    return extent_ > 0 ? std::min(100.0, (double)Bounds().h() / extent_ * 100.0) : 100.0;
}

void ScrollViewer::SetVerticalScrollPercent(double pct) {
    SetOffset((float)(std::clamp(pct, 0.0, 100.0) / 100.0 * MaxOffset()));
}

}
