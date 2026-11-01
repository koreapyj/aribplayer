#include "features/PropertiesDialog.h"
#include "Localize.h"

#include <algorithm>
#include <functional>

#include "AppContext.h"
#include "features/FxModel.h"
#include "pui/Buttons.h"
#include "pui/Panels.h"
#include "pui/ScrollViewer.h"
#include "pui/SelectableText.h"
#include "features/NativeMenu.h"
#include "wvp/win32/Utf.h"

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
            float th = pui::SelectableText::HeightFor(m, sections[si].rows[ri].value, PropsValueStyle(), vw);
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
        for (const auto& sec : s_) {
            for (const auto& row : sec.rows) {
                auto v = std::make_unique<pui::SelectableText>(row.value, fx::PropsValueStyle());
                v->SetName(row.label);
                pui::SelectableText* raw = v.get();
                v->onContextMenu = [raw](pui::PointF pt) {
                    std::vector<pui::MenuItemDesc> items;
                    items.push_back(pui::MenuItemDesc::Item(Tr(L"Copy", L"복사"), [raw] { raw->CopySelection(); }, L"Ctrl+C"));
                    items.push_back(pui::MenuItemDesc::Item(Tr(L"Select all", L"모두 선택"), [raw] { raw->SelectAll(); }, L"Ctrl+A"));
                    nativemenu::ShowAtPoint(std::move(items), pt);
                };
                values_.push_back(raw);
                AddChild(std::move(v));
            }
        }
    }
    float HeightForWidth(pui::IRenderer* m, float width) override {
        return fx::PropsTotalHeight(fx::LayoutProps(s_, pui::MeasurerOr(m ? m : m_), width)) + 8;
    }
    pui::SizeF DesiredSize(pui::IRenderer* m) override { return {Bounds().w(), HeightForWidth(m, Bounds().w())}; }
    void OnLayout() override {
        const pui::RectF b = Bounds();
        if (b.w() != laidWidth_) {
            lines_ = fx::LayoutProps(s_, pui::MeasurerOr(m_), b.w());
            laidWidth_ = b.w();
        }
        const float vx = b.l + fx::PropsMetrics::kLabelW + fx::PropsMetrics::kGap;
        const float pv = fx::PropsMetrics::kRowPadV;
        size_t n = 0;
        for (const auto& ln : lines_) {
            if (ln.kind != fx::PropsLine::Kind::Row) continue;
            const float y = b.t + ln.y;
            values_[n++]->SetBounds({vx, y + pv, std::max(vx, b.r), y + ln.h - pv});
        }
    }
    void Paint(pui::IRenderer& r, const pui::Theme& t) override {
        pui::RectF b = Bounds();
        for (auto& ln : lines_) {
            float y = b.t + ln.y;
            if (ln.kind == fx::PropsLine::Kind::Heading) {
                pui::TextStyle hs;
                hs.size = 14; hs.weight = 600;
                r.Text(s_[(size_t)ln.section].title, pui::RectF::XYWH(b.l, y + 6, b.w(), 24), hs, t.textPrimary);
                r.Line({b.l, y + ln.h - 1}, {b.r, y + ln.h - 1}, t.dividerStroke, 1);
            } else {
                const auto& row = s_[(size_t)ln.section].rows[(size_t)ln.row];
                float pv = fx::PropsMetrics::kRowPadV;
                r.Text(row.label, pui::RectF::XYWH(b.l, y + pv, fx::PropsMetrics::kLabelW, ln.h - 2 * pv), fx::PropsLabelStyle(), t.textSecondary);
            }
        }
    }
private:
    std::vector<fx::PropSection> s_;
    pui::IRenderer* m_;
    std::vector<pui::SelectableText*> values_;
    std::vector<fx::PropsLine> lines_;
    float laidWidth_ = -1;
};

class PropertiesPageWidget : public pui::Widget {
public:
    PropertiesPageWidget(std::vector<fx::PropSection> sections, pui::IRenderer* m, std::function<void()> onClose) : Widget(pui::Role::Pane), m_(m) {
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
        close_->onClick = std::move(onClose);
        AddChild(std::move(close));
    }
    pui::Button* CloseButton() const { return close_; }

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
private:
    pui::IRenderer* m_;
    pui::TextBlock* title_ = nullptr;
    pui::ScrollViewer* sv_ = nullptr;
    PropsView* view_ = nullptr;
    pui::Button* close_ = nullptr;
};

}

PropertiesPage CreatePropertiesPage(std::vector<fx::PropSection> sections, pui::IRenderer* measurer, std::function<void()> onClose) {
    auto page = std::make_unique<PropertiesPageWidget>(std::move(sections), measurer, std::move(onClose));
    PropertiesPage out;
    out.initialFocus = page->CloseButton();
    out.root = std::move(page);
    return out;
}

void ShowPropertiesDialog(AppContext& ctx) {
    wvp::FfPlayer* p = ctx.Player();
    std::optional<wvp::MediaInfo> info = p ? p->GetMediaInfo() : std::nullopt;
    if (!info) {
        ctx.ShowToast(Tr(L"Nothing is playing", L"재생 중인 항목이 없습니다"));
        return;
    }
    std::optional<std::int64_t> size;
    const std::wstring wideUrl = wvp::ToWide(info->Url);
    if (fx::IsLocalPath(wideUrl)) {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(wideUrl.c_str(), GetFileExInfoStandard, &fa))
            size = (std::int64_t)(((std::uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow);
    }
    ctx.ShowPropertiesWindow(fx::BuildProperties(*info, size));
}

}
