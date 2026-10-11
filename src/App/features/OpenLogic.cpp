#include "features/OpenLogic.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>

#include "LogicTest.h"
#include "Localize.h"
#include "wvp/MediaUri.h"
#include "wvp/win32/Utf.h"

namespace app::open {

namespace {
std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}
std::wstring ExtOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    size_t dot = path.rfind(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return {};
    return Lower(path.substr(dot));
}
std::wstring Join(const std::vector<std::wstring>& v) {
    std::wstring s;
    for (size_t i = 0; i < v.size(); i++) {
        if (i) s += L';';
        s += L"*" + v[i];
    }
    return s;
}
}

const std::vector<std::wstring>& MediaExtensions() {
    static const std::vector<std::wstring> e = {
        L".mp4", L".mkv", L".avi", L".mov", L".wmv", L".webm", L".ts", L".m2ts", L".mts", L".mpg", L".mpeg", L".flv",
        L".m4v", L".3gp", L".ogv", L".vob", L".mp3", L".m4a", L".aac", L".flac", L".wav", L".ogg", L".opus", L".wma", L".mka"};
    return e;
}

std::wstring PlaylistFilterPattern() { return L"*.m3u;*.m3u8"; }
std::wstring MediaFilterPattern() { return Join(MediaExtensions()) + L";" + PlaylistFilterPattern(); }

bool IsMediaFile(const std::wstring& path) {
    auto e = ExtOf(path);
    if (e.empty()) return false;
    const auto& v = MediaExtensions();
    return std::find(v.begin(), v.end(), e) != v.end();
}
bool IsPlaylistFile(const std::wstring& path) {
    auto e = ExtOf(path);
    return e == L".m3u" || e == L".m3u8";
}

std::string PlaylistName(const std::string& source, const std::string& title) {
    if (!title.empty()) return title;
    auto uri = wvp::MediaUri::Parse(source);
    std::string s = uri.localPath ? *uri.localPath : source;
    size_t q = s.find_first_of("?#");
    if (q != std::string::npos) s.resize(q);
    size_t slash = s.find_last_of("\\/");
    std::string name = slash == std::string::npos ? s : s.substr(slash + 1);
    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) name.resize(dot);
    return name.empty() ? "Playlist" : name;
}

bool NaturalLess(const std::wstring& a, const std::wstring& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (iswdigit(a[i]) && iswdigit(b[j])) {
            size_t i2 = i, j2 = j;
            while (i2 < a.size() && iswdigit(a[i2])) i2++;
            while (j2 < b.size() && iswdigit(b[j2])) j2++;
            size_t zi = i, zj = j;
            while (zi < i2 - 1 && a[zi] == L'0') zi++;
            while (zj < j2 - 1 && b[zj] == L'0') zj++;
            size_t la = i2 - zi, lb = j2 - zj;
            if (la != lb) return la < lb;
            int c = a.compare(zi, la, b, zj, lb);
            if (c != 0) return c < 0;
            i = i2;
            j = j2;
            continue;
        }
        wchar_t ca = (wchar_t)towlower(a[i]), cb = (wchar_t)towlower(b[j]);
        if (ca != cb) return ca < cb;
        i++;
        j++;
    }
    if ((a.size() - i) != (b.size() - j)) return (a.size() - i) < (b.size() - j);
    return a < b;   // deterministic tie-break for names differing only by case / leading zeros
}

void NaturalSort(std::vector<std::wstring>& names) { std::sort(names.begin(), names.end(), NaturalLess); }

