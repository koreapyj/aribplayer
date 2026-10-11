#include "wvp/Utf8.h"

namespace wvp::utf8 {

namespace {

char32_t Decode(std::string_view s, std::size_t& i, bool& ok) {
    ok = false;
    const unsigned char b0 = (unsigned char)s[i];
    if (b0 < 0x80) { ++i; ok = true; return b0; }
    std::size_t len;
    char32_t c, min;
    if (b0 >= 0xC2 && b0 <= 0xDF) { len = 2; c = b0 & 0x1F; min = 0x80; }
    else if (b0 >= 0xE0 && b0 <= 0xEF) { len = 3; c = b0 & 0x0F; min = 0x800; }
    else if (b0 >= 0xF0 && b0 <= 0xF4) { len = 4; c = b0 & 0x07; min = 0x10000; }
    else { ++i; return kReplacement; }
    if (s.size() - i < len) { ++i; return kReplacement; }
    for (std::size_t k = 1; k < len; ++k) {
        const unsigned char b = (unsigned char)s[i + k];
        if ((b & 0xC0) != 0x80) { ++i; return kReplacement; }
        c = (c << 6) | (b & 0x3F);
    }
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) { ++i; return kReplacement; }
    i += len;
    ok = true;
    return c;
}

}

char32_t Next(std::string_view s, std::size_t& i) {
    bool ok;
    return Decode(s, i, ok);
}

bool IsValid(std::string_view s) {
    bool ok = true;
    for (std::size_t i = 0; ok && i < s.size();) Decode(s, i, ok);
    return ok;
}

void Append(std::string& out, char32_t c) {
    if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) c = kReplacement;
    if (c < 0x80) out.push_back((char)c);
    else if (c < 0x800) {
        out.push_back((char)(0xC0 | (c >> 6)));
        out.push_back((char)(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        out.push_back((char)(0xE0 | (c >> 12)));
        out.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (c & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (c >> 18)));
        out.push_back((char)(0x80 | ((c >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (c & 0x3F)));
    }
}

std::string Lossy(std::string_view s) {
    if (IsValid(s)) return std::string(s);
    std::string out;
    out.reserve(s.size() + 8);
    for (std::size_t i = 0; i < s.size();) Append(out, Next(s, i));
    return out;
}

std::string FromLatin1(std::string_view bytes) {
    std::string out;
    out.reserve(bytes.size());
    for (unsigned char b : bytes) Append(out, b);
    return out;
}

std::string FromUtf16(const std::uint8_t* b, std::size_t size, bool bigEndian) {
    const std::size_t n = size / 2;
    auto unit = [&](std::size_t k) -> char32_t {
        return bigEndian ? (char32_t)((b[2 * k] << 8) | b[2 * k + 1]) : (char32_t)(b[2 * k] | (b[2 * k + 1] << 8));
    };
    std::string out;
    out.reserve(n);
    for (std::size_t k = 0; k < n; ++k) {
        char32_t c = unit(k);
        if (c >= 0xD800 && c <= 0xDBFF && k + 1 < n) {
            const char32_t lo = unit(k + 1);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                ++k;
            }
        }
        Append(out, c);
    }
    return out;
}

std::size_t Utf16Length(std::string_view s) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size();) n += Next(s, i) >= 0x10000 ? 2 : 1;
    return n;
}

std::string ToLowerAscii(std::string_view s) {
    std::string out(s);
    for (auto& c : out) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return out;
}

bool IsSpace(char32_t c) {
    return (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0x85 || c == 0xA0 || c == 0x1680 || c == 0x180E
        || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

std::string_view Trim(std::string_view s) {
    std::size_t b = 0;
    while (b < s.size()) {
        std::size_t i = b;
        if (!IsSpace(Next(s, i))) break;
        b = i;
    }
    std::size_t e = s.size();
    while (e > b) {
        std::size_t start = e - 1;   // back up to the lead byte of the last code point
        while (start > b && ((unsigned char)s[start] & 0xC0) == 0x80 && e - start < 4) --start;
        std::size_t i = start;
        const char32_t c = Next(s, i);
        if (i != e || !IsSpace(c)) break;
        e = start;
    }
    return s.substr(b, e - b);
}

}
