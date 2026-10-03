#include "features/PropertiesDialog.h"
#include "Localize.h"

#include <algorithm>

#include "AppContext.h"
#include "features/FxModel.h"
#include "pui/Buttons.h"
#include "pui/Panels.h"
#include "pui/ScrollViewer.h"

namespace app {

namespace fx {
pui::TextStyle PropsValueStyle() {
    pui::TextStyle s;
    s.size = 14; s.wrap = true; s.ellipsis = false; s.valign = pui::ParaAlign::Top;
    return s;
}
pui::TextStyle PropsLabelStyle() {
    pui::TextStyle s;
    s.size = 14; s.valign = pui::ParaAlign::Top;
    return s;
}

std::vector<PropsLine> LayoutProps(const std::vector<PropSection>& sections, pui::IRenderer& m, float width) {
    std::vector<PropsLine> out;
    float y = 0;
    float vw = std::max(40.f, width - PropsMetrics::kLabelW - PropsMetrics::kGap);
    for (size_t si = 0; si < sections.size(); si++) {
        out.push_back({PropsLine::Kind::Heading, (int)si, -1, y, PropsMetrics::kHeadingH});
        y += PropsMetrics::kHeadingH;
        for (size_t ri = 0; ri < sections[si].rows.size(); ri++) {
            float th = m.MeasureText(sections[si].rows[ri].value.empty() ? L" " : sections[si].rows[ri].value, PropsValueStyle(), vw).h;
            float h = std::max(PropsMetrics::kRowMin, th + 2 * PropsMetrics::kRowPadV);
            out.push_back({PropsLine::Kind::Row, (int)si, (int)ri, y, h});
            y += h;
        }
    }
    return out;
}

float PropsTotalHeight(const std::vector<PropsLine>& lines) { return lines.empty() ? 0.f : lines.back().y + lines.back().h; }
}

namespace {

class PropsView : public pui::Widget, public pui::IHeightForWidth {
public:
    PropsView(std::vector<fx::PropSection> sections, pui::IRenderer* m) : Widget(pui::Role::List), s_(std::move(sections)), m_(m) {
        SetName(Tr(L"Properties", L"재생 정보"));
        SetHitTestVisible(false);
    }
    float HeightForWidth(pui::IRenderer* m, float width) override {
        return fx::PropsTotalHeight(fx::LayoutProps(s_, pui::MeasurerOr(m ? m : m_), width)) + 8;
    }
    pui::SizeF DesiredSize(pui::IRenderer* m) override { return {Bounds().w(), HeightForWidth(m, Bounds().w())}; }
    void Paint(pui::IRenderer& r, const pui::Theme& t) override {
        pui::RectF b = Bounds();
        auto lines = fx::LayoutProps(s_, r, b.w());
        for (auto& ln : lines) {
            float y = b.t + ln.y;
            if (y > b.b + 2000) break;
            if (ln.kind == fx::PropsLine::Kind::Heading) {
                pui::TextStyle hs;
                hs.size = 14; hs.weight = 600;
                r.Text(s_[(size_t)ln.section].title, pui::RectF::XYWH(b.l, y + 6, b.w(), 24), hs, t.textPrimary);
                r.Line({b.l, y + ln.h - 1}, {b.r, y + ln.h - 1}, t.dividerStroke, 1);
            } else {
                const auto& row = s_[(size_t)ln.section].rows[(size_t)ln.row];
                float pv = fx::PropsMetrics::kRowPadV;
                r.Text(row.label, pui::RectF::XYWH(b.l, y + pv, fx::PropsMetrics::kLabelW, ln.h - 2 * pv), fx::PropsLabelStyle(), t.textSecondary);
                float vx = b.l + fx::PropsMetrics::kLabelW + fx::PropsMetrics::kGap;
                r.Text(row.value, pui::RectF::XYWH(vx, y + pv, std::max(0.f, b.r - vx), ln.h - 2 * pv), fx::PropsValueStyle(), t.textPrimary);
            }
        }
    }
private:
    std::vector<fx::PropSection> s_;
    pui::IRenderer* m_;
};

class PropertiesDialogWidget : public pui::Widget {
public:
    PropertiesDialogWidget(std::vector<fx::PropSection> sections, pui::IRenderer* m) : Widget(pui::Role::Pane), m_(m) {
        SetName(Tr(L"Properties", L"재생 정보"));
        auto title = std::make_unique<pui::TextBlock>(Tr(L"Properties", L"재생 정보"), pui::TextKind::Subtitle);
        title_ = title.get();
        AddChild(std::move(title));
        auto sv = std::make_unique<pui::ScrollViewer>();
        sv_ = sv.get();
        sv_->SetMeasurer(m_);
        auto view = std::make_unique<PropsView>(std::move(sections), m_);
        view_ = view.get();
        sv_->SetContent(std::move(view));
        AddChild(std::move(sv));
        auto close = std::make_unique<pui::Button>(Tr(L"Close", L"닫기"), pui::ButtonStyle::Accent);
        close_ = close.get();
        close_->onClick = [this] { Close(); };
        AddChild(std::move(close));
    }
    void Close() { if (Host()) Host()->ClosePopup(this); }
    pui::Button* CloseButton() const { return close_; }
    pui::ScrollViewer* Scroll() const { return sv_; }

