#include "features/FiltersPanel.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <memory>

#include "AppContext.h"
#include "Localize.h"
#include "BarLogic.h"
#include "features/FxModel.h"
#include "features/FxUtil.h"
#include "features/SettingsService.h"
#include "features/TextInputDialog.h"
#include "pui/Buttons.h"
#include "pui/ComboBox.h"
#include "pui/Panels.h"
#include "pui/ScrollViewer.h"
#include "pui/Slider.h"
#include "pui/Toggles.h"

namespace app {

using wvp::FilterSettings;
namespace F = wvp::filters;

namespace {
double AreaAspect(AppContext& ctx) {
    pui::SizeF sz = ctx.Ui().Size();
    return fx::VideoAreaAspect(sz.w, sz.h, kStripDip, ctx.IsFullscreen() || ctx.IsMiniPlayer());
}
UINT_PTR g_zoomTimer = 0;
}

FilterSettings CurrentUserFilters(AppContext& ctx) { return fx::UserView(ctx.Svc().filters.Settings()); }

void SendFilterChange(AppContext& ctx, const wvp::FilterChange& change) {
    if (!fx::HasEffect(change)) return;
    if (auto* c = ctx.Controller()) c->ApplyFilterChange(change);
}

void ApplyFilterEdit(AppContext& ctx, const std::function<void(FilterSettings&)>& fn) {
    FilterSettings user = CurrentUserFilters(ctx);
    fn(user);
    fx::ClearFilterError();
    fx::Zoom().aspect = AreaAspect(ctx);
    SendFilterChange(ctx, fx::ApplyUser(ctx.Svc().filters, user, fx::Zoom()));
    QueueFilterSettingsSave(ctx);
}

void SetPlaybackSpeed(AppContext& ctx, double speed) {
    speed = std::clamp(speed, F::kMinSpeed, F::kMaxSpeed);
    ApplyFilterEdit(ctx, [&](FilterSettings& s) { s.speed = speed; });
    ctx.ShowToast(Tr(L"Speed: ", L"재생 속도: ") + fx::SpeedLabel(ctx.Svc().filters.Settings().speed));
}

bool IsZoomToFill() { return fx::Zoom().on; }

void RefreshZoomAspect(AppContext& ctx) {
    if (!fx::Zoom().on) return;
    if (!g_zoomTimer) g_zoomTimer = fx::SetUiTimer(ctx.MainHwnd(), 400, [&ctx] { RefreshZoomAspect(ctx); }, true);
    double a = AreaAspect(ctx), old = fx::Zoom().aspect;
    if (std::fabs(a - old) / old < 0.005) return;
    fx::Zoom().aspect = a;
    SendFilterChange(ctx, fx::ApplyUser(ctx.Svc().filters, CurrentUserFilters(ctx), fx::Zoom()));
    // Viewport-only changes do not schedule a settings write; a pending user edit still flushes the latest snapshot.
}

void SetZoomToFill(AppContext& ctx, bool on) {
    if (fx::Zoom().on == on) return;
    fx::Zoom().on = on;
    ApplyFilterEdit(ctx, [](FilterSettings&) {});
    fx::KillUiTimer(ctx.MainHwnd(), g_zoomTimer);
    g_zoomTimer = 0;
    if (on) g_zoomTimer = fx::SetUiTimer(ctx.MainHwnd(), 400, [&ctx] { RefreshZoomAspect(ctx); }, true);
    ctx.ShowToast(on ? Tr(L"Zoom to fill: on", L"화면 채우기: 켜짐") : Tr(L"Zoom to fill: off", L"화면 채우기: 꺼짐"));
}

void ToggleZoomToFill(AppContext& ctx) { SetZoomToFill(ctx, !fx::Zoom().on); }

namespace {

/// label | control (fill or natural width) | trailing widget, one 32 DIP row.
class FxRow : public pui::Widget {
public:
    FxRow(pui::IRenderer* m, const std::wstring& label, std::unique_ptr<pui::Widget> control, bool fill,
          std::unique_ptr<pui::Widget> trailing = {}, float trailingW = -1, float labelW = 96)
        : Widget(pui::Role::Group), m_(m), fill_(fill), trailingW_(trailingW), labelW_(label.empty() ? 0.f : labelW) {
        if (!label.empty()) {
            auto t = std::make_unique<pui::TextBlock>(label, pui::TextKind::Body);
            t->SetColorRole(pui::TextColorRole::Secondary);
            label_ = t.get();
            AddChild(std::move(t));
        }
        control_ = AddChild(std::move(control));
        if (trailing) trailing_ = AddChild(std::move(trailing));
    }
    pui::Widget* Control() const { return control_; }
    pui::Widget* Trailing() const { return trailing_; }
    pui::SizeF DesiredSize(pui::IRenderer*) override { return {Bounds().w() > 0 ? Bounds().w() : 300.f, 32.f}; }
    void OnLayout() override {
        pui::RectF b = Bounds();
        float x = b.l, r = b.r;
        if (label_) {
            float lw = std::max(labelW_, std::ceil(label_->DesiredSize(m_).w) + 2.f);
            label_->SetBounds(pui::RectF::XYWH(x, b.t, lw, b.h()));
            x += lw + 8;
        }
        if (trailing_) {
            float w = trailingW_ >= 0 ? trailingW_ : trailing_->DesiredSize(m_).w;
            float h = std::min(b.h(), trailing_->DesiredSize(m_).h);
            trailing_->SetBounds(pui::RectF::XYWH(r - w, b.t + (b.h() - h) / 2, w, h));
            r -= w + 8;
        }
        pui::SizeF d = control_->DesiredSize(m_);
        float cw = fill_ ? std::max(0.f, r - x) : std::min(d.w, std::max(0.f, r - x));
        float ch = fill_ ? std::min(b.h(), std::max(d.h, 20.f)) : std::min(b.h(), d.h);
        control_->SetBounds(pui::RectF::XYWH(x, b.t + (b.h() - ch) / 2, cw, ch));
    }
private:
    pui::IRenderer* m_;
    bool fill_;
    float trailingW_, labelW_;
    pui::TextBlock* label_ = nullptr;
    pui::Widget* control_ = nullptr;
    pui::Widget* trailing_ = nullptr;
};

/// Wrapped critical-colour line with the error glyph (hidden when empty).
class FxErrorLine : public pui::Widget, public pui::IHeightForWidth {
public:
    FxErrorLine() : Widget(pui::Role::Text) { SetHitTestVisible(false); SetVisible(false); }
    void SetText(const std::wstring& t) { text_ = t; SetName(t); SetVisible(!t.empty()); Invalidate(); }
    const std::wstring& Text() const { return text_; }
    float HeightForWidth(pui::IRenderer* m, float width) override {
        if (text_.empty()) return 0;
        pui::TextStyle ts = Style();
        return std::max(24.f, pui::MeasurerOr(m).MeasureText(text_, ts, std::max(10.f, width - 28)).h + 6);
    }
    pui::SizeF DesiredSize(pui::IRenderer* m) override { return {Bounds().w(), HeightForWidth(m, Bounds().w())}; }
    void Paint(pui::IRenderer& r, const pui::Theme& t) override {
        if (text_.empty()) return;
        pui::RectF b = Bounds();
        r.Text(std::wstring(1, pui::glyph::Error), pui::RectF::XYWH(b.l, b.t + 2, 20, 20), pui::IconTextStyle(14), t.critical);
        r.Text(text_, pui::RectF::XYWH(b.l + 28, b.t, std::max(0.f, b.w() - 28), b.h()), Style(), t.critical);
    }
private:
    static pui::TextStyle Style() {
        pui::TextStyle s;
        s.size = 12; s.wrap = true; s.ellipsis = false; s.valign = pui::ParaAlign::Top;
        return s;
    }
    std::wstring text_;
};

class FiltersPanelWidget;
FiltersPanelWidget* g_open = nullptr;

class FiltersPanelWidget : public pui::Widget {
public:
    static constexpr float kW = 440, kPad = 16;

