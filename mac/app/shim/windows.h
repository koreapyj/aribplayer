#pragma once
// The Win32 names used by the src/App files compiled by the Mac app. src/Core's sources do not include windows.h
// on POSIX (src/Core/src/posix/Platform.cpp); its wvp/win32/Utf.h header and the App files do.
// This directory precedes the SDK on the include path, so
// `#include <windows.h>` / `<Windows.h>` (case-insensitive file system) lands here. Implementations: src/Win32Names.mm.
//
// Consumers in the compiled App files and their headers:
//   HWND                                   AppContext.h (MainHwnd), features/NativeMenu.h, features/TextInputDialog.h
//   COLORREF                               features/TextInputDialog.h (DialogColors)
//   WideCharToMultiByte / MultiByteToWideChar / CP_UTF8
//                                          src/Core/include/wvp/win32/Utf.h (ToUtf8 / ToWide)
//   GetSystemMetrics(SM_CXDRAG)            shell/PlaylistPanel.cpp (drag slop)
//   ShellExecuteW / SW_SHOWNORMAL          shell/PlaylistPanel.cpp, pages/SettingsPage.cpp (<shellapi.h>)
//   UINT / UINT_PTR / DWORD, SetTimer / KillTimer (TIMERPROC, CALLBACK)
//                                          features/FxUtil.h/.cpp (SetUiTimer: callback timers on the main window), and
//                                          through it FiltersPanel.cpp, PlaybackExtras.cpp
//   VK_LEFT / VK_RIGHT                     pages/SettingsPage.cpp (accent swatch arrows)
//   FindResourceW / LoadResource / LockResource / SizeofResource, MAKEINTRESOURCEW, RT_RCDATA
//                                          pages/LicensesDialog.cpp (the THIRD-PARTY-NOTICES text; served from the bundle's
//                                          Resources/THIRD-PARTY-NOTICES.txt)
//   MAX_PATH, GetTempPathW, GetCurrentProcessId, GetTickCount64, CreateDirectoryW, RemoveDirectoryW, DeleteFileW,
//   CreateFileW / CloseHandle, FindFirstFileW / FindNextFileW / FindClose (WIN32_FIND_DATAW), HANDLE
//                                          features/OpenLogic.cpp (ListMediaFiles + its LOGIC_TEST temp dirs),
//                                          features/SettingsLogic.cpp and pages/SettingsPage.cpp (LOGIC_TEST temp dirs).
//   GetFileAttributesExW (WIN32_FILE_ATTRIBUTE_DATA)   features/PropertiesDialog.cpp (file size)
//                                          Paths are wide; '\\' is accepted as a separator (converted to '/').
//   SYSTEMTIME / GetLocalTime, GetStdHandle / SetStdHandle (STD_*_HANDLE), WriteFile, GetConsoleMode / WriteConsoleW,
//   AttachConsole / AllocConsole, FILE_SHARE_*, freopen_s
//                                          features/Logging.cpp (the log sink: local time stamps, the --console handle;
//                                          a Mac process has no console object, so GetConsoleMode fails and the sink
//                                          writes UTF-8 bytes with WriteFile to the std handle = fd + 1)
//   GetUserDefaultLocaleName / LOCALE_NAME_MAX_LENGTH   features/OpenLogic.cpp (subtitle language preference)
//   _wcsicmp, array swprintf_s             features/PlaybackExtras.cpp, features/SettingsLogic.cpp
//   LOGFONTW / GDI font-query names         pages/SettingsPage.cpp (native-dialog path; Mac replaces the button handlers)
// No BOOL (Objective-C++ files have objc's). The font-dialog compatibility names do not create native windows.
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwchar>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

typedef struct HWND__* HWND;
typedef struct HMENU__* HMENU;
typedef struct HRSRC__* HRSRC;
typedef void* HGLOBAL;
typedef void* HANDLE;
typedef void* HMODULE;
typedef std::uint32_t COLORREF;          // 0x00BBGGRR
typedef const wchar_t* LPCWSTR;
typedef wchar_t* LPWSTR;
typedef wchar_t* PWSTR;
typedef unsigned int UINT;
typedef std::uintptr_t UINT_PTR;
typedef std::uint32_t DWORD;
typedef std::int32_t HRESULT;

#define CALLBACK
#define WINAPI
#define MAX_PATH 260
#define CP_UTF8 65001u
#define SM_CXDRAG 68
#define SM_CYDRAG 69
#define VK_LEFT 0x25
#define VK_RIGHT 0x27

#define S_OK ((HRESULT)0)
#define S_FALSE ((HRESULT)1)
#define E_FAIL ((HRESULT)0x80004005)
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr) (((HRESULT)(hr)) < 0)

