#include "wvptest.h"

#include "wvp/MediaItem.h"
#include "wvp/MediaUri.h"
#include "wvp/Platform.h"

using namespace wvp;

TEST_CASE(MediaUri_Win32_FileUriAndUncConversions) {
    CHECK_EQ(*MediaUri::Parse("file:///C:/v/a%20b.mp4").localPath, std::string("C:\\v\\a b.mp4"));
    MediaUriInfo unc = MediaUri::Parse("file://srv/share/x.mp4");
    CHECK_EQ(*unc.localPath, std::string("\\\\srv\\share\\x.mp4"));
    CHECK(unc.IsUnc());
    CHECK_EQ(MediaUri::Parse("C:/a/b.mp4").FfmpegInput(), std::string("C:\\a\\b.mp4"));
    CHECK_EQ(MediaUri::Parse("rtmp://h/x").FfmpegInput(), std::string("rtmp://h/x"));
}

TEST_CASE(MediaUri_Win32_PathHelpers) {
    using namespace wvp::platform;
    CHECK_EQ(DirectoryName("D:\\lists\\a.m3u"), std::string("D:\\lists"));
    CHECK_EQ(DirectoryName("D:\\a.m3u"), std::string("D:\\"));
    CHECK_EQ(DirectoryName("\\\\srv\\sh\\dir\\p.m3u"), std::string("\\\\srv\\sh\\dir"));
    CHECK_EQ(DirectoryName("p.m3u"), std::string(""));
    CHECK_EQ(PathRoot("D:\\lists\\a.m3u"), std::string("D:\\"));
    CHECK_EQ(PathRoot("\\\\srv\\sh\\dir\\p.m3u"), std::string("\\\\srv\\sh\\"));
    CHECK_EQ(Combine("D:\\lists", "a\\b.mp4"), std::string("D:\\lists\\a\\b.mp4"));
    CHECK_EQ(Combine("D:\\lists", "E:\\x.mp4"), std::string("E:\\x.mp4"));
    CHECK_EQ(Extension("C:\\a.b\\c"), std::string(""));
    CHECK_EQ(Extension("c.tar.gz"), std::string(".gz"));
}
