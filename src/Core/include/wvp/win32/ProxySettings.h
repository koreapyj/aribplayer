#pragma once
#include <string>
#include <string_view>

namespace wvp::platform::win32 {

/// The proxy for a request to `host` over `scheme` from Windows' manual proxy settings: `server` is the lpszProxy of
/// WinHttpGetIEProxyConfigForCurrentUser ("host:port" for every scheme, or "http=host:port;https=host:port;socks=host:port")
/// and `bypass` its lpszProxyBypass (';'-separated host patterns with '*', and "<local>" for host names without a dot).
/// "" = connect directly.
std::string ProxyFromSettings(std::string_view server, std::string_view bypass, std::string_view scheme,
                              std::string_view host);

}
