#include "wvptest.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "Ws2_32.lib")

#include "wvp/M3uParser.h"
#include "wvp/PlaylistLoader.h"
#include "wvp/PlaylistStore.h"
#include "wvp/Utf.h"

using namespace wvp;

namespace {

std::vector<std::uint8_t> Bytes(const std::string& s) { return {s.begin(), s.end()}; }

struct TempDir {
    std::wstring path;
    TempDir() {
        wchar_t buf[MAX_PATH + 1];
        GetTempPathW(MAX_PATH, buf);
        path = std::wstring(buf) + L"wvp_pl_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
        CreateDirectoryW(path.c_str(), nullptr);
    }
    ~TempDir() {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((path + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((path + L"\\" + fd.cFileName).c_str()); } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryW(path.c_str());
    }
    std::wstring File(const std::wstring& n) const { return path + L"\\" + n; }
    void Write(const std::wstring& n, const std::vector<std::uint8_t>& data) const {
        HANDLE f = CreateFileW(File(n).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD w = 0;
        if (f != INVALID_HANDLE_VALUE) { WriteFile(f, data.data(), (DWORD)data.size(), &w, nullptr); CloseHandle(f); }
    }
};

HttpFetchFn FakeFetch(std::string body, std::wstring contentType, int* requests, std::wstring finalUrl = L"", bool throwOnCall = false) {
    return [=](const std::wstring& url, const std::function<bool(const std::wstring&)>& want, std::size_t) {
        if (requests) ++*requests;
        if (throwOnCall) throw std::runtime_error("no http expected");
        HttpResponse r;
        r.status = 200;
        r.contentType = contentType;
        r.finalUrl = finalUrl.empty() ? url : finalUrl;
        if (want(contentType)) r.body = Bytes(body);
        return r;
    };
}

class LocalHttpServer {
public:
    LocalHttpServer(std::string body, int status = 200) : body_(std::move(body)), status_(status) {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("WSAStartup failed");
        started_ = true;
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == INVALID_SOCKET) throw std::runtime_error("socket failed");
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR || listen(listener_, SOMAXCONN) == SOCKET_ERROR)
            throw std::runtime_error("bind/listen failed");
        int len = sizeof(addr);
        if (getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len) == SOCKET_ERROR) throw std::runtime_error("getsockname failed");
        port_ = ntohs(addr.sin_port);
        worker_ = std::thread([this] { Serve(); });
    }
    ~LocalHttpServer() {
        stopping_ = true;
        if (listener_ != INVALID_SOCKET) { shutdown(listener_, SD_BOTH); closesocket(listener_); }
        if (worker_.joinable()) worker_.join();
        if (started_) WSACleanup();
    }
    std::wstring Url() const { return L"http://127.0.0.1:" + std::to_wstring(port_) + L"/list.m3u"; }
    void SetResponse(int status, std::string body) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = status;
        body_ = std::move(body);
    }
    int Requests() const { return requests_.load(); }
private:
    void Serve() {
        while (!stopping_) {
            SOCKET client = accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) break;
            char req[2048];
            recv(client, req, sizeof(req), 0);
            std::string body;
            int status;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                body = body_;
                status = status_;
            }
            requests_++;
            const std::string reason = status == 200 ? "OK" : "Service Unavailable";
            std::string response = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n" +
                "Content-Type: application/vnd.apple.mpegurl\r\nContent-Length: " + std::to_string(body.size()) +
                "\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n" + body;
            size_t sent = 0;
            while (sent < response.size()) {
                int n = send(client, response.data() + sent, (int)std::min<size_t>(response.size() - sent, INT_MAX), 0);
                if (n <= 0) break;
                sent += (size_t)n;
            }
            shutdown(client, SD_BOTH);
            closesocket(client);
        }
    }
    SOCKET listener_ = INVALID_SOCKET;
    unsigned short port_ = 0;
    std::string body_;
    int status_ = 200;
    std::mutex mutex_;
    std::atomic<int> requests_{0};
    std::atomic<bool> stopping_{false};
    bool started_ = false;
    std::thread worker_;
};

}

