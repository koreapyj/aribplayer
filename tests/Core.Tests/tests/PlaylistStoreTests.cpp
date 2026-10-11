#include "wvptest.h"
#include "TempDir.h"

#include <string>

#include "wvp/JsonWriter.h"
#include "wvp/PlaylistLoader.h"
#include "wvp/PlaylistStore.h"

using namespace wvp;
using wvptest::TempDir;

namespace {

std::vector<MediaItem> Sample() {
    return { MediaItem(L"C:\\media\\one.mp4", L"One", 61.0), MediaItem(L"http://h/two.ts", L"\xb450\xbc88\xc9f8") };
}

}

TEST_CASE(PlaylistStore_save_load_list_exists) {
    TempDir t;
    PlaylistStore s(t.Sub(L"Playlists"));   // directory does not exist yet
    CHECK(s.List().empty());
    CHECK(!s.Exists(L"Mix"));
    REQUIRE(s.Save(L"Mix", Sample()));
    CHECK(s.Exists(L"Mix"));
    CHECK(fileio::IsRegularFile(s.Dir() + fileio::kSep + L"Mix.m3u8"));
    auto back = s.Load(L"Mix");
    REQUIRE(back.size() == 2);
    CHECK_EQ(back[0].url, L"C:\\media\\one.mp4");
    CHECK_EQ(back[0].title, L"One");
    CHECK_EQ(back[1].title, L"\xb450\xbc88\xc9f8");
    auto list = s.List();
    REQUIRE(list.size() == 1);
    CHECK_EQ(list[0].name, L"Mix");
    CHECK_EQ(list[0].itemCount, 2);
    CHECK_EQ(list[0].path, s.Dir() + fileio::kSep + L"Mix.m3u8");
    CHECK(list[0].modifiedMs > 1600000000000LL);
    CHECK(s.Load(L"Missing").empty());
}

TEST_CASE(PlaylistStore_list_sorted_case_insensitive_and_ignores_other_files) {
    TempDir t;
    PlaylistStore s(t.path);
    REQUIRE(s.Save(L"banana", Sample()));
    REQUIRE(s.Save(L"Apple", Sample()));
    REQUIRE(s.Save(L"cherry", {}));
    REQUIRE(s.Save(L"\xac00\xb098", Sample()));
    REQUIRE(fileio::AtomicWriteFile(t.Sub(L"notes.txt"), "x"));
    TempDir::MakeDir(t.Sub(L"folder.m3u8"));
    auto list = s.List();
    REQUIRE(list.size() == 4);
    CHECK_EQ(list[0].name, L"Apple");
    CHECK_EQ(list[1].name, L"banana");
    CHECK_EQ(list[2].name, L"cherry");
    CHECK_EQ(list[2].itemCount, 0);
    CHECK_EQ(list[3].name, L"\xac00\xb098");
}

TEST_CASE(PlaylistStore_create_unique_name) {
    TempDir t;
    PlaylistStore s(t.path);
    CHECK_EQ(s.CreateUniqueName(), L"Untitled playlist");
    REQUIRE(s.Save(L"Untitled playlist", {}));
    CHECK_EQ(s.CreateUniqueName(), L"Untitled playlist 2");
    REQUIRE(s.Save(L"Untitled playlist 2", {}));
    CHECK_EQ(s.CreateUniqueName(), L"Untitled playlist 3");
    CHECK_EQ(s.CreateUniqueName(L"Road trip"), L"Road trip");
    CHECK_EQ(s.CreateUniqueName(L"a/b"), L"a_b");
    CHECK_EQ(s.CreateUniqueName(L"???"), L"___");
    CHECK_EQ(s.CreateUniqueName(L"   "), L"Untitled playlist 3");
}

TEST_CASE(PlaylistStore_rename_success_collision_and_invalid) {
    TempDir t;
    PlaylistStore s(t.path);
    REQUIRE(s.Save(L"One", Sample()));
    REQUIRE(s.Save(L"Two", {}));
    std::wstring err;
    CHECK(!s.Rename(L"One", L"Two", &err));
    CHECK(!err.empty());
    CHECK(s.Exists(L"One"));
    CHECK_EQ(s.Load(L"Two").size(), (size_t)0);
    CHECK(!s.Rename(L"One", L"two", &err));
    err.clear();
    CHECK(!s.Rename(L"One", L"bad/name", &err));
    CHECK(!err.empty());
    err.clear();
    CHECK(!s.Rename(L"One", L"", &err));
    CHECK(!err.empty());
    err.clear();
    CHECK(!s.Rename(L"Ghost", L"Other", &err));
    CHECK(!err.empty());
    CHECK(!s.Rename(L"One", L"NUL"));

    CHECK(s.Rename(L"One", L"Uno \xd55c\xae00", &err));
    CHECK(!s.Exists(L"One"));
    CHECK(s.Exists(L"Uno \xd55c\xae00"));
    CHECK_EQ(s.Load(L"Uno \xd55c\xae00").size(), (size_t)2);
    CHECK(s.Rename(L"Uno \xd55c\xae00", L"uno \xd55c\xae00"));
    bool found = false;
    for (auto& i : s.List()) if (i.name == L"uno \xd55c\xae00") found = true;
    CHECK(found);
    CHECK(s.Rename(L"Two", L"Two"));
}

