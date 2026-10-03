#include "wvp/PlaylistLoader.h"
#include "wvp/Utf.h"

#include <winhttp.h>

#include <algorithm>
#include <memory>
#include <stdexcept>

#pragma comment(lib, "winhttp.lib")

namespace wvp {

namespace {

struct WinHttpHandle {
    HINTERNET h = nullptr;
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET x) : h(x) {}
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    ~WinHttpHandle() { if (h) WinHttpCloseHandle(h); }
};

[[noreturn]] void Fail(const char* what) {
    throw std::runtime_error(std::string(what) + " (error " + std::to_string(GetLastError()) + ")");
}

bool ReadAllBytes(const std::wstring& path, std::vector<std::uint8_t>& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    if ((unsigned long long)sz.QuadPart > PlaylistLoader::MaxPlaylistBytes * 4ull) { CloseHandle(f); throw std::runtime_error("Playlist is too large."); }
    out.resize((size_t)sz.QuadPart);
    size_t done = 0;
    while (done < out.size()) {
        DWORD got = 0;
        if (!ReadFile(f, out.data() + done, (DWORD)std::min<size_t>(out.size() - done, 1u << 20), &got, nullptr) || got == 0) break;
        done += got;
    }
    out.resize(done);
    CloseHandle(f);
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

HttpResponse WinHttpFetch(const std::wstring& url, const std::function<bool(const std::wstring&)>& wantBody, std::size_t maxBytes) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) Fail("Invalid URL");

    WinHttpHandle session(WinHttpOpen(L"aribplayer/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) Fail("WinHttpOpen failed");
    WinHttpSetTimeouts(session.h, 30000, 30000, 30000, 30000);
    DWORD decomp = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(session.h, WINHTTP_OPTION_DECOMPRESSION, &decomp, sizeof(decomp));

    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    WinHttpHandle conn(WinHttpConnect(session.h, host.c_str(), uc.nPort, 0));
    if (!conn.h) Fail("WinHttpConnect failed");
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    WinHttpHandle req(WinHttpOpenRequest(conn.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!req.h) Fail("WinHttpOpenRequest failed");

    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) Fail("Request failed");
    if (!WinHttpReceiveResponse(req.h, nullptr)) Fail("No response");

    HttpResponse r;
    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                        WINHTTP_NO_HEADER_INDEX);
    r.status = (int)status;

    DWORD len = 0;
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &len, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && len > 0) {
        std::wstring ct(len / sizeof(wchar_t), L'\0');
        if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, ct.data(), &len, WINHTTP_NO_HEADER_INDEX)) {
            ct.resize(len / sizeof(wchar_t));
            r.contentType = ct;
        }
    }

    len = 0;
    WinHttpQueryOption(req.h, WINHTTP_OPTION_URL, nullptr, &len);
    if (len > 0) {
        std::wstring fu(len / sizeof(wchar_t), L'\0');
        if (WinHttpQueryOption(req.h, WINHTTP_OPTION_URL, fu.data(), &len)) {
            fu.resize(len / sizeof(wchar_t));
            while (!fu.empty() && fu.back() == L'\0') fu.pop_back();
            r.finalUrl = fu;
        }
    }
    if (r.finalUrl.empty()) r.finalUrl = url;

    if (r.status < 200 || r.status > 299) return r;
    if (!wantBody(r.contentType)) return r;

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail)) Fail("Read failed");
        if (avail == 0) break;
        size_t old = r.body.size();
        r.body.resize(old + avail);
        DWORD got = 0;
        if (!WinHttpReadData(req.h, r.body.data() + old, avail, &got)) Fail("Read failed");
        r.body.resize(old + got);
        if (r.body.size() > maxBytes) throw std::runtime_error("Playlist is too large.");
        if (got == 0) break;
    }
    return r;
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
