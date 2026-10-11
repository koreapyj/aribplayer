#include "wvp/Platform.h"
#include "wvp/MediaUri.h"
#include "wvp/win32/ProxySettings.h"
#include "wvp/win32/Utf.h"

#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <iterator>
#include <vector>

namespace wvp::platform {

namespace {

bool IsAsciiLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool IsSep(char c) { return c == '\\' || c == '/'; }
bool StartsWith(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

std::string ReplaceSlashes(std::string s) {
    std::replace(s.begin(), s.end(), '/', '\\');
    return s;
}

size_t RootLength(std::string_view p) {
    if (StartsWith(p, "\\\\?\\") || StartsWith(p, "\\\\.\\")) {
        if (p.size() >= 6 && p[5] == ':') return p.size() >= 7 && IsSep(p[6]) ? 7 : 6;
        return 4;
    }
    if (p.size() >= 2 && IsSep(p[0]) && IsSep(p[1])) {
        size_t i = 2;
        while (i < p.size() && !IsSep(p[i])) ++i;
        if (i < p.size()) ++i;
        while (i < p.size() && !IsSep(p[i])) ++i;
        if (i < p.size()) ++i;
        return i;
    }
    if (p.size() >= 2 && IsAsciiLetter(p[0]) && p[1] == ':') return p.size() >= 3 && IsSep(p[2]) ? 3 : 2;
    if (!p.empty() && IsSep(p[0])) return 1;
    return 0;
}

std::string ParentOf(const std::string& path) {
    size_t cut = path.find_last_of("\\/");
    return cut != std::string::npos && cut > 0 ? path.substr(0, cut) : std::string();
}

std::int64_t FileTimeToUnixMs(const FILETIME& ft) {
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    return (std::int64_t)(u.QuadPart / 10000ULL) - 11644473600000LL;
}

}

ReadResult ReadAllBytes(const std::string& path, std::string& out, std::uint64_t limit) {
    out.clear();
    HANDLE h = CreateFileW(ToWide(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return ReadResult::Failed;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0) { CloseHandle(h); return ReadResult::Failed; }
    if ((std::uint64_t)sz.QuadPart > limit) { CloseHandle(h); return ReadResult::TooLarge; }
    out.resize((size_t)sz.QuadPart);
    bool ok = true;
    size_t done = 0;
    while (ok && done < out.size()) {
        DWORD got = 0;
        ok = ReadFile(h, out.data() + done, (DWORD)std::min<size_t>(out.size() - done, 1u << 24), &got, nullptr) && got != 0;
        done += got;
    }
    out.resize(done);
    CloseHandle(h);
    return ok ? ReadResult::Ok : ReadResult::Failed;
}

bool EnsureDirectory(const std::string& dir) {
    if (dir.empty()) return false;
    const std::wstring w = ToWide(dir);
    DWORD a = GetFileAttributesW(w.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    size_t cut = dir.find_last_of("\\/");
    if (cut != std::string::npos && cut > 0 && !(cut == 2 && dir[1] == ':')) {
        if (!EnsureDirectory(dir.substr(0, cut))) return false;
    }
    if (CreateDirectoryW(w.c_str(), nullptr)) return true;
    a = GetFileAttributesW(w.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool AtomicWriteFile(const std::string& path, std::string_view bytes, bool durable) {
    const std::string parent = ParentOf(path);
    if (!parent.empty() && !EnsureDirectory(parent)) return false;
    const std::wstring target = ToWide(path);
    const std::wstring tmp = target + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    size_t done = 0;
    while (ok && done < bytes.size()) {
        DWORD put = 0;
        ok = WriteFile(h, bytes.data() + done, (DWORD)std::min<size_t>(bytes.size() - done, 1u << 24), &put, nullptr) && put != 0;
        done += put;
    }
    if (ok && durable) ok = FlushFileBuffers(h) != 0;
    CloseHandle(h);
    if (ok) ok = MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | (durable ? MOVEFILE_WRITE_THROUGH : 0)) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

bool Exists(const std::string& path) {
    DWORD a = GetFileAttributesW(ToWide(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool RemoveFile(const std::string& path) { return DeleteFileW(ToWide(path).c_str()) != 0; }

bool RenameNoReplace(const std::string& from, const std::string& to) {
    return MoveFileExW(ToWide(from).c_str(), ToWide(to).c_str(), 0) != 0;
}

std::vector<FileEntry> ListFiles(const std::string& dir, std::string_view ext) {
    std::vector<FileEntry> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(ToWide(Combine(dir, "*" + std::string(ext))).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string name = ToUtf8(fd.cFileName);
        // FindFirstFile also matches 8.3 aliases (*.m3u8 finds "x.m3u8x"): require the exact extension.
        if (name.size() <= ext.size()) continue;
        std::string tail = name.substr(name.size() - ext.size());
        std::transform(tail.begin(), tail.end(), tail.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; });
        std::string want(ext);
        std::transform(want.begin(), want.end(), want.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; });
        if (tail != want) continue;
        out.push_back({ std::move(name), FileTimeToUnixMs(fd.ftLastWriteTime) });
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

std::string AppDataDir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, (DWORD)std::size(buf));
    std::wstring base;
    if (n > 0 && n < std::size(buf)) base.assign(buf, n);
    else {
        DWORD t = GetTempPathW((DWORD)std::size(buf), buf);
        if (t > 0 && t < std::size(buf)) base.assign(buf, t);
    }
    while (!base.empty() && (base.back() == L'\\' || base.back() == L'/')) base.pop_back();
    return ToUtf8(base) + "\\aribplayer";
}

std::string FullPath(const std::string& p) {
    const std::wstring w = ToWide(p);
    DWORD n = GetFullPathNameW(w.c_str(), 0, nullptr, nullptr);
    if (n == 0) return p;
    std::wstring out(n, L'\0');
    DWORD m = GetFullPathNameW(w.c_str(), n, out.data(), nullptr);
    if (m == 0 || m >= n) return p;
    out.resize(m);
    return ToUtf8(out);
}

std::string PathRoot(const std::string& p) { return p.substr(0, RootLength(p)); }

std::string DirectoryName(const std::string& p) {
    size_t rootLen = RootLength(p);
    if (p.size() <= rootLen) return "";
    size_t last = p.find_last_of("\\/");
    if (last == std::string::npos || last < rootLen) return p.substr(0, rootLen);
    size_t end = last;
    while (end > rootLen && IsSep(p[end - 1])) --end;
    return p.substr(0, end);
}

std::string Combine(const std::string& a, const std::string& b) {
    if (b.empty()) return a;
    if (a.empty()) return b;
    if (IsSep(b[0]) || (b.size() >= 2 && IsAsciiLetter(b[0]) && b[1] == ':')) return b;
    char last = a.back();
    if (IsSep(last) || last == ':') return a + b;
    return a + '\\' + b;
}

std::string Extension(const std::string& p) {
    size_t sep = p.find_last_of("\\/:");
    size_t dot = p.rfind('.');
    if (dot == std::string::npos || (sep != std::string::npos && dot < sep)) return "";
    if (dot + 1 >= p.size()) return "";
    return p.substr(dot);
}

bool IsAbsolutePath(std::string_view p) {
    bool drive = p.size() >= 3 && IsAsciiLetter(p[0]) && p[1] == ':' && p[2] == '\\';
    return drive || (p.size() >= 2 && p[0] == '\\' && p[1] == '\\');
}

std::string NativePath(std::string_view s) {
    std::string path = ReplaceSlashes(std::string(s));
    if (StartsWith(s, "//") && !StartsWith(s, "//?/")) {
        size_t k = path.find_first_not_of('\\');
        path = "\\\\" + (k == std::string::npos ? std::string() : path.substr(k));
    }
    return path;
}

std::string FileUriToPath(std::string_view uri) {
    std::string_view rest = uri.substr(5);
    if (StartsWith(rest, "//")) {
        std::string_view after = rest.substr(2);
        size_t cut = after.find_first_of("?#");
        if (cut != std::string_view::npos) after = after.substr(0, cut);
        size_t slash = after.find('/');
        std::string_view authority = slash == std::string_view::npos ? after : after.substr(0, slash);
        std::string decoded = MediaUri::PercentDecode(slash == std::string_view::npos ? std::string_view() : after.substr(slash));
        if (!authority.empty() && authority != "localhost") return "\\\\" + std::string(authority) + ReplaceSlashes(decoded);
        // /C:/x -> C:\x ; /x -> \x
        if (decoded.size() >= 3 && decoded[0] == '/' && IsAsciiLetter(decoded[1]) && (decoded[2] == ':' || decoded[2] == '|'))
            decoded.erase(0, 1);
        std::string local = ReplaceSlashes(std::move(decoded));
        if (local.size() >= 2 && local[1] == '|') local[1] = ':';
        return local;
    }
    size_t k = rest.find_first_not_of('/');
    return ReplaceSlashes(MediaUri::PercentDecode(k == std::string_view::npos ? std::string_view() : rest.substr(k)));
}

bool NamesEqualIgnoreCase(std::string_view a, std::string_view b) {
    const std::wstring wa = ToWide(a), wb = ToWide(b);
    return CompareStringOrdinal(wa.data(), (int)wa.size(), wb.data(), (int)wb.size(), TRUE) == CSTR_EQUAL;
}

char32_t FoldCase(char32_t c) {
    // The CRT's towlower (C locale), as the natural sort order requires.
    return c <= 0xFFFF ? (char32_t)std::towlower((wint_t)c) : c;
}

std::optional<std::string> DecodeSystemCodePage(std::string_view bytes) {
    if (bytes.empty()) return std::string();
    int len = MultiByteToWideChar(CP_ACP, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
    if (len <= 0) return std::nullopt;
    std::wstring w((size_t)len, L'\0');
    MultiByteToWideChar(CP_ACP, 0, bytes.data(), (int)bytes.size(), w.data(), len);
    return ToUtf8(w);
}

namespace {

bool EqualsAsciiNoCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        return std::tolower((unsigned char)x) == std::tolower((unsigned char)y);
    });
}

std::vector<std::string_view> SplitList(std::string_view s) {
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ';' || s[i] == ' ' || s[i] == '\t')) ++i;
        size_t j = i;
        while (j < s.size() && s[j] != ';' && s[j] != ' ' && s[j] != '\t') ++j;
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

bool GlobMatch(std::string_view pat, std::string_view s) {
    if (pat.empty()) return s.empty();
    if (pat[0] == '*') {
        for (size_t k = 0; k <= s.size(); ++k)
            if (GlobMatch(pat.substr(1), s.substr(k))) return true;
        return false;
    }
    return !s.empty() && std::tolower((unsigned char)pat[0]) == std::tolower((unsigned char)s[0]) &&
           GlobMatch(pat.substr(1), s.substr(1));
}

}

namespace win32 {

std::string ProxyFromSettings(std::string_view server, std::string_view bypass, std::string_view scheme,
                              std::string_view host) {
    for (std::string_view p : SplitList(bypass)) {
        if (EqualsAsciiNoCase(p, "<local>")) {
            if (host.find('.') == std::string_view::npos && host.find(':') == std::string_view::npos) return {};
        } else if (GlobMatch(p, host)) {
            return {};
        }
    }
    const auto entries = SplitList(server);
    if (server.find('=') == std::string_view::npos) return entries.empty() ? std::string() : std::string(entries[0]);
    std::string socks;
    for (std::string_view e : entries) {
        const size_t eq = e.find('=');
        if (eq == std::string_view::npos) continue;
        if (EqualsAsciiNoCase(e.substr(0, eq), scheme)) return std::string(e.substr(eq + 1));
        // Windows' "socks=" entry is a SOCKS4 proxy.
        if (EqualsAsciiNoCase(e.substr(0, eq), "socks")) socks = "socks4://" + std::string(e.substr(eq + 1));
    }
    return socks;
}

}

std::optional<std::string> SystemProxyFor(std::string_view scheme, std::string_view host) {
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG cfg = {};
    if (!WinHttpGetIEProxyConfigForCurrentUser(&cfg)) return std::string();
    // lpszProxy is null while the manual proxy is switched off; fAutoDetect / lpszAutoConfigUrl are not used.
    const std::string server = cfg.lpszProxy ? ToUtf8(cfg.lpszProxy) : std::string();
    const std::string bypass = cfg.lpszProxyBypass ? ToUtf8(cfg.lpszProxyBypass) : std::string();
    for (LPWSTR s : {cfg.lpszAutoConfigUrl, cfg.lpszProxy, cfg.lpszProxyBypass})
        if (s) GlobalFree(s);
    return win32::ProxyFromSettings(server, bypass, scheme, host);
}

}
