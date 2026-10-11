#include "wvptest.h"

#include <string>
#include <vector>

#include "wvp/Platform.h"
#include "wvp/M3uParser.h"
#include "wvp/M3uWriter.h"
#include "wvp/MediaUri.h"
#include "wvp/PlaylistLoader.h"
#include "TempDir.h"

using namespace wvp;
using wvptest::TempDir;

namespace {

bool Contains(const std::string& h, const std::string& n) { return h.find(n) != std::string::npos; }

}

TEST_CASE(M3uWriter_round_trips_urls_titles_and_durations) {
    std::vector<MediaItem> items = {
        MediaItem("http://h/one.mp4", "One, with comma", 61.4),
        MediaItem("http://h/two.ts", "두번째", std::nullopt),
        MediaItem("https://h/three.mkv", "Three\nline", 5.0),
    };
    std::string text = M3uWriter::ToString(items);
    CHECK(text.compare(0, 9, "#EXTM3U\r\n") == 0);
    CHECK(Contains(text, "#EXTINF:61,One, with comma\r\n"));
    CHECK(Contains(text, "#EXTINF:-1,두번째\r\n"));
    auto back = M3uParser::Parse(text, "http://h/any/x.m3u8").items;
    REQUIRE(back.size() == 3);
    CHECK_EQ(back[0].url, "http://h/one.mp4");
    CHECK_EQ(back[0].title, "One, with comma");
    CHECK(back[0].duration.has_value() && *back[0].duration == 61.0);
    CHECK_EQ(back[1].url, "http://h/two.ts");
    CHECK_EQ(back[1].title, "두번째");
    CHECK(!back[1].duration.has_value());
    CHECK_EQ(back[2].url, "https://h/three.mkv");
    CHECK_EQ(back[2].title, "Three line");
}

TEST_CASE(M3uWriter_saves_utf8_without_bom_and_relative_paths) {
    TempDir t;
    std::string media = t.File("sub", "a.mp4");
    std::string file = t.File("q.m3u8");
    REQUIRE(M3uWriter::Save(file, { MediaItem(media, "A", 3.0) }, true));
    std::string bytes;
    REQUIRE(platform::ReadAllBytes(file, bytes, 1ull << 30) == platform::ReadResult::Ok);
    REQUIRE(!bytes.empty());
    CHECK((unsigned char)bytes[0] == '#');
    CHECK(bytes.find(platform::Combine("sub", "a.mp4")) != std::string::npos);
    CHECK(bytes.find(t.path) == std::string::npos);
    auto back = M3uParser::Parse(bytes, file);
    REQUIRE(back.items.size() == 1);
    CHECK_EQ(back.items[0].url, media);
    CHECK(!platform::Exists(file + ".tmp"));
}

TEST_CASE(M3uWriter_absolute_when_not_relative) {
    TempDir t;
    std::string media = t.File("sub", "a.mp4");
    std::string file = t.File("abs.m3u8");
    REQUIRE(M3uWriter::Save(file, { MediaItem(media, "A") }, false));
    std::string bytes;
    REQUIRE(platform::ReadAllBytes(file, bytes, 1ull << 30) == platform::ReadResult::Ok);
    CHECK(bytes.find(media) != std::string::npos);
}

TEST_CASE(M3uWriter_overwrites_existing_file_atomically) {
    TempDir t;
    std::string file = t.File("o.m3u8");
    REQUIRE(M3uWriter::Save(file, { MediaItem("http://a/1.mp4", "First") }));
    REQUIRE(M3uWriter::Save(file, { MediaItem("http://a/2.mp4", "Second"), MediaItem("http://a/3.mp4", "Third") }));
    std::string bytes;
    REQUIRE(platform::ReadAllBytes(file, bytes, 1ull << 30) == platform::ReadResult::Ok);
    auto back = M3uParser::Parse(bytes, file).items;
    CHECK_EQ(back.size(), (size_t)2);
    CHECK_EQ(back[0].title, "Second");
}

TEST_CASE(M3uWriter_relative_paths_unicode_and_parent_dirs) {
    TempDir t;
    std::string sub = t.File("들악");
    REQUIRE(platform::EnsureDirectory(sub));
    std::string file = platform::Combine(sub, "list.m3u8");
    std::string insideRelative = platform::Combine("영상", "日本.mp4");
    std::string inside = platform::Combine(sub, insideRelative);
    std::string sibling = t.File("other", "b.mp4");
    std::string text = M3uWriter::ToString({ MediaItem(inside, "in"), MediaItem(sibling, "out"), MediaItem("https://x/y.mp4", "url") }, sub);
    CHECK(Contains(text, "\r\n" + insideRelative + "\r\n"));
    CHECK(Contains(text, "\r\n" + platform::Combine("..", platform::Combine("other", "b.mp4")) + "\r\n"));
    CHECK(Contains(text, "\r\nhttps://x/y.mp4\r\n"));
    auto back = M3uParser::Parse(text, file).items;
    REQUIRE(back.size() == 3);
    CHECK_EQ(back[0].url, inside);
    CHECK_EQ(back[1].url, sibling);
    CHECK_EQ(back[2].url, "https://x/y.mp4");
}

TEST_CASE(M3uWriter_loads_through_PlaylistLoader) {
    TempDir t;
    std::string file = t.File("pl.m3u8");
    std::string media = t.File("v.mp4");
    REQUIRE(M3uWriter::Save(file, { MediaItem(media, "Tést ☃", 12.0), MediaItem("http://h/s.ts", "Stream") }, true));
    PlaylistLoader loader;
    auto r = loader.Load(file);
    CHECK(r.isPlaylist);
    REQUIRE(r.items.size() == 2);
    CHECK_EQ(r.items[0].url, media);
    CHECK_EQ(r.items[0].title, "Tést ☃");
    CHECK(r.items[0].duration.has_value() && *r.items[0].duration == 12.0);
    CHECK_EQ(r.items[1].url, "http://h/s.ts");
}