TEST_CASE(M3u_ParsesExtinfTitleAndDurationAndSkipsOtherComments) {
    const std::wstring text = L"#EXTM3U\n#PLAYLIST:My list\n#EXTVLCOPT:foo=bar\n#EXTINF:123.5,Song One\nC:\\music\\one.mp3\n#EXTINF:-1,Live\nhttp://h/x.ts\nplain.mp4\n";
    M3uPlaylist pl = M3uParser::Parse(text, L"D:\\lists\\a.m3u");
    CHECK_EQ(pl.title, std::wstring(L"My list"));
    REQUIRE(pl.items.size() == 3);
    CHECK(pl.items[0] == MediaItem(L"C:\\music\\one.mp3", L"Song One", 123.5));
    CHECK_EQ(pl.items[1].title, std::wstring(L"Live"));
    CHECK(!pl.items[1].duration.has_value());
    CHECK_EQ(pl.items[1].url, std::wstring(L"http://h/x.ts"));
    CHECK_EQ(pl.items[2].url, std::wstring(L"D:\\lists\\plain.mp4"));
    CHECK(pl.items[2].title.empty());
}

TEST_CASE(M3u_ExtInfWithAttributesAndCommaInsideQuotes) {
    M3uPlaylist pl = M3uParser::Parse(L"#EXTINF:-1 tvg-name=\"A, B\" group-title=\"x\",Channel 5\nhttp://h/c5\n");
    REQUIRE(pl.items.size() == 1);
    CHECK_EQ(pl.items[0].title, std::wstring(L"Channel 5"));
}

TEST_CASE(M3u_ResolvesLocalEntriesAgainstPlaylistPath) {
    struct Row { const wchar_t* entry; const wchar_t* expected; };
    const Row rows[] = {
        {L"a/b.mp4", L"D:\\lists\\a\\b.mp4"},
        {L"a\\b.mp4", L"D:\\lists\\a\\b.mp4"},
        {L"..\\x.mp4", L"D:\\x.mp4"},
        {L"./y.mp4", L"D:\\lists\\y.mp4"},
        {L"E:\\abs\\z.mp4", L"E:\\abs\\z.mp4"},
        {L"E:/abs/z.mp4", L"E:\\abs\\z.mp4"},
        {L"\\\\srv\\share\\u.mp4", L"\\\\srv\\share\\u.mp4"},
        {L"file:///E:/with%20space/f.mp4", L"E:\\with space\\f.mp4"},
        {L"\\rooted\\r.mp4", L"D:\\rooted\\r.mp4"},
    };
    for (auto& r : rows) CHECK_EQ(MediaPathResolver::Resolve(r.entry, L"D:\\lists\\a.m3u"), std::wstring(r.expected));
}

TEST_CASE(M3u_ResolvesAgainstHttpBaseAndFileUriBase) {
    CHECK_EQ(MediaPathResolver::Resolve(L"v/a.ts", L"http://h/dir/list.m3u"), std::wstring(L"http://h/dir/v/a.ts"));
    CHECK_EQ(MediaPathResolver::Resolve(L"v\\a.ts", L"http://h/dir/list.m3u"), std::wstring(L"http://h/dir/v/a.ts"));
    CHECK_EQ(MediaPathResolver::Resolve(L"/top.ts", L"http://h/dir/list.m3u"), std::wstring(L"http://h/top.ts"));
    CHECK_EQ(MediaPathResolver::Resolve(L"https://o/x.mp4", L"http://h/list.m3u"), std::wstring(L"https://o/x.mp4"));
    CHECK_EQ(MediaPathResolver::Resolve(L"a.mp4", L"file:///D:/lists/p.m3u"), std::wstring(L"D:\\lists\\a.mp4"));
    CHECK_EQ(MediaPathResolver::Resolve(L"a.mp4", L"\\\\srv\\sh\\dir\\p.m3u"), std::wstring(L"\\\\srv\\sh\\dir\\a.mp4"));
    CHECK_EQ(MediaPathResolver::Resolve(L"../up/a b.ts", L"http://h/dir/sub/list.m3u"), std::wstring(L"http://h/dir/up/a%20b.ts"));
}

TEST_CASE(M3u_HandlesCrlfCrAndBomChar) {
    M3uPlaylist pl = M3uParser::Parse(L"\uFEFF#EXTM3U\r\n#EXTINF:5,A\r\na.mp4\r\n\r\n#EXTINF:6,B\rb.mp4\r");
    REQUIRE(pl.items.size() == 2);
    CHECK_EQ(pl.items[0].title, std::wstring(L"A"));
    CHECK_EQ(pl.items[1].url, std::wstring(L"b.mp4"));
}

