#include "wvp/PlaylistLoader.h"
#include "wvp/FileIo.h"
#include "wvp/Utf.h"

#include <algorithm>
#include <memory>
#include <stdexcept>

namespace wvp {

namespace {

bool ReadAllBytes(const std::wstring& path, std::vector<std::uint8_t>& out) {
    auto size = fileio::FileSize(path);
    if (!size) return false;
    if (*size > PlaylistLoader::MaxPlaylistBytes * 4ull) throw std::runtime_error("Playlist is too large.");
    std::string bytes;
    if (!fileio::ReadAllBytes(path, bytes)) return false;
    out.assign(bytes.begin(), bytes.end());
    return true;
}

bool ContainsI(const std::wstring& s, const std::wstring& needle) {
    return WinPath::ToLower(s).find(needle) != std::wstring::npos;
}

std::wstring MediaTypeOf(const std::wstring& contentType) {
    size_t semi = contentType.find(L';');
    std::wstring t = contentType.substr(0, semi);
    size_t b = t.find_first_not_of(L" \t"), e = t.find_last_not_of(L" \t");
    return b == std::wstring::npos ? L"" : t.substr(b, e - b + 1);
}

}

PlaylistLoader::PlaylistLoader(HttpFetchFn fetch) : fetch_(fetch ? std::move(fetch) : HttpFetchFn(WinHttpFetch)) {}

PlaylistLoadResult PlaylistLoader::Load(const std::wstring& input) const {
    MediaUriInfo info = MediaUri::Parse(input);
    if (info.kind == MediaSourceKind::Empty) return PlaylistLoadResult{};

    auto single = [&](const std::wstring& url) {
        PlaylistLoadResult r;
        r.source = info.original;
        r.items.emplace_back(url);
        return r;
    };
    auto build = [&](const std::wstring& hlsItemUrl, const std::vector<std::uint8_t>& bytes, const std::wstring& baseLocation, bool isM3u8) {
        std::wstring text = M3uParser::Decode(bytes, !isM3u8);
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
        const std::wstring& path = *info.localPath;
        if (!info.HasPlaylistExtension()) return single(info.FfmpegInput());
        std::vector<std::uint8_t> bytes;
        if (!ReadAllBytes(path, bytes)) throw std::runtime_error("Could not read playlist file: " + ToUtf8(path));
        return build(path, bytes, path, info.extension == L".m3u8");
    }

    if (info.IsHttp()) {
        bool extCandidate = info.HasPlaylistExtension();
        HttpResponse resp = fetch_(info.original, [&](const std::wstring& ct) { return extCandidate || MediaUri::IsPlaylistContentType(ct); },
                                   MaxPlaylistBytes);
        if (resp.status < 200 || resp.status > 299)
            throw std::runtime_error("HTTP request failed with status " + std::to_string(resp.status));
        bool candidate = extCandidate || MediaUri::IsPlaylistContentType(resp.contentType);
        if (!candidate) return single(info.original);   // likely a media file: do not download it
        if (resp.body.size() > MaxPlaylistBytes) throw std::runtime_error("Playlist is too large.");
        std::wstring baseUrl = resp.finalUrl.empty() ? info.original : resp.finalUrl;
        bool m3u8 = info.extension == L".m3u8" || ContainsI(MediaTypeOf(resp.contentType), L"mpegurl");
        return build(info.original, resp.body, baseUrl, m3u8);
    }

    return single(info.original);   // rtmp://, udp://, ... straight to ffmpeg
}

}