    FiltersPanelWidget(AppContext& ctx, FiltersTab tab) : Widget(pui::Role::Pane), ctx_(ctx), m_(&ctx.Measurer()) {
        SetName(Tr(L"Filters", L"필터"));
        auto title = std::make_unique<pui::TextBlock>(Tr(L"Filters", L"필터"), pui::TextKind::Subtitle);
        title_ = title.get();
        AddChild(std::move(title));
        auto close = std::make_unique<pui::IconButton>(pui::glyph::Close, Tr(L"Close", L"닫기"));
        close_ = close.get();
        close_->onClick = [this] { Close(); };
        AddChild(std::move(close));
        auto tabs = std::make_unique<pui::RadioGroup>();
        tabs_ = tabs.get();
        tabs_->SetName(Tr(L"Filter categories", L"필터 범주"));
        tabs_->SetHorizontal(true);
        tabs_->SetSpacing(8);
        tabs_->AddOption(Tr(L"Video", L"영상"));
        tabs_->AddOption(Tr(L"Audio", L"소리"));
        tabs_->AddOption(Tr(L"Advanced", L"고급"));
        tabs_->onSelectionChanged = [this](int i) { if (!building_) SetTab((FiltersTab)i); };
        AddChild(std::move(tabs));
        auto err = std::make_unique<FxErrorLine>();
        err_ = err.get();
        AddChild(std::move(err));
        auto sv = std::make_unique<pui::ScrollViewer>();
        sv_ = sv.get();
        sv_->SetMeasurer(m_);
        AddChild(std::move(sv));
        errToken_ = fx::FilterErrorChanged().add([this, alive = alive_] { if (*alive) SyncError(); });
        g_open = this;
        cur_ = CurrentUserFilters(ctx_);
        SyncError();
        SetTab(tab);
    }

