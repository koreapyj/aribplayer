#pragma once
// The Tab debug overlay on macOS: the Mac counterpart of features/DebugOverlay.cpp (DebugOverlayPainter: Direct2D /
// DirectWrite into the DComp layer Layer::DebugOverlay). The model is src/App's:
// features/DebugOverlayModel.cpp (BuildOverlayLines, WrapLines, ClipRows, ComputeOverlayGeometry, DecideTab) and
// features/PipelineOverlay.cpp (the planner-vs-measured "pipeline" section). This widget only measures the monospaced face
// and paints rows, as the painter does: a rounded dark backing with a light hairline, Header rows in the accent colour,
// Warn rows in the caution colour on a caution tint with a caution bar at the left edge, Dim rows grey; drawn through
// pui::IRenderer (backing, bars) and puimac::CGRenderer::MonoText (SF Mono; pui has no monospace font kind).
//
// It is a hit-test-invisible child of the shell root, above the shell widgets and below popups (Windows' layer order:
// above the video and the chrome, below popups).
#include <vector>

#include "features/DebugOverlayModel.h"
#include "pui/Widget.h"

namespace puimac { class CGRenderer; }

namespace mainview {

class DebugOverlay final : public pui::Widget {
public:
    DebugOverlay();
    /// DebugOverlayPainter::SetScale: the monospaced face at kFontPt; CharW / LineH in device pixels (as on Windows).
    void SetScale(puimac::CGRenderer& measurer, float scale);
    float CharW() const { return charW_; }
    float LineH() const { return lineH_; }
    /// The rows to draw (wrapped and clipped by the caller, App::RenderDebugLayer's way).
    void SetRows(std::vector<app::dbg::OverlayLine> rows);
    const std::vector<app::dbg::OverlayLine>& Rows() const { return rows_; }
    int DrawCount() const { return draws_; }
    void Paint(pui::IRenderer& r, const pui::Theme& t) override;

    static constexpr float kFontPt = 11;   // Windows: 12 DIP Cascadia Mono; 11 pt is the Mac ramp's step for 12 DIP text

private:
    std::vector<app::dbg::OverlayLine> rows_;
    float scale_ = 0, charW_ = 7, lineH_ = 16;
    int draws_ = 0;
};

}
