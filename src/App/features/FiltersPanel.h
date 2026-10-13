#pragma once
// The pure model is features/FxModel.h.
#include <functional>

#include "wvp/FilterManager.h"
#include "wvp/PlaybackController.h"

namespace app {
class AppContext;
enum class FiltersTab { Video, Audio, Advanced };

/// Panel placement for a client of the given DIP size: right edge, below the title strip, up to the client height
/// (the panel sizes itself to its content up to maxH and scrolls beyond it).
struct FiltersPanelGeometry { float left = 0, top = 0, w = 0, maxH = 0; };
FiltersPanelGeometry PanelGeometry(float clientW, float clientH);

/// When the panel is already open only the tab switches.
void ShowFiltersPanel(AppContext& ctx, FiltersTab tab);
/// The panel popup is open (drives the pressed look of the bar's 제어창 button).
bool IsFiltersPanelOpen(AppContext& ctx);
/// 제어창 button / F7: closes the panel when open, otherwise opens it on the Video tab.
void ToggleFiltersPanel(AppContext& ctx);
/// Rebuilds the open panel (the opened file's content decides whether the Deinterlace group is usable); no-op when closed.
void RefreshFiltersPanel(AppContext& ctx);

// All of these go FilterManager::Apply -> PlaybackController::ApplyFilterChange and only touch the player when the change
// has a rebuild flag or commands.
/// Manager settings without the zoom-to-fill crop.
wvp::FilterSettings CurrentUserFilters(AppContext& ctx);
/// Clears the last FILTER_ERROR first.
void ApplyFilterEdit(AppContext& ctx, const std::function<void(wvp::FilterSettings&)>& fn);
/// No-op for empty changes or when there is no controller.
void SendFilterChange(AppContext& ctx, const wvp::FilterChange& change);
/// Speed (clamped to 0.25..4), toast "Speed: 1.5x".
void SetPlaybackSpeed(AppContext& ctx, double speed);
bool IsZoomToFill();
void SetZoomToFill(AppContext& ctx, bool on);
void ToggleZoomToFill(AppContext& ctx);
/// Recomputes the zoom crop for the current window size and applies it when the aspect changed by more than 0.5 %.
/// Called by a 400 ms poll while zoom is on; the shell may also call it from its resize handler.
void RefreshZoomAspect(AppContext& ctx);
}
