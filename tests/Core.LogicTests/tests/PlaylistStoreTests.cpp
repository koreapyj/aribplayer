#include "wvptest.h"

#include <string>

#include "wvp/Platform.h"
#include "wvp/PlaylistLoader.h"
#include "wvp/PlaylistStore.h"
#include "TempDir.h"

using namespace wvp;
using wvptest::TempDir;

namespace {

std::string Media(const char* name) { return platform::FullPath(platform::Combine("media", name)); }

std::vector<MediaItem> Sample() {
    return { MediaItem(Media("one.mp4"), "One", 61.0), MediaItem("http://h/two.ts", "\ub450\ubc88\uc9f8") };
}

}

TEST_CASE(PlaylistStore_save_load_list_exists) {
    TempDir t;
    PlaylistStore s(t.File("Playlists"));   // directory does not exist yet
    CHECK(s.List().empty());
    CHECK(!s.Exists("Mix"));
    REQUIRE(s.Save("Mix", Sample()));
    CHECK(s.Exists("Mix"));
    CHECK(platform::Exists(platform::Combine(s.Dir(), "Mix.m3u8")));
    auto back = s.Load("Mix");
    REQUIRE(back.size() == 2);
    CHECK_EQ(back[0].url, Media("one.mp4"));
    CHECK_EQ(back[0].title, "One");
    CHECK_EQ(back[1].title, "\ub450\ubc88\uc9f8");
    auto list = s.List();
    REQUIRE(list.size() == 1);
    CHECK_EQ(list[0].name, "Mix");
    CHECK_EQ(list[0].itemCount, 2);
    CHECK_EQ(list[0].path, platform::Combine(s.Dir(), "Mix.m3u8"));
    CHECK(list[0].modifiedMs > 1600000000000LL);
    CHECK(s.Load("Missing").empty());
}

TEST_CASE(PlaylistStore_list_sorted_case_insensitive_and_ignores_other_files) {
    TempDir t;
    PlaylistStore s(t.path);
    REQUIRE(s.Save("banana", Sample()));
    REQUIRE(s.Save("Apple", Sample()));
    REQUIRE(s.Save("cherry", {}));
    REQUIRE(s.Save("\uac00\ub098", Sample()));
    REQUIRE(platform::AtomicWriteFile(t.File("notes.txt"), "x"));
    REQUIRE(platform::EnsureDirectory(t.File("folder.m3u8")));
    auto list = s.List();
    REQUIRE(list.size() == 4);
    CHECK_EQ(list[0].name, "Apple");
    CHECK_EQ(list[1].name, "banana");
    CHECK_EQ(list[2].name, "cherry");
    CHECK_EQ(list[2].itemCount, 0);
    CHECK_EQ(list[3].name, "\uac00\ub098");
}

TEST_CASE(PlaylistStore_create_unique_name) {
    TempDir t;
    PlaylistStore s(t.path);
    CHECK_EQ(s.CreateUniqueName(), "Untitled playlist");
    REQUIRE(s.Save("Untitled playlist", {}));
    CHECK_EQ(s.CreateUniqueName(), "Untitled playlist 2");
    REQUIRE(s.Save("Untitled playlist 2", {}));
    CHECK_EQ(s.CreateUniqueName(), "Untitled playlist 3");
    CHECK_EQ(s.CreateUniqueName("Road trip"), "Road trip");
    CHECK_EQ(s.CreateUniqueName("a/b"), "a_b");
    CHECK_EQ(s.CreateUniqueName("???"), "___");
    CHECK_EQ(s.CreateUniqueName("   "), "Untitled playlist 3");
}

