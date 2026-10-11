#include "wvp/M3uWriter.h"

#include <algorithm>
#include <cmath>

#include "wvp/MediaUri.h"
#include "wvp/Platform.h"
#include "wvp/Utf8.h"

namespace wvp::M3uWriter {

namespace {

std::string OneLine(std::string s) {
    for (auto& c : s) if (c == '\r' || c == '\n') c = ' ';
    return std::string(utf8::Trim(s));
}

bool IsRooted(const std::string& p) {
    if (p.empty()) return false;
    if (p[0] == '\\' || p[0] == '/') return true;
    return p.size() >= 2 && p[1] == ':';
}

bool EqNoCase(const std::string& a, const std::string& b) { return utf8::ToLowerAscii(a) == utf8::ToLowerAscii(b); }

std::vector<std::string> Segments(const std::string& rest) {
    std::vector<std::string> v;
    size_t i = 0;
    while (i < rest.size()) {
        size_t j = rest.find_first_of("\\/", i);
        if (j == std::string::npos) j = rest.size();
        if (j > i) v.push_back(rest.substr(i, j - i));
        i = j + 1;
    }
    return v;
}

std::string Location(const std::string& url, const std::string& relativeTo) {
    if (relativeTo.empty()) return url;
    MediaUriInfo info = MediaUri::Parse(url);
    if (!info.localPath) return url;
    const std::string& local = *info.localPath;
    if (!IsRooted(local)) return local;
    std::string baseFull = platform::FullPath(relativeTo);
    if (!EqNoCase(platform::PathRoot(local), platform::PathRoot(baseFull))) return local;
    return GetRelativePath(baseFull, local);
}

}

std::string GetRelativePath(const std::string& relativeToDir, const std::string& path) {
    std::string a = platform::FullPath(relativeToDir), b = platform::FullPath(path);
    std::string ra = platform::PathRoot(a), rb = platform::PathRoot(b);
    if (!EqNoCase(ra, rb)) return b;
    auto sa = Segments(a.substr(ra.size())), sb = Segments(b.substr(rb.size()));
    size_t common = 0;
    while (common < sa.size() && common < sb.size() && EqNoCase(sa[common], sb[common])) common++;
    std::string out;
    for (size_t i = common; i < sa.size(); i++) out = platform::Combine(out, "..");
    for (size_t i = common; i < sb.size(); i++) out = platform::Combine(out, sb[i]);
    return out.empty() ? "." : out;
}

std::string ToString(const std::vector<MediaItem>& items, const std::string& relativeTo) {
    std::string sb = "#EXTM3U\r\n";
    for (const auto& it : items) {
        std::string dur = "-1";
        if (it.duration && *it.duration > 0) dur = std::to_string((long long)std::nearbyint(*it.duration));
        std::string title = OneLine(it.title.empty() ? it.DisplayTitle() : it.title);
        sb += "#EXTINF:" + dur + "," + title + "\r\n";
        sb += Location(it.url, relativeTo) + "\r\n";
    }
    return sb;
}

bool Save(const std::string& path, const std::vector<MediaItem>& items, bool makeRelative) {
    std::string dir = makeRelative ? platform::DirectoryName(platform::FullPath(path)) : std::string();
    return platform::AtomicWriteFile(path, ToString(items, dir));
}

}