    ~FiltersPanelWidget() override {
        *alive_ = false;
        fx::FilterErrorChanged().remove(errToken_);
        if (g_open == this) g_open = nullptr;
    }

    void SetTab(FiltersTab tab) {
        tab_ = tab;
        building_ = true;
        tabs_->SetSelectedIndex((int)tab);
        building_ = false;
        cur_ = CurrentUserFilters(ctx_);
        BuildTab();
    }
    FiltersTab Tab() const { return tab_; }

    void Close() {
        if (Host()) Host()->ClosePopup(this);
    }

    bool OnKeyDown(int vk, unsigned mods) override {
        if (vk == pui::key::Escape && mods == 0) { Close(); return true; }
        return false;
    }

    void OnLayout() override {
        pui::RectF b = Bounds();
        float w = std::max(0.f, b.w() - 2 * kPad), y = b.t + kPad;
        title_->SetBounds(pui::RectF::XYWH(b.l + kPad, y, std::max(0.f, w - 40), 36));
        close_->SetBounds(pui::RectF::XYWH(b.r - kPad - 36, y, 36, 36));
        y += 40;
        tabs_->SetBounds(pui::RectF::XYWH(b.l + kPad, y, w, 32));
        y += 36;
        float eh = err_->Visible() ? err_->HeightForWidth(m_, w) : 0.f;
        err_->SetBounds(pui::RectF::XYWH(b.l + kPad, y, w, eh));
        if (eh > 0) y += eh + 4;
        pui::RectF svr = pui::RectF::XYWH(b.l + kPad, y, w, std::max(0.f, b.b - kPad - y));
        sv_->SetBounds(svr);
        UpdateExtent();
    }

    void Paint(pui::IRenderer& r, const pui::Theme& t) override {
        pui::RectF b = Bounds();
        r.FillRoundRect(b, t.overlayRadius, t.flyoutSurface);
        r.StrokeRoundRect(b.Deflate(0.5f, 0.5f, 0.5f, 0.5f), t.overlayRadius, t.flyoutStroke, 1);
    }

    pui::ScrollViewer* Scroll() const { return sv_; }
    const FilterSettings& Cur() const { return cur_; }
    FxErrorLine* ErrorLine() const { return err_; }

