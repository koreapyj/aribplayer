#pragma once
// The player bar (86 DIP dock): seek row (elapsed | slider | total) and transport row (Play/Pause, Stop, Prev, Next, Open,
// codec chips, ... Mute, volume slider, Subtitle, 제어창, Playlist panel, Full screen). Windowed it is docked under the video and
// paints no background (Mica shows through); in full screen it is the auto-hide overlay with `barFill` + a 1 px top stroke.
// One widget per button (tooltip, automation Name, Invoke, accelerator text); the sliders are the shell's own widgets.
// Layout = BarLayout (device pixels, spec metrics) converted to DIPs. Narrow widths hide chips, then shrink the volume
// slider, then hide Open / Subtitle / 제어창 (BarLayout tiers).
//
// Data flows in through SetModel(BarModel) (pure struct, see BarLogic.h); user intent flows out through Actions.
#include <array>
#include <functional>
#include <memory>
#include <string>

#include "BarLogic.h"
#include "Commands.h"
#include "pui/Widget.h"
#include "shell/ShellControls.h"

namespace app {

/// The elapsed / remaining time readout: plain 12 px text that raises Invoke on click (toggles elapsed <-> remaining).
class TimeReadout : public pui::Widget {
public:
    TimeReadout();
    void SetText(const std::wstring& text);
    const std::wstring& Text() const { return text_; }
    std::function<void()> onInvoke;

    void Paint(pui::IRenderer& r, const pui::Theme& t) override;
    bool OnPointerDown(const pui::PointerEvent& e) override;
    bool OnPointerUp(const pui::PointerEvent& e) override;
    bool OnKeyDown(int vk, unsigned mods) override;
    bool SupportsInvoke() const override { return true; }
    void Invoke() override;

private:
    std::wstring text_;
};

class PlayerBar : public pui::Widget {
public:
    struct Actions {
        std::function<void(CommandId)> command;       // button -> command (play-pause, stop, next, full screen, ...)
        std::function<void(double)> seekTo;           // seek slider released/keyboard: fraction 0..1 of the duration
        std::function<void(int)> setVolume;           // 0..100 (volume slider / wheel)
        std::function<void(pui::Widget*)> subtitleMenu;  // Subtitle button right click: anchor
    };

    PlayerBar();

    /// DIP -> device pixel scale; the spec layout is computed in whole device pixels.
    void SetScale(float scale);
    float Scale() const { return scale_; }
    void SetActions(Actions a) { actions_ = std::move(a); }
    void SetModel(const BarModel& m);
    const BarModel& Model() const { return model_; }
    /// Spec layout in device pixels, bar-local (valid after the first OnLayout).
    const BarLayout& Layout() const { return layout_; }

    pui::Widget* ButtonWidget(int btn) const { return buttons_[(size_t)btn]; }
    ShellSlider* Seek() const { return seek_; }
    ShellSlider* Volume() const { return volume_; }
    TimeReadout* Elapsed() const { return elapsed_; }
    /// Chip widget `i` (chip order of BarModel::chips; kMaxChips pooled widgets, hidden when unused).
    Chip* ChipWidget(int i) const { return chips_[(size_t)i]; }
    bool SeekDragging() const { return seek_->Dragging(); }

    void OnLayout() override;
    void Paint(pui::IRenderer& r, const pui::Theme& t) override;

private:
    void ApplyModel();
    pui::RectF ToDip(const pui::RectF& px) const;
    void Fire(CommandId c) { if (actions_.command) actions_.command(c); }
    void UpdateReadouts();

    float scale_ = 1.f;
    BarLayout layout_;
    BarModel model_;
    Actions actions_;
    std::array<GlyphButton*, B_Count> buttons_{};
    std::array<Chip*, BarLayout::kMaxChips> chips_{};
    ShellSlider* seek_ = nullptr;
    ShellSlider* volume_ = nullptr;
    TimeReadout* elapsed_ = nullptr;
};

}
