#pragma once
// Pure mapping of a click inside the video area to the 0..1 coordinates of the displayed picture (IMediaPlayer::CaptionTextAt).
// Tested by --logictest (CaptionPointTests.cpp).
#include <algorithm>
#include <optional>

namespace app {

struct NormPoint { double x = 0, y = 0; };

/// (px, py) = click relative to the video area, in the area's units; (areaW, areaH) = the video area size. The picture is
/// letterboxed to `aspect` (display aspect; <= 0 = unknown, the picture is taken as filling the area) or fills the area
/// (`zoomFill`: the zoom filter already cropped the frame to the area's aspect). nullopt outside the picture.
inline std::optional<NormPoint> CaptionQueryPoint(double px, double py, double areaW, double areaH, double aspect, bool zoomFill) {
    if (!(areaW > 0) || !(areaH > 0)) return std::nullopt;
    double w = areaW, h = areaH;
    if (!zoomFill && aspect > 0) {
        w = std::min(areaW, areaH * aspect);
        h = w / aspect;
    }
    const double x0 = (areaW - w) / 2, y0 = (areaH - h) / 2;
    if (px < x0 || py < y0 || px >= x0 + w || py >= y0 + h) return std::nullopt;
    return NormPoint{(px - x0) / w, (py - y0) / h};
}

}
