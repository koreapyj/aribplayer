#pragma once
// Loads local or remote .m3u/.m3u8 and decides playlist vs. HLS stream vs. plain media. Remote fetch uses WinHTTP (30 s timeout, 8 MiB cap).
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "M3uParser.h"
#include "MediaItem.h"
#include "MediaUri.h"

namespace wvp {

/// IsHlsStream: items has the single URL (hand it to ffplay as-is). IsPlaylist: items parsed from an M3U.
/// Neither: a plain media file/URL (one item).
struct PlaylistLoadResult {
    std::wstring source;
    bool isPlaylist = false;
    bool isHlsStream = false;
    std::vector<MediaItem> items;
    std::wstring title;
};

struct HttpResponse {
    int status = 0;                       // HTTP status code
    std::wstring contentType;             // full Content-Type header value ("" if none)
    std::wstring finalUrl;                // URL after redirects
    std::vector<std::uint8_t> body;       // only filled when wantBody returned true
};

/// Fetch hook (injectable for tests). Must call wantBody(contentType) after the headers arrive and only
/// download the body (at most maxBytes, else throw std::runtime_error) when it returns true.
/// Throws std::runtime_error on transport errors.
using HttpFetchFn = std::function<HttpResponse(const std::wstring& url,
                                               const std::function<bool(const std::wstring& contentType)>& wantBody,
                                               std::size_t maxBytes)>;

/// Default WinHTTP implementation: 30 s timeouts, follows redirects, reports the final URL.
HttpResponse WinHttpFetch(const std::wstring& url, const std::function<bool(const std::wstring&)>& wantBody, std::size_t maxBytes);

class PlaylistLoader {
public:
    static constexpr std::size_t MaxPlaylistBytes = 8 * 1024 * 1024;

    explicit PlaylistLoader(HttpFetchFn fetch = {});

    /// Blocking. Throws std::runtime_error on I/O, HTTP-status or size errors.
    PlaylistLoadResult Load(const std::wstring& input) const;

private:
    HttpFetchFn fetch_;
};

}