TEST_CASE(M3u_DecodeHandlesUtf8WithAndWithoutBomAndUtf16) {
    const std::wstring s = L"#EXTM3U\n#EXTINF:1,한글 Café\nx.mp4\n";
    std::string u8 = ToUtf8(s);
    CHECK(M3uParser::Decode(Bytes(u8)) == s);
    std::vector<std::uint8_t> withBom = {0xEF, 0xBB, 0xBF};
    withBom.insert(withBom.end(), u8.begin(), u8.end());
    CHECK(M3uParser::Decode(withBom) == s);
    std::vector<std::uint8_t> u16 = {0xFF, 0xFE};
    for (wchar_t c : s) { u16.push_back((std::uint8_t)(c & 0xFF)); u16.push_back((std::uint8_t)(c >> 8)); }
    CHECK(M3uParser::Decode(u16) == s);
    std::vector<std::uint8_t> u16be = {0xFE, 0xFF};
    for (wchar_t c : s) { u16be.push_back((std::uint8_t)(c >> 8)); u16be.push_back((std::uint8_t)(c & 0xFF)); }
    CHECK(M3uParser::Decode(u16be) == s);
}

TEST_CASE(M3u_DecodeFallsBackToAnsiForInvalidUtf8) {
    std::vector<std::uint8_t> bytes = {'#', 'E', 0xE9, ' ', 'x'};   // not valid UTF-8
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char*)bytes.data(), (int)bytes.size(), nullptr, 0);
    CHECK_EQ(len, 0);
    int n = MultiByteToWideChar(CP_ACP, 0, (const char*)bytes.data(), (int)bytes.size(), nullptr, 0);
    std::wstring ansi((size_t)n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, (const char*)bytes.data(), (int)bytes.size(), ansi.data(), n);
    CHECK(M3uParser::Decode(bytes, true) == ansi);
    // .m3u8: lenient UTF-8 (replacement char), no ANSI guess
    CHECK(M3uParser::Decode(bytes, false).find(L'\uFFFD') != std::wstring::npos);
}

TEST_CASE(Hls_DetectsHls) {
    CHECK(M3uParser::IsHls(L"#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:6\n#EXTINF:6,\nseg0.ts\n#EXT-X-ENDLIST\n"));
    CHECK(M3uParser::IsHls(L"#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=800000\nlow.m3u8\n"));
    CHECK(M3uParser::IsHls(L"#EXTM3U\n#EXT-X-MEDIA-SEQUENCE:0\n#EXTINF:2,\nseg0.ts\n"));
}

TEST_CASE(Hls_PlainPlaylistIsNotHls) {
    CHECK(!M3uParser::IsHls(L"#EXTM3U\n#EXTINF:10,A\na.mp4\n#EXTINF:10,B\nb.mp4\n"));
}

TEST_CASE(Loader_RemoteHlsMasterWithStreamInfIsAVideo) {
    PlaylistLoader l(FakeFetch("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=800000\nlow.m3u8\n", L"application/vnd.apple.mpegurl", nullptr));
    auto res = l.Load(L"https://h/master.m3u8");
    CHECK(res.isHlsStream);
    CHECK(!res.isPlaylist);
    REQUIRE(res.items.size() == 1);
    CHECK_EQ(res.items[0].url, L"https://h/master.m3u8");
}

TEST_CASE(Loader_RemoteHlsIsASingleItemWithTheUrl) {
    PlaylistLoader l(FakeFetch("#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXTINF:2,\ns0.ts\n#EXT-X-ENDLIST\n", L"application/vnd.apple.mpegurl", nullptr));
    auto res = l.Load(L"https://h/live/index.m3u8");
    CHECK(res.isHlsStream);
    CHECK(!res.isPlaylist);
    REQUIRE(res.items.size() == 1);
    CHECK_EQ(res.items[0].url, std::wstring(L"https://h/live/index.m3u8"));
}

TEST_CASE(Loader_PlaylistOfHlsStreamsIsAPlaylistNotAnHlsManifest) {
    PlaylistLoader l(FakeFetch("#EXTM3U\n#PLAYLIST:Channels\n#EXTINF:-1,News\nchannels/news.m3u8\n",
                               L"application/vnd.apple.mpegurl", nullptr));
    auto res = l.Load(L"https://h/lists/channels.m3u");
    CHECK(res.isPlaylist);
    CHECK(!res.isHlsStream);
    REQUIRE(res.items.size() == 1);
    CHECK_EQ(res.items[0].url, L"https://h/lists/channels/news.m3u8");
    CHECK_EQ(res.title, L"Channels");
}