/// UTF-32 wchar_t (macOS) <-> UTF-8, with the Win32 size contract: srcLen -1 = NUL-terminated (the NUL is converted and
/// counted); dstLen 0 = return the required length. Only CP_UTF8 is understood (0 for any other code page). Invalid input
/// becomes U+FFFD. The trailing default-char parameters are accepted and ignored.
int WideCharToMultiByte(unsigned codePage, unsigned flags, const wchar_t* src, int srcLen, char* dst, int dstLen,
                        const char* defaultChar = nullptr, int* usedDefaultChar = nullptr);
int MultiByteToWideChar(unsigned codePage, unsigned flags, const char* src, int srcLen, wchar_t* dst, int dstLen);

// --- locale / CRT -----------------------------------------------------------------------------------------------------
#define LOCALE_NAME_MAX_LENGTH 85
/// NSLocale's first preferred language as a BCP-47 tag, independent of the bundle's UI localizations.
/// Returns the length INCLUDING NUL, or 0 for invalid arguments / insufficient capacity; never truncates.
int GetUserDefaultLocaleName(LPWSTR name, int count);

/// Like the CRT, uses the current C locale's case mapping, not a filesystem's case-sensitivity rules.
inline int _wcsicmp(const wchar_t* a, const wchar_t* b) { return ::wcscasecmp(a, b); }

/// The array overload used by App's numeric labels. Clears the output on error instead of leaving a truncated label.
/// Formats use POSIX wide printf conventions (the compiled consumers only format numbers).
template <std::size_t N, typename... Args>
int swprintf_s(wchar_t (&buffer)[N], const wchar_t* format, Args... args) {
    if (!format) { buffer[0] = L'\0'; errno = EINVAL; return -1; }
    const int n = std::swprintf(buffer, N, format, args...);
    if (n < 0 || static_cast<std::size_t>(n) >= N) { buffer[0] = L'\0'; return -1; }
    return n;
}

// --- SettingsPage's native font-dialog path (Mac uses pui handlers in SettingsWindowMac) -------------------------------
typedef void* HDC;
typedef void* HFONT;
typedef void* HGDIOBJ;
typedef std::intptr_t LPARAM;
typedef std::uintptr_t WPARAM;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#define DEFAULT_CHARSET 1
#define LF_FACESIZE 32
#define LOGPIXELSY 90
#define WM_INITDIALOG 0x0110
#define SW_HIDE 0
#define _TRUNCATE ((std::size_t)-1)
struct LOGFONTW {
    int lfHeight, lfWeight;
    unsigned char lfItalic, lfCharSet;
    wchar_t lfFaceName[LF_FACESIZE];
};
struct TEXTMETRICW { unsigned char tmCharSet; };
// No GDI context exists: the shared picker skips its optional localized-name probe.
inline HDC GetDC(HWND) { return nullptr; }
inline int ReleaseDC(HWND, HDC) { return 0; }
inline int GetDeviceCaps(HDC, int) { return 96; }
inline HFONT CreateFontIndirectW(const LOGFONTW*) { return nullptr; }
inline HGDIOBJ SelectObject(HDC, HGDIOBJ) { return nullptr; }
inline int GetTextFaceW(HDC, int, wchar_t*) { return 0; }
inline int GetTextMetricsW(HDC, TEXTMETRICW*) { return 0; }
inline int DeleteObject(HGDIOBJ) { return 0; }
inline HWND GetDlgItem(HWND, int) { return nullptr; }
inline int ShowWindow(HWND, int) { return 0; }
inline int MulDiv(int number, int numerator, int denominator) {
    if (!denominator) return -1;
    const std::int64_t product = (std::int64_t)number * numerator;
    const std::int64_t divisor = denominator < 0 ? -(std::int64_t)denominator : denominator;
    const std::int64_t rounded = ((product < 0 ? -product : product) + divisor / 2) / divisor;
    return static_cast<int>((product < 0) != (denominator < 0) ? -rounded : rounded);
}
template <std::size_t N>
int wcsncpy_s(wchar_t (&dst)[N], const wchar_t* src, std::size_t count) {
    if (!src) { dst[0] = 0; return EINVAL; }
    std::size_t i = 0;
    while (i + 1 < N && i < count && src[i]) { dst[i] = src[i]; ++i; }
    dst[i] = 0;
    return 0;
}
constexpr COLORREF RGB(unsigned r, unsigned g, unsigned b) { return (r & 255u) | ((g & 255u) << 8) | ((b & 255u) << 16); }
constexpr unsigned GetRValue(COLORREF c) { return c & 255u; }
constexpr unsigned GetGValue(COLORREF c) { return (c >> 8) & 255u; }
constexpr unsigned GetBValue(COLORREF c) { return (c >> 16) & 255u; }

/// SM_CXDRAG / SM_CYDRAG: 4 (AppKit's drag slop); 0 for anything else.
int GetSystemMetrics(int index);

