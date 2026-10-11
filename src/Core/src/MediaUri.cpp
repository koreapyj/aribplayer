#include "wvp/MediaUri.h"
#include "wvp/FileIo.h"
#include "wvp/MediaItem.h"
#include "wvp/Utf.h"

#include <cwctype>

namespace wvp {

namespace {

bool IsAsciiLetter(wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'); }
bool IsAsciiLetterOrDigit(wchar_t c) { return IsAsciiLetter(c) || (c >= L'0' && c <= L'9'); }
bool IsSep(wchar_t c) { return c == L'\\' || c == L'/'; }
bool StartsWith(std::wstring_view s, std::wstring_view p) { return s.substr(0, p.size()) == p; }

std::wstring Trim(std::wstring_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::iswspace(s[b])) ++b;
    while (e > b && std::iswspace(s[e - 1])) --e;
    return std::wstring(s.substr(b, e - b));
}

std::wstring TrimQuotes(std::wstring_view s) {
    size_t b = 0, e = s.size();
    while (b < e && s[b] == L'"') ++b;
    while (e > b && s[e - 1] == L'"') --e;
    return std::wstring(s.substr(b, e - b));
}

bool IsNetworkScheme(const std::wstring& s) {
    static const wchar_t* const names[] = {
        L"rtmp", L"rtmps", L"rtmpe", L"rtmpt", L"rtmpte", L"rtmpts", L"rtsp", L"rtsps", L"udp", L"rtp", L"srt", L"tcp", L"tls",
        L"mms", L"mmsh", L"mmst", L"ftp", L"sftp", L"rist", L"unix", L"tcp4", L"udplite", L"icecast", L"gopher", L"gophers", L"sap"};
    for (auto n : names) if (s == n) return true;
    return false;
}

bool IsSchemeName(std::wstring_view s) {
    if (s.empty() || !IsAsciiLetter(s[0])) return false;
    for (wchar_t c : s) if (!(IsAsciiLetterOrDigit(c) || c == L'+' || c == L'-' || c == L'.')) return false;
    return true;
}

std::wstring ReplaceChar(std::wstring s, wchar_t from, wchar_t to) {
    for (auto& c : s) if (c == from) c = to;
    return s;
}

int HexVal(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

std::wstring UnescapeDataString(std::wstring_view s) {
    std::string bytes;
    std::wstring out;
    auto flush = [&] { if (!bytes.empty()) { out += ToWide(bytes); bytes.clear(); } };
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'%' && i + 2 < s.size() &&HexVal(s[i + 1]) >= 0 && HexVal(s[i + 2]) >= 0) {
            bytes.push_back((char)(HexVal(s[i + 1]) * 16 + HexVal(s[i + 2])));
            i += 2;
        } else {
            flush();
            out.push_back(s[i]);
        }
    }
    flush();
    return out;
}

std::wstring ExtensionOfPath(const std::wstring& path) {
    return WinPath::ToLower(WinPath::GetExtension(path));
}

std::wstring ExtensionOfUrl(const std::wstring& url) {
    size_t start = url.find(L"://");
    std::wstring p = start != std::wstring::npos ? url.substr(start + 3) : url.substr(url.find(L':') + 1);
    size_t q = p.find_first_of(L"?#");
    if (q != std::wstring::npos) p.resize(q);
    size_t firstSlash = p.find(L'/');
    if (start != std::wstring::npos) p = firstSlash != std::wstring::npos ? p.substr(firstSlash) : L"";
    size_t slash = p.rfind(L'/');
    std::wstring name = slash != std::wstring::npos ? p.substr(slash + 1) : p;
    size_t dot = name.rfind(L'.');
    return dot != std::wstring::npos && dot + 1 < name.size() ? WinPath::ToLower(name.substr(dot)) : L"";
}

MediaUriInfo Local(const std::wstring& s) {
    std::wstring path = ReplaceChar(s, L'/', L'\\');
    if (StartsWith(s, L"//") && !StartsWith(s, L"//?/")) {
        size_t k = path.find_first_not_of(L'\\');
        path = L"\\\\" + (k == std::wstring::npos ? std::wstring() : path.substr(k));
    }
    MediaUriInfo i;
    i.kind = MediaSourceKind::LocalPath;
    i.original = s;
    i.localPath = path;
    i.extension = ExtensionOfPath(path);
    return i;
}

MediaUriInfo FromFileUri(const std::wstring& s) {
    std::wstring rest = s.substr(5);
    std::wstring local;
    if (StartsWith(rest, L"//")) {
        std::wstring after = rest.substr(2);
        size_t cut = after.find_first_of(L"?#");
        if (cut != std::wstring::npos) after.resize(cut);
        size_t slash = after.find(L'/');
        std::wstring authority = slash == std::wstring::npos ? after : after.substr(0, slash);
        std::wstring path = slash == std::wstring::npos ? std::wstring() : after.substr(slash);
        std::wstring decoded = UnescapeDataString(path);
        if (authority.empty() || authority == L"localhost") {
            // /C:/x -> C:\x ; /x -> \x
            if (decoded.size() >= 3 && decoded[0] == L'/' && IsAsciiLetter(decoded[1]) && (decoded[2] == L':' || decoded[2] == L'|'))
                decoded = decoded.substr(1);
            local = ReplaceChar(decoded, L'/', L'\\');
            if (local.size() >= 2 && local[1] == L'|') local[1] = L':';
        } else {
            local = L"\\\\" + authority + ReplaceChar(decoded, L'/', L'\\');
        }
    } else {
        size_t k = rest.find_first_not_of(L'/');
        std::wstring t = k == std::wstring::npos ? std::wstring() : rest.substr(k);
        local = ReplaceChar(UnescapeDataString(t), L'/', L'\\');
    }
    MediaUriInfo i;
    i.kind = MediaSourceKind::FileUri;
    i.original = s;
    i.scheme = L"file";
    i.localPath = local;
    i.extension = ExtensionOfPath(local);
    return i;
}

}