TEST_CASE(PlaylistStore_rename_success_collision_and_invalid) {
    TempDir t;
    PlaylistStore s(t.path);
    REQUIRE(s.Save("One", Sample()));
    REQUIRE(s.Save("Two", {}));
    std::string err;
    CHECK(!s.Rename("One", "Two", &err));
    CHECK(!err.empty());
    CHECK(s.Exists("One"));
    CHECK_EQ(s.Load("Two").size(), (size_t)0);
    err.clear();
    CHECK(!s.Rename("One", "bad/name", &err));
    CHECK(!err.empty());
    err.clear();
    CHECK(!s.Rename("One", "", &err));
    CHECK(!err.empty());
    err.clear();
    CHECK(!s.Rename("Ghost", "Other", &err));
    CHECK(!err.empty());
    CHECK(!s.Rename("One", "NUL"));

    CHECK(s.Rename("One", "Uno \ud55c\uae00", &err));
    CHECK(!s.Exists("One"));
    CHECK(s.Exists("Uno \ud55c\uae00"));
    CHECK_EQ(s.Load("Uno \ud55c\uae00").size(), (size_t)2);
    CHECK(s.Rename("Uno \ud55c\uae00", "uno \ud55c\uae00"));
    bool found = false;
    for (auto& i : s.List()) if (i.name == "uno \ud55c\uae00") found = true;
    CHECK(found);
    CHECK(s.Rename("Two", "Two"));
}

TEST_CASE(PlaylistStore_delete) {
    TempDir t;
    PlaylistStore s(t.path);
    REQUIRE(s.Save("Gone", Sample()));
    CHECK(s.Delete("Gone"));
    CHECK(!s.Exists("Gone"));
    CHECK(!s.Delete("Gone"));
    CHECK(!s.Delete("..\\evil"));
    CHECK(s.List().empty());
}

TEST_CASE(PlaylistStore_sanitize_name) {
    CHECK_EQ(PlaylistStore::SanitizeName("Plain name"), "Plain name");
    CHECK_EQ(PlaylistStore::SanitizeName("a<b>c:d\"e/f\\g|h?i*j"), "a_b_c_d_e_f_g_h_i_j");
    CHECK_EQ(PlaylistStore::SanitizeName("  padded  "), "padded");
    CHECK_EQ(PlaylistStore::SanitizeName("dots..."), "dots");
    CHECK_EQ(PlaylistStore::SanitizeName("tab\there"), "tab_here");
    CHECK_EQ(PlaylistStore::SanitizeName(""), "");
    CHECK_EQ(PlaylistStore::SanitizeName("  . "), "");
    CHECK_EQ(PlaylistStore::SanitizeName("con"), "con_");
    CHECK_EQ(PlaylistStore::SanitizeName("LPT1"), "LPT1_");
    CHECK_EQ(PlaylistStore::SanitizeName("com9.txt"), "com9.txt_");
    CHECK_EQ(PlaylistStore::SanitizeName("console"), "console");
    CHECK_EQ(PlaylistStore::SanitizeName(std::string(300, 'x')).size(), (size_t)100);
    CHECK_EQ(PlaylistStore::SanitizeName("\ud55c\uae00 \uc7ac\uc0dd\ubaa9\ub85d"), "\ud55c\uae00 \uc7ac\uc0dd\ubaa9\ub85d");
    CHECK(PlaylistStore::IsValidName("Fine"));
    CHECK(!PlaylistStore::IsValidName("a/b"));
    CHECK(!PlaylistStore::IsValidName(""));
    CHECK(!PlaylistStore::IsValidName(" lead"));
}

TEST_CASE(PlaylistStore_save_rejects_invalid_names_and_overwrites) {
    TempDir t;
    PlaylistStore s(t.path);
    CHECK(!s.Save("a/b", Sample()));
    CHECK(!s.Save("", Sample()));
    CHECK(!s.Save("..", Sample()));
    REQUIRE(s.Save("P", Sample()));
    REQUIRE(s.Save("P", { MediaItem("http://x/y.mp4", "Y") }));
    auto back = s.Load("P");
    REQUIRE(back.size() == 1);
    CHECK_EQ(back[0].title, "Y");
}

