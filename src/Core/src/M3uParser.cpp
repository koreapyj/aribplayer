#include "wvp/M3uParser.h"
#include "wvp/MediaUri.h"
#include "wvp/Platform.h"
#include "wvp/Utf8.h"

#include <charconv>
#include <cmath>

namespace wvp {

namespace {

bool IsAsciiSpace(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
char AsciiLower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

std::string Trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && IsAsciiSpace(s[b])) ++b;
    while (e > b && IsAsciiSpace(s[e - 1])) --e;
    return std::string(s.substr(b, e - b));
}

bool StartsWithI(std::string_view s, std::string_view p) {
    if (s.size() < p.size()) return false;
    for (size_t i = 0; i < p.size(); ++i)
        if (AsciiLower(s[i]) != AsciiLower(p[i])) return false;
    return true;
}

bool ContainsI(std::string_view s, std::string_view p) {
    if (p.empty()) return true;
    for (size_t i = 0; i + p.size() <= s.size(); ++i)
        if (StartsWithI(s.substr(i), p)) return true;
    return false;
}

std::string RemoveDotSegments(const std::string& path) {
    std::vector<std::string> out;
    size_t i = 0;
    bool trailingSlash = false;
    while (i <= path.size()) {
        size_t j = path.find('/', i);
        std::string seg = path.substr(i, j == std::string::npos ? std::string::npos : j - i);
        bool last = j == std::string::npos;
        if (seg == "..") { if (out.size() > 1) out.pop_back(); trailingSlash = last; }
        else if (seg == ".") { trailingSlash = last; }
        else { out.push_back(seg); trailingSlash = false; }
        if (last) break;
        i = j + 1;
    }
    std::string r;
    for (size_t k = 0; k < out.size(); ++k) { if (k) r += '/'; r += out[k]; }
    if (trailingSlash) r += '/';
    return r;
}

std::string PercentEncodeUrl(const std::string& s) {
    std::string out;
    static const char* hex = "0123456789ABCDEF";
    for (unsigned char c : s) {
        bool escape = c <= 0x20 || c >= 0x7F || c == '"' || c == '<' || c == '>' || c == '^' || c == '`' || c == '{' || c == '|' || c == '}' || c == '\\';
        if (escape) { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += (char)c;
    }
    return out;
}

std::string ResolveUrl(const std::string& base, const std::string& rel) {
    size_t se = base.find("://");
    if (se == std::string::npos) return "";
    std::string scheme = base.substr(0, se);
    std::string rest = base.substr(se + 3);
    size_t cut = rest.find_first_of("?#");
    std::string beforeQuery = cut == std::string::npos ? rest : rest.substr(0, cut);
    std::string query = cut != std::string::npos && rest[cut] == '?' ? rest.substr(cut) : "";
    if (size_t h = query.find('#'); h != std::string::npos) query.resize(h);
    size_t slash = beforeQuery.find('/');
    std::string authority = slash == std::string::npos ? beforeQuery : beforeQuery.substr(0, slash);
    std::string path = slash == std::string::npos ? "/" : beforeQuery.substr(slash);
    if (authority.empty()) return "";

    std::string r = rel;
    size_t hash = r.find('#');
    std::string frag = hash != std::string::npos ? r.substr(hash) : "";
    if (hash != std::string::npos) r.resize(hash);

    std::string outPath, outQuery;
    if (r.rfind("//", 0) == 0) return PercentEncodeUrl(scheme + ":" + r + frag);
    if (r.empty()) { outPath = path; outQuery = query; }
    else if (r[0] == '?') { outPath = path; outQuery = r; }
    else {
        size_t q = r.find('?');
        std::string rpath = q == std::string::npos ? r : r.substr(0, q);
        outQuery = q == std::string::npos ? "" : r.substr(q);
        if (rpath[0] == '/') outPath = RemoveDotSegments(rpath);
        else {
            size_t ls = path.rfind('/');
            outPath = RemoveDotSegments(path.substr(0, ls + 1) + rpath);
        }
    }
    return scheme + "://" + authority + PercentEncodeUrl(outPath) + PercentEncodeUrl(outQuery) + PercentEncodeUrl(frag);
}

}

namespace M3uParser {

bool IsHls(std::string_view text) {
    return ContainsI(text, "#EXT-X-TARGETDURATION") || ContainsI(text, "#EXT-X-STREAM-INF") || ContainsI(text, "#EXT-X-MEDIA-SEQUENCE");
}

std::string Decode(const std::uint8_t* b, size_t n, bool allowAnsiFallback) {
    const std::string_view bytes((const char*)b, n);
    if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) return utf8::Lossy(bytes.substr(3));
    if (n >= 2 && b[0] == 0xFF && b[1] == 0xFE) return utf8::FromUtf16(b + 2, n - 2, false);
    if (n >= 2 && b[0] == 0xFE && b[1] == 0xFF) return utf8::FromUtf16(b + 2, n - 2, true);
    if (utf8::IsValid(bytes)) return std::string(bytes);
    if (!allowAnsiFallback) return utf8::Lossy(bytes);
    if (auto ansi = platform::DecodeSystemCodePage(bytes)) return std::move(*ansi);
    return utf8::FromLatin1(bytes);
}

std::string Decode(const std::vector<std::uint8_t>& bytes, bool allowAnsiFallback) {
    return Decode(bytes.data(), bytes.size(), allowAnsiFallback);
}

void ParseExtInf(std::string_view s, std::optional<double>& duration, std::string& title) {
    duration.reset();
    title.clear();
    size_t i = 0;
    while (i < s.size() && s[i] != ',' && s[i] != ' ' && s[i] != '\t') ++i;
    std::string num = Trim(s.substr(0, i));
    if (!num.empty()) {
        const char* first = num.data() + (num[0] == '+' ? 1 : 0);
        const char* last = num.data() + num.size();
        double d = 0;
        auto [end, ec] = std::from_chars(first, last, d);
        if (ec == std::errc() && end == last && std::isfinite(d) && d > 0) duration = d;
    }
    bool inQ = false;
    for (size_t k = i; k < s.size(); ++k) {
        if (s[k] == '"') inQ = !inQ;
        else if (s[k] == ',' && !inQ) { title = Trim(s.substr(k + 1)); return; }
    }
}

M3uPlaylist Parse(std::string_view text, std::string_view baseLocation) {
    if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    M3uPlaylist pl;
    std::string pendingTitle;
    std::optional<double> pendingDur;

    size_t pos = 0;
    while (pos <= text.size()) {
        size_t e = text.find_first_of("\r\n", pos);
        std::string_view rawLine = text.substr(pos, e == std::string_view::npos ? std::string_view::npos : e - pos);
        if (e == std::string_view::npos) pos = text.size() + 1;
        else if (text[e] == '\r' && e + 1 < text.size() && text[e + 1] == '\n') pos = e + 2;
        else pos = e + 1;

        std::string line = Trim(rawLine);
        if (line.empty()) continue;
        if (line[0] == '#') {
            if (StartsWithI(line, "#EXTINF:")) ParseExtInf(std::string_view(line).substr(8), pendingDur, pendingTitle);
            else if (StartsWithI(line, "#PLAYLIST:")) pl.title = Trim(std::string_view(line).substr(10));
            continue;
        }
        std::string url = MediaPathResolver::Resolve(line, baseLocation);
        pl.items.emplace_back(url, Trim(pendingTitle).empty() ? std::string() : pendingTitle, pendingDur);
        pendingTitle.clear();
        pendingDur.reset();
    }
    return pl;
}

}