    /// Panel top and the tallest the panel may get (DIP); the panel then sizes itself to its content up to maxH.
    void SetLimits(float top, float maxH) {
        top_ = top; maxH_ = maxH;
        FitHeight();
    }
    float ContentHeight() const {
        float w = std::max(0.f, Bounds().w() - 2 * kPad);
        float eh = err_->Visible() ? err_->HeightForWidth(m_, w) + 4 : 0.f;
        float ch = content_ ? content_->HeightForWidth(m_, w) : 0.f;
        return kPad + 40 + 36 + eh + ch + kPad;
    }
    /// The ScrollViewer takes over when the content is taller than maxH.
    void FitHeight() {
        if (maxH_ <= 0 || Bounds().w() <= 0) return;
        float h = std::clamp(ContentHeight(), std::min(160.f, maxH_), maxH_);
        pui::RectF b = Bounds();
        if (std::fabs(h - b.h()) > 0.01f) SetBounds(pui::RectF::XYWH(b.l, b.t, b.w(), h));
        RequestLayout();
        OnLayout();
    }

private:
    void Commit() {
        fx::ClearFilterError();
        fx::Zoom().aspect = AreaAspect(ctx_);
        SendFilterChange(ctx_, fx::ApplyUser(ctx_.Svc().filters, cur_, fx::Zoom()));
        QueueFilterSettingsSave(ctx_);
    }
    void Edit(const std::function<void(FilterSettings&)>& fn) {
        if (building_) return;
        fn(cur_);
        Commit();
    }
    void SyncError() {
        const auto& e = fx::LastFilterError();
        err_->SetText(e ? fx::FormatFilterError(*e) + (e->Filter.empty() ? L"" : L"\n" + e->Filter) : L"");
        RequestLayout();
        FitHeight();
    }
    void UpdateExtent() {
        if (!content_) return;
        float w = sv_->Bounds().w();
        sv_->SetExtentHeight(content_->HeightForWidth(m_, w));
        sv_->UpdateLayout();
    }

    std::unique_ptr<pui::TextBlock> Heading(const std::wstring& t) {
        auto h = std::make_unique<pui::TextBlock>(t, pui::TextKind::BodyStrong);
        return h;
    }
    std::unique_ptr<FxRow> SliderRow(const std::wstring& label, double mn, double mx, double step, double value, int decimals,
                                     std::function<void(double)> set, pui::Slider** out = nullptr, bool db = false,
                                     pui::TextBlock** outVal = nullptr) {
        auto s = std::make_unique<pui::Slider>();
        pui::Slider* sp = s.get();
        s->SetName(label);
        s->SetRange(mn, mx);
        s->SetStep(step);
        s->SetLargeStep(step * 10);
        s->SetValue(value);
        auto fmt = [decimals, db](double v) { return db ? fx::DbLabel(v) : fx::Fixed(v, decimals); };
        s->valueFormatter = fmt;
        auto val = std::make_unique<pui::TextBlock>(fmt(value), pui::TextKind::Body);
        val->SetAlign(pui::TextAlign::Trailing);
        val->SetColorRole(pui::TextColorRole::Secondary);
        pui::TextBlock* vp = val.get();
        s->onChanged = [this, set = std::move(set), vp, fmt](double v, bool) {
            vp->SetText(fmt(v));
            if (!building_) set(v);
        };
        if (out) *out = sp;
        if (outVal) *outVal = vp;
        return std::make_unique<FxRow>(m_, label, std::move(s), true, std::move(val), 64.f, db ? 64.f : 96.f);
    }
    std::unique_ptr<FxRow> ToggleRow(const std::wstring& label, bool on, std::function<void(bool)> set) {
        auto t = std::make_unique<pui::ToggleSwitch>();
        t->SetName(label);
        t->SetOn(on);
        t->onToggled = [this, set = std::move(set)](bool v) { if (!building_) set(v); };
        return std::make_unique<FxRow>(m_, label, std::move(t), false);
    }
    std::unique_ptr<FxRow> CheckRow(const std::wstring& text, bool on, std::function<void(bool)> set) {
        auto c = std::make_unique<pui::CheckBox>(text);
        c->SetChecked(on);
        c->onToggled = [this, set = std::move(set)](bool v) { if (!building_) set(v); };
        return std::make_unique<FxRow>(m_, L"", std::move(c), false);
    }
    std::unique_ptr<FxRow> ComboRow(const std::wstring& label, std::vector<std::wstring> items, int sel, std::function<void(int)> set,
                                    pui::ComboBox** out = nullptr) {
        auto c = std::make_unique<pui::ComboBox>();
        c->SetName(label);
        c->SetItems(std::move(items));
        c->SetSelectedIndex(sel);
        c->onSelectionChanged = [this, set = std::move(set)](int i) { if (!building_) set(i); };
        if (out) *out = c.get();
        return std::make_unique<FxRow>(m_, label, std::move(c), true);
    }
    std::unique_ptr<pui::Button> MakeButton(const std::wstring& text, std::function<void()> fn) {
        auto b = std::make_unique<pui::Button>(text);
        b->onClick = std::move(fn);
        return b;
    }

