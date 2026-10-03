#pragma once
// Pure formatting of the open options for logs / the debug overlay, and the equivalent ffplay.exe command line.
// No Win32 calls; tested by --logictest (DebugTests.cpp).
//
// ffplay.exe flags:
//   value options: -vf -af -ss -t -hwaccel -volume -loop -sync -codec -vcodec -acodec -scodec -f -x -y -ast -vst -sst -drp
//                  -lowres -filter_threads -vulkan_params -seek_interval -bytes -window_title -showmode -left -top ...
//   boolean flags: -stats -fast -genpts -framedrop -infbuf -autoexit -an -vn -sn -nodisp -noborder -fs -autorotate
//                  -enable_vulkan -find_stream_info -alwaysontop -exitonkeydown -exitonmousedown (false = -no<name>)
//   libffplay only (no CLI flag): filter_hw_device (ffplay.exe has no -filter_hw_device; libffplay creates the
//                  "type[:device]" hw device and attaches it to every video filter).
//   anything else is an AVFormat/AVCodec/protocol option (user_agent, headers, timeout, ...): ffplay.exe accepts those
//   as -<key> <value> too.
#include <string>

#include "wvp/IMediaPlayer.h"

namespace app::log {

/// "key=value key=value ..." (values containing blanks or quotes are double-quoted). Empty options -> "(none)".
std::wstring FormatOptionsLine(const wvp::OpenOptions& options);

/// Quotes `v` for cmd.exe and PowerShell (double quotes, CommandLineToArgv escaping for embedded quotes/backslashes).
/// `force` quotes even a value without special characters. Values containing '$' or a back-tick still need manual
/// escaping in PowerShell (documented in docs/debugging.md).
std::wstring QuoteArg(const std::wstring& v, bool force = false);

enum class CliKind { Value, Flag, NoCli };
CliKind ClassifyOption(const std::wstring& key);

/// The open options with the live filter strings applied: `vf` / `af` replaced by the latest FilterChange strings (removed
/// when empty), every other option (hwaccel, ...) unchanged and in place. Reproduces what the player runs right now.
wvp::OpenOptions EffectiveOptions(const wvp::OpenOptions& open, const std::wstring& vf, const std::wstring& af);

/// `ffplay.exe [-flags] "<url>"` with every option mapped to the flag ffplay.exe accepts; options without a CLI
/// equivalent are appended as " # no CLI equivalent: key=value" (a comment in PowerShell; delete it in cmd).
std::wstring FfplayCommandLine(const std::wstring& url, const wvp::OpenOptions& options, const std::wstring& exe = L"ffplay.exe");

}
