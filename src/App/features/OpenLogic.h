#pragma once
// Pure helpers of the open flows (no UI): media extension lists, natural sort, folder listing, URL validation,
// relative time. Tested by LOGIC_TEST cases in OpenLogic.cpp.
#include <cstdint>
#include <string>
#include <vector>

namespace app::open {

/// Lower-case extensions with dot of the media types the "Media files" filter accepts (no playlists).
const std::vector<std::wstring>& MediaExtensions();
/// Dialog filter patterns: "*.mp4;*.mkv;..." (media + playlists) / playlists only.
std::wstring MediaFilterPattern();
std::wstring PlaylistFilterPattern();
bool IsMediaFile(const std::wstring& path);      // by extension, case-insensitive
bool IsPlaylistFile(const std::wstring& path);   // .m3u / .m3u8

/// Lower-case extensions with dot of the external subtitle formats (.srt .smi .sami .ass .ssa .vtt .sup).
const std::vector<std::wstring>& SubtitleExtensions();
std::wstring SubtitleFilterPattern();
bool IsSubtitleFile(const std::wstring& path);   // by extension, case-insensitive

/// A subtitle file found next to a video. langTag is lower-case ("ko", "zh-hans"), empty when the name has none.
struct SidecarSubtitle {
    std::wstring path;
    std::wstring langTag;
};
/// Pure: `dirEntries` are the file names in the video's directory (no filesystem access). Matches "<stem>.<sub>",
/// "<stem>.<lang>.<sub>", "<video file name>.<sub>" and "<video file name>.<lang>.<sub>" (case-insensitive).
/// Result: untagged matches first, then tagged ones, each ordered by lower-case file name.
std::vector<SidecarSubtitle> FindSidecarSubtitles(const std::wstring& videoPath, const std::vector<std::wstring>& dirEntries);
/// Lists the video's directory (non-recursive, files only) and calls FindSidecarSubtitles; empty on any error.
std::vector<SidecarSubtitle> ListSidecarSubtitles(const std::wstring& videoPath);
/// A subtitle stream added from a sidecar file: its media-info index and its language (the stream's, else the file name's tag).
struct SidecarTrack {
    int streamIndex = -1;
    std::string lang;
};
/// Pure: the stream index to select after the sidecars were added. The first track whose language matches one of
/// `preferredLangs` (in that order; ISO 639-1, 639-2 and BCP-47 spellings of a language are equal) wins, else the first
/// track. -1 when `tracks` is empty.
int PickSidecarTrack(const std::vector<SidecarTrack>& tracks, const std::vector<std::string>& preferredLangs);
/// Languages the app prefers for subtitles: Korean first, then the Windows UI language (ISO 639-3 or tag, lower case).
std::vector<std::string> PreferredSubtitleLanguages();
/// #PLAYLIST title when present, otherwise the source filename/URL segment without extension or query (UTF-8).
std::string PlaylistName(const std::string& source, const std::string& title = {});

/// Case-insensitive natural order: digit runs compare by value ("a2" < "a10").
bool NaturalLess(const std::wstring& a, const std::wstring& b);
void NaturalSort(std::vector<std::wstring>& names);

/// Non-recursive: full paths of the media files directly inside `folder`, naturally sorted by file name.
std::vector<std::wstring> ListMediaFiles(const std::wstring& folder);

/// Error text for the Open URL dialog; empty when the input is an acceptable http/https/rtmp/rtsp/udp/... URL.
std::wstring ValidateUrl(const std::wstring& text);

/// "Just now", "5 minutes ago", "Yesterday", "3 days ago", ... (now/then in Unix epoch ms).
std::wstring RelativeTime(std::int64_t nowMs, std::int64_t thenMs);

}
