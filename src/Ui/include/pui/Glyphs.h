#pragma once
// Named code points of "Segoe Fluent Icons" (fallback "Segoe MDL2 Assets") used by the app; no third-party assets.
#include <string>

namespace pui::glyph {

constexpr wchar_t Play = L'';
constexpr wchar_t Pause = L'';
constexpr wchar_t Stop = L'';
constexpr wchar_t PreviousLegacy = L'';
constexpr wchar_t NextLegacy = L'';
constexpr wchar_t Shuffle = L'';         // shuffle on (off = same glyph at 0.55 opacity)
constexpr wchar_t PlaybackRate = L'';
constexpr wchar_t Live = L'';

constexpr wchar_t Volume = L'';

constexpr wchar_t FullScreen = L'';
constexpr wchar_t BackToWindow = L'';
constexpr wchar_t MiniPlayer = L'';
constexpr wchar_t ExitMiniPlayer = L'';
constexpr wchar_t Subtitles = L'';
constexpr wchar_t Equalizer = L'';
constexpr wchar_t VideoSettings = L'';   // video camera glyph used for "Video settings"
constexpr wchar_t VideoEnhancer = L'';
constexpr wchar_t Properties = L'';
constexpr wchar_t Info = L'';

constexpr wchar_t Hamburger = L'';
constexpr wchar_t Music = L'';
constexpr wchar_t Settings = L'';
constexpr wchar_t Search = L'';
constexpr wchar_t ChevronDown = L'';
constexpr wchar_t ChevronDownSmall = L'';
constexpr wchar_t ChevronLeftSmall = L'';
constexpr wchar_t ChevronRightSmall = L'';

constexpr wchar_t Add = L'';
constexpr wchar_t Delete = L'';
constexpr wchar_t Rename = L'';
constexpr wchar_t Save = L'';
constexpr wchar_t Copy = L'';
constexpr wchar_t Refresh = L'';
constexpr wchar_t OpenFile = L'';
constexpr wchar_t OpenFolder = L'';
constexpr wchar_t OpenUrl = L'';
constexpr wchar_t Link = L'';            // marks read-only reference playlists
constexpr wchar_t Playlist = L'';

constexpr wchar_t Pin = L'';
constexpr wchar_t Pinned = L'';
constexpr wchar_t ChromeMinimize = L'';
constexpr wchar_t ChromeMaximize = L'';
constexpr wchar_t ChromeRestore = L'';
constexpr wchar_t ChromeClose = L'';
constexpr wchar_t ClosedCaption = L'';
constexpr wchar_t Sort = L'';
constexpr wchar_t Sliders = L'';         // same code point as Equalizer

constexpr wchar_t CheckMark = L'';
constexpr wchar_t Close = L'';
constexpr wchar_t Success = L'';
constexpr wchar_t Warning = L'';
constexpr wchar_t Error = L'';

inline std::wstring Str(wchar_t g) { return std::wstring(1, g); }

}
