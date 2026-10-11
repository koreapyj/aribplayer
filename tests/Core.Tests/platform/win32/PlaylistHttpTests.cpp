// WinHTTP end-to-end: PlaylistLoader's default transport against a loopback server. Windows only (Winsock + WinHTTP);
// the transport-independent loader tests live in tests/PlaylistTests.cpp with a fake fetch.
#include "wvptest.h"
#include "TempDir.h"

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

#pragma comment(lib, "Ws2_32.lib")

#include "wvp/PlaylistLoader.h"
#include "wvp/PlaylistStore.h"
#include "wvp/Utf.h"

using namespace wvp;
using wvptest::TempDir;

namespace {

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
