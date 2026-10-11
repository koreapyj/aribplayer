#include "wvptest.h"

#include "wvp/MediaItem.h"
#include "wvp/MediaUri.h"
#include "wvp/Platform.h"

using namespace wvp;

TEST_CASE(MediaUri_Classifies) {
    struct Row { const char* input; MediaSourceKind kind; const char* ext; };
    const Row rows[] = {
        {"C:\\v\\a.mp4", MediaSourceKind::LocalPath, ".mp4"},
        {"C:/v/a.m3u8", MediaSourceKind::LocalPath, ".m3u8"},
        {"\\\\srv\\share\\a.TS", MediaSourceKind::LocalPath, ".ts"},
        {"\"C:\\quoted path\\a.mkv\"", MediaSourceKind::LocalPath, ".mkv"},
        {"relative/a.m3u", MediaSourceKind::LocalPath, ".m3u"},
        {"file:///C:/v/a%20b.mp4", MediaSourceKind::FileUri, ".mp4"},
        {"http://h/a.mp4?x=1", MediaSourceKind::Http, ".mp4"},
        {"HTTPS://h/live/index.m3u8#t", MediaSourceKind::Https, ".m3u8"},
        {"rtmp://h/app/stream", MediaSourceKind::Network, ""},
        {"rtsp://h:554/cam", MediaSourceKind::Network, ""},
        {"udp://239.0.0.1:1234", MediaSourceKind::Network, ""},
        {"srt://h:9000?mode=caller", MediaSourceKind::Network, ""},
        {"   ", MediaSourceKind::Empty, ""},
    };
    for (auto& r : rows) {
        MediaUriInfo i = MediaUri::Parse(r.input);
        CHECK_MSG(i.kind == r.kind, "kind for " + wvptest::Str(std::string(r.input)));
        CHECK_EQ(i.extension, std::string(r.ext));
    }
}

TEST_CASE(MediaUri_Flags) {
    CHECK(MediaUri::Parse("http://h/x.m3u8").HasPlaylistExtension());
    CHECK(!MediaUri::Parse("http://h/x.mp4").HasPlaylistExtension());
    CHECK(MediaUri::Parse("http://h/x").IsRemote());
    CHECK(MediaUri::Parse("C:\\x.mp4").IsLocal());
    CHECK(MediaUri::IsPlaylistContentType("application/vnd.apple.mpegurl; charset=utf-8"));
    CHECK(MediaUri::IsPlaylistContentType("audio/x-mpegurl"));
    CHECK(!MediaUri::IsPlaylistContentType("video/mp4"));
    CHECK(!MediaUri::IsPlaylistContentType(""));
}

TEST_CASE(MediaUri_MediaItemDisplayTitle) {
    CHECK_EQ(MediaItem("x", "My Song").DisplayTitle(), std::string("My Song"));
    CHECK_EQ(MediaItem("C:\\a\\clip.mp4").DisplayTitle(), std::string("clip"));
    CHECK_EQ(MediaItem("http://h/p/index.m3u8?x=1").DisplayTitle(), std::string("index"));
}

TEST_CASE(MediaUri_ExtensionOfPaths) {
    CHECK_EQ(wvp::platform::Extension("c.tar.gz"), std::string(".gz"));
    CHECK_EQ(wvp::platform::Extension("p"), std::string(""));
}
