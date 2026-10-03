#pragma once
// The shell shows it instead of PlayerBar in ShellMode::MiniPlayer and paints it
// into the bar layer. Geometry = ComputeMiniBarLayout (MiniLogic.h).
#include <functional>

#include "BarLogic.h"
#include "Commands.h"
#include "features/MiniLogic.h"
#include "pui/Buttons.h"
#include "pui/Slider.h"
#include "pui/Widget.h"

namespace app {

class MiniBar : public pui::Widget {
public:
    struct Actions {
        std::function<void(CommandId)> command;   // Previous / PlayPause / Next / ToggleMiniPlayer (back to full view)
        std::function<void(double)> seekTo;       // fraction 0..1 of the duration, on drag release / keyboard
    };

    MiniBar();

    void SetScale(float scale);
    float Scale() const { return scale_; }
    void SetActions(Actions a) { actions_ = std::move(a); }
    void SetModel(const BarModel& m);
    const BarModel& Model() const { return model_; }
    const MiniBarLayout& Layout() const { return layout_; }

    pui::IconButton* PrevButton() const { return prev_; }
    pui::IconButton* PlayButton() const { return play_; }
    pui::IconButton* NextButton() const { return next_; }
    pui::IconButton* ExitButton() const { return exit_; }
    pui::Slider* Seek() const { return seek_; }
    bool SeekDragging() const { return seek_->IsDragging(); }

    void OnLayout() override;
    void Paint(pui::IRenderer& r, const pui::Theme& t) override;

private:
    pui::RectF ToAbs(const pui::RectF& local) const;
    void ApplyModel();
    void Fire(CommandId c) { if (actions_.command) actions_.command(c); }

    float scale_ = 1.f;
    MiniBarLayout layout_;
    BarModel model_;
    Actions actions_;
    pui::IconButton *prev_ = nullptr, *play_ = nullptr, *next_ = nullptr, *exit_ = nullptr;
    pui::Slider* seek_ = nullptr;
};

}
