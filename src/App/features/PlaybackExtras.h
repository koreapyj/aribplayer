#pragma once
// Playback extras (audio-only placeholder, error InfoBar, track-cycling feedback, FILTER_ERROR
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

struct AudioOnlyState {
    bool audioOnly = false;      // the prepared media has no video stream
    std::wstring title;
};
const AudioOnlyState& AudioOnly();
/// Draws the music-note glyph and the title centred in `area` (DIP; the area is black video background, so the colours are
/// fixed light tones, not theme text colours).
void PaintAudioOnlyPlaceholder(pui::IRenderer& r, const pui::Theme& t, const pui::RectF& area, const std::wstring& title);
}
