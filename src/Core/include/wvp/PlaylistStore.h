#pragma once
// User playlists stored as .m3u8 files in AppDataDir()\Playlists (directory injectable for tests).
// Playlist names are file names without extension; they must be valid file names (see SanitizeName).
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "MediaItem.h"

namespace wvp {

class PlaylistLoader;

class PlaylistStore {
public:
    struct Info {
        std::wstring name;
        std::wstring path;
        int itemCount = 0;
        std::int64_t modifiedMs = 0;   // Unix epoch ms
        bool isReference = false;
        bool isRemote = false;
        std::wstring source;           // populated for referenced playlist entries
    };

    struct Reference {
        std::wstring name;             // display name; also the playlist key
        std::wstring source;           // original URL or local path, never copied/rewritten
        bool isRemote = false;
        std::vector<MediaItem> items;  // last successful fetch/read, used as offline fallback
    };

    struct ItemPlaybackState {
        double position = 0;
        double duration = 0;
        bool played = false;
        bool seekable = false;
    };

    struct PlaylistPlaybackState {
        std::wstring lastItem;
        double position = 0;
        int index = -1;
    };

    struct ReferenceRefreshResult {
        std::vector<MediaItem> items;
        std::wstring title, error;
        bool refreshed = false;
        bool isPlaylist = false;
        bool isHlsStream = false;
        bool usedOfflineFallback = false;
    };

    /// Empty dir = AppDataDir()\Playlists.
    explicit PlaylistStore(std::wstring dir = {});

    const std::wstring& Dir() const { return dir_; }
    static std::wstring DefaultDir();

    /// Sorted by name, case-insensitive; reference entries are read from a separate JSON index, never from .m3u8 files.
    std::vector<Info> List() const;
    /// Empty when missing/unreadable. For a reference entry this returns its cached items.
    std::vector<MediaItem> Load(const std::wstring& name) const;
    bool IsReference(const std::wstring& name) const;
    bool IsReadOnly(const std::wstring& name) const { return IsReference(name); }
    std::optional<Reference> FindReferenceByName(const std::wstring& name) const;
    std::optional<Reference> FindReferenceBySource(const std::wstring& source) const;
    /// Adds a reference or refreshes its cache. Existing source keeps its display name; a new source gets a unique display name.
    std::wstring AddOrUpdateReference(const std::wstring& source, const std::wstring& suggestedName,
                                      const std::vector<MediaItem>& items, bool isRemote);
    bool UpdateReferenceItems(const std::wstring& name, const std::vector<MediaItem>& items);
    /// Re-reads a referenced URL/path. On failure returns the cached items with usedOfflineFallback set; never writes the source.
    ReferenceRefreshResult RefreshReference(const std::wstring& name, const PlaylistLoader& loader);
    /// Creates the folder if needed; atomic. Items are written with absolute paths. False for references / invalid names / I-O error.
    bool Save(const std::wstring& name, const std::vector<MediaItem>& items) const;
    /// "base", else "base 2", "base 3", ... whichever does not exist yet.
    std::wstring CreateUniqueName(const std::wstring& base = L"Untitled playlist") const;
    /// Import suffix convention: "name", "name (2)", "name (3)".
    std::wstring CreateUniqueImportName(const std::wstring& base) const;
    /// Saves a detached editable copy; never stores or writes the source path. Returns its unique name.
    std::wstring ImportEditableCopy(const std::wstring& baseName, const std::vector<MediaItem>& items,
                                    bool isPlaylist, bool isHlsStream = false);

    /// The history readers first wait for SavePlaybackStateAsync writes still pending, so they never see stale state.
    ItemPlaybackState GetItemPlaybackState(const std::wstring& url) const;
    /// `played` flag of many items from ONE read of the history (waits for pending async writes like GetItemPlaybackState).
    std::vector<bool> GetPlayedFlags(const std::vector<std::wstring>& urls) const;
    PlaylistPlaybackState GetPlaylistPlaybackState(const std::wstring& name) const;
    bool SavePlaybackState(const std::wstring& playlistName, const std::wstring& itemUrl,
                           double position, double duration, bool played, bool seekable, int index) const;
    /// SavePlaybackState on a background thread (in call order; a newer save of the same playlist + item replaces one
    /// not started yet): the read-modify-write of the history file can stall for hundreds of ms (antivirus scanning),
    /// so it runs off the UI thread. played: stored as played if `playedHint` or the
    /// item is already marked played; the position is then 0, as it is when !seekable. Destruction finishes the queue.
    void SavePlaybackStateAsync(const std::wstring& playlistName, const std::wstring& itemUrl, double position,
                                double duration, bool playedHint, bool seekable, int index) const;
    /// Blocks until every SavePlaybackStateAsync write so far is on disk.
    void FlushPlaybackWrites() const;
    static constexpr double ResumeBoundarySeconds = 5.0;
    static constexpr double PlayedMaxSeconds = 30.0;
    static constexpr double PlayedDurationFraction = 0.05;
    static bool IsPlayedPosition(double position, double duration, bool reachedEof = false);
    static double ResumePosition(const ItemPlaybackState& state);
    /// Fails (false + message) when `from` is missing, `to` is invalid, or `to` already exists.
    bool Rename(const std::wstring& from, const std::wstring& to, std::wstring* err = nullptr);
    bool Delete(const std::wstring& name);
    bool Exists(const std::wstring& name) const;

    /// Replaces characters illegal in file names (<>:"/\|?* and control chars) with '_', trims whitespace and
    /// trailing dots, suffixes reserved device names with '_' and limits the length to 100. May return empty.
    static std::wstring SanitizeName(const std::wstring& name);
    /// Non-empty and unchanged by SanitizeName.
    static bool IsValidName(const std::wstring& name);

    std::wstring PathFor(const std::wstring& name) const;

private:
    std::wstring ReferenceIndexPath() const;
    std::wstring PlaybackHistoryPath() const;
    bool LoadReferences(std::vector<Reference>& out) const;
    bool SaveReferences(const std::vector<Reference>& refs) const;
    static std::wstring ItemKey(const std::wstring& url);
    ItemPlaybackState ReadItemPlaybackState(const std::wstring& url) const;
    std::wstring dir_;
    class HistoryWriter;
    std::shared_ptr<HistoryWriter> writer_;   // created on first use; shared by copies of the store
};

}
