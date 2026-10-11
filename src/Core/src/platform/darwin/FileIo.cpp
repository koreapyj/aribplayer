#include "wvp/FileIo.h"
#include "wvp/Utf.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace wvp::fileio {

const wchar_t kSep = L'/';

bool ReadAllBytes(const std::wstring& path, std::string& out) {
    out.clear();
    int fd = ::open(ToUtf8(path).c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st{};
    bool ok = ::fstat(fd, &st) == 0 && st.st_size >= 0 && st.st_size < (1LL << 30);
    if (ok) {
        out.resize((size_t)st.st_size);
        size_t done = 0;
        while (ok && done < out.size()) {
            ssize_t got = ::read(fd, out.data() + done, std::min<size_t>(out.size() - done, 1u << 24));
            ok = got > 0;
            if (ok) done += (size_t)got;
        }
        out.resize(done);
    }
    ::close(fd);
    return ok;
}

std::optional<std::uint64_t> FileSize(const std::wstring& path) {
    struct stat st{};
    if (::stat(ToUtf8(path).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return std::nullopt;
    return (std::uint64_t)st.st_size;
}

bool IsRegularFile(const std::wstring& path) {
    struct stat st{};
    return ::stat(ToUtf8(path).c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool EnsureDirectory(const std::wstring& dir) {
    if (dir.empty()) return false;
    std::string d = ToUtf8(dir);
    struct stat st{};
    if (::stat(d.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    size_t cut = dir.find_last_of(L'/');
    if (cut != std::wstring::npos && cut > 0) {
        if (!EnsureDirectory(dir.substr(0, cut))) return false;
    }
    if (::mkdir(d.c_str(), 0700) == 0) return true;
    return ::stat(d.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool AtomicWriteFile(const std::wstring& path, std::string_view bytes, bool durable) {
    size_t cut = path.find_last_of(L'/');
    if (cut != std::wstring::npos && cut > 0) { if (!EnsureDirectory(path.substr(0, cut))) return false; }
    std::string target = ToUtf8(path);
    std::string tmp = target + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    bool ok = true;
    size_t done = 0;
    while (ok && done < bytes.size()) {
        ssize_t put = ::write(fd, bytes.data() + done, std::min<size_t>(bytes.size() - done, 1u << 24));
        ok = put > 0;
        if (ok) done += (size_t)put;
    }
    if (ok && durable) ok = ::fsync(fd) == 0;
    ::close(fd);
    if (ok) ok = std::rename(tmp.c_str(), target.c_str()) == 0;
    if (ok && durable) {
        // rename() is atomic but the new directory entry is only durable once the directory itself is flushed.
        std::string dir = cut == std::wstring::npos ? "." : ToUtf8(path.substr(0, cut));
        int dfd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
        if (dfd >= 0) { ::fsync(dfd); ::close(dfd); }
    }
    if (!ok) ::unlink(tmp.c_str());
    return ok;
}

bool RenameNoReplace(const std::wstring& from, const std::wstring& to) {
    std::string f = ToUtf8(from), t = ToUtf8(to);
    struct stat src{}, dst{};
    if (::stat(f.c_str(), &src) != 0) return false;
    // On a case-insensitive volume "to" may already resolve to "from" itself (case-only rename): allow that.
    if (::stat(t.c_str(), &dst) == 0 && !(dst.st_dev == src.st_dev && dst.st_ino == src.st_ino)) return false;
    return std::rename(f.c_str(), t.c_str()) == 0;
}

bool RemoveFile(const std::wstring& path) { return ::unlink(ToUtf8(path).c_str()) == 0; }

std::vector<DirEntry> ListFiles(const std::wstring& dir) {
    std::vector<DirEntry> out;
    std::string d = ToUtf8(dir);
    DIR* h = ::opendir(d.c_str());
    if (!h) return out;
    while (dirent* e = ::readdir(h)) {
        struct stat st{};
        if (::stat((d + "/" + e->d_name).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        out.push_back({ ToWide(e->d_name), (std::int64_t)st.st_mtime * 1000 });
    }
    ::closedir(h);
    return out;
}

std::wstring FullPath(const std::wstring& p) {
    if (p.empty()) return p;
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(std::filesystem::path(ToUtf8(p)), ec);
    if (ec) return p;
    return ToWide(abs.lexically_normal().string());
}

std::wstring LocalAppDataApp() {
    std::string base;
    if (const char* home = std::getenv("HOME"); home && *home) base = std::string(home) + "/Library/Application Support";
    if (base.empty()) {
        const char* tmp = std::getenv("TMPDIR");
        base = tmp && *tmp ? tmp : "/tmp";
    }
    while (!base.empty() && base.back() == '/') base.pop_back();
    return ToWide(base) + L"/aribplayer";
}

}
