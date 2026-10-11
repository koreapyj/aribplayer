#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

#include "pui/Geometry.h"
#include "pui/Theme.h"
#include "wvp/win32/WindowsVideoPlatform.h"

namespace app::acrylic {

enum class FallbackReason {
    None,
    NoVideo,
    HighContrast,
    ModalDialog,
    PresenterUnavailable,
    TransparencyEffectsOff,
    BatterySaver,
    RemoteSession,
    PresenterFailure,
    SurfaceOutsideVideo,
    RegionLimit,
};

struct Conditions {
    bool videoShowing = false;
    bool d3d11Presenter = false;
    bool modalDialog = false;
    bool transparencyEffects = true;
    bool batterySaver = false;
    bool remoteSession = false;
    bool highContrast = false;
    bool presenterFailure = false;
};

inline FallbackReason DecideFallback(const Conditions& c) {
    if (!c.videoShowing) return FallbackReason::NoVideo;
    if (c.highContrast) return FallbackReason::HighContrast;
    if (c.modalDialog) return FallbackReason::ModalDialog;
    if (!c.d3d11Presenter) return FallbackReason::PresenterUnavailable;
    if (!c.transparencyEffects) return FallbackReason::TransparencyEffectsOff;
    if (c.batterySaver) return FallbackReason::BatterySaver;
    if (c.remoteSession) return FallbackReason::RemoteSession;
    if (c.presenterFailure) return FallbackReason::PresenterFailure;
    return FallbackReason::None;
}

inline std::string_view FallbackText(FallbackReason reason) {
    switch (reason) {
    case FallbackReason::None: return {};
    case FallbackReason::NoVideo: return "no video showing";
    case FallbackReason::HighContrast: return "high contrast";
    case FallbackReason::ModalDialog: return "modal dialog";
    case FallbackReason::PresenterUnavailable: return "D3D11 presenter unavailable";
    case FallbackReason::TransparencyEffectsOff: return "Transparency effects are off";
    case FallbackReason::BatterySaver: return "battery saver";
    case FallbackReason::RemoteSession: return "remote session";
    case FallbackReason::PresenterFailure: return "Acrylic presenter failed";
    case FallbackReason::SurfaceOutsideVideo: return "surface is outside the video area";
    case FallbackReason::RegionLimit: return "too many Acrylic surfaces";
    }
    return "Acrylic unavailable";
}

// `surface` is an absolute client-DIP rect; the video child rect is in main-window client pixels.
// Output coords are relative to the video child. Negative/outside coords are retained for presenter clipping.
inline std::optional<wvp::win32::BackdropRegion> RegionFromDip(const pui::RectF& surface, int videoLeftPx, int videoTopPx,
                                                         int videoWidthPx, int videoHeightPx, double scale,
                                                         float radiusDip, const pui::Theme& theme, int z) {
    if (surface.Empty() || videoWidthPx <= 0 || videoHeightPx <= 0 || !(scale > 0) || !std::isfinite(scale) ||
        !std::isfinite(surface.l) || !std::isfinite(surface.t) || !std::isfinite(surface.r) || !std::isfinite(surface.b) ||
        !std::isfinite(radiusDip) || radiusDip < 0)
        return std::nullopt;

    const long long left = std::llround((double)surface.l * scale) - videoLeftPx;
    const long long top = std::llround((double)surface.t * scale) - videoTopPx;
    const long long right = std::llround((double)surface.r * scale) - videoLeftPx;
    const long long bottom = std::llround((double)surface.b * scale) - videoTopPx;
    if (right <= left || bottom <= top || left < std::numeric_limits<int>::min() || top < std::numeric_limits<int>::min() ||
        right > std::numeric_limits<int>::max() || bottom > std::numeric_limits<int>::max() ||
        right - left > std::numeric_limits<int>::max() || bottom - top > std::numeric_limits<int>::max() ||
        right <= 0 || bottom <= 0 || left >= videoWidthPx || top >= videoHeightPx)
        return std::nullopt;

    wvp::win32::BackdropRegion out{};
    out.x = (int)left;
    out.y = (int)top;
    out.width = (int)(right - left);
    out.height = (int)(bottom - top);
    out.cornerRadius = (float)(radiusDip * scale);
    out.tintColor = theme.acrylicTintColor.v;
    out.tintOpacity = theme.acrylicTintOpacity;
    out.luminosityColor = theme.acrylicLuminosityColor.v;
    out.luminosityOpacity = theme.acrylicLuminosityOpacity;
    out.noise = true;
    out.z = z;
    return out;
}

/// An Acrylic surface may not lie under visible chrome (the visible bar, the non-sliding title strip): that
/// chrome would be composed over the transparent fill. Such a surface makes the frame fall back to solid.
inline bool OverlapsVisibleChrome(const pui::RectF& s, const pui::RectF& bar, bool barVisible,
                                  const pui::RectF& title, bool titleVisible) {
    auto hit = [&](const pui::RectF& c) { return s.l < c.r && s.r > c.l && s.t < c.b && s.b > c.t; };
    if (barVisible && !bar.Empty()) {
        if (hit(bar)) return true;
    }
    return titleVisible && !title.Empty() && hit(title);
}

inline bool SameRegions(const std::vector<wvp::win32::BackdropRegion>& a, const std::vector<wvp::win32::BackdropRegion>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const auto& x = a[i];
        const auto& y = b[i];
        if (x.x != y.x || x.y != y.y || x.width != y.width || x.height != y.height ||
            x.cornerRadius != y.cornerRadius || x.tintColor != y.tintColor || x.tintOpacity != y.tintOpacity ||
            x.luminosityColor != y.luminosityColor || x.luminosityOpacity != y.luminosityOpacity ||
            x.noise != y.noise || x.z != y.z)
            return false;
    }
    return true;
}

}