    void BuildTab() {
        building_ = true;
        eqSliders_.clear();
        eqTexts_.clear();
        eqValueCombo_ = nullptr;
        auto sp = std::make_unique<pui::StackPanel>(pui::Orientation::Vertical, 6.f);
        sp->SetPadding({0, 4, 12, 8});
        switch (tab_) {
        case FiltersTab::Video: BuildVideo(*sp); break;
        case FiltersTab::Audio: BuildAudio(*sp); break;
        case FiltersTab::Advanced: BuildAdvanced(*sp); break;
        }
        content_ = sp.get();
        sv_->SetContent(std::move(sp));
        building_ = false;
        UpdateExtent();
        FitHeight();
        Invalidate();
    }

    void BuildVideo(pui::StackPanel& sp) {
        sp.AddChild(Heading(Tr(L"Picture", L"화면")));
        sp.AddChild(ComboRow(Tr(L"Deinterlace", L"디인터레이스"), fx::DeinterlaceLabels(), (int)cur_.deinterlace, [this](int i) {
            Edit([&](FilterSettings& s) { s.deinterlace = (wvp::DeinterlaceMode)i; });
            RebuildLater();   // show/hide the mode's sub-controls (reloads cur_ from the manager, which sanitizes it)
        }));
        const fx::DeinterlaceControls dc = fx::DeinterlaceControlsFor(cur_.deinterlace);
        if (dc.priority)
            sp.AddChild(ComboRow(Tr(L"Priority", L"우선순위"), fx::DeinterlacePriorityLabels(), (int)cur_.deinterlacePriority,
                                 [this](int i) { Edit([&](FilterSettings& s) { s.deinterlacePriority = (wvp::DeinterlacePriority)i; }); }));
        if (dc.rate)
            sp.AddChild(ComboRow(Tr(L"Output rate", L"출력 속도"), {Tr(L"One frame per frame", L"프레임당 한 프레임"), Tr(L"One frame per field", L"필드당 한 프레임")}, (int)cur_.deinterlaceRate,
                                 [this](int i) { Edit([&](FilterSettings& s) { s.deinterlaceRate = (wvp::DeinterlaceRate)i; }); }));
        if (dc.onlyInterlaced)
            sp.AddChild(ToggleRow(Tr(L"Only interlaced frames", L"인터레이스 프레임만"), cur_.deinterlaceOnlyInterlaced,
                                  [this](bool v) { Edit([&](FilterSettings& s) { s.deinterlaceOnlyInterlaced = v; }); }));
        sp.AddChild(SliderRow(Tr(L"Brightness", L"밝기"), F::kBrightnessMin, F::kBrightnessMax, 0.01, cur_.brightness, 2,
                              [this](double v) { Edit([&](FilterSettings& s) { s.brightness = v; }); }));
        sp.AddChild(SliderRow(Tr(L"Contrast", L"대비"), 0, 2, 0.01, std::clamp(cur_.contrast, 0.0, 2.0), 2,
                              [this](double v) { Edit([&](FilterSettings& s) { s.contrast = v; }); }));
        sp.AddChild(SliderRow(Tr(L"Saturation", L"채도"), F::kSaturationMin, F::kSaturationMax, 0.01, cur_.saturation, 2,
                              [this](double v) { Edit([&](FilterSettings& s) { s.saturation = v; }); }));
        sp.AddChild(SliderRow(Tr(L"Gamma", L"감마"), F::kGammaMin, 3.0, 0.01, std::min(cur_.gamma, 3.0), 2,
                              [this](double v) { Edit([&](FilterSettings& s) { s.gamma = v; }); }));
        sp.AddChild(Heading(Tr(L"Orientation", L"방향")));
        {
            auto rg = std::make_unique<pui::RadioGroup>();
            rg->SetName(Tr(L"Rotate", L"회전"));
            rg->SetHorizontal(true);
            rg->SetSpacing(4);
            rg->AddOption(L"0°");
            rg->AddOption(L"90°");
            rg->AddOption(L"180°");
            rg->AddOption(L"270°");
            rg->SetSelectedIndex((int)cur_.rotation / 90);
            rg->onSelectionChanged = [this](int i) { Edit([&](FilterSettings& s) { s.rotation = (wvp::Rotation)(i * 90); }); };
            sp.AddChild(std::make_unique<FxRow>(m_, L"", std::move(rg), true));   // own line: four options do not fit next to a label
        }
        sp.AddChild(CheckRow(Tr(L"Flip horizontally", L"좌우 반전"), cur_.flipHorizontal, [this](bool v) { Edit([&](FilterSettings& s) { s.flipHorizontal = v; }); }));
        sp.AddChild(CheckRow(Tr(L"Flip vertically", L"상하 반전"), cur_.flipVertical, [this](bool v) { Edit([&](FilterSettings& s) { s.flipVertical = v; }); }));
        sp.AddChild(CheckRow(Tr(L"Zoom to fill", L"화면 채우기"), fx::Zoom().on, [this](bool v) { SetZoomToFill(ctx_, v); }));
        sp.AddChild(std::make_unique<FxRow>(m_, L"", MakeButton(Tr(L"Reset video settings", L"영상 설정 초기화"), [this] {
            cur_ = fx::ResetVideoPart(cur_);
            fx::Zoom().on = false;
            fx::KillUiTimer(ctx_.MainHwnd(), g_zoomTimer);
            g_zoomTimer = 0;
            Commit();
            RebuildLater();
        }), false));
    }

