#include "features/MiniBar.h"
#include "Localize.h"

#include <algorithm>

namespace app {

using pui::RectF;

MiniBar::MiniBar() : pui::Widget(pui::Role::Group) {
    SetName(Tr(L"Mini player controls", L"미니 플레이어 제어"));
    SetAutomationId(L"MiniBar");
    seek_ = Add<pui::Slider>();
    seek_->SetName(Tr(L"Seek", L"탐색"));
    seek_->SetAutomationId(L"MiniSeek");
    seek_->SetRange(0, 1);
    seek_->SetEnabled(false);
    seek_->valueFormatter = [](double v) { return FormatTime(v); };
    seek_->onChanged = [this](double v, bool dragging) {
        if (dragging) { Invalidate(); return; }
        if (model_.dur > 0 && actions_.seekTo) actions_.seekTo(std::clamp(v / model_.dur, 0.0, 1.0));
    };

    auto mk = [&](const wchar_t* id, CommandId cmd) {
        auto* b = Add<pui::IconButton>();
        b->SetAutomationId(id);
        b->SetSize(32);
        b->SetGlyphSize(16);
        b->SetAcceleratorKey(ShortcutText(cmd));
        b->onClick = [this, cmd] { Fire(cmd); };
        return b;
    };
    prev_ = mk(L"MiniPrevious", CommandId::Previous);
    play_ = mk(L"MiniPlayPause", CommandId::PlayPause);
    play_->SetControlFill(true);
    next_ = mk(L"MiniNext", CommandId::Next);
    exit_ = mk(L"MiniBackToFullView", CommandId::ToggleMiniPlayer);
    exit_->SetGlyph(pui::glyph::ExitMiniPlayer);
    exit_->SetLabel(Tr(L"Back to full view", L"전체 보기로 돌아가기"));
    ApplyModel();
}

void MiniBar::SetScale(float scale) {
    scale_ = std::max(0.25f, scale);
    OnLayout();
    Invalidate();
}

RectF MiniBar::ToAbs(const RectF& l) const {
    const RectF& b = Bounds();
    return {b.l + l.l, b.t + l.t, b.l + l.r, b.t + l.b};
}

void MiniBar::OnLayout() {
    layout_ = ComputeMiniBarLayout((int)std::lround(Bounds().w() * scale_), scale_);
    seek_->SetBounds(ToAbs(layout_.slider));
    auto place = [&](pui::IconButton* w, bool show, const RectF& r) {
        w->SetVisible(show);
        if (show) w->SetBounds(ToAbs(r));
    };
    place(prev_, layout_.showPrev, layout_.prev);
    place(play_, layout_.showPlay, layout_.play);
    place(next_, layout_.showNext, layout_.next);
    place(exit_, layout_.showExit, layout_.exit);
}

void MiniBar::SetModel(const BarModel& m) {
    model_ = m;
    ApplyModel();
    Invalidate();
}

void MiniBar::ApplyModel() {
    auto glyphOf = [](const BtnInfo& i) { return i.glyph.empty() ? wchar_t(0) : i.glyph[0]; };
    BtnInfo ip = ButtonInfo(model_, B_Prev), il = ButtonInfo(model_, B_Play), in = ButtonInfo(model_, B_Next);
    // The mini player's Prev / Next only switch files (the dock's left click seeks 5 s).
    ip.tip = Tr(L"Previous", L"이전"); ip.enabled = model_.hasPrev;
    in.tip = Tr(L"Next", L"다음"); in.enabled = model_.hasNext;
    il.tip = model_.playing ? Tr(L"Pause", L"일시 정지") : Tr(L"Play", L"재생");
    prev_->SetGlyph(glyphOf(ip)); prev_->SetLabel(ip.tip); prev_->SetEnabled(ip.enabled);
    play_->SetGlyph(glyphOf(il)); play_->SetLabel(il.tip); play_->SetEnabled(il.enabled);
    next_->SetGlyph(glyphOf(in)); next_->SetLabel(in.tip); next_->SetEnabled(in.enabled);
    exit_->SetEnabled(true);

    bool en = model_.Seekable();
    seek_->SetEnabled(en);
    if (model_.dur > 0) {
        seek_->SetRange(0, model_.dur);
        seek_->SetStep(1);
        seek_->SetLargeStep(10);
    } else {
        seek_->SetRange(0, 1);
    }
    if (!seek_->IsDragging()) seek_->SetValue(model_.dur > 0 ? std::clamp(model_.pos, 0.0, model_.dur) : 0.0);
}

void MiniBar::Paint(pui::IRenderer& r, const pui::Theme& t) {
    const RectF& b = Bounds();
    r.FillRect(b, t.barFill);
    r.FillRect(RectF{b.l, b.t, b.r, b.t + std::max(1.f / scale_, 0.5f)}, t.barStroke);
    if (layout_.showTime) {
        pui::TextStyle ts;
        ts.size = 12;
        ts.ellipsis = false;
        double shown = seek_->IsDragging() ? seek_->Value() : model_.pos;
        std::wstring text = FormatTime(shown) + L" / " + (model_.dur > 0 ? FormatTime(model_.dur) : std::wstring(L"--:--"));
        r.Text(text, ToAbs(layout_.time), ts, t.textSecondary);
    }
}

}
