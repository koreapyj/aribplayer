#include "wvptest.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "wvp/PlaylistSort.h"

using namespace wvp;

namespace {
using Order = std::vector<int>;
}

TEST_CASE(Sort_NaturalFileNameCaseInsensitive) {
    std::vector<MediaItem> v{MediaItem("C:/a/ep10.mkv"), MediaItem("C:/a/Ep2.mkv"), MediaItem("C:/a/ep1.mkv")};
    CHECK((SortOrder(v, SortKey::FileName) == Order{2, 1, 0}));
    CHECK((SortOrder(v, SortKey::FileName, true) == Order{0, 1, 2}));
    CHECK((ReverseOrder(3) == Order{2, 1, 0}));
}

TEST_CASE(Sort_TitleExtensionFolderStable) {
    std::vector<MediaItem> v{MediaItem("d/b.mp4", "Zed"), MediaItem("c/a.MKV"), MediaItem("c/a.mp4")};
    CHECK((SortOrder(v, SortKey::Title) == Order{1, 2, 0}));     // "a" == "a" keeps original order
    CHECK((SortOrder(v, SortKey::Extension) == Order{1, 0, 2}));   // .mkv < .mp4, the two .mp4 stay in order
    CHECK((SortOrder(v, SortKey::Folder) == Order{1, 2, 0}));
}

TEST_CASE(Sort_SizeDateDurationUnknownLast) {
    std::vector<MediaItem> v{MediaItem("a", "", 30.0), MediaItem("b"), MediaItem("c", "", 10.0)};
    std::map<std::string, FileMeta> m{{"a", {20, 5}}, {"c", {10, 9}}};
    FileMetaProvider p = [&](const std::string& u) { auto i = m.find(u); return i == m.end() ? FileMeta{} : i->second; };
    CHECK((SortOrder(v, SortKey::Size, false, p) == Order{2, 0, 1}));
    CHECK((SortOrder(v, SortKey::Size, true, p) == Order{0, 2, 1}));
    CHECK((SortOrder(v, SortKey::Date, false, p) == Order{0, 2, 1}));
    CHECK((SortOrder(v, SortKey::Duration) == Order{2, 0, 1}));
}

TEST_CASE(Sort_RandomSeeded) {
    std::vector<MediaItem> v;
    for (int i = 0; i < 8; i++) v.emplace_back("f" + std::to_string(i));
    Order a = SortOrder(v, SortKey::Random, false, {}, 5), b = SortOrder(v, SortKey::Random, false, {}, 5);
    CHECK(a == b);
    Order sorted = a;
    std::sort(sorted.begin(), sorted.end());
    CHECK((sorted == Order{0, 1, 2, 3, 4, 5, 6, 7}));
}
