#pragma once
// A loopback HTTP server that answers every request with one playlist response; the response can change between requests.
#include <algorithm>
#include <atomic>
#include <climits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace wvptest {

class LocalHttpServer {
public:
    LocalHttpServer(std::string body, int status = 200) : body_(std::move(body)), status_(status) {
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("WSAStartup failed");
        started_ = true;
#endif
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == kInvalid) throw std::runtime_error("socket failed");
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listener_, SOMAXCONN) != 0)
            throw std::runtime_error("bind/listen failed");
        SockLen len = sizeof(addr);
        if (getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) throw std::runtime_error("getsockname failed");
        port_ = ntohs(addr.sin_port);
        worker_ = std::thread([this] { Serve(); });
    }
    ~LocalHttpServer() {
        stopping_ = true;
#ifdef _WIN32
        if (listener_ != kInvalid) {
            shutdown(listener_, SD_BOTH);
            Close(listener_);
        }
        if (worker_.joinable()) worker_.join();
        if (started_) WSACleanup();
#else
        if (listener_ != kInvalid) shutdown(listener_, SHUT_RDWR);
        if (worker_.joinable()) worker_.join();
        if (listener_ != kInvalid) Close(listener_);
#endif
    }
    LocalHttpServer(const LocalHttpServer&) = delete;
    LocalHttpServer& operator=(const LocalHttpServer&) = delete;

    std::string Url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/list.m3u"; }
    void SetResponse(int status, std::string body) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = status;
        body_ = std::move(body);
    }
    int Requests() const { return requests_.load(); }

private:
#ifdef _WIN32
    using Socket = SOCKET;
    using SockLen = int;
    static constexpr Socket kInvalid = INVALID_SOCKET;
    static void Close(Socket s) { closesocket(s); }
#else
    using Socket = int;
    using SockLen = socklen_t;
    static constexpr Socket kInvalid = -1;
    static void Close(Socket s) { close(s); }
#endif

    void Serve() {
        while (!stopping_) {
            Socket client = accept(listener_, nullptr, nullptr);
            if (client == kInvalid) break;
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
                auto n = send(client, response.data() + sent, static_cast<int>(std::min<size_t>(response.size() - sent, INT_MAX)), 0);
                if (n <= 0) break;
                sent += static_cast<size_t>(n);
            }
#ifdef _WIN32
            shutdown(client, SD_BOTH);
#else
            shutdown(client, SHUT_RDWR);
#endif
            Close(client);
        }
    }

    Socket listener_ = kInvalid;
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
