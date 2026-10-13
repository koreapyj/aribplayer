#pragma once
// macOS stand-in for src/App/LayerRenderer.h: only the renderer extension shell/ShellRoot.cpp reaches with dynamic_cast
// (ISuppressible). The Direct2D/DirectComposition LayerRenderer itself does not exist on macOS; the Mac host paints the
// whole shell in one CoreGraphics pass (no per-layer hash gating), so SetPopupOnly is never used here.
namespace app {

struct ISuppressible {
    virtual ~ISuppressible() = default;
    virtual void SetDrawEnabled(bool on) = 0;
    virtual bool DrawEnabled() const = 0;
};

}
