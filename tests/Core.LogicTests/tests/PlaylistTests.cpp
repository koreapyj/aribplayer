#include "wvptest.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <locale>
#include <stdexcept>
#include <string>
#include <vector>

#include "wvp/M3uParser.h"
#include "wvp/PlaylistLoader.h"
#include "wvp/PlaylistStore.h"
#include "wvp/Platform.h"
#include "LocalHttpServer.h"
#include "TempDir.h"

using namespace wvp;
using wvptest::LocalHttpServer;
using wvptest::TempDir;

namespace {

std::vector<std::uint8_t> Bytes(const std::string& s) { return {s.begin(), s.end()}; }

void WriteFile(const std::string& path, const std::vector<std::uint8_t>& data) {
    REQUIRE(platform::AtomicWriteFile(path, std::string_view(reinterpret_cast<const char*>(data.data()), data.size())));
}

HttpFetchFn FakeFetch(std::string body, std::string contentType, int* requests, std::string finalUrl = "", bool throwOnCall = false) {
    return [=](const std::string& url, const std::function<bool(const std::string&)>& want, std::size_t) {
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

TEST_CASE(M3u_ExtInfWithAttributesAndCommaInsideQuotes) {
    M3uPlaylist pl = M3uParser::Parse("#EXTINF:-1 tvg-name=\"A, B\" group-title=\"x\",Channel 5\nhttp://h/c5\n");
    REQUIRE(pl.items.size() == 1);
    CHECK_EQ(pl.items[0].title, std::string("Channel 5"));
}

TEST_CASE(M3u_HandlesCrlfCrAndBomChar) {
    M3uPlaylist pl = M3uParser::Parse("\uFEFF#EXTM3U\r\n#EXTINF:5,A\r\na.mp4\r\n\r\n#EXTINF:6,B\rb.mp4\r");
    REQUIRE(pl.items.size() == 2);
    CHECK_EQ(pl.items[0].title, std::string("A"));
    CHECK_EQ(pl.items[1].url, std::string("b.mp4"));
}

TEST_CASE(Hls_DetectsHls) {
    CHECK(M3uParser::IsHls("#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:6\n#EXTINF:6,\nseg0.ts\n#EXT-X-ENDLIST\n"));
    CHECK(M3uParser::IsHls("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=800000\nlow.m3u8\n"));
    CHECK(M3uParser::IsHls("#EXTM3U\n#EXT-X-MEDIA-SEQUENCE:0\n#EXTINF:2,\nseg0.ts\n"));
}

TEST_CASE(Hls_PlainPlaylistIsNotHls) {
    CHECK(!M3uParser::IsHls("#EXTM3U\n#EXTINF:10,A\na.mp4\n#EXTINF:10,B\nb.mp4\n"));
}

TEST_CASE(Loader_RemoteHlsMasterWithStreamInfIsAVideo) {
    PlaylistLoader l(FakeFetch("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=800000\nlow.m3u8\n", "application/vnd.apple.mpegurl", nullptr));
    auto res = l.Load("https://h/master.m3u8");
    CHECK(res.isHlsStream);
    CHECK(!res.isPlaylist);
    REQUIRE(res.items.size() == 1);
    CHECK_EQ(res.items[0].url, "https://h/master.m3u8");
}

TEST_CASE(Loader_RemoteHlsIsASingleItemWithTheUrl) {
    PlaylistLoader l(FakeFetch("#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXTINF:2,\ns0.ts\n#EXT-X-ENDLIST\n", "application/vnd.apple.mpegurl", nullptr));
    auto res = l.Load("https://h/live/index.m3u8");
    CHECK(res.isHlsStream);
    CHECK(!res.isPlaylist);
    REQUIRE(res.items.size() == 1);
    CHECK_EQ(res.items[0].url, std::string("https://h/live/index.m3u8"));
}

TEST_CASE(Loader_PlaylistOfHlsStreamsIsAPlaylistNotAnHlsManifest) {
    PlaylistLoader l(FakeFetch("#EXTM3U\n#PLAYLIST:Channels\n#EXTINF:-1,News\nchannels/news.m3u8\n",
                               "application/vnd.apple.mpegurl", nullptr));
    auto res = l.Load("https://h/lists/channels.m3u");
    CHECK(res.isPlaylist);
    CHECK(!res.isHlsStream);
    REQUIRE(res.items.size() == 1);
    CHECK_EQ(res.items[0].url, "https://h/lists/channels/news.m3u8");
    CHECK_EQ(res.title, "Channels");
}

TEST_CASE(Loader_RemotePlaylistResolvesRelativeEntriesAgainstFinalUrl) {
    PlaylistLoader l(FakeFetch("#EXTM3U\n#EXTINF:5,A\na.mp4\n#EXTINF:6,B\nsub/b.ts\nhttp://other/c.mp4\n", "audio/x-mpegurl", nullptr));
    auto res = l.Load("http://h/dir/list.m3u");
    CHECK(res.isPlaylist);
    REQUIRE(res.items.size() == 3);
    CHECK_EQ(res.items[0].url, std::string("http://h/dir/a.mp4"));
    CHECK_EQ(res.items[1].url, std::string("http://h/dir/sub/b.ts"));
    CHECK_EQ(res.items[2].url, std::string("http://other/c.mp4"));
    CHECK_EQ(res.items[0].title, std::string("A"));
}

TEST_CASE(Loader_RemoteRedirectedFinalUrlIsTheResolveBase) {
    PlaylistLoader l(FakeFetch("#EXTM3U\na.mp4\n", "audio/x-mpegurl", nullptr, "http://cdn/x/y/list.m3u"));
    auto res = l.Load("http://h/dir/list.m3u");
    REQUIRE(res.items.size() == 1);
    CHECK_EQ(res.items[0].url, std::string("http://cdn/x/y/a.mp4"));
}

TEST_CASE(Loader_RemoteUnknownExtensionWithPlaylistContentTypeIsSniffed) {
    PlaylistLoader l(FakeFetch("#EXTM3U\n#EXTINF:5,A\na.mp4\n", "application/x-mpegURL", nullptr));
    auto res = l.Load("http://h/get?id=7");
    CHECK(res.isPlaylist);
    REQUIRE(res.items.size() == 1);
    CHECK_EQ(res.items[0].url, std::string("http://h/a.mp4"));
}

TEST_CASE(Loader_RemoteMediaFileIsNotDownloaded) {
    bool bodyRequested = false;
    PlaylistLoader l([&](const std::string& url, const std::function<bool(const std::string&)>& want, std::size_t) {
        HttpResponse r; r.status = 200; r.contentType = "video/mp4"; r.finalUrl = url;
        bodyRequested = want(r.contentType);
        return r;
    });
    auto res = l.Load("http://h/v/movie.mp4");
    CHECK(!bodyRequested);
    CHECK(!res.isPlaylist);
    CHECK(!res.isHlsStream);
    REQUIRE(res.items.size() == 1);
    CHECK_EQ(res.items[0].url, std::string("http://h/v/movie.mp4"));
}

TEST_CASE(Loader_OtherProtocolsPassStraightThroughWithoutHttp) {
    int requests = 0;
    PlaylistLoader l(FakeFetch("", "", &requests, "", true));
    auto res = l.Load("rtmp://h/app/stream");
    REQUIRE(res.items.size() == 1);
    CHECK_EQ(res.items[0].url, std::string("rtmp://h/app/stream"));
    CHECK_EQ(requests, 0);
}

TEST_CASE(Loader_HttpErrorStatusThrows) {
    PlaylistLoader l([](const std::string& url, const std::function<bool(const std::string&)>&, std::size_t) {
        HttpResponse r; r.status = 404; r.finalUrl = url; return r;
    });
    bool threw = false;
    try { l.Load("http://h/list.m3u"); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

// libcurl end to end (HttpFetch) against a loopback server: playlist, HLS manifest, error status.
TEST_CASE(Loader_HttpAgainstLocalServer) {
    LocalHttpServer server("#EXTM3U\n#EXTINF:10,First\na.mp4\n#EXTINF:20,Second\nsub/b.ts\n");
    const std::string url = server.Url();
    const std::string base = url.substr(0, url.find_last_of('/') + 1);
    PlaylistLoader l;
    auto pl = l.Load(url);
    CHECK(pl.isPlaylist);
    REQUIRE(pl.items.size() == 2);
    CHECK_EQ(pl.items[0].url, base + "a.mp4");
    CHECK_EQ(pl.items[0].title, std::string("First"));
    CHECK_EQ(pl.items[1].url, base + "sub/b.ts");
    server.SetResponse(200, "#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXTINF:6,\nseg0.ts\n");
    auto hls = l.Load(base + "live.m3u8");
    CHECK(hls.isHlsStream);
    server.SetResponse(503, "offline");
    bool threw = false;
    try { l.Load(base + "missing.m3u"); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
    CHECK_EQ(server.Requests(), 3);
}

TEST_CASE(Loader_EmptyInputGivesEmptyResult) {
    PlaylistLoader l;
    auto r = l.Load("   ");
    CHECK(r.items.empty());
    CHECK(!r.isPlaylist);
}

TEST_CASE(ReferenceStore_refetches_each_time_and_uses_offline_cache_from_test_server) {
    TempDir t;
    LocalHttpServer server("#EXTM3U\n#EXTINF:10,First\nfirst.mp4\n");
    PlaylistStore store(t.path);
    const std::string source = server.Url();
    const std::string name = store.AddOrUpdateReference(source, "Loopback", {MediaItem("cached-before.mp4")}, true);
    REQUIRE(!name.empty());

    auto first = store.RefreshReference(name, PlaylistLoader());
    CHECK(first.refreshed);
    REQUIRE(first.items.size() == 1);
    CHECK_EQ(first.items[0].title, "First");
    std::string mediaBase = source.substr(0, source.find_last_of('/') + 1);
    CHECK_EQ(first.items[0].url, mediaBase + "first.mp4");

    server.SetResponse(200, "#EXTM3U\n#EXTINF:20,Second\nsecond.ts\n");
    auto second = store.RefreshReference(name, PlaylistLoader());
    CHECK(second.refreshed);
    REQUIRE(second.items.size() == 1);
    CHECK_EQ(second.items[0].title, "Second");
    CHECK(second.items[0].url.find("second.ts") != std::string::npos);

    server.SetResponse(200, "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=800000\nlow.m3u8\n");
    auto changedToHls = store.RefreshReference(name, PlaylistLoader());
    CHECK(!changedToHls.refreshed);
    CHECK(changedToHls.isHlsStream);
    CHECK(changedToHls.usedOfflineFallback);
    CHECK_EQ(store.Load(name)[0].title, "Second");  // HLS must not replace the playlist cache

    server.SetResponse(503, "offline");
    auto offline = store.RefreshReference(name, PlaylistLoader());
    CHECK(!offline.refreshed);
    CHECK(offline.usedOfflineFallback);
    CHECK(!offline.error.empty());
    REQUIRE(offline.items.size() == 1);
    CHECK_EQ(offline.items[0].title, "Second");
    CHECK_EQ(store.Load(name)[0].title, "Second");
    CHECK_EQ(server.Requests(), 4);
}

TEST_CASE(M3u_DecodeHandlesUtf8WithAndWithoutBomAndUtf16) {
    const std::u16string ws = u"#EXTM3U\n#EXTINF:1,한글 Café\nx.mp4\n";
    const std::string s = "#EXTM3U\n#EXTINF:1,한글 Café\nx.mp4\n";
    std::string u8 = s;
    CHECK(M3uParser::Decode(Bytes(u8)) == s);
    std::vector<std::uint8_t> withBom = {0xEF, 0xBB, 0xBF};
    withBom.insert(withBom.end(), u8.begin(), u8.end());
    CHECK(M3uParser::Decode(withBom) == s);
    std::vector<std::uint8_t> u16 = {0xFF, 0xFE};
    for (char16_t c : ws) { u16.push_back((std::uint8_t)(c & 0xFF)); u16.push_back((std::uint8_t)(c >> 8)); }
    CHECK(M3uParser::Decode(u16) == s);
    std::vector<std::uint8_t> u16be = {0xFE, 0xFF};
    for (char16_t c : ws) { u16be.push_back((std::uint8_t)(c >> 8)); u16be.push_back((std::uint8_t)(c & 0xFF)); }
    CHECK(M3uParser::Decode(u16be) == s);
}

TEST_CASE(M3u_DecodeIsLenientForInvalidUtf8WithoutAnsiGuess) {
    std::vector<std::uint8_t> bytes = {'#', 'E', 0xE9, ' ', 'x'};
    CHECK(M3uParser::Decode(bytes, false).find("�") != std::string::npos);
}

TEST_CASE(M3u_ParsesExtinfTitleAndDurationAndSkipsOtherComments) {
    const std::string text = "#EXTM3U\n#PLAYLIST:My list\n#EXTVLCOPT:foo=bar\n#EXTINF:123.5,Song One\nhttp://h/one.mp3\n#EXTINF:-1,Live\nhttp://h/x.ts\nplain.mp4\n";
    M3uPlaylist pl = M3uParser::Parse(text, "http://h/lists/a.m3u");
    CHECK_EQ(pl.title, std::string("My list"));
    REQUIRE(pl.items.size() == 3);
    CHECK(pl.items[0] == MediaItem("http://h/one.mp3", "Song One", 123.5));
    CHECK_EQ(pl.items[1].title, std::string("Live"));
    CHECK(!pl.items[1].duration.has_value());
    CHECK_EQ(pl.items[1].url, std::string("http://h/x.ts"));
    CHECK_EQ(pl.items[2].url, std::string("http://h/lists/plain.mp4"));
    CHECK(pl.items[2].title.empty());
}

TEST_CASE(M3u_ExtInfParsesPositiveDurations) {
    struct { const char* text; double duration; } cases[] = {
        { "123", 123.0 }, { "123.5", 123.5 }, { "+123.5", 123.5 },
        { "1.25e2", 125.0 }, { "+1E-2", 0.01 }, { ".5", 0.5 }, { "1.", 1.0 },
        { "1e-310", 1e-310 }, { "5e-324", std::numeric_limits<double>::denorm_min() },
        { "2.2250738585072013e-308", std::numeric_limits<double>::min() },
        { "995099695728527255600758247104066505033869613739299874415889206266."
          "879463863002444896015951673680700954524031837595747283233435790300850", 0x1.2e5e81f350442p+219 },
    };
    for (const auto& c : cases) {
        auto pl = M3uParser::Parse(std::string("#EXTINF:") + c.text + ",Title\nhttp://h/a.ts\n");
        REQUIRE(pl.items.size() == 1);
        REQUIRE_MSG(pl.items[0].duration.has_value(), c.text);
        CHECK_EQ(*pl.items[0].duration, c.duration);
        CHECK_EQ(pl.items[0].title, "Title");
    }
}

TEST_CASE(M3u_ExtInfLeavesInvalidDurationsUnset) {
    for (const char* text : { "", "-1", "0", "-0", "+0", "garbage", "1junk", "1e", "1e+", ".",
                             "inf", "+inf", "nan", "0x1", "0x1p0", "++1", "+ 1", "+\v1",
                             "1e309", "1e-400" }) {
        auto pl = M3uParser::Parse(std::string("#EXTINF:5,Previous\n#EXTINF:") + text + ",Title\nhttp://h/a.ts\n");
        REQUIRE(pl.items.size() == 1);
        CHECK_MSG(!pl.items[0].duration.has_value(), text);
        CHECK_EQ(pl.items[0].title, "Title");
    }
}

TEST_CASE(M3u_ExtInfIgnoresGlobalDecimalSeparator) {
    struct CommaPunct : std::numpunct<char> {
        char do_decimal_point() const override { return ','; }
        char do_thousands_sep() const override { return '.'; }
        std::string do_grouping() const override { return "\3"; }
    };
    struct RestoreLocale {
        std::locale previous = std::locale();
        ~RestoreLocale() { std::locale::global(previous); }
    } restore;
    std::locale::global(std::locale(std::locale::classic(), new CommaPunct));
    CHECK_EQ(std::use_facet<std::numpunct<char>>(std::locale()).decimal_point(), ',');

    auto pl = M3uParser::Parse("#EXTINF:123.5,Decimal\nhttp://h/a.ts\n#EXTINF:+1.25e2,Exponent\nhttp://h/b.ts\n#EXTINF:1,5,Title\nhttp://h/c.ts\n");
    REQUIRE(pl.items.size() == 3);
    CHECK(pl.items[0].duration.has_value() && *pl.items[0].duration == 123.5);
    CHECK(pl.items[1].duration.has_value() && *pl.items[1].duration == 125.0);
    CHECK(pl.items[2].duration.has_value() && *pl.items[2].duration == 1.0);
    CHECK_EQ(pl.items[2].title, "5,Title");
}

TEST_CASE(M3u_ResolvesAgainstHttpBase) {
    CHECK_EQ(MediaPathResolver::Resolve("v/a.ts", "http://h/dir/list.m3u"), std::string("http://h/dir/v/a.ts"));
    CHECK_EQ(MediaPathResolver::Resolve("v\\a.ts", "http://h/dir/list.m3u"), std::string("http://h/dir/v/a.ts"));
    CHECK_EQ(MediaPathResolver::Resolve("/top.ts", "http://h/dir/list.m3u"), std::string("http://h/top.ts"));
    CHECK_EQ(MediaPathResolver::Resolve("https://o/x.mp4", "http://h/list.m3u"), std::string("https://o/x.mp4"));
    CHECK_EQ(MediaPathResolver::Resolve("../up/a b.ts", "http://h/dir/sub/list.m3u"), std::string("http://h/dir/up/a%20b.ts"));
}

TEST_CASE(Loader_LocalPlaylistAnsiHlsAndPlainMedia) {
    TempDir t;
    // a.m3u: UTF-8 with BOM
    std::vector<std::uint8_t> a = {0xEF, 0xBB, 0xBF};
    std::string at = "#EXTM3U\r\n#EXTINF:3,One\r\none.mp4\r\n";
    a.insert(a.end(), at.begin(), at.end());
    WriteFile(t.File("a.m3u"), a);
    WriteFile(t.File("b.m3u"), Bytes("#EXTM3U\n#EXTINF:3,Two\ntwo.mp4\n"));
    WriteFile(t.File("h.m3u8"), Bytes("#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXTINF:2,\ns.ts\n"));
    int requests = 0;
    PlaylistLoader loader(FakeFetch("", "", &requests, "", true));

    auto ra = loader.Load(t.File("a.m3u"));
    CHECK(ra.isPlaylist);
    REQUIRE(ra.items.size() == 1);
    CHECK_EQ(ra.items[0].url, t.File("one.mp4"));

    std::string uri = t.File("b.m3u");
    for (auto& c : uri) if (c == '\\') c = '/';
    uri = (uri.front() == '/' ? "file://" : "file:///") + uri;
    auto rb = loader.Load(uri);
    REQUIRE(rb.items.size() == 1);
    CHECK_EQ(rb.items[0].url, t.File("two.mp4"));

    auto rh = loader.Load(t.File("h.m3u8"));
    CHECK(rh.isHlsStream);
    REQUIRE(rh.items.size() == 1);
    CHECK_EQ(rh.items[0].url, t.File("h.m3u8"));

    auto rm = loader.Load(t.File("video.mkv"));
    CHECK(!rm.isPlaylist);
    REQUIRE(rm.items.size() == 1);
    CHECK_EQ(rm.items[0].url, t.File("video.mkv"));
    CHECK_EQ(requests, 0);
}