namespace MediaPathResolver {

std::string Resolve(std::string_view entryIn, std::string_view baseLocation) {
    std::string entry = Trim(entryIn);
    MediaUriInfo e = MediaUri::Parse(entry);
    if (e.kind == MediaSourceKind::FileUri) return e.localPath ? *e.localPath : entry;
    if (e.IsRemote()) return entry;
    if (Trim(baseLocation).empty()) return e.localPath ? *e.localPath : entry;

    MediaUriInfo b = MediaUri::Parse(baseLocation);
    std::string local = e.localPath ? *e.localPath : entry;
    if (b.IsHttp()) {
        const bool urlRooted = entry.size() > 0 && entry[0] == '/' && !(entry.size() > 1 && entry[1] == '/');
        if (platform::IsAbsolutePath(local) && !urlRooted) return local;
        std::string rel = entry;
        for (auto& c : rel) if (c == '\\') c = '/';
        std::string abs = ResolveUrl(b.original, rel);
        return abs.empty() ? entry : abs;
    }

    std::string baseLocal = b.localPath ? *b.localPath : std::string(baseLocation);
    if (platform::IsAbsolutePath(local)) return local;
    std::string dir = platform::DirectoryName(baseLocal);
    if (!local.empty() && local[0] == '\\') {
        std::string root = platform::PathRoot(baseLocal);
        size_t k = local.find_first_not_of('\\');
        return platform::FullPath(platform::Combine(root, k == std::string::npos ? std::string() : local.substr(k)));
    }
    return platform::FullPath(platform::Combine(dir, local));
}

}

}
