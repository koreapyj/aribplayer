#pragma once
// Tooltip: flyout surface + stroke, 12 DIP text, max text width 320 DIP, above the anchor,
// flipping below, clamped inside the client.
#include <string>

#include "Widget.h"

namespace pui {

/// Pure placement: size = full tooltip box. Centred on the anchor when it is narrow (<= 64 DIP) else on the pointer;
/// 8 DIP above the anchor, flipping to 4 DIP below when it does not fit; clamped with `margin`.
RectF PlaceToolTip(SizeF size, const RectF& anchor, PointF pointer, SizeF client, float margin = 4);
/// Exact styled tooltip bounds in client DIPs; also used to report the Acrylic backdrop region.
RectF ToolTipBounds(IRenderer& r, const std::wstring& text, RectF anchor, PointF pointer, SizeF client);

void PaintToolTipStyled(IRenderer& r, const Theme& t, const std::wstring& text, RectF anchor, PointF pointer, SizeF client);

void InstallToolTipStyle(UiHost& host);

}
