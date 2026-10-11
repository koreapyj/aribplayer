#include "wvp/PlaylistSort.h"

#include <algorithm>
#include <filesystem>
#include <numeric>
#include <random>

#include "wvp/Platform.h"
#include "wvp/Utf8.h"

namespace wvp {

namespace {

bool IsSep(char c) { return c == '/' || c == '\\'; }

std::string StripQuery(const std::string& url) {
    if (url.find("://") == std::string::npos) return url;
    size_t q = url.find_first_of("?#");
    return q == std::string::npos ? url : url.substr(0, q);
}

// Decimal digits as the Windows CRT's iswdigit classifies them (BMP), so digit runs in any script sort numerically.
bool IsDigit(char32_t c) {
    static constexpr char32_t ranges[][2] = {
        {0x30, 0x39}, {0xB2, 0xB3}, {0xB9, 0xB9}, {0x660, 0x669}, {0x6F0, 0x6F9}, {0x7C0, 0x7C9}, {0x966, 0x96F},
        {0x9E6, 0x9EF}, {0xA66, 0xA6F}, {0xAE6, 0xAEF}, {0xB66, 0xB6F}, {0xBE6, 0xBEF}, {0xC66, 0xC6F}, {0xCE6, 0xCEF},
        {0xD66, 0xD6F}, {0xE50, 0xE59}, {0xED0, 0xED9}, {0xF20, 0xF29}, {0x1040, 0x1049}, {0x1090, 0x1099},
        {0x17E0, 0x17E9}, {0x1810, 0x1819}, {0x1946, 0x194F}, {0x19D0, 0x19D9}, {0x1B50, 0x1B59}, {0x1BB0, 0x1BB9},
        {0x1C40, 0x1C49}, {0x1C50, 0x1C59}, {0xA620, 0xA629}, {0xA8D0, 0xA8D9}, {0xA900, 0xA909}, {0xAA50, 0xAA59},
        {0xFF10, 0xFF19}};
    if (c > 0xFF19) return false;
    for (const auto& r : ranges) if (c >= r[0] && c <= r[1]) return true;
    return false;
}

std::u32string CodePoints(const std::string& s) {
    std::u32string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) out.push_back(utf8::Next(s, i));
    return out;
}

}

std::string UrlFileName(const std::string& url) {
    std::string u = StripQuery(url);
    while (!u.empty() && IsSep(u.back())) u.pop_back();
    size_t i = u.find_last_of("/\\");
    return i == std::string::npos ? u : u.substr(i + 1);
}

std::string UrlExtension(const std::string& url) {
    std::string n = UrlFileName(url);
    size_t d = n.find_last_of('.');
    return d == std::string::npos || d == 0 ? std::string() : n.substr(d);
}

std::string UrlFolder(const std::string& url) {
    std::string u = StripQuery(url);
    while (!u.empty() && IsSep(u.back())) u.pop_back();
    size_t i = u.find_last_of("/\\");
    return i == std::string::npos ? std::string() : u.substr(0, i);
}

int NaturalCompare(const std::string& a8, const std::string& b8) {
    const std::u32string a = CodePoints(a8), b = CodePoints(b8);
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (IsDigit(a[i]) && IsDigit(b[j])) {
            size_t si = i, sj = j;
            while (i < a.size() && IsDigit(a[i])) i++;
            while (j < b.size() && IsDigit(b[j])) j++;
            size_t zi = si, zj = sj;   // skip leading zeros
            while (zi + 1 < i && a[zi] == U'0') zi++;
            while (zj + 1 < j && b[zj] == U'0') zj++;
            size_t li = i - zi, lj = j - zj;
            if (li != lj) return li < lj ? -1 : 1;
            int c = a.compare(zi, li, b, zj, lj);
            if (c != 0) return c < 0 ? -1 : 1;
        } else {
            char32_t x = platform::FoldCase(a[i]), y = platform::FoldCase(b[j]);
            if (x != y) return x < y ? -1 : 1;
            i++; j++;
        }
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return 0;
}

