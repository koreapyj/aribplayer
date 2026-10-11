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
std::string ToString(const std::vector<MediaItem>& items, const std::string& relativeTo = {});

/// Atomic save (platform::AtomicWriteFile). makeRelative: paths relative to the playlist's folder.
bool Save(const std::string& path, const std::vector<MediaItem>& items, bool makeRelative = false);

/// System.IO.Path.GetRelativePath equivalent (both paths fully qualified).
std::string GetRelativePath(const std::string& relativeToDir, const std::string& path);
}

}