TEST_CASE(PlaylistStore_reference_entries_are_deduplicated_read_only_and_removable) {
    TempDir t;
    PlaylistStore s(t.File("Playlists"));
    const std::string source = t.File("feed.m3u");
    const std::string original = "#EXTM3U\\n#EXTINF:5,One\\none.mp4\\n";
    REQUIRE(platform::AtomicWriteFile(source, original));
    const std::string name = s.AddOrUpdateReference(source, "Feed", {MediaItem(Media("one.mp4"), "One")}, false);
    REQUIRE(name == "Feed");
    CHECK(s.IsReference(name));
    CHECK(s.IsReadOnly(name));
    CHECK(!s.Save(name, {MediaItem(Media("evil.mp4"))}));
    REQUIRE(s.Load(name).size() == (size_t)1);
    std::string caseVariant = source;
    for (auto& c : caseVariant) if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    CHECK_EQ(s.AddOrUpdateReference(caseVariant, "Changed", {MediaItem(Media("two.mp4"), "Two")}, false), name);
    auto list = s.List();
    REQUIRE(list.size() == (size_t)1);
    CHECK(list[0].isReference);
    CHECK_EQ(list[0].source, source);
    CHECK_EQ(s.Load(name)[0].url, Media("two.mp4"));
    std::string err;
    REQUIRE(s.Rename(name, "Renamed", &err));
    CHECK(s.IsReference("Renamed"));
    CHECK_EQ(s.FindReferenceBySource(source)->name, "Renamed");
    CHECK(s.Delete("Renamed"));
    CHECK(!s.Exists("Renamed"));
    std::string after;
    REQUIRE(platform::ReadAllBytes(source, after, 1ull << 30) == platform::ReadResult::Ok);
    CHECK_EQ(after, original);
}

// The pause / seek / position saves run on the store's writer thread; the readers wait for them, a newer save of the
// same item replaces one not started yet, an item already marked played stays played, and destruction finishes the queue.
TEST_CASE(PlaylistStore_playback_state_async_writes_are_ordered_and_flushed) {
    TempDir t;
    const std::string dir = t.File("Playlists");
    const std::string url = "C:\\Media\\async.mp4";
    {
        PlaylistStore s(dir);
        for (int i = 1; i <= 50; ++i) s.SavePlaybackStateAsync("List", url, i, 120, false, true, 1);
        auto item = s.GetItemPlaybackState(url);   // waits for the pending writes
        CHECK_EQ(item.position, 50.0);
        CHECK(!item.played && item.seekable);
        CHECK_EQ(s.GetPlaylistPlaybackState("List").index, 1);
        s.SavePlaybackStateAsync("List", url, 118, 120, true, true, 1);
        CHECK(s.GetItemPlaybackState(url).played);
        CHECK_EQ(s.GetItemPlaybackState(url).position, 0.0);
        s.SavePlaybackStateAsync("List", url, 40, 120, false, true, 1);    // stays played (the writer reads it)
        CHECK(s.GetItemPlaybackState(url).played);
        s.SavePlaybackStateAsync("List", "C:\\Media\\other.mp4", 33, 100, false, false, 2);
        s.FlushPlaybackWrites();
        CHECK_EQ(s.GetItemPlaybackState("C:\\Media\\other.mp4").position, 0.0);
        s.SavePlaybackStateAsync("Last", "C:\\Media\\last.mp4", 21, 100, false, true, 0);
    }   // destroyed with a write pending
    PlaylistStore reopened(dir);
    CHECK_EQ(reopened.GetItemPlaybackState("C:\\Media\\last.mp4").position, 21.0);
}

