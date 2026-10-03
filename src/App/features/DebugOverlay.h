#pragma once
// Direct2D / DirectWrite painter of the Tab debug overlay (DComp layer Layer::DebugOverlay). The layout, text and
// mismatch logic live in DebugOverlayModel (pure, tested); this class only measures a monospace face and draws rows.
#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <string>
#include <vector>

#include "features/DebugOverlayModel.h"
#include "pui/Theme.h"

namespace app::dbg {

class DebugOverlayPainter {
public:
    explicit DebugOverlayPainter(IDWriteFactory* dw) : dw_(dw) {}
    void SetScale(float scale);
    float CharW() const { return charW_; }   // device px
    float LineH() const { return lineH_; }
    /// Draws backing + rows into the current target (origin = top-left of the surface, device pixels).
    void Paint(ID2D1DeviceContext* dc, const std::vector<OverlayLine>& rows, const pui::Theme& th, int wPx, int hPx);
    int DrawCount() const { return draws_; }

private:
    IDWriteFactory* dw_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_;
    float scale_ = 0, charW_ = 7, lineH_ = 16;
    int draws_ = 0;
};

}
