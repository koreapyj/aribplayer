#pragma once
//  * Widgets draw in DIPs. Begin() installs a transform  (p_dip - originDip) * scale  composed on top of the transform the
//    dc already has (DirectComposition surface draws set a translation for the surface atlas offset), so one renderer can
//    paint any layer surface: pass the layer's top-left (in client DIPs) as originDip.
//  * The dc must use D2D1_UNIT_MODE_PIXELS (Begin() sets it) and a 96 dpi target.
//  * Fonts: FontKind::Text = "Segoe UI Variable Text" (fallback "Segoe UI"); FontKind::Icon = "Segoe Fluent Icons"
//    (fallback "Segoe MDL2 Assets"). Availability is detected once from the system font collection.
//    Text formats are cached per (font, device-pixel size, weight, alignment, wrap, ellipsis); a format's size is
//    round(dip * scale) device pixels for pixel-snapped glyph metrics.
//  * Text is clipped to its rect, trimmed with a character ellipsis when TextStyle::ellipsis and not wrapping.
//  * PushClip = axis-aligned clip (aliased); PushOpacity = D2D layer with an opacity parameter.
//  * SetDrawEnabled(false) turns every draw call into a no-op (clip/opacity pushes are still balanced by the caller),
//    which the app uses to skip a subtree.
//  * MeasureText works without a device context (DirectWrite only), so layout code and tests can measure headless.
#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "Renderer.h"

namespace pui {

class D2dRenderer final : public IRenderer {
public:
    /// dc may be nullptr (supply it per frame with Begin()). dwrite must outlive the renderer.
    D2dRenderer(ID2D1DeviceContext* dc, IDWriteFactory* dwrite);
    explicit D2dRenderer(IDWriteFactory* dwrite) : D2dRenderer(nullptr, dwrite) {}
    ~D2dRenderer() override;

    /// DIP -> device pixel scale (dpi / 96). Clears the text format cache when it changes.
    void SetScale(float scale);
    float Scale() const { return scale_; }

    /// Starts painting into dc (inside the caller's BeginDraw/EndDraw). originDip = client DIP position that maps to the
    /// surface's (0,0).
    void Begin(ID2D1DeviceContext* dc, PointF originDip = {});
    /// Pops unbalanced clips/layers and restores the dc transform.
    void End();

    void SetDrawEnabled(bool on) { drawEnabled_ = on; }
    bool DrawEnabled() const { return drawEnabled_; }

    void FillRect(const RectF& r, Color c) override;
    void FillRoundRect(const RectF& r, float radius, Color c) override;
    void StrokeRoundRect(const RectF& r, float radius, Color c, float width = 1) override;
    void FillEllipse(PointF center, float rx, float ry, Color c) override;
    void StrokeEllipse(PointF center, float rx, float ry, Color c, float width = 1) override;
    void Line(PointF a, PointF b, Color c, float width = 1) override;
    void Text(std::wstring_view text, const RectF& r, const TextStyle& style, Color c) override;
    SizeF MeasureText(std::wstring_view text, const TextStyle& style, float maxWidth = 0) override;
    void PushClip(const RectF& r) override;
    void PopClip() override;
    void PushOpacity(float opacity) override;
    void PopOpacity() override;

    /// Drops everything tied to the device context (cached brush, dc pointer) before its device is released and recreated.
    void ReleaseDevice() { ResetDeviceResources(); dc_ = nullptr; inFrame_ = false; stack_.clear(); }
    /// the current dc, or nullptr outside Begin/End.
    ID2D1DeviceContext* Dc() const { return dc_; }
    /// Shared solid brush set to colour c (valid until the next draw call).
    ID2D1SolidColorBrush* Brush(Color c);

private:
    struct FormatEntry {
        Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
    };
    using Key = std::tuple<int, int, int, int, int, bool, bool>;   // font, px size, weight, align, valign, wrap, ellipsis
    IDWriteTextFormat* Format(const TextStyle& s);
    void ResetDeviceResources();

    ID2D1DeviceContext* dc_ = nullptr;
    IDWriteFactory* dw_ = nullptr;
    float scale_ = 1.f;
    bool inFrame_ = false, drawEnabled_ = true;
    D2D1_MATRIX_3X2_F savedTransform_{};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    ID2D1DeviceContext* brushDc_ = nullptr;
    std::map<Key, FormatEntry> formats_;
    std::wstring textFamily_, iconFamily_;
    std::vector<bool> stack_;   // true = layer (opacity), false = clip
};

}