TEST_CASE(PlaylistStore_playback_state_round_trip_and_resume_thresholds) {
    TempDir t;
    const std::string dir = t.File("Playlists");
    PlaylistStore s(dir);
    const std::string url = "C:\\Media\\shared.mp4";
    REQUIRE(s.SavePlaybackState("Road trip", url, 42, 120, false, true, 2));
    auto item = s.GetItemPlaybackState("c:\\media\\SHARED.MP4");
    CHECK_EQ(item.position, 42.0);
    CHECK_EQ(item.duration, 120.0);
    CHECK(item.seekable && !item.played);
    auto pl = s.GetPlaylistPlaybackState("Road trip");
    CHECK_EQ(pl.lastItem, url);
    CHECK_EQ(pl.position, 42.0);
    CHECK_EQ(pl.index, 2);

    PlaylistStore reopened(dir);
    REQUIRE(reopened.SavePlaybackState("Other list", url, 60, 120, false, true, 0));
    CHECK_EQ(reopened.GetItemPlaybackState(url).position, 60.0);  // shared item key across playlists
    CHECK_EQ(reopened.GetPlaylistPlaybackState("Road trip").position, 42.0);
    CHECK_EQ(reopened.GetPlaylistPlaybackState("Other list").lastItem, url);

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
    PlaylistStore store(t.File("Playlists"));
    const std::string source = t.File("source.m3u");
    const std::string original = "#EXTM3U\n#PLAYLIST:Documentaries\n#EXTINF:100,Episode One\nmedia/one.mp4\n";
    REQUIRE(platform::AtomicWriteFile(source, original));
    auto loaded = PlaylistLoader().Load(source);
    REQUIRE(loaded.isPlaylist && !loaded.isHlsStream);
    REQUIRE(loaded.title == "Documentaries");
    REQUIRE(loaded.items.size() == (size_t)1);
    CHECK_EQ(loaded.items[0].url, t.File("media", "one.mp4"));
    const std::string imported = store.ImportEditableCopy(loaded.title, loaded.items, loaded.isPlaylist, loaded.isHlsStream);
    REQUIRE(imported == "Documentaries");
    CHECK(!store.IsReference(imported));
    auto copy = store.Load(imported);
    REQUIRE(copy.size() == (size_t)1);
    CHECK_EQ(copy[0].title, "Episode One");
    CHECK_EQ(copy[0].duration.value_or(0), 100.0);
    copy.emplace_back(t.File("extra.mp4"), "Extra", 12.0);
    REQUIRE(store.Save(imported, copy));
    CHECK_EQ(store.Load(imported).size(), (size_t)2);
    std::string after;
    REQUIRE(platform::ReadAllBytes(source, after, 1ull << 30) == platform::ReadResult::Ok);
    CHECK_EQ(after, original);
    CHECK_EQ(store.ImportEditableCopy(loaded.title, loaded.items, true, false), "Documentaries (2)");

    const std::string hlsPath = t.File("master.m3u8");
    REQUIRE(platform::AtomicWriteFile(hlsPath, "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=800000\nlow.m3u8\n"));
    auto hls = PlaylistLoader().Load(hlsPath);
    CHECK(hls.isHlsStream);
    CHECK(!hls.isPlaylist);
    CHECK(store.ImportEditableCopy("Master", hls.items, hls.isPlaylist, hls.isHlsStream).empty());
    CHECK(!store.Exists("Master"));
    REQUIRE(platform::RemoveFile(source));
    CHECK_EQ(store.Load(imported).size(), (size_t)2);
}

TEST_CASE(PlaylistStore_local_reference_is_reread_on_refresh) {
    TempDir t;
    PlaylistStore store(t.File("Playlists"));
    const std::string source = t.File("local.m3u");
    REQUIRE(platform::AtomicWriteFile(source, "#EXTM3U\n#EXTINF:5,Before\nfirst.mp4\n"));
    auto initial = PlaylistLoader().Load(source);
    const std::string name = store.AddOrUpdateReference(source, "Local", initial.items, false);
    REQUIRE(!name.empty());
    REQUIRE(platform::AtomicWriteFile(source, "#EXTM3U\n#EXTINF:6,After\nsecond.mp4\n"));
    auto refreshed = store.RefreshReference(name, PlaylistLoader());
    CHECK(refreshed.refreshed);
    REQUIRE(refreshed.items.size() == (size_t)1);
    CHECK_EQ(refreshed.items[0].title, "After");
    CHECK_EQ(store.Load(name)[0].url, t.File("second.mp4"));
}

TEST_CASE(PlaylistStore_delete_clears_playlist_resume_but_keeps_item_history) {
    TempDir t;
    PlaylistStore store(t.path);
    const std::string url = Media("one.mp4");
    REQUIRE(store.Save("Repeatable", {MediaItem(url)}));
    REQUIRE(store.SavePlaybackState("Repeatable", url, 45, 100, false, true, 0));
    REQUIRE(store.Delete("Repeatable"));
    REQUIRE(store.Save("Repeatable", {MediaItem(url)}));
    CHECK(store.GetPlaylistPlaybackState("Repeatable").lastItem.empty());
    CHECK_EQ(store.GetItemPlaybackState(url).position, 45.0);
}

TEST_CASE(PlaylistStore_default_dir_is_under_app_data) {
    PlaylistStore s;   // no I/O in the constructor
    std::string suffix = platform::Combine("aribplayer", "Playlists");
    REQUIRE(s.Dir().size() > suffix.size());
    CHECK_EQ(s.Dir().substr(s.Dir().size() - suffix.size()), suffix);
}
