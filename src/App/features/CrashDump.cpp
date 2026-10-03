#include "features/CrashDump.h"

#include <windows.h>
#include <dbghelp.h>
#include <shlobj.h>

#include <csignal>
#include <cstdlib>
#include <exception>
#include <string>

#include "features/Logging.h"

#pragma comment(lib, "dbghelp.lib")

namespace app::crash {
namespace {
LONG g_inFilter = 0;
volatile bool g_clean = false;

void Write(EXCEPTION_POINTERS* ep, const char* what) {
    if (InterlockedExchange(&g_inFilter, 1)) return;   // a fault inside the handler itself
    wchar_t base[MAX_PATH] = {};
    std::wstring path;
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, base))) {
        std::wstring dir = std::wstring(base) + L"\\aribplayer";
        CreateDirectoryW(dir.c_str(), nullptr);
        dir += L"\\crashes";
        CreateDirectoryW(dir.c_str(), nullptr);
        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t name[96];
        swprintf_s(name, L"\\aribplayer-%04u%02u%02u-%02u%02u%02u-%lu.dmp", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                   GetCurrentProcessId());
        path = dir + name;
    }
    bool written = false;
    if (!path.empty()) {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
            written = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                                        (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory | MiniDumpWithThreadInfo |
                                                        MiniDumpWithUnloadedModules),
                                        ep ? &mei : nullptr, nullptr, nullptr) != FALSE;
            CloseHandle(f);
        }
    }
    char buf[96] = {};
    if (ep && ep->ExceptionRecord)
        sprintf_s(buf, " 0x%08lX at %p", ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    std::string narrow;
    if (!path.empty()) {
        int n = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), (int)path.size(), nullptr, 0, nullptr, nullptr);
        narrow.resize((size_t)n);
        WideCharToMultiByte(CP_UTF8, 0, path.c_str(), (int)path.size(), narrow.data(), n, nullptr, nullptr);
    }
    log::Event("crash", std::string(what) + buf + (written ? ", minidump " + narrow : std::string(", minidump not written")), log::kFatal);
}

LONG WINAPI Filter(EXCEPTION_POINTERS* ep) {
    Write(ep, "unhandled exception");
    return EXCEPTION_CONTINUE_SEARCH;   // WER / LocalDumps still see it, the process ends as before
}

void Terminate() { Write(nullptr, "std::terminate (uncaught C++ exception)"); std::abort(); }
void InvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) { Write(nullptr, "invalid CRT parameter"); std::abort(); }
void PureCall() { Write(nullptr, "pure virtual call"); std::abort(); }
void AbortSignal(int) { Write(nullptr, "abort()"); }
void AtExit() {
    if (!g_clean) log::Event("crash", "process is exiting through exit() without returning from wWinMain", log::kFatal);
}
}

void Install() {
    SetUnhandledExceptionFilter(Filter);
    std::set_terminate(Terminate);
    _set_invalid_parameter_handler(InvalidParameter);
    _set_purecall_handler(PureCall);
    std::signal(SIGABRT, AbortSignal);
    std::atexit(AtExit);
}

void MarkCleanExit() { g_clean = true; }

}