TEST_CASE(PlaylistStore_delete) {
    TempDir t;
    PlaylistStore s(t.path);
    REQUIRE(s.Save(L"Gone", Sample()));
    CHECK(s.Delete(L"Gone"));
    CHECK(!s.Exists(L"Gone"));
    CHECK(!s.Delete(L"Gone"));
    CHECK(!s.Delete(L"..\\evil"));
    CHECK(s.List().empty());
}

TEST_CASE(PlaylistStore_sanitize_name) {
    CHECK_EQ(PlaylistStore::SanitizeName(L"Plain name"), L"Plain name");
    CHECK_EQ(PlaylistStore::SanitizeName(L"a<b>c:d\"e/f\\g|h?i*j"), L"a_b_c_d_e_f_g_h_i_j");
    CHECK_EQ(PlaylistStore::SanitizeName(L"  padded  "), L"padded");
    CHECK_EQ(PlaylistStore::SanitizeName(L"dots..."), L"dots");
    CHECK_EQ(PlaylistStore::SanitizeName(L"tab\there"), L"tab_here");
    CHECK_EQ(PlaylistStore::SanitizeName(L""), L"");
    CHECK_EQ(PlaylistStore::SanitizeName(L"  . "), L"");
    CHECK_EQ(PlaylistStore::SanitizeName(L"con"), L"con_");
    CHECK_EQ(PlaylistStore::SanitizeName(L"LPT1"), L"LPT1_");
    CHECK_EQ(PlaylistStore::SanitizeName(L"com9.txt"), L"com9.txt_");
    CHECK_EQ(PlaylistStore::SanitizeName(L"console"), L"console");
    CHECK_EQ(PlaylistStore::SanitizeName(std::wstring(300, L'x')).size(), (size_t)100);
    CHECK_EQ(PlaylistStore::SanitizeName(L"\xd55c\xae00 \xc7ac\xc0dd\xbaa9\xb85d"), L"\xd55c\xae00 \xc7ac\xc0dd\xbaa9\xb85d");
    CHECK(PlaylistStore::IsValidName(L"Fine"));
    CHECK(!PlaylistStore::IsValidName(L"a/b"));
    CHECK(!PlaylistStore::IsValidName(L""));
    CHECK(!PlaylistStore::IsValidName(L" lead"));
}

TEST_CASE(PlaylistStore_save_rejects_invalid_names_and_overwrites) {
    TempDir t;
    PlaylistStore s(t.path);
    CHECK(!s.Save(L"a/b", Sample()));
    CHECK(!s.Save(L"", Sample()));
    CHECK(!s.Save(L"..", Sample()));
    REQUIRE(s.Save(L"P", Sample()));
    REQUIRE(s.Save(L"P", { MediaItem(L"http://x/y.mp4", L"Y") }));
    auto back = s.Load(L"P");
    REQUIRE(back.size() == 1);
    CHECK_EQ(back[0].title, L"Y");
}

TEST_CASE(PlaylistStore_reference_entries_are_deduplicated_read_only_and_removable) {
    TempDir t;
    PlaylistStore s(t.Sub(L"Playlists"));
    const std::wstring source = t.Sub(L"feed.m3u");
    const std::string original = "#EXTM3U\\n#EXTINF:5,One\\none.mp4\\n";
    REQUIRE(fileio::AtomicWriteFile(source, original));
    const std::wstring name = s.AddOrUpdateReference(source, L"Feed", {MediaItem(L"C:\\media\\one.mp4", L"One")}, false);
    REQUIRE(name == L"Feed");
    CHECK(s.IsReference(name));
    CHECK(s.IsReadOnly(name));
    CHECK(!s.Save(name, {MediaItem(L"C:\\media\\evil.mp4")}));
    REQUIRE(s.Load(name).size() == (size_t)1);
    std::wstring caseVariant = source;
    for (auto& c : caseVariant) c = (wchar_t)towupper(c);
    CHECK_EQ(s.AddOrUpdateReference(caseVariant, L"Changed", {MediaItem(L"C:\\media\\two.mp4", L"Two")}, false), name);
    auto list = s.List();
    REQUIRE(list.size() == (size_t)1);
    CHECK(list[0].isReference);
    CHECK_EQ(list[0].source, source);
    CHECK_EQ(s.Load(name)[0].url, L"C:\\media\\two.mp4");
    std::wstring err;
    REQUIRE(s.Rename(name, L"Renamed", &err));
    CHECK(s.IsReference(L"Renamed"));
    CHECK_EQ(s.FindReferenceBySource(source)->name, L"Renamed");
    CHECK(s.Delete(L"Renamed"));
    CHECK(!s.Exists(L"Renamed"));
    std::string after;
    REQUIRE(fileio::ReadAllBytes(source, after));
    CHECK_EQ(after, original);
}

