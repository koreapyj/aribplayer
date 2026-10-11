#include "wvp/FileIo.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <iterator>

namespace wvp::fileio {

const wchar_t kSep = L'\\';

namespace {

std::int64_t FileTimeToUnixMs(const FILETIME& ft) {
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    return (std::int64_t)(u.QuadPart / 10000ULL) - 11644473600000LL;
}

}

bool ReadAllBytes(const std::wstring& path, std::string& out) {
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart >= 0 && sz.QuadPart < (1LL << 30);
    if (ok) {
        out.resize((size_t)sz.QuadPart);
        size_t done = 0;
        while (ok && done < out.size()) {
            DWORD got = 0;
            ok = ReadFile(h, out.data() + done, (DWORD)std::min<size_t>(out.size() - done, 1u << 24), &got, nullptr) && got != 0;
            done += got;
        }
        out.resize(done);
    }
    CloseHandle(h);
    return ok;
}

std::optional<std::uint64_t> FileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) || (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return std::nullopt;
    return ((std::uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
}

bool IsRegularFile(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool EnsureDirectory(const std::wstring& dir) {
    if (dir.empty()) return false;
    DWORD a = GetFileAttributesW(dir.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    size_t cut = dir.find_last_of(L"\\/");
    if (cut != std::wstring::npos && cut > 0 && !(cut == 2 && dir[1] == L':')) {
        if (!EnsureDirectory(dir.substr(0, cut))) return false;
    }
    if (CreateDirectoryW(dir.c_str(), nullptr)) return true;
    a = GetFileAttributesW(dir.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool AtomicWriteFile(const std::wstring& path, std::string_view bytes, bool durable) {
    size_t cut = path.find_last_of(L"\\/");
    if (cut != std::wstring::npos && cut > 0) { if (!EnsureDirectory(path.substr(0, cut))) return false; }
    std::wstring tmp = path + L".tmp";
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
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | (durable ? MOVEFILE_WRITE_THROUGH : 0)) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

bool RenameNoReplace(const std::wstring& from, const std::wstring& to) { return MoveFileExW(from.c_str(), to.c_str(), 0) != 0; }

bool RemoveFile(const std::wstring& path) { return DeleteFileW(path.c_str()) != 0; }

std::vector<DirEntry> ListFiles(const std::wstring& dir) {
    std::vector<DirEntry> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        out.push_back({ fd.cFileName, FileTimeToUnixMs(fd.ftLastWriteTime) });
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

std::wstring FullPath(const std::wstring& p) {
    DWORD n = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
    if (n == 0) return p;
    std::wstring out(n, L'\0');
    DWORD m = GetFullPathNameW(p.c_str(), n, out.data(), nullptr);
    if (m == 0 || m >= n) return p;
    out.resize(m);
    return out;
}

std::wstring LocalAppDataApp() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, (DWORD)std::size(buf));
    std::wstring base;
    if (n > 0 && n < std::size(buf)) base.assign(buf, n);
    else {
        DWORD t = GetTempPathW((DWORD)std::size(buf), buf);
        if (t > 0 && t < std::size(buf)) base.assign(buf, t);
    }
    while (!base.empty() && (base.back() == L'\\' || base.back() == L'/')) base.pop_back();
    return base + L"\\aribplayer";
}

}
