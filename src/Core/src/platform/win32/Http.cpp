#include "wvp/PlaylistLoader.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

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

}
