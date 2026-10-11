#include "wvptest.h"
#include "TempDir.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

#include "wvp/M3uParser.h"
#include "wvp/PlaylistLoader.h"
#include "wvp/PlaylistStore.h"
#include "wvp/Utf.h"

using namespace wvp;
using wvptest::TempDir;

namespace {

std::vector<std::uint8_t> Bytes(const std::string& s) { return {s.begin(), s.end()}; }

HttpFetchFn FakeFetch(std::string body, std::wstring contentType, int* requests, std::wstring finalUrl = L"", bool throwOnCall = false) {
    return [=](const std::wstring& url, const std::function<bool(const std::wstring&)>& want, std::size_t) {
        if (requests) ++*requests;
        if (throwOnCall) throw std::runtime_error("no http expected");
        HttpResponse r;
        r.status = 200;
        r.contentType = contentType;
        r.finalUrl = finalUrl.empty() ? url : finalUrl;
        if (want(contentType)) r.body = Bytes(body);
        return r;
    };
}

}

TEST_CASE(M3u_ParsesExtinfTitleAndDurationAndSkipsOtherComments) {
    const std::wstring text = L"#EXTM3U\n#PLAYLIST:My list\n#EXTVLCOPT:foo=bar\n#EXTINF:123.5,Song One\nC:\\music\\one.mp3\n#EXTINF:-1,Live\nhttp://h/x.ts\nplain.mp4\n";
    M3uPlaylist pl = M3uParser::Parse(text, L"D:\\lists\\a.m3u");
    CHECK_EQ(pl.title, std::wstring(L"My list"));
    REQUIRE(pl.items.size() == 3);
    CHECK(pl.items[0] == MediaItem(L"C:\\music\\one.mp3", L"Song One", 123.5));
    CHECK_EQ(pl.items[1].title, std::wstring(L"Live"));
    CHECK(!pl.items[1].duration.has_value());
    CHECK_EQ(pl.items[1].url, std::wstring(L"http://h/x.ts"));
    CHECK_EQ(pl.items[2].url, std::wstring(L"D:\\lists\\plain.mp4"));
    CHECK(pl.items[2].title.empty());
}

TEST_CASE(M3u_ExtInfWithAttributesAndCommaInsideQuotes) {
    M3uPlaylist pl = M3uParser::Parse(L"#EXTINF:-1 tvg-name=\"A, B\" group-title=\"x\",Channel 5\nhttp://h/c5\n");
    REQUIRE(pl.items.size() == 1);
    CHECK_EQ(pl.items[0].title, std::wstring(L"Channel 5"));
}

TEST_CASE(M3u_ResolvesLocalEntriesAgainstPlaylistPath) {
    struct Row { const wchar_t* entry; const wchar_t* expected; };
    const Row rows[] = {
        {L"a/b.mp4", L"D:\\lists\\a\\b.mp4"},
        {L"a\\b.mp4", L"D:\\lists\\a\\b.mp4"},
        {L"..\\x.mp4", L"D:\\x.mp4"},
        {L"./y.mp4", L"D:\\lists\\y.mp4"},
        {L"E:\\abs\\z.mp4", L"E:\\abs\\z.mp4"},
        {L"E:/abs/z.mp4", L"E:\\abs\\z.mp4"},
        {L"\\\\srv\\share\\u.mp4", L"\\\\srv\\share\\u.mp4"},
        {L"file:///E:/with%20space/f.mp4", L"E:\\with space\\f.mp4"},
        {L"\\rooted\\r.mp4", L"D:\\rooted\\r.mp4"},
    };
    for (auto& r : rows) CHECK_EQ(MediaPathResolver::Resolve(r.entry, L"D:\\lists\\a.m3u"), std::wstring(r.expected));
}

