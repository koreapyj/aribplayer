#include "wvp/M3uParser.h"
#include "wvp/MediaUri.h"
#include "wvp/Utf.h"

#include <cmath>
#include <cwctype>
#include <cwchar>

namespace wvp {

namespace {

std::wstring Trim(std::wstring_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::iswspace(s[b])) ++b;
    while (e > b && std::iswspace(s[e - 1])) --e;
    return std::wstring(s.substr(b, e - b));
}

bool StartsWithI(std::wstring_view s, std::wstring_view p) {
    if (s.size() < p.size()) return false;
    for (size_t i = 0; i < p.size(); ++i)
        if (std::towlower(s[i]) != std::towlower(p[i])) return false;
    return true;
}

bool ContainsI(std::wstring_view s, std::wstring_view p) {
    if (p.empty()) return true;
    for (size_t i = 0; i + p.size() <= s.size(); ++i)
        if (StartsWithI(s.substr(i), p)) return true;
    return false;
}

bool IsAbsoluteWindowsPath(const std::wstring& p) {
    bool drive = p.size() >= 3 && ((p[0] >= L'a' && p[0] <= L'z') || (p[0] >= L'A' && p[0] <= L'Z')) && p[1] == L':' && p[2] == L'\\';
    return drive || (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\');
}


std::wstring RemoveDotSegments(const std::wstring& path) {
    std::vector<std::wstring> out;
    size_t i = 0;
    bool trailingSlash = false;
    while (i <= path.size()) {
        size_t j = path.find(L'/', i);
        std::wstring seg = path.substr(i, j == std::wstring::npos ? std::wstring::npos : j - i);
        bool last = j == std::wstring::npos;
        if (seg == L"..") { if (out.size() > 1) out.pop_back(); trailingSlash = last; }
        else if (seg == L".") { trailingSlash = last; }
        else { out.push_back(seg); trailingSlash = false; }
        if (last) break;
        i = j + 1;
    }
    std::wstring r;
    for (size_t k = 0; k < out.size(); ++k) { if (k) r += L'/'; r += out[k]; }
    if (trailingSlash) r += L'/';
    return r;
}

std::wstring PercentEncodeUrl(const std::wstring& s) {
    std::wstring out;
    std::string u8 = ToUtf8(s);
    static const wchar_t* hex = L"0123456789ABCDEF";
    for (unsigned char c : u8) {
        bool escape = c <= 0x20 || c >= 0x7F || c == '"' || c == '<' || c == '>' || c == '^' || c == '`' || c == '{' || c == '|' || c == '}' || c == '\\';
        if (escape) { out += L'%'; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += (wchar_t)c;
    }
    return out;
}

std::wstring ResolveUrl(const std::wstring& base, const std::wstring& rel) {
    size_t se = base.find(L"://");
    if (se == std::wstring::npos) return L"";
    std::wstring scheme = base.substr(0, se);
    std::wstring rest = base.substr(se + 3);
    size_t cut = rest.find_first_of(L"?#");
    std::wstring beforeQuery = cut == std::wstring::npos ? rest : rest.substr(0, cut);
    std::wstring query = cut != std::wstring::npos && rest[cut] == L'?' ? rest.substr(cut) : L"";
    if (size_t h = query.find(L'#'); h != std::wstring::npos) query.resize(h);
    size_t slash = beforeQuery.find(L'/');
    std::wstring authority = slash == std::wstring::npos ? beforeQuery : beforeQuery.substr(0, slash);
    std::wstring path = slash == std::wstring::npos ? L"/" : beforeQuery.substr(slash);
    if (authority.empty()) return L"";

    std::wstring r = rel;
    size_t hash = r.find(L'#');
    std::wstring frag = hash != std::wstring::npos ? r.substr(hash) : L"";
    if (hash != std::wstring::npos) r.resize(hash);

    std::wstring outPath, outQuery;
    if (r.rfind(L"//", 0) == 0) return PercentEncodeUrl(scheme + L":" + r + frag);
    if (r.empty()) { outPath = path; outQuery = query; }
    else if (r[0] == L'?') { outPath = path; outQuery = r; }
    else {
        size_t q = r.find(L'?');
        std::wstring rpath = q == std::wstring::npos ? r : r.substr(0, q);
        outQuery = q == std::wstring::npos ? L"" : r.substr(q);
        if (rpath[0] == L'/') outPath = RemoveDotSegments(rpath);
        else {
            size_t ls = path.rfind(L'/');
            outPath = RemoveDotSegments(path.substr(0, ls + 1) + rpath);
        }
    }
    return scheme + L"://" + authority + PercentEncodeUrl(outPath) + PercentEncodeUrl(outQuery) + PercentEncodeUrl(frag);
}

}

namespace M3uParser {

bool IsHls(std::wstring_view text) {
    return ContainsI(text, L"#EXT-X-TARGETDURATION") || ContainsI(text, L"#EXT-X-STREAM-INF") || ContainsI(text, L"#EXT-X-MEDIA-SEQUENCE");
}

static std::wstring MbToWide(UINT cp, const char* p, size_t n, bool strict, bool* ok) {
    if (ok) *ok = true;
    if (n == 0) return {};
    int len = MultiByteToWideChar(cp, strict ? MB_ERR_INVALID_CHARS : 0, p, (int)n, nullptr, 0);
    if (len <= 0) { if (ok) *ok = false; return {}; }
    std::wstring w((size_t)len, L'\0');
    MultiByteToWideChar(cp, strict ? MB_ERR_INVALID_CHARS : 0, p, (int)n, w.data(), len);
    return w;
}

std::wstring Decode(const std::uint8_t* b, size_t n, bool allowAnsiFallback) {
    if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF)
        return MbToWide(CP_UTF8, (const char*)b + 3, n - 3, false, nullptr);
    if (n >= 2 && b[0] == 0xFF && b[1] == 0xFE) {
        std::wstring w((n - 2) / 2, L'\0');
        for (size_t i = 0; i < w.size(); ++i) w[i] = (wchar_t)(b[2 + 2 * i] | (b[3 + 2 * i] << 8));
        return w;
    }
    if (n >= 2 && b[0] == 0xFE && b[1] == 0xFF) {
        std::wstring w((n - 2) / 2, L'\0');
        for (size_t i = 0; i < w.size(); ++i) w[i] = (wchar_t)((b[2 + 2 * i] << 8) | b[3 + 2 * i]);
        return w;
    }
    bool ok = false;
    std::wstring w = MbToWide(CP_UTF8, (const char*)b, n, true, &ok);
    if (ok) return w;
    if (!allowAnsiFallback) return MbToWide(CP_UTF8, (const char*)b, n, false, nullptr);
    w = MbToWide(CP_ACP, (const char*)b, n, false, &ok);
    if (ok) return w;
    w.resize(n);
    for (size_t i = 0; i < n; ++i) w[i] = b[i];
    return w;
}

std::wstring Decode(const std::vector<std::uint8_t>& bytes, bool allowAnsiFallback) {
    return Decode(bytes.data(), bytes.size(), allowAnsiFallback);
}

void ParseExtInf(std::wstring_view s, std::optional<double>& duration, std::wstring& title) {
    duration.reset();
    title.clear();
    size_t i = 0;
    while (i < s.size() && s[i] != L',' && s[i] != L' ' && s[i] != L'\t') ++i;
    std::wstring num = Trim(s.substr(0, i));
    if (!num.empty()) {
        wchar_t* end = nullptr;
        double d = std::wcstod(num.c_str(), &end);
        if (end && *end == L'\0' && std::isfinite(d) && d > 0) duration = d;
    }
    bool inQ = false;
    for (size_t k = i; k < s.size(); ++k) {
        if (s[k] == L'"') inQ = !inQ;
        else if (s[k] == L',' && !inQ) { title = Trim(s.substr(k + 1)); return; }
    }
}

M3uPlaylist Parse(std::wstring_view text, std::wstring_view baseLocation) {
    if (!text.empty() && text[0] == 0xFEFF) text.remove_prefix(1);
    M3uPlaylist pl;
    std::wstring pendingTitle;
    std::optional<double> pendingDur;

    size_t pos = 0;
    while (pos <= text.size()) {
        size_t e = text.find_first_of(L"\r\n", pos);
        std::wstring_view rawLine = text.substr(pos, e == std::wstring_view::npos ? std::wstring_view::npos : e - pos);
        if (e == std::wstring_view::npos) pos = text.size() + 1;
        else if (text[e] == L'\r' && e + 1 < text.size() && text[e + 1] == L'\n') pos = e + 2;
        else pos = e + 1;

        std::wstring line = Trim(rawLine);
        if (line.empty()) continue;
        if (line[0] == L'#') {
            if (StartsWithI(line, L"#EXTINF:")) ParseExtInf(std::wstring_view(line).substr(8), pendingDur, pendingTitle);
            else if (StartsWithI(line, L"#PLAYLIST:")) pl.title = Trim(std::wstring_view(line).substr(10));
            continue;
        }
        std::wstring url = MediaPathResolver::Resolve(line, baseLocation);
        pl.items.emplace_back(url, Trim(pendingTitle).empty() ? std::wstring() : pendingTitle, pendingDur);
        pendingTitle.clear();
        pendingDur.reset();
    }
    return pl;
}

}

namespace MediaPathResolver {

std::wstring Resolve(std::wstring_view entryIn, std::wstring_view baseLocation) {
    std::wstring entry = Trim(entryIn);
    MediaUriInfo e = MediaUri::Parse(entry);
    if (e.kind == MediaSourceKind::FileUri) return e.localPath ? *e.localPath : entry;
    if (e.IsRemote()) return entry;
    if (Trim(baseLocation).empty()) return e.localPath ? *e.localPath : entry;

    MediaUriInfo b = MediaUri::Parse(baseLocation);
    std::wstring local = e.localPath ? *e.localPath : entry;
    if (b.IsHttp()) {
        if (IsAbsoluteWindowsPath(local)) return local;
        std::wstring rel = entry;
        for (auto& c : rel) if (c == L'\\') c = L'/';
        std::wstring abs = ResolveUrl(b.original, rel);
        return abs.empty() ? entry : abs;
    }

    std::wstring baseLocal = b.localPath ? *b.localPath : std::wstring(baseLocation);
    if (IsAbsoluteWindowsPath(local)) return local;
    std::wstring dir = WinPath::GetDirectoryName(baseLocal);
    if (!local.empty() && local[0] == L'\\') {
        std::wstring root = WinPath::GetPathRoot(baseLocal);
        size_t k = local.find_first_not_of(L'\\');
        return WinPath::GetFullPath(WinPath::Combine(root, k == std::wstring::npos ? std::wstring() : local.substr(k)));
    }
    return WinPath::GetFullPath(WinPath::Combine(dir, local));
}

}

}
