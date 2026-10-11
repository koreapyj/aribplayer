#pragma once
// Pure formatting of the open options for logs / the debug overlay. No Win32 calls; tested by --logictest (DebugTests.cpp).
#include <string>

#include "wvp/IMediaPlayer.h"

namespace app::log {

/// "key=value key=value ..." (values containing blanks or quotes are double-quoted). Empty options -> "(none)".
std::string FormatOptionsLine(const wvp::OpenOptions& options);

/// Quotes `v` (double quotes, CommandLineToArgv escaping for embedded quotes/backslashes). `force` quotes even a value
/// without special characters.
std::string QuoteArg(const std::string& v, bool force = false);

/// The open options with the live filter strings applied: `vf` / `af` replaced by the latest FilterChange strings (removed
/// when empty), every other option (hwaccel, ...) unchanged and in place. Reproduces what the player runs right now.
wvp::OpenOptions EffectiveOptions(const wvp::OpenOptions& open, const std::string& vf, const std::string& af);

}
