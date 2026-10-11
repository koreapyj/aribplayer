// Call wvp::SilenceFailureDialogs() first thing in every test entry point. Tests run on the user's desktop, so a CRT
// assert, a /RTC failure, an abort() or a hard error must never show a dialog: it is printed to stderr and the process exits non-zero.
#pragma once
#include <windows.h>
#include <crtdbg.h>
#include <cstdio>
#include <cstdlib>
#ifdef _DEBUG
#include <rtcapi.h>
#endif

namespace wvp {
namespace testguard_detail {
#ifdef _DEBUG
inline int __cdecl RtcFailure(int errType, const wchar_t* file, int line, const wchar_t* module, const wchar_t* format, ...) {
    (void)format;
    std::fprintf(stderr, "\n[RTC FAILURE] type=%d at %ls(%d) module=%ls (stale .obj after a header change? rebuild)\n", errType,
                 file ? file : L"?", line, module ? module : L"?");
    std::fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 70);
    return 0;
}
inline int __cdecl CrtHook(int reportType, char* message, int* returnValue) {
    std::fprintf(stderr, "\n[CRT %s] %s\n", reportType == _CRT_ASSERT ? "ASSERT" : reportType == _CRT_ERROR ? "ERROR" : "WARN",
                 message ? message : "");
    std::fflush(stderr);
    if (reportType != _CRT_WARN) TerminateProcess(GetCurrentProcess(), 71);
    if (returnValue) *returnValue = 0;
    return TRUE;   // handled: no dialog
}
#endif
}

inline void SilenceFailureDialogs() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_error_mode(_OUT_TO_STDERR);
#ifdef _DEBUG
    for (int t : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(t, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(t, _CRTDBG_FILE_STDERR);
    }
    _CrtSetReportHook(testguard_detail::CrtHook);
    _RTC_SetErrorFuncW(testguard_detail::RtcFailure);
#endif
}
}
