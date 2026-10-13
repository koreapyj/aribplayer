// Implementations of shim/windows.h and shim/shellapi.h: the Win32 calls the src/App files of the app make.
#import <AppKit/AppKit.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/time.h>

#include <cerrno>
#include <ctime>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "shellapi.h"
#include "windows.h"

namespace {

void AppendUtf8(std::string& out, char32_t c) {
    if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) c = 0xFFFD;
    if (c < 0x80) out += (char)c;
    else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
    else { out += (char)(0xF0 | (c >> 18)); out += (char)(0x80 | ((c >> 12) & 0x3F)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
}

std::string ToUtf8(const wchar_t* src, int len) {
    std::string out;
    for (int i = 0; i < len; i++) {
        char32_t c = (char32_t)src[i];
        if (sizeof(wchar_t) == 2 && c >= 0xD800 && c <= 0xDBFF && i + 1 < len && src[i + 1] >= 0xDC00 && src[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + ((char32_t)src[i + 1] - 0xDC00);
            i++;
        }
        AppendUtf8(out, c);
    }
    return out;
}

/// Decodes one UTF-8 sequence at s[i] (i < len) into c and returns its length; 0 for an invalid or truncated sequence.
int DecodeUtf8(const unsigned char* s, int i, int len, char32_t& c) {
    const unsigned char b = s[i];
    int n;
    if (b < 0x80) { c = b; return 1; }
    else if ((b & 0xE0) == 0xC0) { n = 1; c = b & 0x1F; }
    else if ((b & 0xF0) == 0xE0) { n = 2; c = b & 0x0F; }
    else if ((b & 0xF8) == 0xF0) { n = 3; c = b & 0x07; }
    else return 0;
    if (i + n >= len) return 0;   // truncated: the continuation bytes s[i + 1 .. i + n] are not all there
    for (int k = 1; k <= n; k++) {
        if ((s[i + k] & 0xC0) != 0x80) return 0;
        c = (c << 6) | (s[i + k] & 0x3F);
    }
    const bool overlong = (n == 1 && c < 0x80) || (n == 2 && c < 0x800) || (n == 3 && c < 0x10000);
    if (overlong || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return 0;
    return n + 1;
}

std::wstring ToWide(const char* src, int len) {
    std::wstring out;
    const unsigned char* s = (const unsigned char*)src;
    for (int i = 0; i < len;) {
        char32_t c;
        int n = DecodeUtf8(s, i, len, c);
        if (n == 0) { out += (wchar_t)0xFFFD; i++; continue; }
        i += n;
        if (sizeof(wchar_t) == 2 && c >= 0x10000) {
            c -= 0x10000;
            out += (wchar_t)(0xD800 + (c >> 10));
            out += (wchar_t)(0xDC00 + (c & 0x3FF));
        } else {
            out += (wchar_t)c;
        }
    }
    return out;
}

int Length(const wchar_t* s) { int n = 0; while (s[n]) n++; return n + 1; }   // NUL included, as Win32 counts it
int Length(const char* s) { int n = 0; while (s[n]) n++; return n + 1; }

}

int WideCharToMultiByte(unsigned codePage, unsigned, const wchar_t* src, int srcLen, char* dst, int dstLen, const char*, int*) {
    if (codePage != CP_UTF8 || !src) return 0;
    const std::string u = ToUtf8(src, srcLen < 0 ? Length(src) : srcLen);
    if (dstLen == 0) return (int)u.size();
    if ((int)u.size() > dstLen) return 0;
    u.copy(dst, u.size());
    return (int)u.size();
}

int MultiByteToWideChar(unsigned codePage, unsigned, const char* src, int srcLen, wchar_t* dst, int dstLen) {
    if (codePage != CP_UTF8 || !src) return 0;
    const std::wstring w = ToWide(src, srcLen < 0 ? Length(src) : srcLen);
    if (dstLen == 0) return (int)w.size();
    if ((int)w.size() > dstLen) return 0;
    w.copy(dst, w.size());
    return (int)w.size();
}

// --- locale ----------------------------------------------------------------------------------------------------------

int GetUserDefaultLocaleName(LPWSTR name, int count) {
    if (!name || count <= 0) return 0;
    @autoreleasepool {
        // The language preference is BCP-47 and independent of the bundle's supported UI localizations.
        NSString* tag = NSLocale.preferredLanguages.firstObject;
        const char* utf8 = tag.UTF8String;
        if (!utf8 || !*utf8) return 0;
        const std::wstring wide = ToWide(utf8, Length(utf8) - 1);
        if (wide.size() + 1 > static_cast<std::size_t>(count)) return 0;
        wide.copy(name, wide.size());
        name[wide.size()] = L'\0';
        return static_cast<int>(wide.size() + 1);
    }
}

int GetSystemMetrics(int index) { return index == SM_CXDRAG || index == SM_CYDRAG ? 4 : 0; }

void* ShellExecuteW(HWND, LPCWSTR operation, LPCWSTR file, LPCWSTR, LPCWSTR, int) {
    if (!operation || !file || ToUtf8(operation, Length(operation) - 1) != "open") return nullptr;
    const std::string target = ToUtf8(file, Length(file) - 1);
    NSString* s = [NSString stringWithUTF8String:target.c_str()];
    NSURL* url = nil;
    if (!s) return nullptr;
    if ([s hasPrefix:@"/"]) url = [NSURL fileURLWithPath:s];
    else if ([s rangeOfString:@":"].location != NSNotFound) url = [NSURL URLWithString:s];
    if (url) [[NSWorkspace sharedWorkspace] openURL:url];
    return nullptr;
}

// --- timers -----------------------------------------------------------------------------------------------------------

namespace {
struct TimerKey {
    HWND hwnd;
    UINT_PTR id;
    bool operator<(const TimerKey& o) const { return hwnd != o.hwnd ? hwnd < o.hwnd : id < o.id; }
};
std::map<TimerKey, dispatch_source_t>& Timers() { static std::map<TimerKey, dispatch_source_t> t; return t; }
}

UINT_PTR SetTimer(HWND hwnd, UINT_PTR id, UINT ms, TIMERPROC proc) {
    if (!proc || !id) return 0;
    KillTimer(hwnd, id);
    dispatch_source_t t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    const uint64_t ns = (uint64_t)std::max<UINT>(ms, 1) * NSEC_PER_MSEC;
    dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW, (int64_t)ns), ns, ns / 10);
    dispatch_source_set_event_handler(t, ^{ proc(hwnd, 0x0113 /* WM_TIMER */, id, (DWORD)GetTickCount64()); });
    Timers()[{hwnd, id}] = t;
    dispatch_resume(t);
    return id;
}

int KillTimer(HWND hwnd, UINT_PTR id) {
    auto it = Timers().find({hwnd, id});
    if (it == Timers().end()) return 0;
    dispatch_source_cancel(it->second);
    Timers().erase(it);
    return 1;
}

// --- process / time ---------------------------------------------------------------------------------------------------

DWORD GetCurrentProcessId() { return (DWORD)getpid(); }

std::uint64_t GetTickCount64() {
    using namespace std::chrono;
    return (std::uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// --- files ------------------------------------------------------------------------------------------------------------

namespace {
std::string PathOf(LPCWSTR p) {
    if (!p) return {};
    std::string s = ToUtf8(p, Length(p) - 1);
    for (char& c : s) if (c == '\\') c = '/';
    return s;
}

std::wstring Wide(const std::string& s) { return ToWide(s.data(), (int)s.size()); }

struct FindState {
    DIR* dir = nullptr;
    std::string base;
    std::string single;   // a non-wildcard pattern: that one entry
    bool singleDone = false;
};

bool Fill(FindState& st, const std::string& name, WIN32_FIND_DATAW* fd) {
    struct stat sb{};
    const std::string full = st.base.empty() ? name : st.base + "/" + name;
    if (stat(full.c_str(), &sb) != 0 && name != "." && name != "..") return false;
    fd->dwFileAttributes = S_ISDIR(sb.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    const std::wstring w = Wide(name);
    const size_t n = std::min(w.size(), (size_t)MAX_PATH - 1);
    w.copy(fd->cFileName, n);
    fd->cFileName[n] = 0;
    return true;
}

bool NextEntry(FindState& st, WIN32_FIND_DATAW* fd) {
    if (!st.dir) {
        if (st.singleDone) return false;
        st.singleDone = true;
        return Fill(st, st.single, fd);
    }
    while (dirent* e = readdir(st.dir))
        if (Fill(st, e->d_name, fd)) return true;
    return false;
}
}

DWORD GetTempPathW(DWORD size, wchar_t* buffer) {
    std::string t = NSTemporaryDirectory().UTF8String;
    if (t.empty() || t.back() != '/') t += '/';
    const std::wstring w = Wide(t);
    if (!buffer || w.size() + 1 > size) return 0;
    w.copy(buffer, w.size());
    buffer[w.size()] = 0;
    return (DWORD)w.size();
}

int CreateDirectoryW(LPCWSTR path, void*) { return mkdir(PathOf(path).c_str(), 0755) == 0 ? 1 : 0; }
int RemoveDirectoryW(LPCWSTR path) { return rmdir(PathOf(path).c_str()) == 0 ? 1 : 0; }
int DeleteFileW(LPCWSTR path) { return unlink(PathOf(path).c_str()) == 0 ? 1 : 0; }

DWORD GetFileAttributesW(LPCWSTR path) {
    struct stat sb{};
    if (stat(PathOf(path).c_str(), &sb) != 0) return INVALID_FILE_ATTRIBUTES;
    return S_ISDIR(sb.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}

int GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS, void* info) {
    struct stat sb{};
    if (!info || stat(PathOf(path).c_str(), &sb) != 0) return 0;
    auto* d = static_cast<WIN32_FILE_ATTRIBUTE_DATA*>(info);
    d->dwFileAttributes = S_ISDIR(sb.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    d->nFileSizeHigh = (DWORD)((std::uint64_t)sb.st_size >> 32);
    d->nFileSizeLow = (DWORD)((std::uint64_t)sb.st_size & 0xFFFFFFFFu);
    return 1;
}

HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD, void*, DWORD disposition, DWORD, HANDLE) {
    int flags = (access & GENERIC_WRITE) ? ((access & GENERIC_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    if (disposition == CREATE_ALWAYS) flags |= O_CREAT | O_TRUNC;
    const int fd = open(PathOf(path).c_str(), flags, 0644);
    return fd < 0 ? INVALID_HANDLE_VALUE : (HANDLE)(std::intptr_t)(fd + 1);
}

int CloseHandle(HANDLE h) {
    if (!h || h == INVALID_HANDLE_VALUE) return 0;
    return close((int)(std::intptr_t)h - 1) == 0 ? 1 : 0;
}

HANDLE FindFirstFileW(LPCWSTR pattern, WIN32_FIND_DATAW* data) {
    std::string p = PathOf(pattern);
    auto st = std::make_unique<FindState>();
    if (p.size() >= 2 && p.compare(p.size() - 2, 2, "/*") == 0) {
        st->base = p.substr(0, p.size() - 2);
        st->dir = opendir(st->base.empty() ? "/" : st->base.c_str());
        if (!st->dir) return INVALID_HANDLE_VALUE;
    } else {
        const size_t slash = p.rfind('/');
        st->base = slash == std::string::npos ? std::string() : p.substr(0, slash);
        st->single = slash == std::string::npos ? p : p.substr(slash + 1);
    }
    if (!NextEntry(*st, data)) {
        if (st->dir) closedir(st->dir);
        return INVALID_HANDLE_VALUE;
    }
    return st.release();
}

int FindNextFileW(HANDLE h, WIN32_FIND_DATAW* data) {
    if (!h || h == INVALID_HANDLE_VALUE) return 0;
    return NextEntry(*static_cast<FindState*>(h), data) ? 1 : 0;
}

int FindClose(HANDLE h) {
    if (!h || h == INVALID_HANDLE_VALUE) return 0;
    auto* st = static_cast<FindState*>(h);
    if (st->dir) closedir(st->dir);
    delete st;
    return 1;
}

// --- features/Logging.cpp: local time, std handles --------------------------------------------------------------------

void GetLocalTime(SYSTEMTIME* st) {
    if (!st) return;
    timeval tv{};
    gettimeofday(&tv, nullptr);
    std::tm lt{};
    const time_t secs = tv.tv_sec;
    localtime_r(&secs, &lt);
    st->wYear = (WORD)(lt.tm_year + 1900);
    st->wMonth = (WORD)(lt.tm_mon + 1);
    st->wDayOfWeek = (WORD)lt.tm_wday;
    st->wDay = (WORD)lt.tm_mday;
    st->wHour = (WORD)lt.tm_hour;
    st->wMinute = (WORD)lt.tm_min;
    st->wSecond = (WORD)lt.tm_sec;
    st->wMilliseconds = (WORD)(tv.tv_usec / 1000);
}

HANDLE GetStdHandle(DWORD which) {
    const int fd = which == STD_INPUT_HANDLE ? 0 : which == STD_OUTPUT_HANDLE ? 1 : which == STD_ERROR_HANDLE ? 2 : -1;
    if (fd < 0) return INVALID_HANDLE_VALUE;
    return (HANDLE)(std::intptr_t)(fd + 1);
}

int SetStdHandle(DWORD, HANDLE) { return 1; }

int WriteFile(HANDLE h, const void* data, DWORD size, DWORD* written, void*) {
    if (written) *written = 0;
    if (!h || h == INVALID_HANDLE_VALUE) return 0;
    const ssize_t n = write((int)(std::intptr_t)h - 1, data, size);
    if (n < 0) return 0;
    if (written) *written = (DWORD)n;
    return 1;
}

int GetConsoleMode(HANDLE, DWORD* mode) {
    if (mode) *mode = 0;
    return 0;
}

int WriteConsoleW(HANDLE h, const wchar_t* text, DWORD chars, DWORD* written, void*) {
    const std::string u = ToUtf8(text, (int)chars);
    DWORD bytes = 0;
    const int ok = WriteFile(h, u.data(), (DWORD)u.size(), &bytes, nullptr);
    if (written) *written = ok ? chars : 0;
    return ok;
}

int AttachConsole(DWORD) { return 0; }
int AllocConsole() { return 0; }

int freopen_s(FILE** out, const char* path, const char* mode, FILE* stream) {
    if (!out || !path || !mode || !stream) return EINVAL;
    *out = freopen(path, mode, stream);
    return *out ? 0 : errno;
}

// --- resources --------------------------------------------------------------------------------------------------------

namespace {
struct Resource { NSData* data; };
}

HRSRC FindResourceW(HMODULE, LPCWSTR name, LPCWSTR type) {
    if ((std::uintptr_t)name != 101 || (std::uintptr_t)type != 10) return nullptr;   // IDR_THIRD_PARTY_NOTICES, RT_RCDATA
    static Resource notices{nil};
    if (!notices.data) {
        NSString* path = [NSBundle.mainBundle pathForResource:@"THIRD-PARTY-NOTICES" ofType:@"txt"];
        notices.data = path ? [NSData dataWithContentsOfFile:path] : nil;
    }
    return notices.data ? reinterpret_cast<HRSRC>(&notices) : nullptr;
}

HGLOBAL LoadResource(HMODULE, HRSRC res) { return res; }
DWORD SizeofResource(HMODULE, HRSRC res) { return res ? (DWORD)reinterpret_cast<Resource*>(res)->data.length : 0; }
void* LockResource(HGLOBAL mem) { return mem ? const_cast<void*>(reinterpret_cast<Resource*>(mem)->data.bytes) : nullptr; }
