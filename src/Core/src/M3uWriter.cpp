#include "wvp/M3uWriter.h"

#include <algorithm>
#include <cmath>

#include "wvp/JsonWriter.h"
#include "wvp/MediaUri.h"
#include "wvp/Utf.h"

namespace wvp::M3uWriter {

namespace {

std::wstring OneLine(std::wstring s) {
    for (auto& c : s) if (c == L'\r' || c == L'\n') c = L' ';
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) a++;
    while (b > a && iswspace(s[b - 1])) b--;
    return s.substr(a, b - a);
}

bool IsRooted(const std::wstring& p) {
    if (p.empty()) return false;
    if (p[0] == L'\\' || p[0] == L'/') return true;
    return p.size() >= 2 && p[1] == L':';
}

bool EqNoCase(const std::wstring& a, const std::wstring& b) { return WinPath::ToLower(a) == WinPath::ToLower(b); }

std::vector<std::wstring> Segments(const std::wstring& rest) {
    std::vector<std::wstring> v;
    size_t i = 0;
    while (i < rest.size()) {
        size_t j = rest.find_first_of(L"\\/", i);
        if (j == std::wstring::npos) j = rest.size();
        if (j > i) v.push_back(rest.substr(i, j - i));
        i = j + 1;
    }
    return v;
}

std::wstring Location(const std::wstring& url, const std::wstring& relativeTo) {
    if (relativeTo.empty()) return url;
    MediaUriInfo info = MediaUri::Parse(url);
    if (!info.localPath) return url;
    const std::wstring& local = *info.localPath;
    if (!IsRooted(local)) return local;
    std::wstring baseFull = WinPath::GetFullPath(relativeTo);
    if (!EqNoCase(WinPath::GetPathRoot(local), WinPath::GetPathRoot(baseFull))) return local;
    return GetRelativePath(baseFull, local);
}

}

std::wstring GetRelativePath(const std::wstring& relativeToDir, const std::wstring& path) {
    std::wstring a = WinPath::GetFullPath(relativeToDir), b = WinPath::GetFullPath(path);
    std::wstring ra = WinPath::GetPathRoot(a), rb = WinPath::GetPathRoot(b);
    if (!EqNoCase(ra, rb)) return b;
    auto sa = Segments(a.substr(ra.size())), sb = Segments(b.substr(rb.size()));
    size_t common = 0;
    while (common < sa.size() && common < sb.size() && EqNoCase(sa[common], sb[common])) common++;
    std::wstring out;
    for (size_t i = common; i < sa.size(); i++) out += out.empty() ? L".." : L"\\..";
    for (size_t i = common; i < sb.size(); i++) { if (!out.empty()) out += L'\\'; out += sb[i]; }
    return out.empty() ? L"." : out;
}

std::wstring ToString(const std::vector<MediaItem>& items, const std::wstring& relativeTo) {
    std::wstring sb = L"#EXTM3U\r\n";
    for (const auto& it : items) {
        std::wstring dur = L"-1";
        if (it.duration && *it.duration > 0) dur = std::to_wstring((long long)std::nearbyint(*it.duration));
        std::wstring title = OneLine(it.title.empty() ? it.DisplayTitle() : it.title);
        sb += L"#EXTINF:" + dur + L"," + title + L"\r\n";
        sb += Location(it.url, relativeTo) + L"\r\n";
    }
    return sb;
}

std::string ToUtf8(const std::vector<MediaItem>& items, const std::wstring& relativeTo) {
    return wvp::ToUtf8(ToString(items, relativeTo));
}

bool Save(const std::wstring& path, const std::vector<MediaItem>& items, bool makeRelative) {
    std::wstring dir = makeRelative ? WinPath::GetDirectoryName(WinPath::GetFullPath(path)) : std::wstring();
    return fileio::AtomicWriteFile(path, M3uWriter::ToUtf8(items, dir));
}

}
