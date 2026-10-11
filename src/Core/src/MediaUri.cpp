#include "wvp/MediaUri.h"
#include "wvp/MediaItem.h"
#include "wvp/Platform.h"
#include "wvp/Utf8.h"

namespace wvp {

namespace {

bool IsAsciiLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool IsAsciiLetterOrDigit(char c) { return IsAsciiLetter(c) || (c >= '0' && c <= '9'); }
bool StartsWith(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

std::string Trim(std::string_view s) { return std::string(utf8::Trim(s)); }

std::string TrimQuotes(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && s[b] == '"') ++b;
    while (e > b && s[e - 1] == '"') --e;
    return std::string(s.substr(b, e - b));
}

bool IsNetworkScheme(const std::string& s) {
    static const char* const names[] = {
        "rtmp", "rtmps", "rtmpe", "rtmpt", "rtmpte", "rtmpts", "rtsp", "rtsps", "udp", "rtp", "srt", "tcp", "tls",
        "mms", "mmsh", "mmst", "ftp", "sftp", "rist", "unix", "tcp4", "udplite", "icecast", "gopher", "gophers", "sap"};
    for (auto n : names) if (s == n) return true;
    return false;
}

bool IsSchemeName(std::string_view s) {
    if (s.empty() || !IsAsciiLetter(s[0])) return false;
    for (char c : s) if (!(IsAsciiLetterOrDigit(c) || c == '+' || c == '-' || c == '.')) return false;
    return true;
}

int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string ExtensionOfPath(const std::string& path) {
    return utf8::ToLowerAscii(platform::Extension(path));
}

std::string ExtensionOfUrl(const std::string& url) {
    size_t start = url.find("://");
    std::string p = start != std::string::npos ? url.substr(start + 3) : url.substr(url.find(':') + 1);
    size_t q = p.find_first_of("?#");
    if (q != std::string::npos) p.resize(q);
    size_t firstSlash = p.find('/');
    if (start != std::string::npos) p = firstSlash != std::string::npos ? p.substr(firstSlash) : "";
    size_t slash = p.rfind('/');
    std::string name = slash != std::string::npos ? p.substr(slash + 1) : p;
    size_t dot = name.rfind('.');
    return dot != std::string::npos && dot + 1 < name.size() ? utf8::ToLowerAscii(name.substr(dot)) : "";
}

MediaUriInfo Local(const std::string& s) {
    MediaUriInfo i;
    i.kind = MediaSourceKind::LocalPath;
    i.original = s;
    i.localPath = platform::NativePath(s);
    i.extension = ExtensionOfPath(*i.localPath);
    return i;
}

MediaUriInfo FromFileUri(const std::string& s) {
    MediaUriInfo i;
    i.kind = MediaSourceKind::FileUri;
    i.original = s;
    i.scheme = "file";
    i.localPath = platform::FileUriToPath(s);
    i.extension = ExtensionOfPath(*i.localPath);
    return i;
}

}

bool MediaUriInfo::IsUnc() const {
    return localPath && StartsWith(*localPath, "\\\\") && !StartsWith(*localPath, "\\\\?\\");
}
bool MediaUriInfo::HasPlaylistExtension() const { return MediaUri::IsPlaylistExtension(extension); }

namespace MediaUri {

std::string PercentDecode(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && HexVal(s[i + 1]) >= 0 && HexVal(s[i + 2]) >= 0) {
            out.push_back((char)(HexVal(s[i + 1]) * 16 + HexVal(s[i + 2])));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return utf8::Lossy(out);
}

bool IsPlaylistExtension(std::string_view ext) {
    std::string e = utf8::ToLowerAscii(std::string(ext));
    return e == ".m3u" || e == ".m3u8";
}

bool IsPlaylistContentType(std::string_view contentType) {
    if (Trim(contentType).empty()) return false;
    size_t semi = contentType.find(';');
    std::string ct = utf8::ToLowerAscii(Trim(contentType.substr(0, semi)));
    return ct == "application/vnd.apple.mpegurl" || ct == "application/x-mpegurl" || ct == "application/mpegurl"
        || ct == "audio/mpegurl" || ct == "audio/x-mpegurl";
}

MediaUriInfo Parse(std::string_view input) {
    std::string s = Trim(TrimQuotes(Trim(input)));
    if (s.empty()) return MediaUriInfo{};

    if ((s.size() >= 2 && IsAsciiLetter(s[0]) && s[1] == ':' && (s.size() == 2 || s[2] == '\\' || s[2] == '/'))
        || StartsWith(s, "\\\\") || StartsWith(s, "//"))
        return Local(s);

    size_t colon = s.find(':');
    if (colon != std::string::npos && colon > 1) {
        std::string scheme = s.substr(0, colon);
        if (IsSchemeName(scheme) && s.size() > colon + 1) {
            std::string lower = utf8::ToLowerAscii(scheme);
            if (lower == "file") return FromFileUri(s);
            std::optional<MediaSourceKind> kind;
            if (lower == "http") kind = MediaSourceKind::Http;
            else if (lower == "https") kind = MediaSourceKind::Https;
            else if (IsNetworkScheme(lower) || StartsWith(std::string_view(s).substr(colon + 1), "//")) kind = MediaSourceKind::Network;
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

std::string MediaItem::DisplayTitle() const {
    if (!Trim(title).empty()) return title;
    std::string u = url;
    size_t q = u.find_first_of("?#");
    if (q != std::string::npos) u.resize(q);
    while (!u.empty() && (u.back() == '/' || u.back() == '\\')) u.pop_back();
    size_t i = u.find_last_of("/\\");
    std::string name = i != std::string::npos ? u.substr(i + 1) : u;
    std::string ext = platform::Extension(name);
    std::string noExt = name.substr(0, name.size() - ext.size());
    if (noExt.empty()) return name.empty() ? url : name;
    return noExt;
}

}