    bool OnKeyDown(int vk, unsigned mods) override {
        if (vk == pui::key::Escape && mods == 0) { Close(); return true; }
        return false;
    }
    void OnLayout() override {
        pui::RectF b = Bounds();
        constexpr float pad = 24;
        title_->SetBounds(pui::RectF::XYWH(b.l + pad, b.t + 20, std::max(0.f, b.w() - 2 * pad), 32));
        pui::SizeF cd = close_->DesiredSize(m_);
        float bw = std::max(96.f, cd.w);
        close_->SetBounds(pui::RectF::XYWH(b.r - pad - bw, b.b - 16 - cd.h, bw, cd.h));
        pui::RectF svr{b.l + pad, b.t + 60, b.r - pad, b.b - 16 - cd.h - 12};
        if (svr.b < svr.t) svr.b = svr.t;
        sv_->SetExtentHeight(view_->HeightForWidth(m_, std::max(0.f, svr.w() - 12)));
        sv_->SetBounds(svr);
        sv_->UpdateLayout();
    }
    void Paint(pui::IRenderer& r, const pui::Theme& t) override {
        pui::RectF b = Bounds();
        r.FillRoundRect(b, t.overlayRadius, t.flyoutSurface);
        r.StrokeRoundRect(b.Deflate(0.5f, 0.5f, 0.5f, 0.5f), t.overlayRadius, t.flyoutStroke, 1);
    }
private:
    pui::IRenderer* m_;
    pui::TextBlock* title_ = nullptr;
    pui::ScrollViewer* sv_ = nullptr;
    PropsView* view_ = nullptr;
    pui::Button* close_ = nullptr;
};

}

void ShowPropertiesDialog(AppContext& ctx) {
    wvp::FfPlayer* p = ctx.Player();
    std::optional<wvp::MediaInfo> info = p ? p->GetMediaInfo() : std::nullopt;
    if (!info) {
        ctx.ShowToast(Tr(L"Nothing is playing", L"재생 중인 항목이 없습니다"));
        return;
    }
    std::optional<std::int64_t> size;
    if (fx::IsLocalPath(info->Url)) {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(info->Url.c_str(), GetFileExInfoStandard, &fa))
            size = (std::int64_t)(((std::uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow);
    }
    auto dlg = std::make_unique<PropertiesDialogWidget>(fx::BuildProperties(*info, size), &ctx.Measurer());
    pui::SizeF cs = ctx.Ui().Size();
    float w = std::min(560.f, std::max(280.f, cs.w - 32)), h = std::min(560.f, std::max(240.f, cs.h - 64));
    dlg->SetBounds(pui::RectF::XYWH(std::round((cs.w - w) / 2), std::round((cs.h - h) / 2), w, h));
    pui::PopupOptions po;
    po.modal = true;
    po.lightDismiss = false;
    po.takeFocus = true;
    ctx.Ui().OpenPopup(std::move(dlg), po);
}

}