namespace WinPath {

std::wstring ToLower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)std::towlower(c);
    return s;
}

static size_t RootLength(const std::wstring& p) {
    if (StartsWith(p, L"\\\\?\\") || StartsWith(p, L"\\\\.\\")) {
        if (p.size() >= 6 && p[5] == L':') return p.size() >= 7 && IsSep(p[6]) ? 7 : 6;
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
    if (p.size() >= 2 && IsAsciiLetter(p[0]) && p[1] == L':') return p.size() >= 3 && IsSep(p[2]) ? 3 : 2;
    if (!p.empty() && IsSep(p[0])) return 1;
    return 0;
}

std::wstring GetPathRoot(const std::wstring& p) { return p.substr(0, RootLength(p)); }

std::wstring GetDirectoryName(const std::wstring& p) {
    size_t rootLen = RootLength(p);
    if (p.size() <= rootLen) return L"";
    size_t last = p.find_last_of(L"\\/");
    if (last == std::wstring::npos || last < rootLen) return p.substr(0, rootLen);
    size_t end = last;
    while (end > rootLen && IsSep(p[end - 1])) --end;
    return p.substr(0, end);
}

std::wstring Combine(const std::wstring& a, const std::wstring& b) {
    if (b.empty()) return a;
    if (a.empty()) return b;
    if (IsSep(b[0]) || (b.size() >= 2 && IsAsciiLetter(b[0]) && b[1] == L':')) return b;
    wchar_t last = a.back();
    if (IsSep(last) || last == L':') return a + b;
    return a + L'\\' + b;
}

std::wstring GetFullPath(const std::wstring& p) { return fileio::FullPath(p); }

std::wstring GetExtension(const std::wstring& p) {
    size_t sep = p.find_last_of(L"\\/:");
    size_t dot = p.rfind(L'.');
    if (dot == std::wstring::npos || (sep != std::wstring::npos && dot < sep)) return L"";
    if (dot + 1 >= p.size()) return L"";
    return p.substr(dot);
}

}

bool MediaUriInfo::IsUnc() const {
    return localPath && StartsWith(*localPath, L"\\\\") && !StartsWith(*localPath, L"\\\\?\\");
}
bool MediaUriInfo::HasPlaylistExtension() const { return MediaUri::IsPlaylistExtension(extension); }

namespace MediaUri {

bool IsPlaylistExtension(std::wstring_view ext) {
    std::wstring e = WinPath::ToLower(std::wstring(ext));
    return e == L".m3u" || e == L".m3u8";
}

bool IsPlaylistContentType(std::wstring_view contentType) {
    if (Trim(contentType).empty()) return false;
    size_t semi = contentType.find(L';');
    std::wstring ct = WinPath::ToLower(Trim(contentType.substr(0, semi)));
    return ct == L"application/vnd.apple.mpegurl" || ct == L"application/x-mpegurl" || ct == L"application/mpegurl"
        || ct == L"audio/mpegurl" || ct == L"audio/x-mpegurl";
}

MediaUriInfo Parse(std::wstring_view input) {
    std::wstring s = Trim(TrimQuotes(Trim(input)));
    if (s.empty()) return MediaUriInfo{};

    if ((s.size() >= 2 && IsAsciiLetter(s[0]) && s[1] == L':' && (s.size() == 2 || s[2] == L'\\' || s[2] == L'/'))
        || StartsWith(s, L"\\\\") || StartsWith(s, L"//"))
        return Local(s);

    size_t colon = s.find(L':');
    if (colon != std::wstring::npos && colon > 1) {
        std::wstring scheme = s.substr(0, colon);
        if (IsSchemeName(scheme) && s.size() > colon + 1) {
            std::wstring lower = WinPath::ToLower(scheme);
            if (lower == L"file") return FromFileUri(s);
            std::optional<MediaSourceKind> kind;
            if (lower == L"http") kind = MediaSourceKind::Http;
            else if (lower == L"https") kind = MediaSourceKind::Https;
            else if (IsNetworkScheme(lower) || StartsWith(std::wstring_view(s).substr(colon + 1), L"//")) kind = MediaSourceKind::Network;
            if (kind) {
                MediaUriInfo i;
                i.kind = *kind;
                i.original = s;
                i.scheme = lower;
                i.extension = ExtensionOfUrl(s);
                return i;
            }
        }
    }
    return Local(s);
}

}

std::wstring MediaItem::DisplayTitle() const {
    if (!Trim(title).empty()) return title;
    std::wstring u = url;
    size_t q = u.find_first_of(L"?#");
    if (q != std::wstring::npos) u.resize(q);
    while (!u.empty() && (u.back() == L'/' || u.back() == L'\\')) u.pop_back();
    size_t i = u.find_last_of(L"/\\");
    std::wstring name = i != std::wstring::npos ? u.substr(i + 1) : u;
    std::wstring ext = WinPath::GetExtension(name);
    std::wstring noExt = name.substr(0, name.size() - ext.size());
    if (noExt.empty()) return name.empty() ? url : name;
    return noExt;
}

}
