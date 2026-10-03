#pragma once
// File properties dialog (ffp_get_media_info).
// Model + formatting: features/FxProps.h. The row layout below is pure so it can be tested with a RecordingRenderer.
#include <vector>

#include "features/FxProps.h"
#include "pui/Renderer.h"

namespace app {
class AppContext;

/// Shows a toast when nothing is playing.
void ShowPropertiesDialog(AppContext& ctx);

namespace fx {
struct PropsLine {
    enum class Kind { Heading, Row } kind = Kind::Row;
    int section = 0, row = -1;   // row -1 for headings
    float y = 0, h = 0;          // relative to the content top
};
struct PropsMetrics {
    static constexpr float kHeadingH = 36, kRowMin = 26, kRowPadV = 4, kLabelW = 150, kGap = 12;
};
/// Heights of every heading/row for a content width (values wrap; labels never wrap). Returns the lines; the total
/// height is the last line's y + h (0 when empty).
std::vector<PropsLine> LayoutProps(const std::vector<PropSection>& sections, pui::IRenderer& m, float width);
float PropsTotalHeight(const std::vector<PropsLine>& lines);
pui::TextStyle PropsValueStyle();
pui::TextStyle PropsLabelStyle();
}
}
