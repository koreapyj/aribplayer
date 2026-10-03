#include "pui/ToolTipStyle.h"

#include <algorithm>

namespace pui {

RectF PlaceToolTip(SizeF size, const RectF& anchor, PointF pointer, SizeF client, float margin) {
    float cx = anchor.w() <= 64 && anchor.w() > 0 ? anchor.cx() : pointer.x;
    float x = std::max(margin, std::min(cx - size.w * 0.5f, client.w - margin - size.w));
    float y = anchor.t - 8 - size.h;
    if (y < margin) y = anchor.b + 4;
    y = std::max(margin, std::min(y, client.h - margin - size.h));
    return RectF::XYWH(x, y, size.w, size.h);
}

RectF ToolTipBounds(IRenderer& r, const std::wstring& text, RectF anchor, PointF pointer, SizeF client) {
    TextStyle st;
    st.size = 12;
    st.wrap = true;
    st.ellipsis = false;
    st.valign = ParaAlign::Top;
    SizeF ts = r.MeasureText(text, st, 320);
    SizeF box{std::min(ts.w, 320.f) + 18, ts.h + 14};
    return PlaceToolTip(box, anchor, pointer, client);
}

void PaintToolTipStyled(IRenderer& r, const Theme& t, const std::wstring& text, RectF anchor, PointF pointer, SizeF client) {
    TextStyle st;
    st.size = 12;
    st.wrap = true;
    st.ellipsis = false;
    st.valign = ParaAlign::Top;
    RectF rc = ToolTipBounds(r, text, anchor, pointer, client);
    r.FillRoundRect(rc, t.controlRadius, t.acrylicEnabled ? Color(0x00000000) : t.acrylicFallbackColor);
    r.StrokeRoundRect(rc.Deflate(0.5f, 0.5f, 0.5f, 0.5f), t.controlRadius, t.flyoutStroke, 1);
    r.Text(text, rc.Deflate(9, 6, 9, 8), st, t.textPrimary);
}

void InstallToolTipStyle(UiHost& host) { host.paintToolTip = &PaintToolTipStyled; }

}
