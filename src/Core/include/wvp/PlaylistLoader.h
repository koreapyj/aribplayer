#pragma once
// Loads local or remote .m3u/.m3u8 and decides playlist vs. HLS stream vs. plain media. Remote fetch: HttpFetch (8 MiB cap).
#include <cstdint>
#include <string>
#include <vector>

#include "HttpFetch.h"
#include "M3uParser.h"
#include "MediaItem.h"
#include "MediaUri.h"

namespace wvp {

/// IsHlsStream: items has the single URL (hand it to ffplay as-is). IsPlaylist: items parsed from an M3U.
/// Neither: a plain media file/URL (one item).
struct PlaylistLoadResult {
    std::string source;
    bool isPlaylist = false;
    bool isHlsStream = false;
    std::vector<MediaItem> items;
    std::string title;
};

class PlaylistLoader {
public:
    static constexpr std::size_t MaxPlaylistBytes = 8 * 1024 * 1024;

    /// Empty fetch = HttpFetch.
    explicit PlaylistLoader(HttpFetchFn fetch = {});

    /// Blocking. Throws std::runtime_error on I/O, HTTP-status or size errors.
    PlaylistLoadResult Load(const std::string& input) const;

private:
    HttpFetchFn fetch_;
};

}