FileMetaProvider DefaultFileMetaProvider() {
    return [](const std::string& url) {
        FileMeta m;
        if (url.empty() || url.find("://") != std::string::npos) return m;
        std::error_code ec;
        const std::filesystem::path p(std::u8string(url.begin(), url.end()));
        auto sz = std::filesystem::file_size(p, ec);
        if (!ec) m.size = (std::int64_t)sz;
        auto t = std::filesystem::last_write_time(p, ec);
        if (!ec) m.modified = (std::int64_t)t.time_since_epoch().count();
        return m;
    };
}

std::vector<int> ReverseOrder(size_t count) {
    std::vector<int> r(count);
    for (size_t i = 0; i < count; i++) r[i] = (int)(count - 1 - i);
    return r;
}

std::vector<int> SortOrder(const std::vector<MediaItem>& items, SortKey key, bool reverse,
                           const FileMetaProvider& metaIn, unsigned randomSeed) {
    std::vector<int> idx(items.size());
    std::iota(idx.begin(), idx.end(), 0);
    if (key == SortKey::Random) {
        std::mt19937 rng(randomSeed);
        for (int i = (int)idx.size() - 1; i > 0; i--)
            std::swap(idx[(size_t)i], idx[(size_t)std::uniform_int_distribution<int>(0, i)(rng)]);
        return idx;
    }

    // Numeric keys: unknown values sort last in both directions; known ones ascend, flipped by `reverse`.
    auto numeric = [&](const std::vector<std::optional<double>>& v) {
        std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) {
            const auto& va = v[(size_t)a];
            const auto& vb = v[(size_t)b];
            if (!va || !vb) return va.has_value() && !vb.has_value();
            return reverse ? *vb < *va : *va < *vb;
        });
    };
    // Text keys: natural, case-insensitive; `second` breaks ties (Folder: file name).
    auto text = [&](const std::vector<std::string>& k1, const std::vector<std::string>* k2 = nullptr) {
        std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) {
            int c = NaturalCompare(k1[(size_t)a], k1[(size_t)b]);
            if (c == 0 && k2) c = NaturalCompare((*k2)[(size_t)a], (*k2)[(size_t)b]);
            return reverse ? c > 0 : c < 0;
        });
    };

    std::vector<std::string> k1(items.size()), k2;
    switch (key) {
    case SortKey::Title:
        for (size_t i = 0; i < items.size(); i++) k1[i] = items[i].DisplayTitle();
        text(k1);
        break;
    case SortKey::FileName:
        for (size_t i = 0; i < items.size(); i++) k1[i] = UrlFileName(items[i].url);
        text(k1);
        break;
    case SortKey::Extension:
        for (size_t i = 0; i < items.size(); i++) k1[i] = utf8::ToLowerAscii(UrlExtension(items[i].url));
        text(k1);
        break;
    case SortKey::Folder:
        k2.resize(items.size());
        for (size_t i = 0; i < items.size(); i++) { k1[i] = UrlFolder(items[i].url); k2[i] = UrlFileName(items[i].url); }
        text(k1, &k2);
        break;
    case SortKey::Duration: {
        std::vector<std::optional<double>> v(items.size());
        for (size_t i = 0; i < items.size(); i++) v[i] = items[i].duration;
        numeric(v);
        break;
    }
    case SortKey::Size:
    case SortKey::Date: {
        FileMetaProvider meta = metaIn ? metaIn : DefaultFileMetaProvider();
        std::vector<std::optional<double>> v(items.size());
        for (size_t i = 0; i < items.size(); i++) {
            FileMeta m = meta(items[i].url);
            auto x = key == SortKey::Size ? m.size : m.modified;
            if (x) v[i] = (double)*x;
        }
        numeric(v);
        break;
    }
    case SortKey::Random: break;
    }
    return idx;
}

}
