#pragma once
#include <functional>
#include <string>
#include <vector>

namespace app {
enum class DropRoute;
class AppContext;
void OpenFiles(AppContext& ctx);     // multi-select; video-only opens replace the unnamed slot, playlist-only opens create references
void OpenFolder(AppContext& ctx);    // non-recursive media + playlist open, naturally sorted
void LoadSubtitleFile(AppContext& ctx);   // "Load subtitle file…": picks subtitle files, attaches them to the current media and selects the first
void OpenUrl(AppContext& ctx);       // URL classification (video/HLS vs. playlist reference) -> OpenPaths
void ImportPlaylist(AppContext& ctx, std::function<void(const std::string&)> onImported = {}); // local editable copy; never touches the source
void AddPlaylistUrlFromPrompt(AppContext& ctx, std::function<void(const std::string&)> onAdded = {});  // prompts for a URL, then AddPlaylistUrl
/// Remote non-HLS M3U -> read-only reference playlist (AddOrUpdateReference; refreshed with RefreshPlaylistReference). onAdded gets its name.
void AddPlaylistUrl(AppContext& ctx, const std::wstring& url, std::function<void(const std::string&)> onAdded = {});
/// Refreshes/re-reads a reference entry, then imports its latest items as a detached editable copy; cached items are used offline.
void ImportReferenceAsEditableCopy(AppContext& ctx, const std::string& name,
                                   std::function<void(const std::string&)> onImported = {});
/// Refreshes a reference playlist and then plays it. `startItem` is a preferred item from the current cached rows;
/// `resumePlaylist` selects the playlist's remembered item instead. `onReady` runs on the UI thread after refresh.
void PlayPlaylistEntry(AppContext& ctx, const std::string& name, int startItem = 0, bool resumePlaylist = false,
                       std::function<void()> onReady = {});
/// Refreshes reference sources on a worker; cached items remain available for offline fallback.
void RefreshPlaylistReference(AppContext& ctx, const std::string& name, std::function<void()> onReady = {});
/// Pick -> append to one playlist (read-only playlists are refused by DropPathsToPlaylist); the playlist panel's Add menu. onDone runs on the UI thread.
void AddFilesToPlaylist(AppContext& ctx, const std::string& playlistName, std::function<void()> onDone = {});
void AddFolderToPlaylist(AppContext& ctx, const std::string& playlistName, std::function<void()> onDone = {});
void AddUrlToPlaylist(AppContext& ctx, const std::string& playlistName, std::function<void()> onDone = {});
/// Classifies each input with PlaylistLoader. Videos replace/play the unnamed slot; playlist-only inputs create/reuse references;
/// mixed video+playlist inputs expand all entries into the unnamed slot in source order.
void OpenPaths(AppContext& ctx, const std::vector<std::wstring>& pathsOrUrls, std::function<void()> onDone = {});
/// Expands dropped paths and appends them to one playlist without interrupting current playback.
void DropPathsToPlaylist(AppContext& ctx, const std::vector<std::wstring>& pathsOrUrls, const std::string& playlistName,
                         std::function<void()> onDone = {});
/// Expands dropped paths and replaces the unnamed playlist, making it current and playing its first item.
void DropPathsOnVideoView(AppContext& ctx, const std::vector<std::wstring>& pathsOrUrls, std::function<void()> onDone = {});
/// Dispatches only accepted view drops. Rejected drops have no playlist, playback or notice side effects.
bool DispatchDroppedPaths(AppContext& ctx, DropRoute route, const std::vector<std::wstring>& pathsOrUrls,
                          std::function<void()> onDone = {});
}
