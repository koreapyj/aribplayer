#pragma once
// Queue ordering: computes a permutation for PlaybackController::Reorder, no state of its own.
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "MediaItem.h"

namespace wvp {

enum class SortKey { Title, FileName, Extension, Size, Duration, Date, Random, Folder };

/// File metadata of one item; nullopt = unknown (sorts last).
struct FileMeta {
    std::optional<std::int64_t> size;       // bytes
    std::optional<std::int64_t> modified;   // any monotonic tick count (default provider: filesystem clock ticks)
};
using FileMetaProvider = std::function<FileMeta(const std::string& url)>;

/// std::filesystem based; URLs (anything with "://") and missing files give unknown values.
FileMetaProvider DefaultFileMetaProvider();

/// The new order: result[newIndex] = oldIndex (feed to PlaybackController::Reorder). Stable.
/// reverse=false: ascending; reverse=true: descending (unknown values still last). Random ignores `reverse` and is a
/// seeded shuffle. Size/Date use `meta` (empty = DefaultFileMetaProvider()); Duration uses MediaItem::duration.
std::vector<int> SortOrder(const std::vector<MediaItem>& items, SortKey key, bool reverse = false,
                           const FileMetaProvider& meta = {}, unsigned randomSeed = 0);

std::vector<int> ReverseOrder(size_t count);

/// Case-insensitive natural comparison (digit runs compare numerically): <0, 0, >0.
int NaturalCompare(const std::string& a, const std::string& b);

/// Pieces of a url used by the sort keys (query/fragment dropped for non-local urls).
std::string UrlFileName(const std::string& url);    // last segment, with extension
std::string UrlExtension(const std::string& url);   // with the dot, "" when none
std::string UrlFolder(const std::string& url);      // everything before the last separator, "" when none

}