TEST_CASE(M3u_ResolvesAgainstHttpBaseAndFileUriBase) {
    CHECK_EQ(MediaPathResolver::Resolve(L"v/a.ts", L"http://h/dir/list.m3u"), std::wstring(L"http://h/dir/v/a.ts"));
    CHECK_EQ(MediaPathResolver::Resolve(L"v\\a.ts", L"http://h/dir/list.m3u"), std::wstring(L"http://h/dir/v/a.ts"));
    CHECK_EQ(MediaPathResolver::Resolve(L"/top.ts", L"http://h/dir/list.m3u"), std::wstring(L"http://h/top.ts"));
    CHECK_EQ(MediaPathResolver::Resolve(L"https://o/x.mp4", L"http://h/list.m3u"), std::wstring(L"https://o/x.mp4"));
    CHECK_EQ(MediaPathResolver::Resolve(L"a.mp4", L"file:///D:/lists/p.m3u"), std::wstring(L"D:\\lists\\a.mp4"));
    CHECK_EQ(MediaPathResolver::Resolve(L"a.mp4", L"\\\\srv\\sh\\dir\\p.m3u"), std::wstring(L"\\\\srv\\sh\\dir\\a.mp4"));
    CHECK_EQ(MediaPathResolver::Resolve(L"../up/a b.ts", L"http://h/dir/sub/list.m3u"), std::wstring(L"http://h/dir/up/a%20b.ts"));
}

TEST_CASE(M3u_HandlesCrlfCrAndBomChar) {
    M3uPlaylist pl = M3uParser::Parse(L"\uFEFF#EXTM3U\r\n#EXTINF:5,A\r\na.mp4\r\n\r\n#EXTINF:6,B\rb.mp4\r");
    REQUIRE(pl.items.size() == 2);
    CHECK_EQ(pl.items[0].title, std::wstring(L"A"));
    CHECK_EQ(pl.items[1].url, std::wstring(L"b.mp4"));
}

TEST_CASE(M3u_DecodeHandlesUtf8WithAndWithoutBomAndUtf16) {
    const std::wstring s = L"#EXTM3U\n#EXTINF:1,한글 Café\nx.mp4\n";
    std::string u8 = ToUtf8(s);
    CHECK(M3uParser::Decode(Bytes(u8)) == s);
    std::vector<std::uint8_t> withBom = {0xEF, 0xBB, 0xBF};
    withBom.insert(withBom.end(), u8.begin(), u8.end());
    CHECK(M3uParser::Decode(withBom) == s);
    std::vector<std::uint8_t> u16 = {0xFF, 0xFE};
    for (wchar_t c : s) { u16.push_back((std::uint8_t)(c & 0xFF)); u16.push_back((std::uint8_t)(c >> 8)); }
    CHECK(M3uParser::Decode(u16) == s);
    std::vector<std::uint8_t> u16be = {0xFE, 0xFF};
    for (wchar_t c : s) { u16be.push_back((std::uint8_t)(c >> 8)); u16be.push_back((std::uint8_t)(c & 0xFF)); }
    CHECK(M3uParser::Decode(u16be) == s);
}

TEST_CASE(M3u_DecodeFallsBackToAnsiForInvalidUtf8) {
    std::vector<std::uint8_t> bytes = {'#', 'E', 0xE9, ' ', 'x'};   // not valid UTF-8
    std::string_view sv((const char*)bytes.data(), bytes.size());
    CHECK(!IsValidUtf8(sv));
    // .m3u: the system code page where there is one, else the Latin-1 byte map
    std::wstring ansi = FromSystemCodePage(sv).value_or(L"#E\xE9 x");
    CHECK(M3uParser::Decode(bytes, true) == ansi);
    // .m3u8: lenient UTF-8 (replacement char), no ANSI guess
    CHECK(M3uParser::Decode(bytes, false).find(L'\uFFFD') != std::wstring::npos);
}

TEST_CASE(Hls_DetectsHls) {
    CHECK(M3uParser::IsHls(L"#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:6\n#EXTINF:6,\nseg0.ts\n#EXT-X-ENDLIST\n"));
    CHECK(M3uParser::IsHls(L"#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=800000\nlow.m3u8\n"));
    CHECK(M3uParser::IsHls(L"#EXTM3U\n#EXT-X-MEDIA-SEQUENCE:0\n#EXTINF:2,\nseg0.ts\n"));
}

