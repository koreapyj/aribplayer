#include "wvptest.h"

#include <string>

#include "wvp/PlaylistStore.h"
#include "TempDir.h"

using namespace wvp;
using wvptest::TempDir;

TEST_CASE(PlaylistStore_Win32_rename_collides_case_insensitively) {
    TempDir t;
    PlaylistStore s(t.path);
    REQUIRE(s.Save("One", {}));
    REQUIRE(s.Save("Two", {}));
    std::string err;
    CHECK(!s.Rename("One", "two", &err));
    CHECK(!err.empty());
    CHECK(s.Exists("One"));
}