    void BuildAudio(pui::StackPanel& sp) {
        sp.AddChild(Heading(Tr(L"Sound", L"소리")));
        sp.AddChild(ToggleRow(Tr(L"Normalize", L"음량 평준화"), cur_.normalize, [this](bool v) { Edit([&](FilterSettings& s) { s.normalize = v; }); }));
        {
            std::vector<std::wstring> items;
            for (double d : F::kSpeedPresets) items.push_back(fx::SpeedLabel(d));
            int si = fx::SpeedIndex(cur_.speed);
            if (si < 0) { items.push_back(fx::SpeedLabel(cur_.speed)); si = (int)items.size() - 1; }   // e.g. set by the [ ] keys
            sp.AddChild(ComboRow(Tr(L"Speed", L"재생 속도"), std::move(items), si, [this](int i) {
                if (i >= (int)F::kSpeedPresets.size()) return;
                Edit([&](FilterSettings& s) { s.speed = F::kSpeedPresets[(size_t)i]; });
            }));
        }
        sp.AddChild(Heading(Tr(L"Equalizer", L"이퀄라이저")));
        {
            std::vector<std::wstring> items;
            for (auto& p : fx::EqPresets()) items.push_back(p.name);
            items.push_back(Tr(L"Custom", L"사용자 지정"));
            int m = fx::MatchEqPreset(cur_.eqGainsDb);
            sp.AddChild(ComboRow(Tr(L"Preset", L"프리셋"), std::move(items), m < 0 ? (int)fx::EqPresets().size() : m, [this](int i) {
                if (i >= (int)fx::EqPresets().size()) return;
                cur_ = fx::WithEqPreset(cur_, i);
                Commit();
                for (size_t b = 0; b < eqSliders_.size(); b++) eqSliders_[b]->SetValue(cur_.eqGainsDb[b]);
                SyncEqValueTexts();
            }, &eqValueCombo_));
        }
        for (size_t i = 0; i < F::kEqBandCount; i++) {
            pui::Slider* sl = nullptr;
            pui::TextBlock* vt = nullptr;
            sp.AddChild(SliderRow(fx::BandLabel(wvp::FilterManager::kEqBandsHz[i]), -12, 12, 1, std::clamp(cur_.eqGainsDb[i], -12.0, 12.0), 0,
                                  [this, i](double v) {
                                      Edit([&](FilterSettings& s) { s.eqGainsDb[i] = v; });
                                      int m = fx::MatchEqPreset(cur_.eqGainsDb);
                                      if (eqValueCombo_) eqValueCombo_->SetSelectedIndex(m < 0 ? (int)fx::EqPresets().size() : m);
                                  }, &sl, true, &vt));
            eqSliders_.push_back(sl);
            eqTexts_.push_back(vt);
        }
        sp.AddChild(std::make_unique<FxRow>(m_, L"", MakeButton(Tr(L"Reset audio settings", L"소리 설정 초기화"), [this] {
            cur_ = fx::ResetAudioPart(cur_);
            Commit();
            RebuildLater();
        }), false));
    }
    void SyncEqValueTexts() {
        for (size_t b = 0; b < eqTexts_.size() && b < cur_.eqGainsDb.size(); b++) eqTexts_[b]->SetText(fx::DbLabel(cur_.eqGainsDb[b]));
    }
    void RebuildLater() {
        ctx_.PostToUi([this, alive = alive_] { if (*alive) { cur_ = CurrentUserFilters(ctx_); BuildTab(); } });
    }