// The pause / seek / position saves run on the store's writer thread; the readers wait for them, a newer save of the
// same item replaces one not started yet, an item already marked played stays played, and destruction finishes the queue.
TEST_CASE(PlaylistStore_playback_state_async_writes_are_ordered_and_flushed) {
    TempDir t;
    const std::wstring dir = t.Sub(L"Playlists");
    const std::wstring url = L"C:\\Media\\async.mp4";
    {
        PlaylistStore s(dir);
        for (int i = 1; i <= 50; ++i) s.SavePlaybackStateAsync(L"List", url, i, 120, false, true, 1);
        auto item = s.GetItemPlaybackState(url);   // waits for the pending writes
        CHECK_EQ(item.position, 50.0);
        CHECK(!item.played && item.seekable);
        CHECK_EQ(s.GetPlaylistPlaybackState(L"List").index, 1);
        s.SavePlaybackStateAsync(L"List", url, 118, 120, true, true, 1);
        CHECK(s.GetItemPlaybackState(url).played);
        CHECK_EQ(s.GetItemPlaybackState(url).position, 0.0);
        s.SavePlaybackStateAsync(L"List", url, 40, 120, false, true, 1);    // stays played (the writer reads it)
        CHECK(s.GetItemPlaybackState(url).played);
        s.SavePlaybackStateAsync(L"List", L"C:\\Media\\other.mp4", 33, 100, false, false, 2);
        s.FlushPlaybackWrites();
        CHECK_EQ(s.GetItemPlaybackState(L"C:\\Media\\other.mp4").position, 0.0);
        s.SavePlaybackStateAsync(L"Last", L"C:\\Media\\last.mp4", 21, 100, false, true, 0);
    }   // destroyed with a write pending
    PlaylistStore reopened(dir);
    CHECK_EQ(reopened.GetItemPlaybackState(L"C:\\Media\\last.mp4").position, 21.0);
}

TEST_CASE(PlaylistStore_playback_state_round_trip_and_resume_thresholds) {
    TempDir t;
    const std::wstring dir = t.Sub(L"Playlists");
    PlaylistStore s(dir);
    const std::wstring url = L"C:\\Media\\shared.mp4";
    REQUIRE(s.SavePlaybackState(L"Road trip", url, 42, 120, false, true, 2));
    auto item = s.GetItemPlaybackState(L"c:\\media\\SHARED.MP4");
    CHECK_EQ(item.position, 42.0);
    CHECK_EQ(item.duration, 120.0);
    CHECK(item.seekable && !item.played);
    auto pl = s.GetPlaylistPlaybackState(L"Road trip");
    CHECK_EQ(pl.lastItem, url);
    CHECK_EQ(pl.position, 42.0);
    CHECK_EQ(pl.index, 2);

    PlaylistStore reopened(dir);
    REQUIRE(reopened.SavePlaybackState(L"Other list", url, 60, 120, false, true, 0));
    CHECK_EQ(reopened.GetItemPlaybackState(url).position, 60.0);  // shared item key across playlists
    CHECK_EQ(reopened.GetPlaylistPlaybackState(L"Road trip").position, 42.0);
    CHECK_EQ(reopened.GetPlaylistPlaybackState(L"Other list").lastItem, url);

    CHECK(PlaylistStore::IsPlayedPosition(118, 120));
    CHECK(!PlaylistStore::IsPlayedPosition(113, 120));  // 5% of 120 seconds = 6 seconds
    CHECK(PlaylistStore::IsPlayedPosition(3571, 3600));
    CHECK(!PlaylistStore::IsPlayedPosition(3568, 3600)); // 30 second cap
    CHECK(PlaylistStore::IsPlayedPosition(0, 0, true));
    CHECK_EQ(PlaylistStore::ResumePosition({42, 120, false, true}), 42.0);
    CHECK_EQ(PlaylistStore::ResumePosition({5, 120, false, true}), 0.0);
    CHECK_EQ(PlaylistStore::ResumePosition({116, 120, false, true}), 0.0);
    CHECK_EQ(PlaylistStore::ResumePosition({42, 120, true, true}), 0.0);
    CHECK_EQ(PlaylistStore::ResumePosition({42, 120, false, false}), 0.0);
}