TEST_CASE(Loader_RemotePlaylistResolvesRelativeEntriesAgainstFinalUrl) {
    PlaylistLoader l(FakeFetch("#EXTM3U\n#EXTINF:5,A\na.mp4\n#EXTINF:6,B\nsub/b.ts\nhttp://other/c.mp4\n", L"audio/x-mpegurl", nullptr));
    auto res = l.Load(L"http://h/dir/list.m3u");
    CHECK(res.isPlaylist);
    REQUIRE(res.items.size() == 3);
    CHECK_EQ(res.items[0].url, std::wstring(L"http://h/dir/a.mp4"));
    CHECK_EQ(res.items[1].url, std::wstring(L"http://h/dir/sub/b.ts"));
    CHECK_EQ(res.items[2].url, std::wstring(L"http://other/c.mp4"));
    CHECK_EQ(res.items[0].title, std::wstring(L"A"));
}

TEST_CASE(Loader_RemoteRedirectedFinalUrlIsTheResolveBase) {
    PlaylistLoader l(FakeFetch("#EXTM3U\na.mp4\n", L"audio/x-mpegurl", nullptr, L"http://cdn/x/y/list.m3u"));
    auto res = l.Load(L"http://h/dir/list.m3u");
    REQUIRE(res.items.size() == 1);
    CHECK_EQ(res.items[0].url, std::wstring(L"http://cdn/x/y/a.mp4"));
}

TEST_CASE(Loader_RemoteUnknownExtensionWithPlaylistContentTypeIsSniffed) {
    PlaylistLoader l(FakeFetch("#EXTM3U\n#EXTINF:5,A\na.mp4\n", L"application/x-mpegURL", nullptr));
    auto res = l.Load(L"http://h/get?id=7");
    CHECK(res.isPlaylist);
    REQUIRE(res.items.size() == 1);
    CHECK_EQ(res.items[0].url, std::wstring(L"http://h/a.mp4"));
}

TEST_CASE(Loader_RemoteMediaFileIsNotDownloaded) {
    bool bodyRequested = false;
    PlaylistLoader l([&](const std::wstring& url, const std::function<bool(const std::wstring&)>& want, std::size_t) {
        HttpResponse r; r.status = 200; r.contentType = L"video/mp4"; r.finalUrl = url;
        bodyRequested = want(r.contentType);
        return r;
    });
    auto res = l.Load(L"http://h/v/movie.mp4");
    CHECK(!bodyRequested);
    CHECK(!res.isPlaylist);
    CHECK(!res.isHlsStream);
    REQUIRE(res.items.size() == 1);
    CHECK_EQ(res.items[0].url, std::wstring(L"http://h/v/movie.mp4"));
}

TEST_CASE(Loader_OtherProtocolsPassStraightThroughWithoutHttp) {
    int requests = 0;
    PlaylistLoader l(FakeFetch("", L"", &requests, L"", true));
    auto res = l.Load(L"rtmp://h/app/stream");
    REQUIRE(res.items.size() == 1);
    CHECK_EQ(res.items[0].url, std::wstring(L"rtmp://h/app/stream"));
    CHECK_EQ(requests, 0);
}

