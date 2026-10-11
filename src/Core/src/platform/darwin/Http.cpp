#include "wvp/PlaylistLoader.h"

#include <stdexcept>

namespace wvp {

// No default transport outside Windows yet: the app host injects one (e.g. NSURLSession) through PlaylistLoader's
// HttpFetchFn, and the unit tests use fakes.
HttpResponse WinHttpFetch(const std::wstring&, const std::function<bool(const std::wstring&)>&, std::size_t) {
    throw std::runtime_error("HTTP playlist loading is not available on this platform.");
}

}