    void BuildAdvanced(pui::StackPanel& sp) {
        sp.AddChild(Heading(Tr(L"Custom filters", L"사용자 지정 필터")));
        auto textRow = [&](const std::wstring& label, const std::wstring& value, std::function<void()> edit) {
            auto t = std::make_unique<pui::TextBlock>(value.empty() ? Tr(L"(none)", L"(없음)") : value, pui::TextKind::Body);
            t->SetColorRole(value.empty() ? pui::TextColorRole::Tertiary : pui::TextColorRole::Primary);
            t->SetName(label + L": " + (value.empty() ? Tr(L"none", L"없음") : value));
            auto b = MakeButton(Tr(L"Edit…", L"편집…"), std::move(edit));
            b->SetName(std::wstring(Tr(L"Edit ", L"편집: ")) + label);
            sp.AddChild(std::make_unique<FxRow>(m_, label, std::move(t), true, std::move(b), -1.f, 88.f));
        };
        textRow(Tr(L"Video (vf)", L"영상 (vf)"), cur_.customVideoFilter, [this] { EditCustom(true); });
        textRow(Tr(L"Audio (af)", L"소리 (af)"), cur_.customAudioFilter, [this] { EditCustom(false); });
        sp.AddChild(CheckRow(Tr(L"Custom filters replace the presets above", L"사용자 지정 필터로 위의 프리셋을 대체"), cur_.advancedMode,
                             [this](bool v) { Edit([&](FilterSettings& s) { s.advancedMode = v; }); }));
        sp.AddChild(std::make_unique<FxRow>(m_, L"", MakeButton(Tr(L"Clear custom filters", L"사용자 지정 필터 지우기"), [this] {
            cur_.customVideoFilter.clear();
            cur_.customAudioFilter.clear();
            Commit();
            RebuildLater();
        }), false));
        sp.AddChild(std::make_unique<FxRow>(m_, L"", MakeButton(Tr(L"Reset all filters", L"모든 필터 초기화"), [this] {
            cur_ = FilterSettings{};
            fx::Zoom().on = false;
            fx::KillUiTimer(ctx_.MainHwnd(), g_zoomTimer);
            g_zoomTimer = 0;
            Commit();
            RebuildLater();
        }), false));
    }

