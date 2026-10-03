#pragma once
// Queue entry. Strings are UTF-16 (std::wstring).
#include <optional>
#include <string>
#include <utility>

namespace wvp {

struct MediaItem {
    std::wstring url;
    std::wstring title;               // empty = unknown
    std::optional<double> duration;   // seconds

    MediaItem() = default;
    explicit MediaItem(std::wstring u, std::wstring t = {}, std::optional<double> d = std::nullopt)
        : url(std::move(u)), title(std::move(t)), duration(d) {}

    /// Title, or the file/last URL segment without extension when no title is known.
    std::wstring DisplayTitle() const;

    bool operator==(const MediaItem& o) const { return url == o.url && title == o.title && duration == o.duration; }
};

}
