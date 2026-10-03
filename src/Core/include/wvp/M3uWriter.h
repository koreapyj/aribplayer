#pragma once
// Writes a list of items as extended M3U (.m3u8, UTF-8 without BOM, CRLF line endings).
// Output round-trips through M3uParser / PlaylistLoader.
#include <string>
#include <vector>

#include "MediaItem.h"

namespace wvp {

namespace M3uWriter {
/// relativeTo (a folder): local files are written relative to it (".." allowed on the same drive);
/// URLs and files on other drives/UNC roots are written as-is. Empty = no relative paths.
std::string ToUtf8(const std::vector<MediaItem>& items, const std::wstring& relativeTo = {});
std::wstring ToString(const std::vector<MediaItem>& items, const std::wstring& relativeTo = {});

/// Atomic save (temp file + MoveFileExW REPLACE_EXISTING). makeRelative: paths relative to the playlist's folder.
bool Save(const std::wstring& path, const std::vector<MediaItem>& items, bool makeRelative = false);

/// System.IO.Path.GetRelativePath equivalent for Windows paths (both fully qualified).
std::wstring GetRelativePath(const std::wstring& relativeToDir, const std::wstring& path);
}

}
