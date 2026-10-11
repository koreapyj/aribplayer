#include "wvp/Platform.h"
#include "wvp/MediaUri.h"
#include "wvp/Utf8.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace wvp::platform {

namespace {

fs::path P(std::string_view s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string S(const fs::path& p) {
    std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::string AsciiLower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return s;
}

bool WriteAll(int fd, std::string_view bytes) {
    size_t done = 0;
    while (done < bytes.size()) {
        ssize_t put = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (put <= 0) return false;
        done += (size_t)put;
    }
    return true;
}

}

ReadResult ReadAllBytes(const std::string& path, std::string& out, std::uint64_t limit) {
    out.clear();
    std::error_code ec;
    if (!fs::is_regular_file(P(path), ec)) return ReadResult::Failed;
    std::ifstream f(P(path), std::ios::binary);
    if (!f) return ReadResult::Failed;
    auto size = fs::file_size(P(path), ec);
    if (ec) return ReadResult::Failed;
    if (size > limit) return ReadResult::TooLarge;
    out.resize((size_t)size);
    f.read(out.data(), (std::streamsize)out.size());
    if ((size_t)f.gcount() != out.size()) { out.clear(); return ReadResult::Failed; }
    return ReadResult::Ok;
}

bool EnsureDirectory(const std::string& dir) {
    if (dir.empty()) return false;
    std::error_code ec;
    fs::create_directories(P(dir), ec);
    return fs::is_directory(P(dir), ec);
}

bool AtomicWriteFile(const std::string& path, std::string_view bytes, bool durable) {
    const fs::path target = P(path);
    if (target.has_parent_path() && !EnsureDirectory(S(target.parent_path()))) return false;
    const std::string tmp = path + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    bool ok = WriteAll(fd, bytes);
    if (ok && durable) ok = ::fsync(fd) == 0;
    ok = ::close(fd) == 0 && ok;
    if (ok) ok = std::rename(tmp.c_str(), path.c_str()) == 0;
    if (!ok) ::unlink(tmp.c_str());
    return ok;
}

bool Exists(const std::string& path) {
    std::error_code ec;
    auto st = fs::status(P(path), ec);
    return !ec && fs::exists(st) && !fs::is_directory(st);
}

bool RemoveFile(const std::string& path) {
    std::error_code ec;
    return !fs::is_directory(P(path), ec) && fs::remove(P(path), ec);
}

bool RenameNoReplace(const std::string& from, const std::string& to) {
    std::error_code ec;
    if (fs::exists(P(to), ec) && !fs::equivalent(P(from), P(to), ec)) return false;
    fs::rename(P(from), P(to), ec);
    return !ec;
}

std::vector<FileEntry> ListFiles(const std::string& dir, std::string_view ext) {
    std::vector<FileEntry> out;
    const std::string want = AsciiLower(std::string(ext));
    std::error_code ec;
    for (fs::directory_iterator it(P(dir), ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec)) continue;
        std::string name = S(it->path().filename());
        if (name.size() <= want.size() || AsciiLower(name.substr(name.size() - want.size())) != want) continue;
        auto t = it->last_write_time(ec);
        if (ec) { ec.clear(); continue; }
        auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
        out.push_back({ std::move(name), (std::int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(sys.time_since_epoch()).count() });
    }
    return out;
}

std::string AppDataDir() {
    const char* home = std::getenv("HOME");
    const std::string h = home ? home : "";
#ifdef __APPLE__
    return h + "/Library/Application Support/aribplayer";
#else
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && *xdg == '/') return std::string(xdg) + "/aribplayer";
    return h + "/.local/share/aribplayer";
#endif
}

std::string FullPath(const std::string& p) {
    if (p.empty()) return p;
    std::error_code ec;
    fs::path abs = fs::absolute(P(p), ec);
    if (ec) return p;
    return S(abs.lexically_normal());
}

std::string PathRoot(const std::string& p) { return !p.empty() && p[0] == '/' ? "/" : ""; }

std::string DirectoryName(const std::string& p) {
    size_t rootLen = PathRoot(p).size();
    if (p.size() <= rootLen) return "";
    size_t last = p.rfind('/');
    if (last == std::string::npos || last < rootLen) return p.substr(0, rootLen);
    size_t end = last;
    while (end > rootLen && p[end - 1] == '/') --end;
    return p.substr(0, end);
}

std::string Combine(const std::string& a, const std::string& b) {
    if (b.empty()) return a;
    if (a.empty() || b[0] == '/') return b;
    return a.back() == '/' ? a + b : a + '/' + b;
}

std::string Extension(const std::string& p) {
    size_t sep = p.rfind('/');
    size_t dot = p.rfind('.');
    if (dot == std::string::npos || (sep != std::string::npos && dot < sep)) return "";
    if (dot + 1 >= p.size()) return "";
    return p.substr(dot);
}

bool IsAbsolutePath(std::string_view p) { return !p.empty() && p[0] == '/'; }

std::string NativePath(std::string_view p) { return std::string(p); }

std::string FileUriToPath(std::string_view uri) {
    std::string_view rest = uri.substr(5);
    if (rest.substr(0, 2) == "//") {
        std::string_view after = rest.substr(2);
        size_t cut = after.find_first_of("?#");
        if (cut != std::string_view::npos) after = after.substr(0, cut);
        size_t slash = after.find('/');
        std::string_view authority = slash == std::string_view::npos ? after : after.substr(0, slash);
        std::string path = MediaUri::PercentDecode(slash == std::string_view::npos ? std::string_view("/") : after.substr(slash));
        return authority.empty() || authority == "localhost" ? path : "//" + std::string(authority) + path;
    }
    return MediaUri::PercentDecode(rest);
}

bool NamesEqualIgnoreCase(std::string_view a, std::string_view b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size())
        if (FoldCase(utf8::Next(a, i)) != FoldCase(utf8::Next(b, j))) return false;
    return i == a.size() && j == b.size();
}

char32_t FoldCase(char32_t c) { return (char32_t)std::towlower((wint_t)c); }

std::optional<std::string> DecodeSystemCodePage(std::string_view) { return std::nullopt; }

std::optional<std::string> SystemProxyFor(std::string_view, std::string_view) { return std::nullopt; }

}