// --- timers (main queue; hwnd is only part of the key, as in Win32) ------------------------------------------------------
typedef void (*TIMERPROC)(HWND, UINT, UINT_PTR, DWORD);
/// A repeating main-queue timer calling proc(hwnd, WM_TIMER, id, ms-tick) every `ms`; re-arming the same (hwnd, id)
/// replaces it. Returns id (nonzero) or 0 when proc / id is 0.
UINT_PTR SetTimer(HWND hwnd, UINT_PTR id, UINT ms, TIMERPROC proc);
int KillTimer(HWND hwnd, UINT_PTR id);

// --- process / time ------------------------------------------------------------------------------------------------------
DWORD GetCurrentProcessId();
std::uint64_t GetTickCount64();

// --- files (wide paths; '\\' separators become '/') -----------------------------------------------------------------------
#define INVALID_HANDLE_VALUE ((HANDLE)(std::intptr_t)-1)
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define FILE_ATTRIBUTE_NORMAL 0x80u
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define CREATE_ALWAYS 2u
#define OPEN_EXISTING 3u

struct WIN32_FIND_DATAW {
    DWORD dwFileAttributes;
    wchar_t cFileName[MAX_PATH];
};

/// Writes the temp directory with a trailing '/' (NSTemporaryDirectory); returns its length (0 when it does not fit).
DWORD GetTempPathW(DWORD size, wchar_t* buffer);
int CreateDirectoryW(LPCWSTR path, void* security);
int RemoveDirectoryW(LPCWSTR path);
int DeleteFileW(LPCWSTR path);
DWORD GetFileAttributesW(LPCWSTR path);
/// CREATE_ALWAYS creates / truncates, OPEN_EXISTING opens; the handle is only good for CloseHandle.
HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD share, void* security, DWORD disposition, DWORD flags, HANDLE templ);
int CloseHandle(HANDLE h);
/// "dir\\*" / "dir/*" lists dir (including "." and ".."); any other pattern matches that one name.
HANDLE FindFirstFileW(LPCWSTR pattern, WIN32_FIND_DATAW* data);
int FindNextFileW(HANDLE h, WIN32_FIND_DATAW* data);
int FindClose(HANDLE h);
enum GET_FILEEX_INFO_LEVELS { GetFileExInfoStandard };
struct WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes;
    DWORD nFileSizeHigh, nFileSizeLow;
};
/// features/PropertiesDialog.cpp (file size).
int GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, void* info);

// --- features/Logging.cpp: local time, std handles (fd + 1, like CreateFileW's), no console --------------------------
typedef std::uint16_t WORD;
typedef std::uint64_t ULONGLONG;
struct SYSTEMTIME {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
};
/// Local wall-clock time with milliseconds (localtime_r of gettimeofday).
void GetLocalTime(SYSTEMTIME* st);
#define STD_INPUT_HANDLE ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE ((DWORD)-12)
#define ATTACH_PARENT_PROCESS ((DWORD)-1)
#define FILE_SHARE_READ 0x1u
#define FILE_SHARE_WRITE 0x2u
/// fd 0 / 1 / 2 as handles (fd + 1); SetStdHandle is accepted and ignored (the fds are the std streams).
HANDLE GetStdHandle(DWORD which);
int SetStdHandle(DWORD which, HANDLE h);
/// write(2) to the handle's fd; *written = bytes written.
int WriteFile(HANDLE h, const void* data, DWORD size, DWORD* written, void* overlapped);
/// No console objects on macOS: 0 (so callers use WriteFile). WriteConsoleW writes UTF-8 anyway.
int GetConsoleMode(HANDLE h, DWORD* mode);
int WriteConsoleW(HANDLE h, const wchar_t* text, DWORD chars, DWORD* written, void* reserved);
/// 0: a macOS process has no console to attach to or allocate (the std streams are whatever launched it).
int AttachConsole(DWORD pid);
int AllocConsole();
/// The CRT's checked freopen (EINVAL on bad arguments).
int freopen_s(FILE** out, const char* path, const char* mode, FILE* stream);

// --- resources: the RCDATA of app.rc, served from the main bundle's Resources -------------------------------------------
#define MAKEINTRESOURCEW(i) ((LPCWSTR)(std::uintptr_t)(unsigned short)(i))
#define RT_RCDATA MAKEINTRESOURCEW(10)
/// IDR_THIRD_PARTY_NOTICES (101) -> Resources/THIRD-PARTY-NOTICES.txt; nullptr for anything else / when missing.
HRSRC FindResourceW(HMODULE module, LPCWSTR name, LPCWSTR type);
HGLOBAL LoadResource(HMODULE module, HRSRC res);
DWORD SizeofResource(HMODULE module, HRSRC res);
void* LockResource(HGLOBAL mem);