TEST_CASE(Loader_HttpErrorStatusThrows) {
    PlaylistLoader l([](const std::wstring& url, const std::function<bool(const std::wstring&)>&, std::size_t) {
        HttpResponse r; r.status = 404; r.finalUrl = url; return r;
    });
    bool threw = false;
    try { l.Load(L"http://h/list.m3u"); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

TEST_CASE(Loader_LocalPlaylistAnsiHlsAndPlainMedia) {
    TempDir t;
    // a.m3u: UTF-8 with BOM
    std::vector<std::uint8_t> a = {0xEF, 0xBB, 0xBF};
    std::string at = "#EXTM3U\r\n#EXTINF:3,One\r\none.mp4\r\n";
    a.insert(a.end(), at.begin(), at.end());
    t.Write(L"a.m3u", a);
    t.Write(L"b.m3u", Bytes("#EXTM3U\n#EXTINF:3,Two\ntwo.mp4\n"));
    t.Write(L"h.m3u8", Bytes("#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXTINF:2,\ns.ts\n"));
    int requests = 0;
    PlaylistLoader loader(FakeFetch("", L"", &requests, L"", true));

    auto ra = loader.Load(t.File(L"a.m3u"));
    CHECK(ra.isPlaylist);
    REQUIRE(ra.items.size() == 1);
    CHECK_EQ(ra.items[0].url, t.File(L"one.mp4"));

    std::wstring uri = L"file:///" + t.File(L"b.m3u");
    for (auto& c : uri) if (c == L'\\') c = L'/';
    auto rb = loader.Load(uri);
    REQUIRE(rb.items.size() == 1);
    CHECK_EQ(rb.items[0].url, t.File(L"two.mp4"));

    auto rh = loader.Load(t.File(L"h.m3u8"));
    CHECK(rh.isHlsStream);
    REQUIRE(rh.items.size() == 1);
    CHECK_EQ(rh.items[0].url, t.File(L"h.m3u8"));

    auto rm = loader.Load(t.File(L"video.mkv"));
    CHECK(!rm.isPlaylist);
    REQUIRE(rm.items.size() == 1);
    CHECK_EQ(rm.items[0].url, t.File(L"video.mkv"));
    CHECK_EQ(requests, 0);
}

TEST_CASE(Loader_EmptyInputGivesEmptyResult) {
    PlaylistLoader l;
    auto r = l.Load(L"   ");
    CHECK(r.items.empty());
    CHECK(!r.isPlaylist);
}

TEST_CASE(ReferenceStore_refetches_each_time_and_uses_offline_cache_from_test_server) {
    TempDir t;
    LocalHttpServer server("#EXTM3U\n#EXTINF:10,First\nfirst.mp4\n");
    PlaylistStore store(t.path);
    const std::wstring source = server.Url();
    const std::wstring name = store.AddOrUpdateReference(source, L"Loopback", {MediaItem(L"cached-before.mp4")}, true);
    REQUIRE(!name.empty());

    auto first = store.RefreshReference(name, PlaylistLoader());
    CHECK(first.refreshed);
    REQUIRE(first.items.size() == 1);
    CHECK_EQ(first.items[0].title, L"First");
    std::wstring mediaBase = source.substr(0, source.find_last_of(L'/') + 1);
    CHECK_EQ(first.items[0].url, mediaBase + L"first.mp4");

    server.SetResponse(200, "#EXTM3U\n#EXTINF:20,Second\nsecond.ts\n");
    auto second = store.RefreshReference(name, PlaylistLoader());
    CHECK(second.refreshed);
    REQUIRE(second.items.size() == 1);
    CHECK_EQ(second.items[0].title, L"Second");
    CHECK(second.items[0].url.find(L"second.ts") != std::wstring::npos);

    server.SetResponse(200, "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=800000\nlow.m3u8\n");
    auto changedToHls = store.RefreshReference(name, PlaylistLoader());
    CHECK(!changedToHls.refreshed);
    CHECK(changedToHls.isHlsStream);
    CHECK(changedToHls.usedOfflineFallback);
    CHECK_EQ(store.Load(name)[0].title, L"Second");  // HLS must not replace the playlist cache

    server.SetResponse(503, "offline");
    auto offline = store.RefreshReference(name, PlaylistLoader());
    CHECK(!offline.refreshed);
    CHECK(offline.usedOfflineFallback);
    CHECK(!offline.error.empty());
    REQUIRE(offline.items.size() == 1);
    CHECK_EQ(offline.items[0].title, L"Second");
    CHECK_EQ(store.Load(name)[0].title, L"Second");
    CHECK_EQ(server.Requests(), 4);
}

// WinHTTP end-to-end against a local server: set WVP_TEST_HTTP_PORT (e.g. python -m http.server serving a dir
// that contains list.m3u, hls.m3u8). Skipped (passes trivially) when the variable is absent.
TEST_CASE_INTEGRATION(Loader_WinHttpAgainstLocalServer) {
    char* port = nullptr; size_t n = 0;
    _dupenv_s(&port, &n, "WVP_TEST_HTTP_PORT");
    if (!port) { std::printf("    (skipped: WVP_TEST_HTTP_PORT not set)\n"); return; }
    std::wstring base = L"http://127.0.0.1:" + ToWide(port) + L"/";
    free(port);
    PlaylistLoader l;
    auto pl = l.Load(base + L"list.m3u");
    CHECK(pl.isPlaylist);
    REQUIRE(pl.items.size() == 2);
    CHECK_EQ(pl.items[0].url, base + L"a.mp4");
    CHECK_EQ(pl.items[0].title, std::wstring(L"First"));
    auto hls = l.Load(base + L"hls.m3u8");
    CHECK(hls.isHlsStream);
    auto media = l.Load(base + L"clip.mp4");
    CHECK(!media.isPlaylist);
    bool threw = false;
    try { l.Load(base + L"missing.m3u"); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}
