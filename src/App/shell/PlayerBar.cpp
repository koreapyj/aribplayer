#include "shell/PlayerBar.h"
#include "Localize.h"

#include <algorithm>
#include <cmath>

#include "pui/Buttons.h"

namespace app {

using pui::RectF;

namespace {
CommandId CommandForButton(int b) {
    switch (b) {
    case B_Play: return CommandId::PlayPause;
    case B_Stop: return CommandId::Stop;
    case B_Prev: return CommandId::SeekBack5;       // right click: Previous
    case B_Next: return CommandId::SeekForward5;    // right click: Next
    case B_Open: return CommandId::OpenFiles;
    case B_Mute: return CommandId::ToggleMute;
    case B_Subs: return CommandId::ToggleSubtitles;
    case B_Control: return CommandId::ToggleControlPanel;
    case B_Panel: return CommandId::TogglePlaylistPanel;
    case B_Full: return CommandId::ToggleFullscreen;
    default: return CommandId::None;
    }
}
const wchar_t* AutomationIdFor(int b) {
    static const wchar_t* ids[B_Count] = {L"BarPlayPause", L"BarStop", L"BarPrevious", L"BarNext", L"BarOpen",
                                          L"BarMute", L"BarSubtitles", L"BarControlPanel", L"BarPlaylistPanel", L"BarFullScreen"};
    return ids[b];
}
constexpr float kVolumeWheelStep = 5;   // same step as the Up / Down keys
}

TimeReadout::TimeReadout() : pui::Widget(pui::Role::Button) {
    SetFocusable(true);
}

void TimeReadout::SetText(const std::wstring& text) {
    if (text == text_) return;
    text_ = text;
    SetName(text_);
    Invalidate();
}

void TimeReadout::Paint(pui::IRenderer& r, const pui::Theme& t) {
    pui::TextStyle ts;
    ts.size = 12;
    ts.ellipsis = false;
    r.Text(text_, Bounds(), ts, IsHovered() && IsEffectivelyEnabled() ? t.textPrimary : t.textSecondary);
    PaintFocusVisual(r, t, Bounds(), 2.f);
}

bool TimeReadout::OnPointerDown(const pui::PointerEvent& e) { return e.button == pui::PointerButton::Left; }

bool TimeReadout::OnPointerUp(const pui::PointerEvent& e) {
    if (e.button == pui::PointerButton::Left && Bounds().Contains(e.pt)) Invoke();
    return true;
}

bool TimeReadout::OnKeyDown(int vk, unsigned mods) {
    if (mods == 0 && (vk == pui::key::Enter || vk == pui::key::Space)) { Invoke(); return true; }
    return false;
}

void TimeReadout::Invoke() {
    if (!IsEffectivelyEnabled()) return;
    Raise(pui::UiEvent::Invoked);
    if (onInvoke) onInvoke();
}

PlayerBar::PlayerBar() : pui::Widget(pui::Role::Group) {
    SetName(Tr(L"Player controls", L"플레이어 제어"));
    SetAutomationId(L"PlayerBar");
    BarModel defaults;

    elapsed_ = Add<TimeReadout>();
    elapsed_->SetAutomationId(L"BarElapsed");
    elapsed_->SetToolTip(Tr(L"Show remaining time", L"남은 시간 표시"));
    elapsed_->onInvoke = [this] { Fire(CommandId::ToggleTimeMode); };

    seek_ = Add<ShellSlider>(Tr(L"Seek", L"탐색"));
    seek_->SetAutomationId(L"BarSeek");
    seek_->SetEnabled(false);
    seek_->onCommit = [this](double f) { if (actions_.seekTo) actions_.seekTo(f); UpdateReadouts(); };
    seek_->onChanging = [this](double) { UpdateReadouts(); Invalidate(); };
    seek_->valueText = [this](double f) { return FormatTime(f * model_.dur); };

    auto makeButton = [&](int b) {
        BtnInfo i = ButtonInfo(defaults, b);
        auto* btn = Add<GlyphButton>(i.tip, i.glyph, 16.f, GlyphButton::Shape::Rounded);
        buttons_[(size_t)b] = btn;
        btn->SetAutomationId(AutomationIdFor(b));
        btn->SetToolTip(i.tip);
        btn->SetAcceleratorKey(ShortcutText(CommandForButton(b)));
        btn->onInvoke = [this, b] { Fire(CommandForButton(b)); };
        return btn;
    };
    for (int b : {B_Play, B_Stop, B_Prev, B_Next, B_Open}) makeButton(b);
    for (int i = 0; i < BarLayout::kMaxChips; i++) {
        chips_[(size_t)i] = Add<Chip>(L"");
        chips_[(size_t)i]->SetAutomationId(L"BarChip" + std::to_wstring(i));
        chips_[(size_t)i]->SetVisible(false);
    }
    makeButton(B_Mute);
    volume_ = Add<ShellSlider>(Tr(L"Volume", L"볼륨"));
    volume_->SetAutomationId(L"BarVolume");
    volume_->SetRangeScale(100);
    volume_->SetSteps(0.05, 0.10);
    volume_->onChanging = [this](double f) { if (actions_.setVolume) actions_.setVolume((int)std::lround(f * 100)); };
    volume_->onCommit = [this](double f) { if (actions_.setVolume) actions_.setVolume((int)std::lround(f * 100)); };
    volume_->onWheelSteps = [this](int n) {
        if (actions_.setVolume) actions_.setVolume(std::clamp(model_.volume + (int)kVolumeWheelStep * n, 0, 100));
    };
    for (int b : {B_Subs, B_Control, B_Panel, B_Full}) makeButton(b);

    buttons_[B_Play]->SetAccentStyle(true);
    buttons_[B_Prev]->onRightClick = [this] { Fire(CommandId::Previous); };
    buttons_[B_Next]->onRightClick = [this] { Fire(CommandId::Next); };
    buttons_[B_Subs]->onRightClick = [this] { if (actions_.subtitleMenu) actions_.subtitleMenu(buttons_[B_Subs]); };
    ApplyModel();
}

void PlayerBar::SetScale(float scale) {
    scale_ = std::max(0.25f, scale);
    OnLayout();
    Invalidate();
}

RectF PlayerBar::ToDip(const RectF& px) const {
    const RectF& b = Bounds();
    return {b.l + px.l / scale_, b.t + px.t / scale_, b.l + px.r / scale_, b.t + px.b / scale_};
}

void PlayerBar::OnLayout() {
    std::vector<float> widths;
    pui::IRenderer& m = pui::MeasurerOr(nullptr);
    for (size_t i = 0; i < model_.chips.size() && i < (size_t)BarLayout::kMaxChips; i++) widths.push_back(Chip::WidthFor(m, model_.chips[i].text));
    layout_.Compute(Bounds().w(), scale_, widths);
    elapsed_->SetBounds(ToDip(layout_.elapsed));
    seek_->SetBounds(ToDip(layout_.seek));
    volume_->SetVisible(layout_.volumeVisible);
    if (layout_.volumeVisible) volume_->SetBounds(ToDip(layout_.volume));
    for (int b = 0; b < B_Count; b++) {
        BtnInfo i = ButtonInfo(model_, b);
        buttons_[(size_t)b]->SetVisible(layout_.buttonVisible[b] && i.visible);
        buttons_[(size_t)b]->SetBounds(ToDip(layout_.btn[b]));
    }
    for (int i = 0; i < BarLayout::kMaxChips; i++) {
        bool show = i < layout_.chipCount && layout_.chipVisible[i];
        chips_[(size_t)i]->SetVisible(show);
        if (show) chips_[(size_t)i]->SetBounds(ToDip(layout_.chip[i]));
    }
}

void PlayerBar::SetModel(const BarModel& m) {
    bool chipsChanged = m.chips != model_.chips;
    model_ = m;
    ApplyModel();
    if (chipsChanged) OnLayout();
    Invalidate();
}

void PlayerBar::UpdateReadouts() {
    double shown = seek_->Dragging() ? seek_->Fraction() * model_.dur : model_.pos;
    sl::TimeDisplayMode mode = model_.dur > 0 ? model_.timeMode : sl::TimeDisplayMode::Elapsed;
    elapsed_->SetText(sl::FormatTimeDisplay(mode, shown, model_.dur));
}

void PlayerBar::ApplyModel() {
    for (int b = 0; b < B_Count; b++) {
        BtnInfo i = ButtonInfo(model_, b);
        GlyphButton* btn = buttons_[(size_t)b];
        btn->SetVisible(layout_.buttonVisible[b] && i.visible);
        btn->SetEnabled(i.enabled);
        btn->SetGlyph(i.glyph);
        btn->SetToggled(i.toggled);
        btn->SetName(i.tip);
        btn->SetToolTip(i.tip);
    }
    for (int i = 0; i < BarLayout::kMaxChips; i++) {
        Chip* c = chips_[(size_t)i];
        if ((size_t)i >= model_.chips.size()) { c->SetVisible(false); continue; }
        const badge::Chip& mc = model_.chips[(size_t)i];
        c->SetText(mc.text);
    }
    elapsed_->SetToolTip(model_.timeMode == sl::TimeDisplayMode::Elapsed ? Tr(L"Show remaining time", L"남은 시간 표시") : Tr(L"Show elapsed time", L"경과 시간 표시"));
    elapsed_->SetEnabled(model_.loaded);
    UpdateReadouts();
    seek_->SetEnabled(model_.Seekable());
    seek_->SetRangeScale(model_.dur > 0 ? model_.dur : 1);
    if (model_.dur > 0) seek_->SetSteps(5.0 / model_.dur, 30.0 / model_.dur);
    seek_->SetFraction(model_.dur > 0 ? std::clamp(model_.pos / model_.dur, 0.0, 1.0) : 0.0);
    volume_->SetFraction(model_.volume / 100.0);
}

void PlayerBar::Paint(pui::IRenderer& r, const pui::Theme& t) {
    const RectF& b = Bounds();
    if (model_.fullscreen) {   // the full screen overlay sits on the video; the docked bar lets Mica show through
        r.FillRect(b, t.barFill);
        r.FillRect(ToDip(layout_.stroke), t.barStroke);   // 1 px top stroke
    }
    pui::TextStyle ts;
    ts.size = 12;
    ts.ellipsis = false;
    ts.align = pui::TextAlign::Trailing;
    if (model_.Live()) {                                                                // live glyph replaces the total time
        pui::TextStyle ico;
        ico.font = pui::FontKind::Icon;
        ico.size = 12;
        ico.align = pui::TextAlign::Trailing;
        ico.ellipsis = false;
        r.Text(pui::glyph::Str(pui::glyph::Live), ToDip(layout_.total), ico, t.textSecondary);
    } else {
        r.Text(FormatTime(model_.dur), ToDip(layout_.total), ts, t.textSecondary);
    }
}

}
