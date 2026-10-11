#pragma once
// HTTP(S) GET over libcurl.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace wvp {

struct HttpResponse {
    int status = 0;                       // HTTP status code
    std::string contentType;              // full Content-Type header value ("" if none)
    std::string finalUrl;                 // URL after redirects
    std::vector<std::uint8_t> body;       // only filled when wantBody returned true
};

/// Fetch hook (injectable for tests). Must call wantBody(contentType) after the headers of a 2xx response arrive and
/// only download the body (at most maxBytes, else throw std::runtime_error) when it returns true.
/// Throws std::runtime_error on transport errors.
using HttpFetchFn = std::function<HttpResponse(const std::string& url,
                                               const std::function<bool(const std::string& contentType)>& wantBody,
                                               std::size_t maxBytes)>;

/// The default HttpFetchFn: follows redirects (finalUrl is the last one), 30 s connect timeout, aborts after 30 s
/// without data, verifies TLS certificates. The proxy is platform::SystemProxyFor's choice.
HttpResponse HttpFetch(const std::string& url, const std::function<bool(const std::string& contentType)>& wantBody,
                       std::size_t maxBytes);

}
