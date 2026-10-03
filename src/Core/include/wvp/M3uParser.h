#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "MediaItem.h"

namespace wvp {

struct M3uPlaylist {
    std::vector<MediaItem> items;
    std::wstring title;        // empty = none (#PLAYLIST:)
};

namespace M3uParser {
/// True when the text is an HLS (media or master) playlist rather than an M3U list of files.
bool IsHls(std::wstring_view text);

/// Decodes playlist bytes: UTF-8/UTF-16 BOM honoured; strict UTF-8 first; for non-UTF-8 content
/// ".m3u" falls back to the system ANSI code page, ".m3u8" (allowAnsiFallback=false) to lenient UTF-8.
std::wstring Decode(const std::uint8_t* bytes, size_t size, bool allowAnsiFallback = true);
std::wstring Decode(const std::vector<std::uint8_t>& bytes, bool allowAnsiFallback = true);

/// baseLocation: playlist path or URL used to resolve relative entries (may be empty).
M3uPlaylist Parse(std::wstring_view text, std::wstring_view baseLocation = {});

/// Parses "duration[ key=\"v\" ...],title" (text after "#EXTINF:").
void ParseExtInf(std::wstring_view s, std::optional<double>& duration, std::wstring& title);
}

namespace MediaPathResolver {
/// Resolves a playlist entry against the playlist's location (local path or http(s) URL).
std::wstring Resolve(std::wstring_view entry, std::wstring_view baseLocation);
}

}
