#include "wvp/HttpFetch.h"
#include "wvp/Platform.h"

#include <curl/curl.h>

#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace wvp {

namespace {

struct Transfer {
    CURL* curl = nullptr;
    const std::function<bool(const std::string&)>* wantBody = nullptr;
    std::size_t maxBytes = 0;
    HttpResponse response;
    bool headersSeen = false;
    bool stopped = false;     // the body was declined (or the status is not 2xx): the transfer was aborted on purpose
    bool tooLarge = false;
    std::exception_ptr failure;   // thrown by wantBody
};

void ReadHeaders(Transfer& t) {
    t.headersSeen = true;
    long status = 0;
    curl_easy_getinfo(t.curl, CURLINFO_RESPONSE_CODE, &status);
    t.response.status = (int)status;
    const char* ct = nullptr;
    if (curl_easy_getinfo(t.curl, CURLINFO_CONTENT_TYPE, &ct) == CURLE_OK && ct) t.response.contentType = ct;
    const char* url = nullptr;
    if (curl_easy_getinfo(t.curl, CURLINFO_EFFECTIVE_URL, &url) == CURLE_OK && url) t.response.finalUrl = url;
}

bool Accept(Transfer& t) {
    ReadHeaders(t);
    if (t.response.status < 200 || t.response.status > 299) return false;
    return (*t.wantBody)(t.response.contentType);
}

size_t OnBody(char* data, size_t size, size_t count, void* user) {
    auto& t = *static_cast<Transfer*>(user);
    const size_t n = size * count;
    if (!t.headersSeen) {
        bool want = false;
        try { want = Accept(t); } catch (...) { t.failure = std::current_exception(); }
        if (!want) { t.stopped = true; return 0; }
    }
    if (t.response.body.size() + n > t.maxBytes) { t.tooLarge = true; return 0; }
    t.response.body.insert(t.response.body.end(), (const std::uint8_t*)data, (const std::uint8_t*)data + n);
    return n;
}

// curl rejects a URL with spaces and would send non-ASCII bytes raw: percent-encode both (existing %XX escapes stay).
std::string EscapeUrl(const std::string& url) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : url) {
        if (c <= 0x20 || c >= 0x7F) {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        } else {
            out += (char)c;
        }
    }
    return out;
}

std::optional<std::string> ProxyFor(const std::string& url) {
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> u(curl_url(), &curl_url_cleanup);
    if (!u || curl_url_set(u.get(), CURLUPART_URL, url.c_str(), 0) != CURLUE_OK) return std::nullopt;
    char* scheme = nullptr;
    char* host = nullptr;
    std::optional<std::string> proxy;
    if (curl_url_get(u.get(), CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
        curl_url_get(u.get(), CURLUPART_HOST, &host, 0) == CURLUE_OK)
        proxy = platform::SystemProxyFor(scheme, host);
    curl_free(scheme);
    curl_free(host);
    return proxy;
}

void GlobalInit() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) throw std::runtime_error("curl_global_init failed");
    });
}

}

HttpResponse HttpFetch(const std::string& url, const std::function<bool(const std::string&)>& wantBody, std::size_t maxBytes) {
    GlobalInit();
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) throw std::runtime_error("curl_easy_init failed");
    Transfer t;
    t.curl = curl.get();
    t.wantBody = &wantBody;
    t.maxBytes = maxBytes;
    char error[CURL_ERROR_SIZE] = {};

    CURL* c = curl.get();
    const std::string escaped = EscapeUrl(url);
    curl_easy_setopt(c, CURLOPT_URL, escaped.c_str());
    // Chosen for the requested host; a redirect to another host keeps it.
    const std::optional<std::string> proxy = ProxyFor(escaped);
    if (proxy) curl_easy_setopt(c, CURLOPT_PROXY, proxy->c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, "aribplayer/1.0");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, error);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, &OnBody);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &t);

    CURLcode rc = curl_easy_perform(c);
    if (t.failure) std::rethrow_exception(t.failure);
    if (t.tooLarge) throw std::runtime_error("Playlist is too large.");
    if (rc != CURLE_OK && !(rc == CURLE_WRITE_ERROR && t.stopped))
        throw std::runtime_error(std::string("HTTP request failed: ") + (error[0] ? error : curl_easy_strerror(rc)));
    if (!t.headersSeen) Accept(t);   // empty body: the write callback never ran
    if (t.response.finalUrl.empty()) t.response.finalUrl = url;
    return t.response;
}

}
