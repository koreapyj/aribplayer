#include "wvp/Utf.h"

#include <cstdint>

namespace wvp {

namespace {

// Handles both wchar_t widths so this file also serves a UTF-16 wchar_t toolchain (e.g. MinGW) unchanged.
constexpr bool kWideIs16 = sizeof(wchar_t) == 2;

void AppendUtf8(std::string& s, std::uint32_t c) {
    if (c < 0x80) s += (char)c;
    else if (c < 0x800) { s += (char)(0xC0 | (c >> 6)); s += (char)(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { s += (char)(0xE0 | (c >> 12)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
    else { s += (char)(0xF0 | (c >> 18)); s += (char)(0x80 | ((c >> 12) & 0x3F)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
}

void AppendWide(std::wstring& w, std::uint32_t c) {
    if (kWideIs16 && c >= 0x10000) {
        c -= 0x10000;
        w += (wchar_t)(0xD800 + (c >> 10));
        w += (wchar_t)(0xDC00 + (c & 0x3FF));
    } else {
        w += (wchar_t)c;
    }
}

/// Decodes the sequence at u[i] and advances i. false = malformed (i advanced by one byte, like MultiByteToWideChar's replacement).
bool NextUtf8(std::string_view u, size_t& i, std::uint32_t& c) {
    unsigned char b = (unsigned char)u[i];
    int n; std::uint32_t min;
    if (b < 0x80) { c = b; i++; return true; }
    else if ((b & 0xE0) == 0xC0) { c = b & 0x1F; n = 1; min = 0x80; }
    else if ((b & 0xF0) == 0xE0) { c = b & 0x0F; n = 2; min = 0x800; }
    else if ((b & 0xF8) == 0xF0) { c = b & 0x07; n = 3; min = 0x10000; }
    else { i++; return false; }
    for (int k = 1; k <= n; k++) {
        if (i + k >= u.size() || ((unsigned char)u[i + k] & 0xC0) != 0x80) { i++; return false; }
        c = (c << 6) | ((unsigned char)u[i + k] & 0x3F);
    }
    i += n + 1;
    return c >= min && c <= 0x10FFFF && !(c >= 0xD800 && c <= 0xDFFF);
}

}

std::string ToUtf8(std::wstring_view w) {
    std::string s;
    s.reserve(w.size());
    for (size_t i = 0; i < w.size(); i++) {
        std::uint32_t c = (std::uint32_t)w[i];
        if (kWideIs16 && c >= 0xD800 && c <= 0xDBFF && i + 1 < w.size()
            && (std::uint32_t)w[i + 1] >= 0xDC00 && (std::uint32_t)w[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + ((std::uint32_t)w[i + 1] - 0xDC00);
            i++;
        } else if ((c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFF) {
            c = 0xFFFD;
        }
        AppendUtf8(s, c);
    }
    return s;
}

std::wstring ToWide(std::string_view u) {
    std::wstring w;
    w.reserve(u.size());
    for (size_t i = 0; i < u.size();) {
        std::uint32_t c;
        if (!NextUtf8(u, i, c)) c = 0xFFFD;
        AppendWide(w, c);
    }
    return w;
}

bool IsValidUtf8(std::string_view u) {
    for (size_t i = 0; i < u.size();) {
        std::uint32_t c;
        if (!NextUtf8(u, i, c)) return false;
    }
    return true;
}

std::optional<std::wstring> FromSystemCodePage(std::string_view) { return std::nullopt; }

}
