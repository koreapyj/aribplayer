#include "pages/LicensesDialog.h"

#include <windows.h>

#include <algorithm>
#include <cmath>

#include "Localize.h"
#include "pui/Buttons.h"
#include "pui/Panels.h"
#include "pui/ScrollViewer.h"
#include "resource.h"

namespace app {

std::wstring LoadThirdPartyNotices() {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_THIRD_PARTY_NOTICES), RT_RCDATA);
    if (!res) return {};
    HGLOBAL mem = LoadResource(nullptr, res);
    const DWORD size = SizeofResource(nullptr, res);
    const char* p = mem ? static_cast<const char*>(LockResource(mem)) : nullptr;
    if (!p || !size) return {};
    DWORD skip = (size >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) ? 3 : 0;
    int n = MultiByteToWideChar(CP_UTF8, 0, p + skip, (int)(size - skip), nullptr, 0);
    std::wstring w((size_t)std::max(0, n), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, p + skip, (int)(size - skip), w.data(), n);
    return w;
}

namespace {

pui::TextStyle NoticeStyle() {
    pui::TextStyle s;
    s.size = 13;
    s.wrap = true;
    s.ellipsis = false;
    s.valign = pui::ParaAlign::Top;
    return s;
}

class NoticeText final : public pui::Widget, public pui::IHeightForWidth {
public:
    NoticeText(std::wstring text, pui::IRenderer* m) : Widget(pui::Role::Text), text_(std::move(text)), m_(m) {
        SetName(Tr(L"Open source licenses", L"오픈 소스 라이선스"));
        SetHitTestVisible(false);
    }
    float HeightForWidth(pui::IRenderer* m, float width) override {
        return std::ceil(pui::MeasurerOr(m ? m : m_).MeasureText(text_, NoticeStyle(), std::max(40.f, width)).h) + 8;
    }
    pui::SizeF DesiredSize(pui::IRenderer* m) override { return {Bounds().w(), HeightForWidth(m, Bounds().w())}; }
    void Paint(pui::IRenderer& r, const pui::Theme& t) override {
        pui::RectF b = Bounds();
        r.Text(text_, pui::RectF{b.l, b.t, b.r, b.b}, NoticeStyle(), t.textPrimary);
    }
private:
    std::wstring text_;
    pui::IRenderer* m_;
};

class LicensesDialogWidget final : public pui::Widget {
public:
    LicensesDialogWidget(std::wstring text, pui::IRenderer* m) : Widget(pui::Role::Pane), m_(m) {
        SetName(Tr(L"Open source licenses", L"오픈 소스 라이선스"));
        SetAutomationId(L"LicensesDialog");
        auto title = std::make_unique<pui::TextBlock>(Tr(L"Open source licenses", L"오픈 소스 라이선스"), pui::TextKind::Subtitle);
        title_ = title.get();
        AddChild(std::move(title));
        auto sv = std::make_unique<pui::ScrollViewer>();
        sv_ = sv.get();
        sv_->SetMeasurer(m_);
        sv_->SetName(Tr(L"Licenses text", L"라이선스 내용"));
        auto view = std::make_unique<NoticeText>(std::move(text), m_);
        view_ = view.get();
        sv_->SetContent(std::move(view));
        AddChild(std::move(sv));
        auto close = std::make_unique<pui::Button>(Tr(L"Close", L"닫기"), pui::ButtonStyle::Accent);
        close_ = close.get();
        close_->SetAutomationId(L"LicensesCloseButton");
        close_->onClick = [this] { Close(); };
        AddChild(std::move(close));
    }
    void Close() { if (Host()) Host()->ClosePopup(this); }
    pui::Button* CloseButton() const { return close_; }

    bool OnPointerDown(const pui::PointerEvent&) override { return true; }
    bool OnPointerUp(const pui::PointerEvent&) override { return true; }
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
    NoticeText* view_ = nullptr;
    pui::Button* close_ = nullptr;
};

}

void ShowLicensesDialog(pui::UiHost& host, pui::IRenderer& measurer) {
    std::wstring text = LoadThirdPartyNotices();
    if (text.empty()) text = Tr(L"The license notices are not available.", L"라이선스 고지를 사용할 수 없습니다.");
    auto dlg = std::make_unique<LicensesDialogWidget>(std::move(text), &measurer);
    LicensesDialogWidget* raw = dlg.get();
    pui::SizeF cs = host.Size();
    float w = std::min(640.f, std::max(280.f, cs.w - 32)), h = std::min(560.f, std::max(240.f, cs.h - 64));
    dlg->SetBounds(pui::RectF::XYWH(std::round((cs.w - w) / 2), std::round((cs.h - h) / 2), w, h));
    pui::PopupOptions po;
    po.modal = true;
    po.lightDismiss = false;
    po.takeFocus = true;
    host.OpenPopup(std::move(dlg), po);
    host.SetFocus(raw->CloseButton(), true);
}

}
