#pragma once
// Single-instance support (named mutex + WM_COPYDATA).
// Main.cpp calls ForwardToExistingInstance before creating the window; the App calls RegisterPrimaryInstance after
// creating it and HandleCopyData from WM_COPYDATA.
#include <windows.h>

#include <string>
#include <vector>

namespace app {
class AppContext;

/// dwData of the WM_COPYDATA command-line message.
constexpr ULONG_PTR kCopyDataMagic = 0x41525031;   // 'ARP1'

/// true = another instance exists and received the args (the caller exits). Otherwise this process claims the
/// per-session mutex "Local\aribplayer.SingleInstance" and returns false. Options (--x) are not forwarded.
bool ForwardToExistingInstance(const std::vector<std::wstring>& args);
/// Keeps the mutex for the process lifetime (claims it when ForwardToExistingInstance did not run). No-op for
/// --headless-check runs.
void RegisterPrimaryInstance(HWND mainWindow);
/// Returns true when the message was a forwarded command line and was handled.
bool HandleCopyData(AppContext& ctx, const COPYDATASTRUCT* cds);

struct ForwardedCommandLine {
    std::wstring cwd;
    std::vector<std::wstring> args;
};
/// UTF-16 payload: cwd, then each arg, every field terminated by '\0'.
std::wstring EncodeCommandLine(const std::wstring& cwd, const std::vector<std::wstring>& args);
/// False when the payload is malformed (null, odd byte size, empty).
bool DecodeCommandLine(const void* data, size_t byteCount, ForwardedCommandLine& out);
/// Makes relative file paths absolute against cwd; URLs (scheme://) and rooted paths are kept; empty args dropped.
std::vector<std::wstring> ResolveArgs(const std::wstring& cwd, const std::vector<std::wstring>& args);
}
