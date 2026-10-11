#include "wvptest.h"

#include <string>
#include <vector>

#include "wvp/M3uParser.h"
#include "wvp/M3uWriter.h"
#include "wvp/MediaItem.h"

using namespace wvp;

namespace {

bool Contains(const std::string& h, const std::string& n) { return h.find(n) != std::string::npos; }

}

TEST_CASE(M3uWriter_round_trips_drive_and_unc_paths) {
    std::vector<MediaItem> items = {
        MediaItem("C:\\media\\one.mp4", "One, with comma", 61.4),
        MediaItem("http://h/two.ts", "\ub450\ubc88\uc9f8", std::nullopt),
        MediaItem("\\\\srv\\share\\three.mkv", "Three\nline", 5.0),
    };
    std::string text = M3uWriter::ToString(items);
    auto back = M3uParser::Parse(text, "Z:\\any\\x.m3u8").items;
    REQUIRE(back.size() == 3);
    CHECK_EQ(back[0].url, "C:\\media\\one.mp4");
    CHECK_EQ(back[1].url, "http://h/two.ts");
    CHECK_EQ(back[2].url, "\\\\srv\\share\\three.mkv");
}

TEST_CASE(M3uWriter_empty_title_uses_display_title) {
    std::string text = M3uWriter::ToString({ MediaItem("C:\\media\\My Movie.mkv") });
    CHECK(Contains(text, "#EXTINF:-1,My Movie\r\n"));
}

TEST_CASE(M3uWriter_get_relative_path) {
    CHECK_EQ(M3uWriter::GetRelativePath("C:\\a\\b", "C:\\a\\b\\c\\d.mp4"), "c\\d.mp4");
    CHECK_EQ(M3uWriter::GetRelativePath("C:\\a\\b", "C:\\a\\x.mp4"), "..\\x.mp4");
    CHECK_EQ(M3uWriter::GetRelativePath("C:\\A\\B", "c:\\a\\b\\q.mp4"), "q.mp4");
    CHECK_EQ(M3uWriter::GetRelativePath("C:\\a", "D:\\a\\q.mp4"), "D:\\a\\q.mp4");
}
