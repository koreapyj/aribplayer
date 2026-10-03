#include "pui/D2dRenderer.h"

#include <d2d1helper.h>

#include <algorithm>
#include <cmath>

namespace pui {

using Microsoft::WRL::ComPtr;

namespace {
inline D2D1_COLOR_F Cf(Color c) { return D2D1::ColorF(c.r(), c.g(), c.b(), c.a()); }
inline D2D1_RECT_F Rf(const RectF& r) { return D2D1::RectF(r.l, r.t, r.r, r.b); }

bool FamilyExists(IDWriteFontCollection* coll, const wchar_t* name) {
    if (!coll) return false;
    UINT32 idx = 0;
    BOOL exists = FALSE;
    coll->FindFamilyName(name, &idx, &exists);
    return exists != FALSE;
}
}

D2dRenderer::D2dRenderer(ID2D1DeviceContext* dc, IDWriteFactory* dwrite) : dc_(dc), dw_(dwrite) {
    ComPtr<IDWriteFontCollection> coll;
    if (dw_) dw_->GetSystemFontCollection(&coll, FALSE);
    textFamily_ = FamilyExists(coll.Get(), L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    iconFamily_ = FamilyExists(coll.Get(), L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
}

D2dRenderer::~D2dRenderer() = default;

void D2dRenderer::SetScale(float scale) {
    scale = std::max(0.25f, scale);
    if (scale == scale_) return;
    scale_ = scale;
    formats_.clear();
}

void D2dRenderer::ResetDeviceResources() {
    brush_.Reset();
    brushDc_ = nullptr;
}

void D2dRenderer::Begin(ID2D1DeviceContext* dc, PointF originDip) {
    if (dc != dc_) ResetDeviceResources();
    dc_ = dc;
    if (!dc_) return;
    dc_->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    dc_->GetTransform(&savedTransform_);
    D2D1_MATRIX_3X2_F layer = D2D1::Matrix3x2F::Translation(-originDip.x, -originDip.y) * D2D1::Matrix3x2F::Scale(scale_, scale_);
    dc_->SetTransform(layer * savedTransform_);
    inFrame_ = true;
    drawEnabled_ = true;
    stack_.clear();
}

void D2dRenderer::End() {
    if (!dc_ || !inFrame_) return;
    while (!stack_.empty()) {
        if (stack_.back()) dc_->PopLayer(); else dc_->PopAxisAlignedClip();
        stack_.pop_back();
    }
    dc_->SetTransform(savedTransform_);
    inFrame_ = false;
    drawEnabled_ = true;
}

ID2D1SolidColorBrush* D2dRenderer::Brush(Color c) {
    if (!dc_) return nullptr;
    if (!brush_ || brushDc_ != dc_) {
        brush_.Reset();
        dc_->CreateSolidColorBrush(Cf(c), &brush_);
        brushDc_ = dc_;
    } else {
        brush_->SetColor(Cf(c));
    }
    return brush_.Get();
}

IDWriteTextFormat* D2dRenderer::Format(const TextStyle& s) {
    int px = std::max(1, (int)std::lround(s.size * scale_));
    Key key{(int)s.font, px, s.weight, (int)s.align, (int)s.valign, s.wrap, s.ellipsis && !s.wrap};
    auto it = formats_.find(key);
    if (it != formats_.end()) return it->second.format.Get();
    if (!dw_) return nullptr;
    const std::wstring& fam = s.font == FontKind::Icon ? iconFamily_ : textFamily_;
    ComPtr<IDWriteTextFormat> f;
    float sizeDip = (float)px / scale_;   // device pixels = px after the scale transform
    if (FAILED(dw_->CreateTextFormat(fam.c_str(), nullptr, (DWRITE_FONT_WEIGHT)std::clamp(s.weight, 1, 999), DWRITE_FONT_STYLE_NORMAL,
                                     DWRITE_FONT_STRETCH_NORMAL, sizeDip, L"en-us", &f)) || !f)
        return nullptr;
    f->SetTextAlignment(s.align == TextAlign::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                        : s.align == TextAlign::Trailing ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
    f->SetParagraphAlignment(s.valign == ParaAlign::Center ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER
                             : s.valign == ParaAlign::Bottom ? DWRITE_PARAGRAPH_ALIGNMENT_FAR : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    f->SetWordWrapping(s.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (!s.wrap && s.ellipsis) {
        DWRITE_TRIMMING t{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> sign;
        dw_->CreateEllipsisTrimmingSign(f.Get(), &sign);
        f->SetTrimming(&t, sign.Get());
    }
    auto& e = formats_[key];
    e.format = f;
    return e.format.Get();
}

void D2dRenderer::FillRect(const RectF& r, Color c) {
    if (!drawEnabled_ || !dc_ || r.Empty() || c.a() <= 0) return;
    dc_->FillRectangle(Rf(r), Brush(c));
}

void D2dRenderer::FillRoundRect(const RectF& r, float radius, Color c) {
    if (!drawEnabled_ || !dc_ || r.Empty() || c.a() <= 0) return;
    dc_->FillRoundedRectangle(D2D1::RoundedRect(Rf(r), radius, radius), Brush(c));
}

void D2dRenderer::StrokeRoundRect(const RectF& r, float radius, Color c, float width) {
    if (!drawEnabled_ || !dc_ || r.Empty() || c.a() <= 0) return;
    RectF in = r.Inflate(-width * 0.5f);   // keep the stroke inside the rect
    if (in.Empty()) return;
    float rr = std::max(0.f, radius - width * 0.5f);
    dc_->DrawRoundedRectangle(D2D1::RoundedRect(Rf(in), rr, rr), Brush(c), width);
}

void D2dRenderer::FillEllipse(PointF p, float rx, float ry, Color c) {
    if (!drawEnabled_ || !dc_ || c.a() <= 0) return;
    dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(p.x, p.y), rx, ry), Brush(c));
}

void D2dRenderer::StrokeEllipse(PointF p, float rx, float ry, Color c, float width) {
    if (!drawEnabled_ || !dc_ || c.a() <= 0) return;
    dc_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(p.x, p.y), rx, ry), Brush(c), width);
}

void D2dRenderer::Line(PointF a, PointF b, Color c, float width) {
    if (!drawEnabled_ || !dc_ || c.a() <= 0) return;
    dc_->DrawLine(D2D1::Point2F(a.x, a.y), D2D1::Point2F(b.x, b.y), Brush(c), width);
}

void D2dRenderer::Text(std::wstring_view text, const RectF& r, const TextStyle& style, Color c) {
    if (!drawEnabled_ || !dc_ || text.empty() || r.Empty() || c.a() <= 0) return;
    IDWriteTextFormat* f = Format(style);
    if (!f) return;
    dc_->DrawText(text.data(), (UINT32)text.size(), f, Rf(r), Brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}

SizeF D2dRenderer::MeasureText(std::wstring_view text, const TextStyle& style, float maxWidth) {
    IDWriteTextFormat* f = Format(style);
    if (!f || !dw_) return {0.5f * style.size * (float)text.size(), 1.33f * style.size};
    float w = maxWidth > 0 ? maxWidth : 100000.f;
    ComPtr<IDWriteTextLayout> lay;
    // An empty string still has a line height.
    const wchar_t* data = text.empty() ? L" " : text.data();
    UINT32 len = text.empty() ? 1u : (UINT32)text.size();
    if (FAILED(dw_->CreateTextLayout(data, len, f, w, 100000.f, &lay)) || !lay) return {0, 1.33f * style.size};
    DWRITE_TEXT_METRICS tm{};
    lay->GetMetrics(&tm);
    // Unconstrained measures get ~1 DIP of slack: a control sized to exactly this width is drawn with an ellipsis-trimming
    // layout of the same width, and sub-pixel differences can make DirectWrite trim the last characters.
    float tw = text.empty() ? 0.f : tm.widthIncludingTrailingWhitespace;
    if (maxWidth <= 0 && tw > 0) tw = std::ceil(tw) + 1.f;
    return {tw, tm.height};
}

void D2dRenderer::PushClip(const RectF& r) {
    if (dc_ && inFrame_) dc_->PushAxisAlignedClip(Rf(r), D2D1_ANTIALIAS_MODE_ALIASED);
    stack_.push_back(false);
}

void D2dRenderer::PopClip() {
    if (stack_.empty() || stack_.back()) return;
    stack_.pop_back();
    if (dc_ && inFrame_) dc_->PopAxisAlignedClip();
}

void D2dRenderer::PushOpacity(float opacity) {
    if (dc_ && inFrame_)
        dc_->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(),
                                              std::clamp(opacity, 0.f, 1.f), nullptr, D2D1_LAYER_OPTIONS1_NONE),
                       nullptr);
    stack_.push_back(true);
}

void D2dRenderer::PopOpacity() {
    if (stack_.empty() || !stack_.back()) return;
    stack_.pop_back();
    if (dc_ && inFrame_) dc_->PopLayer();
}

}
