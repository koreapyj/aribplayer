#pragma once
// Playback extras (error InfoBar, track-cycling feedback, FILTER_ERROR
// capture). Subscribes to the AppContext hooks from InitPlaybackExtras.
#include <string>
#include <vector>

#include "ShellLogic.h"
#include "pui/Geometry.h"
#include "pui/Renderer.h"
#include "pui/Theme.h"
#include "wvp/PlayerTypes.h"

namespace app {
class AppContext;

/// Called once after the main window and shell widgets exist.
void InitPlaybackExtras(AppContext& ctx);

/// ffp_select_stream by media-info index (subtitle -1 = off; ARIB audio: 0 / 1 = the audio preference), then the toast
/// "Audio: English (2/3)" / "Subtitles: Off" once the selection changed.
void SelectTrack(AppContext& ctx, wvp::StreamType type, int index);
/// Keyboard cycle: next subtitle track, then off, then the first again; "No subtitles" when the media has none.
void CycleSubtitleTrack(AppContext& ctx);
/// Subtitle button: shows / hides the caption layer (every subtitle kind) and toasts; "No subtitles" when there is no track.
void ToggleSubtitles(AppContext& ctx);
/// Makes the caption layer visible (the setting showCaptions) when a user action selects or loads a subtitle.
void ShowSubtitles(AppContext& ctx);
/// Subtitle sync: moves the delay by deltaMs (0 = reset to 0) and toasts the new value.
void AdjustSubtitleDelay(AppContext& ctx, int deltaMs);
/// Adds subtitle files to the current media (Load subtitle file, drops). Returns the first new stream index or a negative
/// value when none was added; `select` selects it and shows the captions.
int AttachSubtitleFiles(AppContext& ctx, const std::vector<std::wstring>& paths, bool select);
/// Error InfoBar (pui::InfoBar in a popup, top centre under the title strip, auto-dismiss 12 s, replaces the previous one).
void ShowPlaybackError(AppContext& ctx, const wvp::PlayerErrorInfo& e);

/// The prepared media has audio but no video stream: the video area stays plain black.
bool AudioOnlyMedia();
}
