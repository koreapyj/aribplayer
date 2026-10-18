#pragma once
// Command ids and the global keyboard table. Pure data + lookup (no Win32 calls), tested by --logictest.
//
// Routing in the player views (windowed / full screen / mini player): no widget holds keyboard focus. An open popup
// (menu, dialog) gets the key first, then the playlist panel while it is active (clicked; its playlist
// accelerators and list keys), then this table via LookupKey() and AppContext::Execute(). The playlist search box is
// the only focusable widget there; while it has focus, typing keys go to it.
// Lookup is exact-modifier, so plain C never collides with Ctrl+C or Ctrl+Shift+C. Shift+Tab is unbound; Tab moves focus
// while a popup is open (features/DebugOverlayModel.h DecideTab).
#include <string>
#include <vector>

namespace app {

enum class CommandId : int {
    None = 0,
    PlayPause, Previous, Next, SeekBack5, SkipForward30,
    ToggleSubtitles, CycleSubtitleTrack, LoadSubtitleFile, SubtitleDelayEarlier, SubtitleDelayLater, SubtitleDelayReset,
    ToggleMute, VolumeUp, VolumeDown,
    ToggleFullscreen, Escape, ToggleMiniPlayer,
    OpenFiles, OpenFolder, OpenUrl, CaptureFrame, CaptureFrameWithCaptions,
    ShowProperties, ShowEqualizer,
    ZoomToFill, VideoEnhancements,
    OpenSettings,
    ToggleDebugOverlay,
    Exit,
    SeekBack30, SeekBack60, SeekForward60, SeekBack300, SeekForward300,
    SpeedDown, SpeedUp, SpeedReset, FrameNext, FramePrevious,
    WindowSize50, WindowSize100, WindowSize150, WindowSize200, WindowSizeMax,
    KeyframePrevious, KeyframeNext,
    SeekForward5,
    Stop, ToggleControlPanel, TogglePlaylistPanel, CycleAlwaysOnTop, ToggleTimeMode,
    FocusPlaylistSearch, CopyFfmpegCommandLine
};

/// Modifier bits (identical to pui::Mods): 1 Shift, 2 Ctrl, 4 Alt.
constexpr unsigned kModShift = 1, kModCtrl = 2, kModAlt = 4;

struct KeyBinding {
    int vk;
    unsigned mods;
    CommandId command;
};

/// The full table (order = priority; first exact match wins). Pure data.
const std::vector<KeyBinding>& KeyTable();
/// Exact-modifier lookup. CommandId::None when unbound.
CommandId LookupKey(int vk, unsigned mods);
/// "Ctrl+P" style text for the first binding of a command ("" when none). Used in tooltips/menus.
std::wstring ShortcutText(CommandId c);

/// Seek commands: signed seconds (SeekBack5 -5 ... SeekForward300 +300); 0 for any other command. Ctrl+Shift+Left / Right
/// are KeyframePrevious / KeyframeNext, not timed seeks.
int SeekSeconds(CommandId c);
/// Window-size commands: video scale (0.5, 1, 1.5, 2), -1 for WindowSizeMax, 0 for any other command.
double WindowScale(CommandId c);

/// Keyboard speed steps: the current speed moved by `tenths` x 0.1, rounded to the 0.1 grid (no float drift), clamped
/// to [0.1, maxSpeed].
double SteppedSpeed(double current, int tenths, double maxSpeed);

}
