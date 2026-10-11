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
