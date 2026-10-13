#include "DebugOverlayMac.h"

#include <algorithm>
#include <cmath>

#include "puimac/CGRenderer.h"
#include "wvp/win32/Utf.h"

namespace mainview {

using app::dbg::LineKind;

DebugOverlay::DebugOverlay() : pui::Widget(pui::Role::Text) {
    SetHitTestVisible(false);
    SetVisible(false);
    SetName(L"Debug overlay");
    SetAutomationId(L"DebugOverlay");
}

void DebugOverlay::SetScale(puimac::CGRenderer& measurer, float scale) {
    if (scale <= 0 || std::fabs(scale - scale_) < 0.001f) return;
    scale_ = scale;
    const pui::SizeF cell = measurer.MonoCell(kFontPt);
    charW_ = std::max(1.f, cell.w * scale);
    lineH_ = std::ceil(cell.h * scale);
}

void DebugOverlay::SetRows(std::vector<app::dbg::OverlayLine> rows) {
    if (rows == rows_) return;
    rows_ = std::move(rows);
    Invalidate();
}

void DebugOverlay::Paint(pui::IRenderer& r, const pui::Theme& th) {
    auto* cg = dynamic_cast<puimac::CGRenderer*>(&r);
    if (!cg || scale_ <= 0) return;
    draws_++;
    const pui::RectF b = Bounds();
    const float s = scale_;
    const float pad = std::round(app::dbg::kPadDip * s) / s, rad = 6.f, lineH = lineH_ / s;
    const pui::RectF bg = b.Deflate(0.5f, 0.5f, 0.5f, 0.5f);
    r.FillRoundRect(bg, rad, pui::Color(0xC70E1013));   // (0.055, 0.063, 0.075) at 0.78, the painter's backing
    r.StrokeRoundRect(bg, rad, pui::Color(0x29FFFFFF), 1.f);   // white at 0.16
    for (size_t i = 0; i < rows_.size(); i++) {
        const app::dbg::OverlayLine& row = rows_[i];
        const float y = b.t + pad + (float)i * lineH;
        if (y + lineH > b.b + 0.01f) break;
        pui::Color col(0xFFE8E8E8);
        switch (row.kind) {
        case LineKind::Header: col = th.accent; break;
        case LineKind::Warn: col = th.caution; break;
        case LineKind::Dim: col = pui::Color(0xFFA6ABB2); break;
        default: break;
        }
        if (row.kind == LineKind::Warn) {
            const pui::Color tint = pui::WithAlpha(th.caution, th.caution.a() * 0.16f);
            r.FillRect({b.l + pad * 0.5f, y, b.r - pad * 0.5f, y + lineH}, tint);
            r.FillRect({b.l + 1.f, y, b.l + 1.f + std::max(2.f / s, 2.f), y + lineH}, th.caution);
        }
        cg->MonoText(wvp::ToWide(row.text), {b.l + pad, y, b.r - pad, y + lineH}, kFontPt, col);
    }
}

}
