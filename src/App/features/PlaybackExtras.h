#pragma once
// Playback extras (error InfoBar, track-cycling feedback, FILTER_ERROR
// capture). Subscribes to the AppContext hooks from InitPlaybackExtras.
#include <string>

#include "ShellLogic.h"
#include "pui/Geometry.h"
#include "pui/Renderer.h"
#include "pui/Theme.h"
#include "wvp/PlayerTypes.h"

namespace app {
class AppContext;

/// Called once after the main window and shell widgets exist.
void InitPlaybackExtras(AppContext& ctx);

/// ffp_cycle_stream(type) plus the toast "Audio: English (2/3)" / "Subtitles: Off" once the selection changed.
/// Subtitle cycling with no subtitle stream shows "No subtitles". Use instead of a bare CycleStream in Execute.
void CycleTrack(AppContext& ctx, wvp::StreamType type);
/// Selects the index-th stream of the type (subtitle -1 = off) by cycling the needed number of times, then toasts.
void SelectTrack(AppContext& ctx, wvp::StreamType type, int index);
/// Error InfoBar (pui::InfoBar in a popup, top centre under the title strip, auto-dismiss 12 s, replaces the previous one).
void ShowPlaybackError(AppContext& ctx, const wvp::PlayerErrorInfo& e);

/// The prepared media has audio but no video stream: the video area stays plain black.
bool AudioOnlyMedia();
}
