#include "wvp/PlaylistSort.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <numeric>
#include <random>

namespace wvp {

namespace {

bool IsSep(wchar_t c) { return c == L'/' || c == L'\\'; }

std::wstring StripQuery(const std::wstring& url) {
    if (url.find(L"://") == std::wstring::npos) return url;
    size_t q = url.find_first_of(L"?#");
    return q == std::wstring::npos ? url : url.substr(0, q);
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)std::towlower(c);
    return s;
}

}

std::wstring UrlFileName(const std::wstring& url) {
    std::wstring u = StripQuery(url);
    while (!u.empty() && IsSep(u.back())) u.pop_back();
    size_t i = u.find_last_of(L"/\\");
    return i == std::wstring::npos ? u : u.substr(i + 1);
}

std::wstring UrlExtension(const std::wstring& url) {
    std::wstring n = UrlFileName(url);
    size_t d = n.find_last_of(L'.');
    return d == std::wstring::npos || d == 0 ? std::wstring() : n.substr(d);
}

std::wstring UrlFolder(const std::wstring& url) {
    std::wstring u = StripQuery(url);
    while (!u.empty() && IsSep(u.back())) u.pop_back();
    size_t i = u.find_last_of(L"/\\");
    return i == std::wstring::npos ? std::wstring() : u.substr(0, i);
}

int NaturalCompare(const std::wstring& a, const std::wstring& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::iswdigit(a[i]) && std::iswdigit(b[j])) {
            size_t si = i, sj = j;
            while (i < a.size() && std::iswdigit(a[i])) i++;
            while (j < b.size() && std::iswdigit(b[j])) j++;
            size_t zi = si, zj = sj;   // skip leading zeros
            while (zi + 1 < i && a[zi] == L'0') zi++;
            while (zj + 1 < j && b[zj] == L'0') zj++;
            size_t li = i - zi, lj = j - zj;
            if (li != lj) return li < lj ? -1 : 1;
            int c = a.compare(zi, li, b, zj, lj);
            if (c != 0) return c < 0 ? -1 : 1;
        } else {
            wchar_t x = (wchar_t)std::towlower(a[i]), y = (wchar_t)std::towlower(b[j]);
            if (x != y) return x < y ? -1 : 1;
            i++; j++;
        }
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return 0;
}

FileMetaProvider DefaultFileMetaProvider() {
    return [](const std::wstring& url) {
        FileMeta m;
        if (url.empty() || url.find(L"://") != std::wstring::npos) return m;
        std::error_code ec;
        std::filesystem::path p(url);
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
    auto text = [&](const std::vector<std::wstring>& k1, const std::vector<std::wstring>* k2 = nullptr) {
        std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) {
            int c = NaturalCompare(k1[(size_t)a], k1[(size_t)b]);
            if (c == 0 && k2) c = NaturalCompare((*k2)[(size_t)a], (*k2)[(size_t)b]);
            return reverse ? c > 0 : c < 0;
        });
    };

    std::vector<std::wstring> k1(items.size()), k2;
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
        for (size_t i = 0; i < items.size(); i++) k1[i] = Lower(UrlExtension(items[i].url));
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