std::vector<std::wstring> ListMediaFiles(const std::wstring& folder) {
    std::vector<std::wstring> names;
    if (folder.empty()) return names;
    std::wstring base = folder;
    if (base.back() == L'\\' || base.back() == L'/') base.pop_back();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((base + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return names;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (IsMediaFile(fd.cFileName)) names.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    NaturalSort(names);
    for (auto& n : names) n = base + L"\\" + n;
    return names;
}

std::wstring ValidateUrl(const std::wstring& text) {
    auto info = wvp::MediaUri::Parse(wvp::ToUtf8(text));
    if (info.kind == wvp::MediaSourceKind::Empty) return Tr(L"Enter a web address or stream URL.", L"웹 주소 또는 스트림 주소를 입력하세요.");
    if (info.IsLocal()) return Tr(L"Enter a web address (http://, https://, rtmp://, rtsp://, udp://). Use Open file for local files.", L"웹 주소(http://, https://, rtmp://, rtsp://, udp://)를 입력하세요. 로컬 파일은 파일 열기를 사용하세요.");
    if (info.IsRemote()) {
        const std::wstring s = wvp::ToWide(info.original);
        size_t p = s.find(L"://");
        if (p == std::wstring::npos || p + 3 >= s.size()) return Tr(L"The address is incomplete.", L"주소가 완전하지 않습니다.");
        for (wchar_t c : s)
            if (iswspace(c)) return Tr(L"The address must not contain spaces.", L"주소에는 공백을 넣을 수 없습니다.");
        return {};
    }
    return Tr(L"Unsupported address. Use http://, https://, rtmp://, rtsp:// or udp://.", L"지원되지 않는 주소입니다. http://, https://, rtmp://, rtsp:// 또는 udp://를 사용하세요.");
}

std::wstring RelativeTime(std::int64_t nowMs, std::int64_t thenMs) {
    if (thenMs <= 0) return {};
    std::int64_t s = (nowMs - thenMs) / 1000;
    if (s < 60) return Tr(L"Just now", L"방금 전");
    auto plural = [](std::int64_t n, const wchar_t* unit, const wchar_t* koUnit) {
        return TrW(std::to_wstring(n) + L" " + unit + (n == 1 ? L"" : L"s") + L" ago", std::to_wstring(n) + koUnit + L" 전");
    };
    if (s < 3600) return plural(s / 60, L"minute", L"분");
    if (s < 86400) return plural(s / 3600, L"hour", L"시간");
    if (s < 2 * 86400) return Tr(L"Yesterday", L"어제");
    if (s < 30 * 86400) return plural(s / 86400, L"day", L"일");
    if (s < 365 * 86400) return plural(s / (30 * 86400), L"month", L"개월");
    return plural(s / (365 * 86400), L"year", L"년");
}

namespace {
struct TempDir {
    std::wstring path;
    TempDir() {
        wchar_t t[MAX_PATH];
        GetTempPathW(MAX_PATH, t);
        path = std::wstring(t) + L"aribplayer-open-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        CreateDirectoryW(path.c_str(), nullptr);
    }
    void Touch(const std::wstring& name) {
        HANDLE h = CreateFileW((path + L"\\" + name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    void Mkdir(const std::wstring& name) { CreateDirectoryW((path + L"\\" + name).c_str(), nullptr); }
    ~TempDir() {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((path + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                std::wstring n = fd.cFileName;
                if (n == L"." || n == L"..") continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW((path + L"\\" + n).c_str());
                else DeleteFileW((path + L"\\" + n).c_str());
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryW(path.c_str());
    }
};
}

LOGIC_TEST(open_media_filter_and_patterns) {
    LT_CHECK(IsMediaFile(L"C:\\a\\Movie.MP4"));
    LT_CHECK(IsMediaFile(L"x.flac"));
    LT_CHECK(!IsMediaFile(L"notes.txt"));
    LT_CHECK(!IsMediaFile(L"C:\\dir.mp4\\noext"));
    LT_CHECK(!IsMediaFile(L"a.m3u"));
    LT_CHECK(IsPlaylistFile(L"a.M3U8"));
    LT_CHECK(!IsPlaylistFile(L"a.mp4"));
    auto p = MediaFilterPattern();
    LT_CHECK(p.find(L"*.mp4") != std::wstring::npos);
    LT_CHECK(p.find(L"*.m3u8") != std::wstring::npos);
    LT_CHECK(p.find(L"**") == std::wstring::npos);
}

LOGIC_TEST(open_playlist_name_prefers_m3u_title_then_source_name) {
    LT_CHECK_EQ(PlaylistName("C:\\lists\\news.m3u", "News list"), std::string("News list"));
    LT_CHECK_EQ(PlaylistName("C:\\lists\\news.m3u"), std::string("news"));
    LT_CHECK_EQ(PlaylistName("https://example.test/feeds/current.m3u8?token=secret"), std::string("current"));
}

LOGIC_TEST(open_natural_sort) {
    std::vector<std::wstring> v = {L"ep10.mkv", L"Ep2.mkv", L"ep1.mkv", L"B.mkv", L"a.mkv"};
    NaturalSort(v);
    std::vector<std::wstring> want = {L"a.mkv", L"B.mkv", L"ep1.mkv", L"Ep2.mkv", L"ep10.mkv"};
    LT_CHECK(v == want);
    LT_CHECK(NaturalLess(L"a9", L"a10"));
    LT_CHECK(!NaturalLess(L"a10", L"a9"));
    LT_CHECK(!NaturalLess(L"same", L"same"));
}

LOGIC_TEST(open_list_media_files_non_recursive) {
    TempDir d;
    d.Touch(L"b10.mp4");
    d.Touch(L"b2.MKV");
    d.Touch(L"a.mp3");
    d.Touch(L"readme.txt");
    d.Touch(L"list.m3u");
    d.Mkdir(L"sub.mp4");
    auto f = ListMediaFiles(d.path);
    LT_CHECK_EQ(f.size(), (size_t)3);
    if (f.size() == 3) {
        LT_CHECK(f[0] == d.path + L"\\a.mp3");
        LT_CHECK(f[1] == d.path + L"\\b2.MKV");
        LT_CHECK(f[2] == d.path + L"\\b10.mp4");
    }
    LT_CHECK(ListMediaFiles(d.path + L"\\missing").empty());
    LT_CHECK(ListMediaFiles(L"").empty());
}

LOGIC_TEST(open_validate_url) {
    LT_CHECK(ValidateUrl(L"https://example.com/a.mp4").empty());
    LT_CHECK(ValidateUrl(L"http://example.com/live.m3u8").empty());
    LT_CHECK(ValidateUrl(L"rtmp://host/app/stream").empty());
    LT_CHECK(ValidateUrl(L"rtsp://10.0.0.1:554/cam").empty());
    LT_CHECK(ValidateUrl(L"udp://@239.0.0.1:1234").empty());
    LT_CHECK(!ValidateUrl(L"").empty());
    LT_CHECK(!ValidateUrl(L"   ").empty());
    LT_CHECK(!ValidateUrl(L"C:\\movies\\a.mp4").empty());
    LT_CHECK(!ValidateUrl(L"just some words").empty());
    LT_CHECK(!ValidateUrl(L"http://").empty());
    LT_CHECK(!ValidateUrl(L"http://a b/c").empty());
}

LOGIC_TEST(open_relative_time) {
    const std::int64_t now = 1000000000000LL;
    LT_CHECK(RelativeTime(now, now - 5000) == Tr(L"Just now", L"방금 전"));
    LT_CHECK(RelativeTime(now, now - 60000) == L"1 minute ago");
    LT_CHECK(RelativeTime(now, now - 5 * 60000) == L"5 minutes ago");
    LT_CHECK(RelativeTime(now, now - 3600000LL) == L"1 hour ago");
    LT_CHECK(RelativeTime(now, now - 30 * 3600000LL) == Tr(L"Yesterday", L"어제"));
    LT_CHECK(RelativeTime(now, now - 3 * 86400000LL) == L"3 days ago");
    LT_CHECK(RelativeTime(now, now - 70 * 86400000LL) == L"2 months ago");
    LT_CHECK(RelativeTime(now, 0).empty());
}

}
