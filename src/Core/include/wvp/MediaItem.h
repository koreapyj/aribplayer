#pragma once
// Queue entry. Strings are UTF-8.
#include <optional>
#include <string>
#include <utility>

namespace wvp {

struct MediaItem {
    std::string url;
    std::string title;               // empty = unknown
    std::optional<double> duration;   // seconds

    MediaItem() = default;
    explicit MediaItem(std::string u, std::string t = {}, std::optional<double> d = std::nullopt)
        : url(std::move(u)), title(std::move(t)), duration(d) {}

    /// Title, or the file/last URL segment without extension when no title is known.
    std::string DisplayTitle() const;

    bool operator==(const MediaItem& o) const { return url == o.url && title == o.title && duration == o.duration; }
};

}
