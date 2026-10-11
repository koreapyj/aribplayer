#pragma once
// UTF-8 text helpers (no OS calls).
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace wvp::utf8 {

inline constexpr char32_t kReplacement = 0xFFFD;

/// Strict: no overlong forms, surrogates or code points above U+10FFFF.
bool IsValid(std::string_view s);
/// Decodes the code point at s[i] and advances i past it. An invalid or truncated sequence yields U+FFFD and advances
/// by one byte. Requires i < s.size().
char32_t Next(std::string_view s, std::size_t& i);
void Append(std::string& out, char32_t c);
/// Invalid sequences replaced with U+FFFD.
std::string Lossy(std::string_view s);
std::string FromLatin1(std::string_view bytes);
/// UTF-16 bytes (no BOM handling) to UTF-8; unpaired surrogates become U+FFFD, a trailing odd byte is dropped.
std::string FromUtf16(const std::uint8_t* bytes, std::size_t size, bool bigEndian);
/// Length in UTF-16 code units.
std::size_t Utf16Length(std::string_view s);

/// Only ASCII letters change (what the C locale's towlower does).
std::string ToLowerAscii(std::string_view s);

/// Unicode white space (the characters the Windows CRT's iswspace accepts).
bool IsSpace(char32_t c);
/// Strips IsSpace code points from both ends.
std::string_view Trim(std::string_view s);

}
