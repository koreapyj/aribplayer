#pragma once
// With a NULL reference, insertAbove=TRUE puts a new visual BELOW all existing siblings. Gfx inserts each visual above
// the current topmost one so the popup layer stays above the bar and title, and records the stack in Gfx::ZOrder().
namespace app {

enum class Layer { Content, Panel, DebugOverlay, Title, Bar, Popup, Count };

constexpr Layer kLayerStack[] = {Layer::Content, Layer::Panel, Layer::DebugOverlay, Layer::Title, Layer::Bar, Layer::Popup};

constexpr int LayerZ(Layer l) {
    for (int i = 0; i < (int)Layer::Count; i++)
        if (kLayerStack[i] == l) return i;
    return -1;
}

}
