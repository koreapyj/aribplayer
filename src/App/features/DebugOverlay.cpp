#include "features/DebugOverlay.h"

#include <algorithm>
#include <cmath>

#include "wvp/win32/Utf.h"

namespace app::dbg {

namespace {
bool FamilyExists(IDWriteFactory* dw, const wchar_t* name) {
    Microsoft::WRL::ComPtr<IDWriteFontCollection> coll;
    if (FAILED(dw->GetSystemFontCollection(&coll, FALSE)) || !coll) return false;
    UINT32 idx = 0;
    BOOL exists = FALSE;
    return SUCCEEDED(coll->FindFamilyName(name, &idx, &exists)) && exists;
}
D2D1_COLOR_F C(pui::Color c, float alphaMul = 1.f) { return D2D1::ColorF(c.r(), c.g(), c.b(), c.a() * alphaMul); }
}

void DebugOverlayPainter::SetScale(float scale) {
    if (scale <= 0 || (fmt_ && std::fabs(scale - scale_) < 0.001f)) return;
    scale_ = scale;
    fmt_.Reset();
    if (!dw_) return;
    const wchar_t* family = FamilyExists(dw_, L"Cascadia Mono") ? L"Cascadia Mono" : L"Consolas";
    float px = std::round(12.f * scale);
    if (FAILED(dw_->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px, L"en-us", &fmt_)) || !fmt_) return;
    fmt_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    Microsoft::WRL::ComPtr<IDWriteTextLayout> lay;
    const wchar_t* probe = L"0000000000";
    if (SUCCEEDED(dw_->CreateTextLayout(probe, 10, fmt_.Get(), 10000.f, 1000.f, &lay)) && lay) {
        DWRITE_TEXT_METRICS m{};
        if (SUCCEEDED(lay->GetMetrics(&m))) {
            charW_ = std::max(1.f, m.width / 10.f);
            lineH_ = std::ceil(m.height);
        }
    }
    fmt_->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lineH_, lineH_ * 0.8f);
}

void DebugOverlayPainter::Paint(ID2D1DeviceContext* dc, const std::vector<OverlayLine>& rows, const pui::Theme& th, int wPx, int hPx) {
    if (!dc || !fmt_) return;
    draws_++;
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    float pad = std::round(kPadDip * scale_), rad = std::round(6.f * scale_);
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &brush))) return;
    D2D1_ROUNDED_RECT bg = D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, (float)wPx - 0.5f, (float)hPx - 0.5f), rad, rad);
    brush->SetColor(D2D1::ColorF(0.055f, 0.063f, 0.075f, 0.78f));
    dc->FillRoundedRectangle(bg, brush.Get());
    brush->SetColor(D2D1::ColorF(1, 1, 1, 0.16f));
    dc->DrawRoundedRectangle(bg, brush.Get(), 1.f);
    for (size_t i = 0; i < rows.size(); i++) {
        const OverlayLine& r = rows[i];
        float y = pad + (float)i * lineH_;
        if (y + lineH_ > (float)hPx) break;
        pui::Color col(0xFFE8E8E8);
        switch (r.kind) {
        case LineKind::Header: col = th.accent; break;
        case LineKind::Warn: col = th.caution; break;
        case LineKind::Dim: col = pui::Color(0xFFA6ABB2); break;
        default: break;
        }
        if (r.kind == LineKind::Warn) {
            brush->SetColor(C(th.caution, 0.16f));
            dc->FillRectangle(D2D1::RectF(pad * 0.5f, y, (float)wPx - pad * 0.5f, y + lineH_), brush.Get());
            brush->SetColor(C(th.caution));
            dc->FillRectangle(D2D1::RectF(1.f, y, 1.f + std::max(2.f, std::round(2.f * scale_)), y + lineH_), brush.Get());
        }
        brush->SetColor(C(col));
        std::wstring w = wvp::ToWide(r.text);
        dc->DrawText(w.c_str(), (UINT32)w.size(), fmt_.Get(), D2D1::RectF(pad, y, (float)wPx - pad, y + lineH_), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}

}
