#include "wvp/Utf.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace wvp {

namespace {

std::optional<std::wstring> MbToWide(UINT cp, std::string_view u, DWORD flags) {
    if (u.empty()) return std::wstring();
    int n = MultiByteToWideChar(cp, flags, u.data(), (int)u.size(), nullptr, 0);
    if (n <= 0) return std::nullopt;
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(cp, flags, u.data(), (int)u.size(), w.data(), n);
    return w;
}

}

std::string ToUtf8(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring ToWide(std::string_view u) { return MbToWide(CP_UTF8, u, 0).value_or(std::wstring()); }

bool IsValidUtf8(std::string_view u) { return MbToWide(CP_UTF8, u, MB_ERR_INVALID_CHARS).has_value(); }

std::optional<std::wstring> FromSystemCodePage(std::string_view bytes) { return MbToWide(CP_ACP, bytes, 0); }

}
