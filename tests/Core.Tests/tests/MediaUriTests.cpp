#include "wvptest.h"

#include "wvp/MediaItem.h"
#include "wvp/MediaUri.h"

using namespace wvp;

TEST_CASE(MediaUri_Classifies) {
    struct Row { const wchar_t* input; MediaSourceKind kind; const wchar_t* ext; };
    const Row rows[] = {
        {L"C:\\v\\a.mp4", MediaSourceKind::LocalPath, L".mp4"},
        {L"C:/v/a.m3u8", MediaSourceKind::LocalPath, L".m3u8"},
        {L"\\\\srv\\share\\a.TS", MediaSourceKind::LocalPath, L".ts"},
        {L"\"C:\\quoted path\\a.mkv\"", MediaSourceKind::LocalPath, L".mkv"},
        {L"relative/a.m3u", MediaSourceKind::LocalPath, L".m3u"},
        {L"file:///C:/v/a%20b.mp4", MediaSourceKind::FileUri, L".mp4"},
        {L"http://h/a.mp4?x=1", MediaSourceKind::Http, L".mp4"},
        {L"HTTPS://h/live/index.m3u8#t", MediaSourceKind::Https, L".m3u8"},
        {L"rtmp://h/app/stream", MediaSourceKind::Network, L""},
        {L"rtsp://h:554/cam", MediaSourceKind::Network, L""},
        {L"udp://239.0.0.1:1234", MediaSourceKind::Network, L""},
        {L"srt://h:9000?mode=caller", MediaSourceKind::Network, L""},
        {L"   ", MediaSourceKind::Empty, L""},
    };
    for (auto& r : rows) {
        MediaUriInfo i = MediaUri::Parse(r.input);
        CHECK_MSG(i.kind == r.kind, "kind for " + wvptest::Str(std::wstring(r.input)));
        CHECK_EQ(i.extension, std::wstring(r.ext));
    }
}

TEST_CASE(MediaUri_FileUriAndUncConversions) {
    CHECK_EQ(*MediaUri::Parse(L"file:///C:/v/a%20b.mp4").localPath, std::wstring(L"C:\\v\\a b.mp4"));
    MediaUriInfo unc = MediaUri::Parse(L"file://srv/share/x.mp4");
    CHECK_EQ(*unc.localPath, std::wstring(L"\\\\srv\\share\\x.mp4"));
    CHECK(unc.IsUnc());
    CHECK_EQ(MediaUri::Parse(L"C:/a/b.mp4").FfmpegInput(), std::wstring(L"C:\\a\\b.mp4"));
    CHECK_EQ(MediaUri::Parse(L"rtmp://h/x").FfmpegInput(), std::wstring(L"rtmp://h/x"));
}

TEST_CASE(MediaUri_Flags) {
    CHECK(MediaUri::Parse(L"http://h/x.m3u8").HasPlaylistExtension());
    CHECK(!MediaUri::Parse(L"http://h/x.mp4").HasPlaylistExtension());
    CHECK(MediaUri::Parse(L"http://h/x").IsRemote());
    CHECK(MediaUri::Parse(L"C:\\x.mp4").IsLocal());
    CHECK(MediaUri::IsPlaylistContentType(L"application/vnd.apple.mpegurl; charset=utf-8"));
    CHECK(MediaUri::IsPlaylistContentType(L"audio/x-mpegurl"));
    CHECK(!MediaUri::IsPlaylistContentType(L"video/mp4"));
    CHECK(!MediaUri::IsPlaylistContentType(L""));
}

TEST_CASE(MediaUri_MediaItemDisplayTitle) {
    CHECK_EQ(MediaItem(L"x", L"My Song").DisplayTitle(), std::wstring(L"My Song"));
    CHECK_EQ(MediaItem(L"C:\\a\\clip.mp4").DisplayTitle(), std::wstring(L"clip"));
    CHECK_EQ(MediaItem(L"http://h/p/index.m3u8?x=1").DisplayTitle(), std::wstring(L"index"));
}

TEST_CASE(MediaUri_WinPathHelpers) {
    using namespace wvp::WinPath;
    CHECK_EQ(GetDirectoryName(L"D:\\lists\\a.m3u"), std::wstring(L"D:\\lists"));
    CHECK_EQ(GetDirectoryName(L"D:\\a.m3u"), std::wstring(L"D:\\"));
    CHECK_EQ(GetDirectoryName(L"\\\\srv\\sh\\dir\\p.m3u"), std::wstring(L"\\\\srv\\sh\\dir"));
    CHECK_EQ(GetDirectoryName(L"p.m3u"), std::wstring(L""));
    CHECK_EQ(GetPathRoot(L"D:\\lists\\a.m3u"), std::wstring(L"D:\\"));
    CHECK_EQ(GetPathRoot(L"\\\\srv\\sh\\dir\\p.m3u"), std::wstring(L"\\\\srv\\sh\\"));
    CHECK_EQ(Combine(L"D:\\lists", L"a\\b.mp4"), std::wstring(L"D:\\lists\\a\\b.mp4"));
    CHECK_EQ(Combine(L"D:\\lists", L"E:\\x.mp4"), std::wstring(L"E:\\x.mp4"));
    CHECK_EQ(GetExtension(L"C:\\a.b\\c"), std::wstring(L""));
    CHECK_EQ(GetExtension(L"c.tar.gz"), std::wstring(L".gz"));
}