    void EditCustom(bool video) {
        TextPromptOptions o;
        o.title = video ? Tr(L"Custom video filter", L"사용자 지정 영상 필터") : Tr(L"Custom audio filter", L"사용자 지정 소리 필터");
        o.label = video ? Tr(L"FFmpeg video filter graph (vf), empty to clear", L"FFmpeg 영상 필터 그래프 (vf), 지우려면 비워 두세요") : Tr(L"FFmpeg audio filter graph (af), empty to clear", L"FFmpeg 소리 필터 그래프 (af), 지우려면 비워 두세요");
        o.initial = video ? cur_.customVideoFilter : cur_.customAudioFilter;
        o.okText = Tr(L"Apply", L"적용");
        AppContext& ctx = ctx_;
        auto alive = alive_;
        auto res = PromptText(ctx, o);   // nested loop; the shell closes every popup when the owner window deactivates
        if (res) {
            std::wstring v = *res;
            while (!v.empty() && std::iswspace(v.front())) v.erase(v.begin());
            while (!v.empty() && std::iswspace(v.back())) v.pop_back();
            ApplyFilterEdit(ctx, [&](FilterSettings& s) { (video ? s.customVideoFilter : s.customAudioFilter) = v; });
        }
        // The panel was closed by the activation change: bring it back on the same tab so the inline error line (and the
        // new filter text) are visible. A fresh panel reads the settings and the last FILTER_ERROR itself.
        if (!*alive) ShowFiltersPanel(ctx, FiltersTab::Advanced);
        else if (res) { cur_ = CurrentUserFilters(ctx_); RebuildLater(); }
    }

    AppContext& ctx_;
    pui::IRenderer* m_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    FiltersTab tab_ = FiltersTab::Video;
    float top_ = 0, maxH_ = 0;
    FilterSettings cur_;
    bool building_ = false;
    pui::TextBlock* title_ = nullptr;
    pui::IconButton* close_ = nullptr;
    pui::RadioGroup* tabs_ = nullptr;
    FxErrorLine* err_ = nullptr;
    pui::ScrollViewer* sv_ = nullptr;
    pui::StackPanel* content_ = nullptr;
    std::vector<pui::Slider*> eqSliders_;
    std::vector<pui::TextBlock*> eqTexts_;
    pui::ComboBox* eqValueCombo_ = nullptr;
    wvp::Event<>::Token errToken_ = 0;
};

}

FiltersPanelGeometry PanelGeometry(float clientW, float clientH) {
    FiltersPanelGeometry g;
    g.w = std::min(FiltersPanelWidget::kW, std::max(280.f, clientW - 16));
    g.left = std::max(8.f, clientW - g.w - 16);
    g.top = kStripDip + 8;
    g.maxH = std::max(200.f, clientH - 8 - g.top);   // may overlap the bar: the panel scrolls when its content is taller
    return g;
}

bool IsFiltersPanelOpen(AppContext& ctx) { return g_open && g_open->Host() && ctx.Ui().IsPopupOpen(g_open); }

void ToggleFiltersPanel(AppContext& ctx) {
    if (IsFiltersPanelOpen(ctx)) ctx.Ui().ClosePopup(g_open);
    else ShowFiltersPanel(ctx, FiltersTab::Video);
}

void ShowFiltersPanel(AppContext& ctx, FiltersTab tab) {
    if (g_open && g_open->Host() && ctx.Ui().IsPopupOpen(g_open)) {
        g_open->SetTab(tab);
        return;
    }
    pui::SizeF cs = ctx.Ui().Size();
    FiltersPanelGeometry g = PanelGeometry(cs.w, cs.h);
    auto panel = std::make_unique<FiltersPanelWidget>(ctx, tab);
    panel->SetBounds(pui::RectF::XYWH(g.left, g.top, g.w, g.maxH));
    panel->SetLimits(g.top, g.maxH);
    pui::PopupOptions po;
    po.lightDismiss = true;
    po.takeFocus = true;
    po.onClosed = [&ctx] { FlushFilterSettings(ctx); };   // flush at close, not deferred widget destruction
    ctx.Ui().OpenPopup(std::move(panel), po);
}

}