TEST_CASE(Hls_PlainPlaylistIsNotHls) {
    CHECK(!M3uParser::IsHls(L"#EXTM3U\n#EXTINF:10,A\na.mp4\n#EXTINF:10,B\nb.mp4\n"));
}

TEST_CASE(Loader_RemoteHlsMasterWithStreamInfIsAVideo) {
    PlaylistLoader l(FakeFetch("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=800000\nlow.m3u8\n", L"application/vnd.apple.mpegurl", nullptr));
    auto res = l.Load(L"https://h/master.m3u8");
    CHECK(res.isHlsStream);
    CHECK(!res.isPlaylist);
    REQUIRE(res.items.size() == 1);
    CHECK_EQ(res.items[0].url, L"https://h/master.m3u8");
}

TEST_CASE(Loader_RemoteHlsIsASingleItemWithTheUrl) {
    PlaylistLoader l(FakeFetch("#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXTINF:2,\ns0.ts\n#EXT-X-ENDLIST\n", L"application/vnd.apple.mpegurl", nullptr));
    auto res = l.Load(L"https://h/live/index.m3u8");
    CHECK(res.isHlsStream);
    CHECK(!res.isPlaylist);
    REQUIRE(res.items.size() == 1);
    CHECK_EQ(res.items[0].url, std::wstring(L"https://h/live/index.m3u8"));
}

TEST_CASE(Loader_PlaylistOfHlsStreamsIsAPlaylistNotAnHlsManifest) {
    PlaylistLoader l(FakeFetch("#EXTM3U\n#PLAYLIST:Channels\n#EXTINF:-1,News\nchannels/news.m3u8\n",
                               L"application/vnd.apple.mpegurl", nullptr));
    auto res = l.Load(L"https://h/lists/channels.m3u");
    CHECK(res.isPlaylist);
    CHECK(!res.isHlsStream);
    REQUIRE(res.items.size() == 1);
    CHECK_EQ(res.items[0].url, L"https://h/lists/channels/news.m3u8");
    CHECK_EQ(res.title, L"Channels");
}

TEST_CASE(Loader_RemotePlaylistResolvesRelativeEntriesAgainstFinalUrl) {
    PlaylistLoader l(FakeFetch("#EXTM3U\n#EXTINF:5,A\na.mp4\n#EXTINF:6,B\nsub/b.ts\nhttp://other/c.mp4\n", L"audio/x-mpegurl", nullptr));
    auto res = l.Load(L"http://h/dir/list.m3u");
    CHECK(res.isPlaylist);
    REQUIRE(res.items.size() == 3);
    CHECK_EQ(res.items[0].url, std::wstring(L"http://h/dir/a.mp4"));
    CHECK_EQ(res.items[1].url, std::wstring(L"http://h/dir/sub/b.ts"));
    CHECK_EQ(res.items[2].url, std::wstring(L"http://other/c.mp4"));
    CHECK_EQ(res.items[0].title, std::wstring(L"A"));
}

TEST_CASE(Loader_RemoteRedirectedFinalUrlIsTheResolveBase) {
    PlaylistLoader l(FakeFetch("#EXTM3U\na.mp4\n", L"audio/x-mpegurl", nullptr, L"http://cdn/x/y/list.m3u"));
    auto res = l.Load(L"http://h/dir/list.m3u");
    REQUIRE(res.items.size() == 1);
    CHECK_EQ(res.items[0].url, std::wstring(L"http://cdn/x/y/a.mp4"));
}