TEST_CASE(PlaylistStore_import_creates_detached_editable_m3u8_copy_and_refuses_hls) {
    TempDir t;
    PlaylistStore store(t.Sub(L"Playlists"));
    const std::wstring source = t.Sub(L"source.m3u");
    const std::string original = "#EXTM3U\n#PLAYLIST:Documentaries\n#EXTINF:100,Episode One\nmedia/one.mp4\n";
    REQUIRE(fileio::AtomicWriteFile(source, original));
    auto loaded = PlaylistLoader().Load(source);
    REQUIRE(loaded.isPlaylist && !loaded.isHlsStream);
    REQUIRE(loaded.title == L"Documentaries");
    REQUIRE(loaded.items.size() == (size_t)1);
    CHECK_EQ(loaded.items[0].url, t.Sub(std::wstring(L"media") + fileio::kSep + L"one.mp4"));
    const std::wstring imported = store.ImportEditableCopy(loaded.title, loaded.items, loaded.isPlaylist, loaded.isHlsStream);
    REQUIRE(imported == L"Documentaries");
    CHECK(!store.IsReference(imported));
    auto copy = store.Load(imported);
    REQUIRE(copy.size() == (size_t)1);
    CHECK_EQ(copy[0].title, L"Episode One");
    CHECK_EQ(copy[0].duration.value_or(0), 100.0);
    copy.emplace_back(t.Sub(L"extra.mp4"), L"Extra", 12.0);
    REQUIRE(store.Save(imported, copy));
    CHECK_EQ(store.Load(imported).size(), (size_t)2);
    std::string after;
    REQUIRE(fileio::ReadAllBytes(source, after));
    CHECK_EQ(after, original);
    CHECK_EQ(store.ImportEditableCopy(loaded.title, loaded.items, true, false), L"Documentaries (2)");

    const std::wstring hlsPath = t.Sub(L"master.m3u8");
    REQUIRE(fileio::AtomicWriteFile(hlsPath, "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=800000\nlow.m3u8\n"));
    auto hls = PlaylistLoader().Load(hlsPath);
    CHECK(hls.isHlsStream);
    CHECK(!hls.isPlaylist);
    CHECK(store.ImportEditableCopy(L"Master", hls.items, hls.isPlaylist, hls.isHlsStream).empty());
    CHECK(!store.Exists(L"Master"));
    fileio::RemoveFile(source);
    CHECK_EQ(store.Load(imported).size(), (size_t)2);
}

TEST_CASE(PlaylistStore_local_reference_is_reread_on_refresh) {
    TempDir t;
    PlaylistStore store(t.Sub(L"Playlists"));
    const std::wstring source = t.Sub(L"local.m3u");
    REQUIRE(fileio::AtomicWriteFile(source, "#EXTM3U\n#EXTINF:5,Before\nfirst.mp4\n"));
    auto initial = PlaylistLoader().Load(source);
    const std::wstring name = store.AddOrUpdateReference(source, L"Local", initial.items, false);
    REQUIRE(!name.empty());
    REQUIRE(fileio::AtomicWriteFile(source, "#EXTM3U\n#EXTINF:6,After\nsecond.mp4\n"));
    auto refreshed = store.RefreshReference(name, PlaylistLoader());
    CHECK(refreshed.refreshed);
    REQUIRE(refreshed.items.size() == (size_t)1);
    CHECK_EQ(refreshed.items[0].title, L"After");
    CHECK_EQ(store.Load(name)[0].url, t.Sub(L"second.mp4"));
}

TEST_CASE(PlaylistStore_delete_clears_playlist_resume_but_keeps_item_history) {
    TempDir t;
    PlaylistStore store(t.path);
    const std::wstring url = L"C:\\media\\one.mp4";
    REQUIRE(store.Save(L"Repeatable", {MediaItem(url)}));
    REQUIRE(store.SavePlaybackState(L"Repeatable", url, 45, 100, false, true, 0));
    REQUIRE(store.Delete(L"Repeatable"));
    REQUIRE(store.Save(L"Repeatable", {MediaItem(url)}));
    CHECK(store.GetPlaylistPlaybackState(L"Repeatable").lastItem.empty());
    CHECK_EQ(store.GetItemPlaybackState(url).position, 45.0);
}

TEST_CASE(PlaylistStore_default_dir_is_under_app_data) {
    PlaylistStore s;   // no I/O in the constructor
    std::wstring suffix = std::wstring(L"aribplayer") + fileio::kSep + L"Playlists";
    REQUIRE(s.Dir().size() > suffix.size());
    CHECK_EQ(s.Dir().substr(s.Dir().size() - suffix.size()), suffix);
}
