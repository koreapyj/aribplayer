#include "wvp/PlaylistLoader.h"
#include "wvp/Platform.h"
#include "wvp/Utf8.h"

#include <stdexcept>

namespace wvp {

namespace {

std::string ReadPlaylistFile(const std::string& path) {
    std::string bytes;
    switch (platform::ReadAllBytes(path, bytes, PlaylistLoader::MaxPlaylistBytes * 4ull)) {
    case platform::ReadResult::Ok: return bytes;
    case platform::ReadResult::TooLarge: throw std::runtime_error("Playlist is too large.");
    default: throw std::runtime_error("Could not read playlist file: " + path);
    }
}

bool ContainsI(const std::string& s, const std::string& needle) {
    return utf8::ToLowerAscii(s).find(needle) != std::string::npos;
}

std::string MediaTypeOf(const std::string& contentType) {
    size_t semi = contentType.find(';');
    std::string t = contentType.substr(0, semi);
    size_t b = t.find_first_not_of(" \t"), e = t.find_last_not_of(" \t");
    return b == std::string::npos ? "" : t.substr(b, e - b + 1);
}

}

PlaylistLoader::PlaylistLoader(HttpFetchFn fetch) : fetch_(fetch ? std::move(fetch) : HttpFetchFn(HttpFetch)) {}

PlaylistLoadResult PlaylistLoader::Load(const std::string& input) const {
    MediaUriInfo info = MediaUri::Parse(input);
    if (info.kind == MediaSourceKind::Empty) return PlaylistLoadResult{};

    auto single = [&](const std::string& url) {
        PlaylistLoadResult r;
        r.source = info.original;
        r.items.emplace_back(url);
        return r;
    };
    auto build = [&](const std::string& hlsItemUrl, const std::uint8_t* bytes, size_t size, const std::string& baseLocation, bool isM3u8) {
        std::string text = M3uParser::Decode(bytes, size, !isM3u8);
        PlaylistLoadResult r;
        r.source = info.original;
        if (M3uParser::IsHls(text)) {
            r.isHlsStream = true;
            r.items.emplace_back(hlsItemUrl);
            return r;
        }
        M3uPlaylist pl = M3uParser::Parse(text, baseLocation);
        r.isPlaylist = true;
        r.items = std::move(pl.items);
        r.title = std::move(pl.title);
        return r;
    };

    if (info.IsLocal()) {
        const std::string& path = *info.localPath;
        if (!info.HasPlaylistExtension()) return single(info.FfmpegInput());
        const std::string bytes = ReadPlaylistFile(path);
        return build(path, (const std::uint8_t*)bytes.data(), bytes.size(), path, info.extension == ".m3u8");
    }

    if (info.IsHttp()) {
        bool extCandidate = info.HasPlaylistExtension();
        HttpResponse resp = fetch_(info.original, [&](const std::string& ct) { return extCandidate || MediaUri::IsPlaylistContentType(ct); },
                                   MaxPlaylistBytes);
        if (resp.status < 200 || resp.status > 299)
            throw std::runtime_error("HTTP request failed with status " + std::to_string(resp.status));
        bool candidate = extCandidate || MediaUri::IsPlaylistContentType(resp.contentType);
        if (!candidate) return single(info.original);   // likely a media file: do not download it
        if (resp.body.size() > MaxPlaylistBytes) throw std::runtime_error("Playlist is too large.");
        std::string baseUrl = resp.finalUrl.empty() ? info.original : resp.finalUrl;
        bool m3u8 = info.extension == ".m3u8" || ContainsI(MediaTypeOf(resp.contentType), "mpegurl");
        return build(info.original, resp.body.data(), resp.body.size(), baseUrl, m3u8);
    }

    return single(info.original);   // rtmp://, udp://, ... straight to ffmpeg
}

}
