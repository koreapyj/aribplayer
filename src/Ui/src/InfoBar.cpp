#include "pui/InfoBar.h"
#include "pui/UiText.h"

#include <algorithm>

namespace pui {

InfoBar::InfoBar() : Widget(Role::Group) {
    close_ = Add<IconButton>(glyph::Close, UiText(L"Close", L"닫기"));
    close_->SetGlyphSize(12);
    close_->SetSize(kCloseSize);
    close_->onClick = [this] {
        SetOpen(false);
        if (onClosed) onClosed();
    };
    action_ = Add<Button>();
    action_->SetVisible(false);
    action_->onClick = [this] { if (onAction) onAction(); };
}

wchar_t InfoBar::SeverityGlyph(InfoBarSeverity s) {
    switch (s) {
    case InfoBarSeverity::Success: return glyph::Success;
    case InfoBarSeverity::Warning: return glyph::Warning;
    case InfoBarSeverity::Error: return glyph::Error;
    default: return glyph::Info;
    }
}

Color InfoBar::SeverityColor(const Theme& t) const {
    switch (severity_) {
    case InfoBarSeverity::Success: return t.success;
    case InfoBarSeverity::Warning: return t.caution;
    case InfoBarSeverity::Error: return t.critical;
    default: return t.accent;
    }
}

Color InfoBar::BackgroundColor(const Theme& t) const {
    switch (severity_) {
    case InfoBarSeverity::Success: return t.successBg;
    case InfoBarSeverity::Warning: return t.cautionBg;
    case InfoBarSeverity::Error: return t.criticalBg;
    default: return t.attentionBg;
    }
}

void InfoBar::SetTitle(std::wstring t) {
    if (t == title_) return;
    std::wstring old = std::move(title_);
    title_ = std::move(t);
    SyncAutoName(*this, old, title_);
    Changed();
}

void InfoBar::SetMessage(std::wstring m) {
    if (m == message_) return;
    message_ = std::move(m);
    SetHelpText(message_);
    Changed();
}

void InfoBar::SetActionText(std::wstring text) {
    action_->SetText(text);
    action_->SetVisible(!text.empty());
    Changed();
}

void InfoBar::SetClosable(bool c) {
    closable_ = c;
    close_->SetVisible(c);
    Changed();
}

void InfoBar::SetOpen(bool open) {
    if (open == open_) return;
    open_ = open;
    SetVisible(open);
}

float InfoBar::TextWidth(float width) const {
    float right = 16;
    if (closable_) right = 4 + kCloseSize + 8;
    if (action_->Visible()) right += action_->DesiredSize(nullptr).w + 12;
    return std::max(10.f, width - kTextLeft - right);
}

float InfoBar::HeightForWidth(IRenderer* m, float width) {
    IRenderer& mr = MeasurerOr(m);
    float tw = TextWidth(width);
    TextStyle ts = BodyTextStyle(14, 600); ts.wrap = true; ts.ellipsis = false; ts.valign = ParaAlign::Top;
    TextStyle ms = BodyTextStyle(14, 400); ms.wrap = true; ms.ellipsis = false; ms.valign = ParaAlign::Top;
    float h = 0;
    if (!title_.empty()) h += mr.MeasureText(title_, ts, tw).h;
    if (!message_.empty()) h += mr.MeasureText(message_, ms, tw).h;
    return std::max(kMinHeight, h + 2 * kPadV);
}

SizeF InfoBar::DesiredSize(IRenderer* m) {
    float w = Bounds().w() > 0 ? Bounds().w() : 480.f;
    return {w, HeightForWidth(m, w)};
}

void InfoBar::OnLayout() {
    RectF b = Bounds();
    float right = b.r - 4;
    if (closable_) {
        float y = b.t + std::min(8.f, (b.h() - kCloseSize) * 0.5f);
        close_->SetBounds(RectF::XYWH(right - kCloseSize, std::max(b.t, y), kCloseSize, kCloseSize));
        right -= kCloseSize + 8;
    } else right = b.r - 16;
    if (action_->Visible()) {
        SizeF d = action_->DesiredSize(nullptr);
        action_->SetBounds(RectF::XYWH(right - d.w, b.t + 8, d.w, d.h));
    }
}

void InfoBar::Paint(IRenderer& r, const Theme& t) {
    RectF b = Bounds();
    r.FillRoundRect(b, t.controlRadius, BackgroundColor(t));
    r.StrokeRoundRect(b.Deflate(0.5f, 0.5f, 0.5f, 0.5f), t.controlRadius, t.cardStroke, 1);
    Color sev = SeverityColor(t);
    float cx = b.l + kIconX + 8, cy = b.t + kPadV + 9;
    r.FillEllipse({cx, cy}, 8, 8, sev);
    Color gc = severity_ == InfoBarSeverity::Informational ? t.textOnAccent : (t.light ? Color(0xFFFFFFFF) : Color(0xFF000000));
    r.Text(glyph::Str(SeverityGlyph(severity_)), RectF::XYWH(cx - 8, cy - 8, 16, 16), IconTextStyle(10), gc);
    float tw = TextWidth(b.w());
    IRenderer& mr = MeasurerOr(&r);
    TextStyle ts = BodyTextStyle(14, 600); ts.wrap = true; ts.ellipsis = false; ts.valign = ParaAlign::Top;
    TextStyle ms = BodyTextStyle(14, 400); ms.wrap = true; ms.ellipsis = false; ms.valign = ParaAlign::Top;
    float y = b.t + kPadV;
    if (!title_.empty()) {
        float h = mr.MeasureText(title_, ts, tw).h;
        r.Text(title_, RectF::XYWH(b.l + kTextLeft, y, tw, h), ts, t.textPrimary);
        y += h;
    }
    if (!message_.empty()) {
        float h = mr.MeasureText(message_, ms, tw).h;
        r.Text(message_, RectF::XYWH(b.l + kTextLeft, y, tw, h), ms, t.textPrimary);
    }
}

}
