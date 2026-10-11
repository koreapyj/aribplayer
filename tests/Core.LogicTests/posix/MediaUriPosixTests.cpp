#include "wvptest.h"

#include "wvp/M3uParser.h"
#include "wvp/MediaUri.h"
#include "wvp/Platform.h"

using namespace wvp;

TEST_CASE(MediaUri_Posix_FileUriConversions) {
    CHECK_EQ(*MediaUri::Parse("file:///tmp/v/a%20b.mp4").localPath, std::string("/tmp/v/a b.mp4"));
    CHECK_EQ(MediaUri::Parse("/tmp/a/b.mp4").FfmpegInput(), std::string("/tmp/a/b.mp4"));
    CHECK(MediaUri::Parse("/tmp/x.mp4").IsLocal());
}

TEST_CASE(MediaUri_Posix_PathHelpers) {
    using namespace wvp::platform;
    CHECK_EQ(DirectoryName("/lists/a.m3u"), std::string("/lists"));
    CHECK_EQ(DirectoryName("/a.m3u"), std::string("/"));
    CHECK_EQ(DirectoryName("p.m3u"), std::string(""));
    CHECK_EQ(PathRoot("/lists/a.m3u"), std::string("/"));
    CHECK_EQ(Combine("/lists", "a/b.mp4"), std::string("/lists/a/b.mp4"));
    CHECK_EQ(Combine("/lists", "/x.mp4"), std::string("/x.mp4"));
    CHECK(IsAbsolutePath("/tmp/x"));
    CHECK(!IsAbsolutePath("tmp/x"));
    CHECK_EQ(Extension("/a.b/c"), std::string(""));
}

TEST_CASE(M3u_Posix_ResolvesLocalEntriesAgainstPlaylistPath) {
    struct Row { const char* entry; const char* expected; };
    const Row rows[] = {
        {"a/b.mp4", "/lists/a/b.mp4"},
        {"../x.mp4", "/x.mp4"},
        {"./y.mp4", "/lists/y.mp4"},
        {"/abs/z.mp4", "/abs/z.mp4"},
        {"file:///abs/with%20space/f.mp4", "/abs/with space/f.mp4"},
    };
    for (auto& r : rows) CHECK_EQ(MediaPathResolver::Resolve(r.entry, "/lists/a.m3u"), std::string(r.expected));
    CHECK_EQ(MediaPathResolver::Resolve("a.mp4", "file:///lists/p.m3u"), std::string("/lists/a.mp4"));
}
