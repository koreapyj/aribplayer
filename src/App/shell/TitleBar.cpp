#include "shell/TitleBar.h"

#include <cmath>

#include "Localize.h"
#include "pui/Buttons.h"

namespace app {

namespace G = pui::glyph;

TitleBar::TitleBar() : pui::Widget(pui::Role::Group) {
    SetName(Tr(L"Title bar", L"제목 표시줄"));
    SetAutomationId(L"TitleBar");
    menu_ = Add<GlyphButton>(Tr(L"Main menu", L"메인 메뉴"), G::Str(G::Hamburger), 16.f, GlyphButton::Shape::Rect);
    menu_->SetToolTip(Tr(L"Main menu", L"메인 메뉴"));
    menu_->SetAutomationId(L"TitleMainMenu");
    menu_->onInvoke = [this] { if (onMainMenu) onMainMenu(); };

    pin_ = Add<GlyphButton>(Tr(L"Always on top", L"항상 위에"), G::Str(G::Pin), 14.f, GlyphButton::Shape::Rect);
    pin_->SetAutomationId(L"TitlePin");
    pin_->SetToolTip(PinToolTip(topmost_));
    pin_->onInvoke = [this] { if (onPin) onPin(); };

    min_ = Add<GlyphButton>(Tr(L"Minimize", L"최소화"), G::Str(G::ChromeMinimize), 10.f, GlyphButton::Shape::Rect);
    min_->SetAutomationId(L"TitleMinimize");
    min_->SetToolTip(Tr(L"Minimize", L"최소화"));
    min_->onInvoke = [this] { if (onMinimize) onMinimize(); };

    max_ = Add<GlyphButton>(Tr(L"Maximize", L"최대화"), G::Str(G::ChromeMaximize), 10.f, GlyphButton::Shape::Rect);
    max_->SetAutomationId(L"TitleMaximize");
    max_->SetToolTip(Tr(L"Maximize", L"최대화"));
    max_->onInvoke = [this] { if (onMaximize) onMaximize(); };

    exitFs_ = Add<GlyphButton>(Tr(L"Exit full screen", L"전체 화면 해제"), G::Str(G::BackToWindow), 10.f, GlyphButton::Shape::Rect);
    exitFs_->SetAutomationId(L"ExitFullscreen");
    exitFs_->SetToolTip(Tr(L"Exit full screen", L"전체 화면 해제"));
    exitFs_->SetVisible(false);
    exitFs_->onInvoke = [this] { if (onExitFullscreen) onExitFullscreen(); };

    close_ = Add<GlyphButton>(Tr(L"Close", L"닫기"), G::Str(G::ChromeClose), 10.f, GlyphButton::Shape::Rect);
    close_->SetAutomationId(L"TitleClose");
    close_->SetToolTip(Tr(L"Close", L"닫기"));
    close_->SetCaptionCloseStyle(true);
    close_->onInvoke = [this] { if (onClose) onClose(); };
}

std::wstring TitleBar::PinToolTip(wvp::AlwaysOnTop mode) {
    switch (mode) {
    case wvp::AlwaysOnTop::Enable: return Tr(L"Cycle always on top (Always on top)", L"항상 위에 전환(항상 위에)");
    case wvp::AlwaysOnTop::WhenPlaying: return Tr(L"Cycle always on top (On top when playing)", L"항상 위에 전환(재생시 항상 위에)");
    case wvp::AlwaysOnTop::WhenFullscreen: return Tr(L"Cycle always on top (On top when full screen)", L"항상 위에 전환(전체 화면시 항상 위에)");
    default: return Tr(L"Cycle always on top", L"항상 위에 전환");
    }
}

void TitleBar::SetTitle(std::wstring title) {   // empty (nothing open): the strip shows no name
    if (title == title_) return;
    title_ = std::move(title);
    Invalidate();
}

void TitleBar::SetFullscreenMode(bool fullscreen) {
    if (fullscreen_ == fullscreen) return;
    fullscreen_ = fullscreen;
    pin_->SetVisible(!fullscreen);
    max_->SetVisible(!fullscreen);
    exitFs_->SetVisible(fullscreen);
    OnLayout();
    Invalidate();
}

void TitleBar::SetMaximized(bool maximized) {
    if (maximized == maximized_) return;
    maximized_ = maximized;
    max_->SetGlyph(G::Str(maximized ? G::ChromeRestore : G::ChromeMaximize));
    const std::wstring text = maximized ? Tr(L"Restore", L"이전 크기로 복원") : Tr(L"Maximize", L"최대화");
    max_->SetName(text);
    max_->SetToolTip(text);
}

void TitleBar::SetTopmostMode(wvp::AlwaysOnTop mode) {
    if (mode == topmost_) return;
    topmost_ = mode;
    pin_->SetGlyph(G::Str(mode == wvp::AlwaysOnTop::Disable ? G::Pin : G::Pinned));
    pin_->SetToolTip(PinToolTip(mode));
}

void TitleBar::SetScale(float scale) {
    scale_ = std::max(0.25f, scale);
    Invalidate();
}

TitleLayout TitleBar::Layout() const {
    TitleLayoutIn in;
    in.widthDip = Bounds().w();
    in.heightDip = Bounds().h() > 0 ? Bounds().h() : kStripDip;
    in.fullscreen = fullscreen_;
    return TitleLayout::Compute(in);
}

void TitleBar::OnLayout() {
    const TitleLayout l = Layout();
    const float ox = Bounds().l, oy = Bounds().t;
    auto place = [&](pui::Widget* w, const pui::RectF& r) {
        if (!r.Empty()) w->SetBounds({r.l + ox, r.t + oy, r.r + ox, r.b + oy});
    };
    place(menu_, l.mainMenu);
    place(pin_, l.pin);
    place(min_, l.min);
    place(max_, l.max);
    place(exitFs_, l.max);
    place(close_, l.close);
}

pui::Widget* TitleBar::HitTest(pui::PointF p) {
    if (!Visible()) return nullptr;
    const TitleZone z = Zone(p);
    if (z == TitleZone::None || z == TitleZone::Drag) return nullptr;   // the drag area is HTCAPTION, not a UI target
    pui::Widget* w = Widget::HitTest(p);
    return w == this ? nullptr : w;
}

void TitleBar::Paint(pui::IRenderer& r, const pui::Theme& t) {
    const pui::RectF& b = Bounds();
    if (fullscreen_) {
        r.FillRect(b, t.barFill);
        const float stroke = std::max(1.f / scale_, 0.01f);
        r.Line({b.l, b.b - stroke * 0.5f}, {b.r, b.b - stroke * 0.5f}, t.barStroke, stroke);
    }
    const TitleLayout l = Layout();
    if (l.name.Empty()) return;
    pui::TextStyle ts;
    ts.size = 12;
    ts.ellipsis = true;
    r.Text(title_, pui::RectF{l.name.l + b.l, l.name.t + b.t, l.name.r + b.l, l.name.b + b.t}, ts, t.textSecondary);
}

}
