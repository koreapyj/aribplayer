#include "wvptest.h"

#include <windows.h>

#include <string>
#include <vector>

#include "wvp/JsonWriter.h"
#include "wvp/M3uParser.h"
#include "wvp/M3uWriter.h"
#include "wvp/MediaUri.h"
#include "wvp/PlaylistLoader.h"
#include "wvp/Utf.h"

using namespace wvp;

namespace {

void RemoveTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveTree(dir + L"\\" + n);
            else DeleteFileW((dir + L"\\" + n).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

struct TempDir {
    std::wstring path;
    TempDir() {
        static int counter = 0;
        wchar_t buf[MAX_PATH + 1];
        GetTempPathW(MAX_PATH, buf);
        path = std::wstring(buf) + L"wvp_m3uw_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()) + L"_" + std::to_wstring(counter++);
        CreateDirectoryW(path.c_str(), nullptr);
    }
    ~TempDir() { RemoveTree(path); }
    std::wstring File(const std::wstring& n) const { return path + L"\\" + n; }
};

bool Contains(const std::wstring& h, const std::wstring& n) { return h.find(n) != std::wstring::npos; }

}

TEST_CASE(M3uWriter_round_trips) {
    std::vector<MediaItem> items = {
        MediaItem(L"C:\\media\\one.mp4", L"One, with comma", 61.4),
        MediaItem(L"http://h/two.ts", L"\xb450\xbc88\xc9f8", std::nullopt),
        MediaItem(L"\\\\srv\\share\\three.mkv", L"Three\nline", 5.0),
    };
    std::wstring text = M3uWriter::ToString(items);
    CHECK(text.compare(0, 9, L"#EXTM3U\r\n") == 0);
    CHECK(Contains(text, L"#EXTINF:61,One, with comma\r\n"));
    CHECK(Contains(text, L"#EXTINF:-1,\xb450\xbc88\xc9f8\r\n"));
    auto back = M3uParser::Parse(text, L"Z:\\any\\x.m3u8").items;
    REQUIRE(back.size() == 3);
    CHECK_EQ(back[0].url, L"C:\\media\\one.mp4");
    CHECK_EQ(back[0].title, L"One, with comma");
    CHECK(back[0].duration.has_value() && *back[0].duration == 61.0);
    CHECK_EQ(back[1].url, L"http://h/two.ts");
    CHECK_EQ(back[1].title, L"\xb450\xbc88\xc9f8");
    CHECK(!back[1].duration.has_value());
    CHECK_EQ(back[2].url, L"\\\\srv\\share\\three.mkv");
    CHECK_EQ(back[2].title, L"Three line");
}

TEST_CASE(M3uWriter_empty_title_uses_display_title) {
    std::wstring text = M3uWriter::ToString({ MediaItem(L"C:\\media\\My Movie.mkv") });
    CHECK(Contains(text, L"#EXTINF:-1,My Movie\r\n"));
}

TEST_CASE(M3uWriter_saves_utf8_without_bom_and_relative_paths) {
    TempDir t;
    std::wstring media = t.path + L"\\sub\\a.mp4";
    std::wstring file = t.File(L"q.m3u8");
    REQUIRE(M3uWriter::Save(file, { MediaItem(media, L"A", 3.0) }, true));
    std::string bytes;
    REQUIRE(fileio::ReadAllBytes(file, bytes));
    REQUIRE(!bytes.empty());
    CHECK((unsigned char)bytes[0] == '#');
    CHECK(bytes.find("sub\\a.mp4") != std::string::npos);
    CHECK(bytes.find(ToUtf8(t.path)) == std::string::npos);
    auto back = M3uParser::Parse(ToWide(bytes), file);
    REQUIRE(back.items.size() == 1);
    CHECK_EQ(back.items[0].url, media);
    CHECK(GetFileAttributesW((file + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES);
}

TEST_CASE(M3uWriter_absolute_when_not_relative) {
    TempDir t;
    std::wstring media = t.path + L"\\sub\\a.mp4";
    std::wstring file = t.File(L"abs.m3u8");
    REQUIRE(M3uWriter::Save(file, { MediaItem(media, L"A") }, false));
    std::string bytes;
    REQUIRE(fileio::ReadAllBytes(file, bytes));
    CHECK(bytes.find(ToUtf8(media)) != std::string::npos);
}

TEST_CASE(M3uWriter_overwrites_existing_file_atomically) {
    TempDir t;
    std::wstring file = t.File(L"o.m3u8");
    REQUIRE(M3uWriter::Save(file, { MediaItem(L"http://a/1.mp4", L"First") }));
    REQUIRE(M3uWriter::Save(file, { MediaItem(L"http://a/2.mp4", L"Second"), MediaItem(L"http://a/3.mp4", L"Third") }));
    std::string bytes;
    REQUIRE(fileio::ReadAllBytes(file, bytes));
    auto back = M3uParser::Parse(ToWide(bytes), file).items;
    CHECK_EQ(back.size(), (size_t)2);
    CHECK_EQ(back[0].title, L"Second");
}

TEST_CASE(M3uWriter_relative_paths_unicode_and_parent_dirs) {
    TempDir t;
    std::wstring sub = t.path + L"\\\xb4e4\xc545";
    CreateDirectoryW(sub.c_str(), nullptr);
    std::wstring file = sub + L"\\list.m3u8";
    std::wstring inside = sub + L"\\\xc601\xc0c1\\\x65e5\x672c.mp4";
    std::wstring sibling = t.path + L"\\other\\b.mp4";
    std::wstring text = M3uWriter::ToString({ MediaItem(inside, L"in"), MediaItem(sibling, L"out"), MediaItem(L"https://x/y.mp4", L"url") }, sub);
    CHECK(Contains(text, L"\r\n\xc601\xc0c1\\\x65e5\x672c.mp4\r\n"));
    CHECK(Contains(text, L"\r\n..\\other\\b.mp4\r\n"));
    CHECK(Contains(text, L"\r\nhttps://x/y.mp4\r\n"));
    auto back = M3uParser::Parse(text, file).items;
    REQUIRE(back.size() == 3);
    CHECK_EQ(back[0].url, inside);
    CHECK_EQ(back[1].url, sibling);
    CHECK_EQ(back[2].url, L"https://x/y.mp4");
}

TEST_CASE(M3uWriter_get_relative_path) {
    CHECK_EQ(M3uWriter::GetRelativePath(L"C:\\a\\b", L"C:\\a\\b\\c\\d.mp4"), L"c\\d.mp4");
    CHECK_EQ(M3uWriter::GetRelativePath(L"C:\\a\\b", L"C:\\a\\x.mp4"), L"..\\x.mp4");
    CHECK_EQ(M3uWriter::GetRelativePath(L"C:\\A\\B", L"c:\\a\\b\\q.mp4"), L"q.mp4");
    CHECK_EQ(M3uWriter::GetRelativePath(L"C:\\a", L"D:\\a\\q.mp4"), L"D:\\a\\q.mp4");
}

TEST_CASE(M3uWriter_loads_through_PlaylistLoader) {
    TempDir t;
    std::wstring file = t.File(L"pl.m3u8");
    std::wstring media = t.path + L"\\v.mp4";
    REQUIRE(M3uWriter::Save(file, { MediaItem(media, L"T\x00e9st \x2603", 12.0), MediaItem(L"http://h/s.ts", L"Stream") }, true));
    PlaylistLoader loader;
    auto r = loader.Load(file);
    CHECK(r.isPlaylist);
    REQUIRE(r.items.size() == 2);
    CHECK_EQ(r.items[0].url, media);
    CHECK_EQ(r.items[0].title, L"T\x00e9st \x2603");
    CHECK(r.items[0].duration.has_value() && *r.items[0].duration == 12.0);
    CHECK_EQ(r.items[1].url, L"http://h/s.ts");
}