TEST_CASE(Loader_RemoteUnknownExtensionWithPlaylistContentTypeIsSniffed) {
    PlaylistLoader l(FakeFetch("#EXTM3U\n#EXTINF:5,A\na.mp4\n", L"application/x-mpegURL", nullptr));
    auto res = l.Load(L"http://h/get?id=7");
    CHECK(res.isPlaylist);
    REQUIRE(res.items.size() == 1);
    CHECK_EQ(res.items[0].url, std::wstring(L"http://h/a.mp4"));
}

TEST_CASE(Loader_RemoteMediaFileIsNotDownloaded) {
    bool bodyRequested = false;
    PlaylistLoader l([&](const std::wstring& url, const std::function<bool(const std::wstring&)>& want, std::size_t) {
        HttpResponse r; r.status = 200; r.contentType = L"video/mp4"; r.finalUrl = url;
        bodyRequested = want(r.contentType);
        return r;
    });
    auto res = l.Load(L"http://h/v/movie.mp4");
    CHECK(!bodyRequested);
    CHECK(!res.isPlaylist);
    CHECK(!res.isHlsStream);
    REQUIRE(res.items.size() == 1);
    CHECK_EQ(res.items[0].url, std::wstring(L"http://h/v/movie.mp4"));
}

TEST_CASE(Loader_OtherProtocolsPassStraightThroughWithoutHttp) {
    int requests = 0;
    PlaylistLoader l(FakeFetch("", L"", &requests, L"", true));
    auto res = l.Load(L"rtmp://h/app/stream");
    REQUIRE(res.items.size() == 1);
    CHECK_EQ(res.items[0].url, std::wstring(L"rtmp://h/app/stream"));
    CHECK_EQ(requests, 0);
}

TEST_CASE(Loader_HttpErrorStatusThrows) {
    PlaylistLoader l([](const std::wstring& url, const std::function<bool(const std::wstring&)>&, std::size_t) {
        HttpResponse r; r.status = 404; r.finalUrl = url; return r;
    });
    bool threw = false;
    try { l.Load(L"http://h/list.m3u"); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

TEST_CASE(Loader_LocalPlaylistAnsiHlsAndPlainMedia) {
    TempDir t;
    // a.m3u: UTF-8 with BOM
    std::vector<std::uint8_t> a = {0xEF, 0xBB, 0xBF};
    std::string at = "#EXTM3U\r\n#EXTINF:3,One\r\none.mp4\r\n";
    a.insert(a.end(), at.begin(), at.end());
    t.Write(L"a.m3u", a);
    t.Write(L"b.m3u", Bytes("#EXTM3U\n#EXTINF:3,Two\ntwo.mp4\n"));
    t.Write(L"h.m3u8", Bytes("#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXTINF:2,\ns.ts\n"));
    int requests = 0;
    PlaylistLoader loader(FakeFetch("", L"", &requests, L"", true));

    auto ra = loader.Load(t.File(L"a.m3u"));
    CHECK(ra.isPlaylist);
    REQUIRE(ra.items.size() == 1);
    CHECK_EQ(ra.items[0].url, t.File(L"one.mp4"));

    std::wstring uri = L"file:///" + t.File(L"b.m3u");
    for (auto& c : uri) if (c == L'\\') c = L'/';
    auto rb = loader.Load(uri);
    REQUIRE(rb.items.size() == 1);
    CHECK_EQ(rb.items[0].url, t.File(L"two.mp4"));

    auto rh = loader.Load(t.File(L"h.m3u8"));
    CHECK(rh.isHlsStream);
    REQUIRE(rh.items.size() == 1);
    CHECK_EQ(rh.items[0].url, t.File(L"h.m3u8"));

    auto rm = loader.Load(t.File(L"video.mkv"));
    CHECK(!rm.isPlaylist);
    REQUIRE(rm.items.size() == 1);
    CHECK_EQ(rm.items[0].url, t.File(L"video.mkv"));
    CHECK_EQ(requests, 0);
}

TEST_CASE(Loader_EmptyInputGivesEmptyResult) {
    PlaylistLoader l;
    auto r = l.Load(L"   ");
    CHECK(r.items.empty());
    CHECK(!r.isPlaylist);
}
