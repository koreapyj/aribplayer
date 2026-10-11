#pragma once
// UTF-8 <-> wchar_t. wchar_t is UTF-16 on Windows and UTF-32 elsewhere; the per-platform implementation lives in
// src/platform/<os>/Utf.cpp so callers never see that difference.
#include <optional>
#include <string>
#include <string_view>

namespace wvp {

/// Lossy: invalid sequences become U+FFFD.
std::string ToUtf8(std::wstring_view w);
std::wstring ToWide(std::string_view u);

/// true when u is well-formed UTF-8.
bool IsValidUtf8(std::string_view u);

/// Decodes bytes in the system ANSI code page (CP_ACP). nullopt where no such code page exists (non-Windows) or on failure.
std::optional<std::wstring> FromSystemCodePage(std::string_view bytes);

}
