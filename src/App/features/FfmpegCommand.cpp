#include "features/FfmpegCommand.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "wvp/MediaUri.h"
#include "wvp/win32/Utf.h"

namespace app {

namespace {
// Floor, never round: ffmpeg keeps the first frame with pts >= target, so rounding up would skip the frame. The epsilon
// only absorbs binary error of values such as 10.01 * 1000 = 10009.999...
long long FloorMs(double seconds) { return (long long)std::floor(seconds * 1000.0 + 1e-6); }

std::wstring Quote(const std::wstring& s, bool posix) {
    if (!posix) return L"\"" + s + L"\"";
    std::wstring q = L"'";
    for (wchar_t c : s) {
        if (c == L'\'') q += L"'\''";
        else q += c;
    }
    return q + L"'";
}
}

std::wstring FfmpegFrameCommandLine(const std::wstring& input, const wvp::FrameSeekPoint& p, bool posixQuote) {
    const long long inMs = std::max(0LL, FloorMs(p.Key));
    const long long outMs = std::max(0LL, FloorMs(p.Frame) - inMs);
    wchar_t in[48], out[32];
    std::swprintf(in, std::size(in), L"%02lld:%02lld:%02lld.%03lld", inMs / 3600000, inMs / 60000 % 60, inMs / 1000 % 60, inMs % 1000);
    std::swprintf(out, std::size(out), L"%lld.%03lld", outMs / 1000, outMs % 1000);
    return std::wstring(L"ffmpeg -ss ") + in + L" -i " + Quote(input, posixQuote) + L" -ss " + out;
}

std::wstring FfmpegInputOf(const wvp::MediaItem* item) {
    if (!item) return {};
    return wvp::ToWide(wvp::MediaUri::Parse(item->url).FfmpegInput());
}

}
