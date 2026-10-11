#include "wvptest.h"

#include <windows.h>

#include <string>
#include <vector>

#include "wvp/M3uParser.h"
#include "wvp/PlaylistLoader.h"
#include "wvp/win32/Utf.h"

using namespace wvp;

TEST_CASE(M3u_ParsesDriveAndUncEntries) {
    const std::string text = "#EXTM3U\n#PLAYLIST:My list\n#EXTVLCOPT:foo=bar\n#EXTINF:123.5,Song One\nC:\\music\\one.mp3\n#EXTINF:-1,Live\nhttp://h/x.ts\nplain.mp4\n";
    M3uPlaylist pl = M3uParser::Parse(text, "D:\\lists\\a.m3u");
    CHECK_EQ(pl.title, std::string("My list"));
    REQUIRE(pl.items.size() == 3);
    CHECK(pl.items[0] == MediaItem("C:\\music\\one.mp3", "Song One", 123.5));
    CHECK_EQ(pl.items[1].title, std::string("Live"));
    CHECK(!pl.items[1].duration.has_value());
    CHECK_EQ(pl.items[1].url, std::string("http://h/x.ts"));
    CHECK_EQ(pl.items[2].url, std::string("D:\\lists\\plain.mp4"));
    CHECK(pl.items[2].title.empty());
}

TEST_CASE(M3u_ResolvesLocalEntriesAgainstPlaylistPath) {
    struct Row { const char* entry; const char* expected; };
    const Row rows[] = {
        {"a/b.mp4", "D:\\lists\\a\\b.mp4"},
        {"a\\b.mp4", "D:\\lists\\a\\b.mp4"},
        {"..\\x.mp4", "D:\\x.mp4"},
        {"./y.mp4", "D:\\lists\\y.mp4"},
        {"E:\\abs\\z.mp4", "E:\\abs\\z.mp4"},
        {"E:/abs/z.mp4", "E:\\abs\\z.mp4"},
        {"\\\\srv\\share\\u.mp4", "\\\\srv\\share\\u.mp4"},
        {"file:///E:/with%20space/f.mp4", "E:\\with space\\f.mp4"},
        {"\\rooted\\r.mp4", "D:\\rooted\\r.mp4"},
    };
    for (auto& r : rows) CHECK_EQ(MediaPathResolver::Resolve(r.entry, "D:\\lists\\a.m3u"), std::string(r.expected));
}

TEST_CASE(M3u_ResolvesAgainstFileUriAndUncBase) {
    CHECK_EQ(MediaPathResolver::Resolve("a.mp4", "file:///D:/lists/p.m3u"), std::string("D:\\lists\\a.mp4"));
    CHECK_EQ(MediaPathResolver::Resolve("a.mp4", "\\\\srv\\sh\\dir\\p.m3u"), std::string("\\\\srv\\sh\\dir\\a.mp4"));
}

TEST_CASE(M3u_DecodeFallsBackToTheAnsiCodePageForInvalidUtf8) {
    std::vector<std::uint8_t> bytes = {'#', 'E', 0xE9, ' ', 'x'};   // not valid UTF-8
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char*)bytes.data(), (int)bytes.size(), nullptr, 0);
    CHECK_EQ(len, 0);
    int n = MultiByteToWideChar(CP_ACP, 0, (const char*)bytes.data(), (int)bytes.size(), nullptr, 0);
    std::wstring ansi((size_t)n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, (const char*)bytes.data(), (int)bytes.size(), ansi.data(), n);
    CHECK(M3uParser::Decode(bytes, true) == ToUtf8(ansi));
    // .m3u8: lenient UTF-8 (replacement char), no ANSI guess
    CHECK(M3uParser::Decode(bytes, false).find("\uFFFD") != std::string::npos);
}
