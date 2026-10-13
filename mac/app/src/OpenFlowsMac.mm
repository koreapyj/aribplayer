// macOS port of src/App/features/OpenFlows.cpp (the App-side open flows: pick / drop / URL -> PlaylistLoader -> the unnamed
// playlist, references, imports). Same code as the original except the folder listing (POSIX paths through
// std::filesystem instead of FindFirstFileW with "\\" joins, which would produce Windows paths) and PromptText
// (features/TextInputDialog.h: an NSAlert with a text field instead of the Win32 EDIT dialog) at the end. The file
// pickers are the original PickPaths over shim/shobjidl.h (NSOpenPanel, src/FileDialogMac.mm).
#import <AppKit/AppKit.h>

#include "OpenFlowsMac.h"

#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <filesystem>
#include <thread>
#include <utility>

#include "AppContext.h"
#include "features/DialogGuard.h"
#include "Localize.h"
#include "features/OpenLogic.h"
#include "features/PlaybackExtras.h"
#include "features/TextInputDialog.h"
#include "wvp/MediaUri.h"
#include "wvp/Platform.h"
#include "wvp/PlaylistLoader.h"
#include "wvp/win32/Utf.h"
#include "ShellLogic.h"

namespace app {

namespace {
using Microsoft::WRL::ComPtr;

// Client GUIDs make the common dialog remember the last folder separately for files / folders / playlists / subtitles.
constexpr GUID kGuidFiles = {0x6a1f0c11, 0x2b7e, 0x4c53, {0x9a, 0x01, 0x57, 0x8e, 0x21, 0x6b, 0x30, 0x01}};
constexpr GUID kGuidFolder = {0x6a1f0c11, 0x2b7e, 0x4c53, {0x9a, 0x01, 0x57, 0x8e, 0x21, 0x6b, 0x30, 0x02}};
constexpr GUID kGuidPlaylist = {0x6a1f0c11, 0x2b7e, 0x4c53, {0x9a, 0x01, 0x57, 0x8e, 0x21, 0x6b, 0x30, 0x03}};
constexpr GUID kGuidSubtitle = {0x6a1f0c11, 0x2b7e, 0x4c53, {0x9a, 0x01, 0x57, 0x8e, 0x21, 0x6b, 0x30, 0x04}};

// Empty result = cancelled.
std::vector<std::wstring> PickPaths(HWND owner, const GUID& clientGuid, bool folder, bool multi, const COMDLG_FILTERSPEC* filters,
                                    UINT nFilters, const wchar_t* title) {
    std::vector<std::wstring> out;
    if (OsDialogsBlocked()) return out;   // tests never show an OS dialog: cancelled
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
    dlg->SetClientGuid(clientGuid);
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    opts |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    if (folder) opts |= FOS_PICKFOLDERS;
    else opts |= FOS_FILEMUSTEXIST;
    if (multi) opts |= FOS_ALLOWMULTISELECT;
    dlg->SetOptions(opts);
    if (title) dlg->SetTitle(title);
    if (filters && nFilters) dlg->SetFileTypes(nFilters, filters);
    if (FAILED(dlg->Show(owner))) return out;
    ComPtr<IShellItemArray> arr;
    if (FAILED(dlg->GetResults(&arr))) return out;
    DWORD n = 0;
    arr->GetCount(&n);
    for (DWORD i = 0; i < n; i++) {
        ComPtr<IShellItem> it;
        PWSTR p = nullptr;
        if (SUCCEEDED(arr->GetItemAt(i, &it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
            out.push_back(p);
            CoTaskMemFree(p);
        }
    }
    return out;
}

std::vector<std::wstring> PickMediaFiles(AppContext& ctx) {
    static const std::wstring media = open::MediaFilterPattern();
    const COMDLG_FILTERSPEC f[] = {{Tr(L"Media files", L"미디어 파일"), media.c_str()}, {Tr(L"All files", L"모든 파일"), L"*.*"}};
    return PickPaths(ctx.MainHwnd(), kGuidFiles, false, true, f, 2, Tr(L"Open files", L"파일 열기"));
}

std::vector<std::wstring> PickSubtitleFiles(AppContext& ctx) {
    static const std::wstring subs = open::SubtitleFilterPattern();
    const COMDLG_FILTERSPEC f[] = {{Tr(L"Subtitle files", L"자막 파일"), subs.c_str()}, {Tr(L"All files", L"모든 파일"), L"*.*"}};
    return PickPaths(ctx.MainHwnd(), kGuidSubtitle, false, true, f, 2, Tr(L"Load subtitle file", L"자막 파일 불러오기"));
}

// macOS: POSIX paths ('/'), listed with std::filesystem (the original joins "\\*" patterns for FindFirstFileW).
std::vector<std::wstring> ListFolder(const std::wstring& folder, bool* skipped) {
    std::vector<std::wstring> files;
    std::error_code ec;
    const std::filesystem::path base(wvp::ToUtf8(folder));
    for (const auto& e : std::filesystem::directory_iterator(base, std::filesystem::directory_options::skip_permission_denied, ec)) {
        std::error_code fec;
        if (e.is_directory(fec)) continue;
        const std::wstring name = wvp::ToWide(e.path().filename().string());
        if (!name.empty() && name[0] == L'.') continue;   // Finder-hidden files (._*, .DS_Store)
        if (open::IsMediaFile(name) || open::IsPlaylistFile(name)) files.push_back(wvp::ToWide(e.path().string()));
        else if (skipped) *skipped = true;
    }
    open::NaturalSort(files);
    return files;
}

std::vector<std::wstring> ListOpenFolderFiles(const std::wstring& folder) { return ListFolder(folder, nullptr); }

std::vector<std::wstring> PickFolderMedia(AppContext& ctx, bool& cancelled) {
    auto dirs = PickPaths(ctx.MainHwnd(), kGuidFolder, true, false, nullptr, 0, Tr(L"Open folder", L"폴더 열기"));
    cancelled = dirs.empty();
    if (dirs.empty()) return {};
    auto files = ListOpenFolderFiles(dirs[0]);
    if (files.empty()) ctx.ShowToast(Tr(L"No media files or playlists in that folder", L"해당 폴더에 미디어 파일이나 재생목록이 없습니다"));
    return files;
}

std::vector<std::wstring> ListDropFolder(const std::wstring& folder, bool& skipped) { return ListFolder(folder, &skipped); }

enum class DispatchTarget { SpecificPlaylist, VideoView };

struct ResolvedInput {
    std::string source;
    std::string title;
    std::string managedName;
    std::vector<wvp::MediaItem> items;
    bool isPlaylist = false;
    bool isHls = false;
    bool offlineFallback = false;
};

std::string SourceName(const std::string& source) { return open::PlaylistName(source); }

std::string ManagedPlaylistName(wvp::PlaylistStore& store, const std::string& source) {
    auto input = wvp::MediaUri::Parse(source);
    if (!input.IsLocal()) return {};
    const std::string path = wvp::platform::FullPath(input.localPath ? *input.localPath : source);
    for (const auto& info : store.List()) {
        if (info.isReference) continue;
        if (wvp::platform::NamesEqualIgnoreCase(wvp::platform::FullPath(info.path), path)) return info.name;
    }
    return {};
}

std::wstring ErrorText(const std::exception& ex) { return std::wstring(Tr(L"Open failed: ", L"열기 실패: ")) + wvp::ToWide(ex.what()); }

void PlayLoadedPlaylist(AppContext& ctx, const std::string& name, const std::vector<wvp::MediaItem>& items, int startItem,
                        bool resumePlaylist) {
    if (items.empty()) return;
    int start = std::clamp(startItem, 0, (int)items.size() - 1);
    double preferred = -1;
    if (resumePlaylist) {
        const auto state = ctx.Svc().playlists.GetPlaylistPlaybackState(name);
        if (!state.lastItem.empty()) {
            auto found = std::find_if(items.begin(), items.end(), [&](const wvp::MediaItem& item) { return item.url == state.lastItem; });
            if (found != items.end()) start = (int)std::distance(items.begin(), found);
            else if (state.index >= 0 && state.index < (int)items.size()) start = state.index;
        }
        preferred = state.position > 0 ? state.position : -1;
    }
    // The controller resolves this preferred offset together with per-item history for every open path.
    ctx.PlayPlaylistAtPosition(name, items, start, preferred);
}

void Dispatch(AppContext& ctx, const std::vector<std::wstring>& inputs, DispatchTarget target, std::string playlistName = {},
              bool skipUnsupported = false, std::function<void()> onDone = {}) {
    if (inputs.empty()) { if (onDone) onDone(); return; }
    std::thread([&ctx, inputs, target, playlistName = std::move(playlistName), skipUnsupported,
                 onDone = std::move(onDone)]() {
        std::vector<ResolvedInput> resolved;
        std::wstring error;
        bool fallbackErr = false;   // error text is localised: remember the offline-fallback case instead of searching the text
        bool skipped = false;
        wvp::PlaylistLoader loader;
        auto loadOne = [&](const std::wstring& wideSource) {
            const std::string source = wvp::ToUtf8(wideSource);
            if (skipUnsupported) {
                const auto uri = wvp::MediaUri::Parse(source);
                if (uri.IsLocal() && !uri.HasPlaylistExtension() && !open::IsMediaFile(wideSource)) { skipped = true; return; }
            }
            ResolvedInput value;
            value.source = source;
            value.managedName = ManagedPlaylistName(ctx.Svc().playlists, source);
            try {
                auto r = loader.Load(source);
                value.items = std::move(r.items);
                value.title = r.title;
                value.isPlaylist = r.isPlaylist;
                value.isHls = r.isHlsStream;
                if (value.title.empty() && r.isPlaylist) value.title = SourceName(source);
            } catch (const std::exception& ex) {
                auto cached = ctx.Svc().playlists.FindReferenceBySource(source);
                if (cached) {
                    value.items = cached->items;
                    value.title = cached->name;
                    value.isPlaylist = true;
                    value.isHls = false;
                    value.offlineFallback = !cached->items.empty();
                    error = ErrorText(ex);
                    if (value.offlineFallback) { error += Tr(L" Using the last fetched items as an offline fallback.", L" 마지막으로 가져온 항목을 오프라인 대체본으로 사용합니다."); fallbackErr = true; }
                } else {
                    error = ErrorText(ex);
                    return;
                }
            }
            resolved.push_back(std::move(value));
        };
        for (const auto& in : inputs) {
            DWORD attr = GetFileAttributesW(in.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
                const auto files = skipUnsupported ? ListDropFolder(in, skipped) : ListOpenFolderFiles(in);
                for (const auto& file : files) loadOne(file);
                continue;
            }
            loadOne(in);
        }

        ctx.PostToUi([&ctx, resolved = std::move(resolved), error, fallbackErr, skipped, target,
                      playlistName = std::move(playlistName), onDone = std::move(onDone)]() mutable {
            if (!error.empty()) {
                const bool fallback = fallbackErr;
                ctx.ShowInfoBar(fallback ? InfoSeverity::Warning : InfoSeverity::Error,
                                fallback ? Tr(L"Playlist refresh failed", L"재생목록을 새로 고치지 못했습니다") : Tr(L"Could not open", L"열 수 없습니다"), error);
            }
            if (skipped) ctx.ShowInfoBar(InfoSeverity::Warning, Tr(L"Some files were skipped", L"일부 파일을 건너뛰었습니다"), Tr(L"Unsupported files were skipped.", L"지원되지 않는 파일은 건너뛰었습니다."));
            std::vector<wvp::MediaItem> items;
            for (const auto& r : resolved) items.insert(items.end(), r.items.begin(), r.items.end());
            const bool playlistOnly = !resolved.empty() && std::all_of(resolved.begin(), resolved.end(), [](const ResolvedInput& r) { return r.isPlaylist; });
            if (resolved.empty() || (items.empty() && !(target == DispatchTarget::VideoView && playlistOnly))) {
                if (error.empty() && !skipped) ctx.ShowToast(Tr(L"Nothing to play", L"재생할 항목이 없습니다"));
                if (onDone) onDone();
                return;
            }
            if (target == DispatchTarget::SpecificPlaylist && ctx.IsPlaylistReadOnly(playlistName)) {
                ctx.ShowInfoBar(InfoSeverity::Warning, Tr(L"Playlist is read-only", L"재생목록이 읽기 전용입니다"), Tr(L"Import it to make an editable copy.", L"가져오기로 편집 가능한 사본을 만드세요."));
                if (onDone) onDone();
                return;
            }
            if (target == DispatchTarget::SpecificPlaylist) {
                const size_t n = items.size();
                ctx.AppendPlaylistItems(playlistName, items);
                const std::wstring dest = playlistName.empty() ? std::wstring(Tr(L"Unnamed playlist", L"이름 없는 재생목록")) : wvp::ToWide(playlistName);
                ctx.ShowToast(TrW(L"Added " + std::to_wstring(n) + (n == 1 ? L" item to " : L" items to ") + dest,
                                  dest + L"에 " + std::to_wstring(n) + L"개 항목을 추가했습니다"));
            } else {
                const bool hasPlaylist = std::any_of(resolved.begin(), resolved.end(), [](const ResolvedInput& r) { return r.isPlaylist; });
                const bool hasVideo = std::any_of(resolved.begin(), resolved.end(), [](const ResolvedInput& r) { return !r.isPlaylist; });
                if (hasPlaylist && !hasVideo) {
                    std::vector<std::string> names;
                    for (const auto& r : resolved) {
                        if (!r.isPlaylist) continue;
                        std::string name = r.managedName;
                        if (name.empty()) {
                            const bool remote = wvp::MediaUri::Parse(r.source).IsRemote();
                            name = ctx.Svc().playlists.AddOrUpdateReference(r.source, r.title.empty() ? SourceName(r.source) : r.title,
                                                                            r.items, remote);
                            if (name.empty()) continue;
                        }
                        if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
                    }
                    if (!names.empty()) {
                        auto entryItems = ctx.PlaylistItems(names.front());
                        if (entryItems.empty()) {
                            ctx.PlayPlaylistAtPosition(names.front(), entryItems, -1, -1);
                            ctx.ShowToast(Tr(L"This playlist is empty", L"재생목록이 비어 있습니다"));
                        } else {
                            PlayLoadedPlaylist(ctx, names.front(), entryItems, 0, true);
                        }
                    }
                } else {
                    // Video-only and mixed inputs use video-view drop semantics. In a mixed batch every playlist is expanded
                    // in place, preserving the original source order, and no reference entry is created.
                    ctx.ReplaceUnnamedPlaylistAtPosition(std::move(items), -1);
                }
            }
            if (onDone) onDone();
        });
    }).detach();
}

void ImportPlaylistSource(AppContext& ctx, std::string source, std::function<void(const std::string&)> onImported) {
    AppContext* c = &ctx;
    std::thread([c, source = std::move(source), onImported = std::move(onImported)]() mutable {
        std::string importedName;
        std::wstring error;
        std::vector<wvp::MediaItem> items;
        bool hls = false, playlist = false;
        try {
            auto result = wvp::PlaylistLoader().Load(source);
            hls = result.isHlsStream;
            playlist = result.isPlaylist && !result.isHlsStream;
            if (playlist) {
                items = std::move(result.items);
                importedName = result.title.empty() ? SourceName(source) : result.title;
            }
        } catch (const std::exception& ex) { error = wvp::ToWide(ex.what()); }
        c->PostToUi([c, importedName = std::move(importedName), items = std::move(items), hls, playlist,
                     error = std::move(error), onImported = std::move(onImported)]() mutable {
            if (!error.empty()) {
                c->ShowInfoBar(InfoSeverity::Error, Tr(L"Could not import playlist", L"재생목록을 가져올 수 없습니다"), error);
                return;
            }
            if (!playlist || hls) {
                c->ShowInfoBar(InfoSeverity::Warning, Tr(L"Not an importable playlist", L"가져올 수 있는 재생목록이 아닙니다"),
                               hls ? Tr(L"HLS playlists are video streams and cannot be imported as playlists.", L"HLS 재생목록은 동영상 스트림이므로 재생목록으로 가져올 수 없습니다.") : Tr(L"The selected URL is not a playlist.", L"선택한 주소는 재생목록이 아닙니다."));
                return;
            }
            const std::string name = c->Svc().playlists.ImportEditableCopy(importedName, items, playlist, hls);
            if (name.empty()) {
                c->ShowInfoBar(InfoSeverity::Error, Tr(L"Could not import playlist", L"재생목록을 가져올 수 없습니다"), Tr(L"The editable copy could not be saved.", L"편집 가능한 사본을 저장할 수 없습니다."));
                return;
            }
            if (onImported) onImported(name);
            c->ShowToast(TrW(L"Imported " + wvp::ToWide(name), wvp::ToWide(name) + L" 가져옴"));
        });
    }).detach();
}
}

void RefreshPlaylistReference(AppContext& ctx, const std::string& name, std::function<void()> onReady) {
    if (!ctx.Svc().playlists.IsReference(name)) { if (onReady) onReady(); return; }
    AppContext* c = &ctx;
    std::thread([c, name, onReady = std::move(onReady)]() mutable {
        auto result = c->Svc().playlists.RefreshReference(name, wvp::PlaylistLoader());
        c->PostToUi([c, name, result = std::move(result), onReady = std::move(onReady)]() mutable {
            if (!result.error.empty()) {
                const bool fallback = result.usedOfflineFallback;
                c->ShowInfoBar(fallback ? InfoSeverity::Warning : InfoSeverity::Error,
                               fallback ? Tr(L"Playlist refresh failed", L"재생목록을 새로 고치지 못했습니다") : Tr(L"Could not read playlist", L"재생목록을 읽을 수 없습니다"),
                               fallback ? std::wstring(Tr(L"Using the last fetched items as an offline fallback. ", L"마지막으로 가져온 항목을 오프라인 대체본으로 사용합니다. ")) + wvp::ToWide(result.error) : wvp::ToWide(result.error));
            }
            if (onReady) onReady();
        });
    }).detach();
}

void PlayPlaylistEntry(AppContext& ctx, const std::string& name, int startItem, bool resumePlaylist,
                       std::function<void()> onReady) {
    std::string selectedUrl;
    if (!resumePlaylist) {
        auto oldItems = ctx.PlaylistItems(name);
        if (startItem >= 0 && startItem < (int)oldItems.size()) selectedUrl = oldItems[(size_t)startItem].url;
    }
    auto play = [&ctx, name, startItem, resumePlaylist, selectedUrl = std::move(selectedUrl), onReady = std::move(onReady)]() mutable {
        auto items = ctx.PlaylistItems(name);
        if (items.empty()) {
            ctx.ShowToast(Tr(L"This playlist is empty", L"재생목록이 비어 있습니다"));
            if (onReady) onReady();
            return;
        }
        int selected = startItem;
        if (!resumePlaylist && !selectedUrl.empty()) {
            auto found = std::find_if(items.begin(), items.end(), [&](const wvp::MediaItem& item) { return item.url == selectedUrl; });
            if (found != items.end()) selected = (int)std::distance(items.begin(), found);
        }
        PlayLoadedPlaylist(ctx, name, items, selected, resumePlaylist);
        if (onReady) onReady();
    };
    if (ctx.Svc().playlists.IsReference(name)) RefreshPlaylistReference(ctx, name, std::move(play));
    else play();
}

void ImportPlaylist(AppContext& ctx, std::function<void(const std::string&)> onImported) {
    static const COMDLG_FILTERSPEC filters[] = {{Tr(L"Playlist files", L"재생목록 파일"), L"*.m3u;*.m3u8"}, {Tr(L"All files", L"모든 파일"), L"*.*"}};
    auto paths = PickPaths(ctx.MainHwnd(), kGuidPlaylist, false, false, filters, 2, Tr(L"Import playlist", L"재생목록 가져오기"));
    if (paths.empty()) return;
    ImportPlaylistSource(ctx, wvp::ToUtf8(paths.front()), std::move(onImported));
}

void AddPlaylistUrlFromPrompt(AppContext& ctx, std::function<void(const std::string&)> onAdded) {
    TextPromptOptions o;
    o.title = Tr(L"Import playlist from URL", L"URL에서 재생목록 가져오기");
    o.label = Tr(L"Enter the address of a remote M3U playlist", L"원격 M3U 재생목록의 주소를 입력하세요");
    o.placeholder = L"https://";
    o.okText = Tr(L"Import", L"가져오기");
    o.validate = [](const std::wstring& text) { return open::ValidateUrl(text); };
    auto url = PromptText(ctx, o);
    if (!url) return;
    AddPlaylistUrl(ctx, *url, std::move(onAdded));
}

void AddPlaylistUrl(AppContext& ctx, const std::wstring& rawUrl, std::function<void(const std::string&)> onAdded) {
    size_t b = rawUrl.find_first_not_of(L" \t\r\n"), e = rawUrl.find_last_not_of(L" \t\r\n");
    const std::wstring url = b == std::wstring::npos ? std::wstring() : rawUrl.substr(b, e - b + 1);
    const std::wstring validation = open::ValidateUrl(url);
    if (!validation.empty()) {
        ctx.ShowInfoBar(InfoSeverity::Warning, Tr(L"Invalid playlist URL", L"잘못된 재생목록 주소"), validation);
        return;
    }
    AppContext* c = &ctx;
    std::thread([c, url = wvp::ToUtf8(url), onAdded = std::move(onAdded)]() mutable {
        std::string title;
        std::wstring error;
        std::vector<wvp::MediaItem> items;
        bool hls = false, playlist = false;
        try {
            auto result = wvp::PlaylistLoader().Load(url);
            hls = result.isHlsStream;
            playlist = result.isPlaylist && !result.isHlsStream;
            if (playlist) {
                items = std::move(result.items);
                title = result.title.empty() ? SourceName(url) : result.title;
            }
        } catch (const std::exception& ex) { error = wvp::ToWide(ex.what()); }
        c->PostToUi([c, url, title = std::move(title), items = std::move(items), hls, playlist, error = std::move(error),
                     onAdded = std::move(onAdded)]() mutable {
            if (!error.empty()) {
                c->ShowInfoBar(InfoSeverity::Error, Tr(L"Could not import playlist", L"재생목록을 가져올 수 없습니다"), error);
                return;
            }
            if (!playlist || hls) {
                c->ShowInfoBar(InfoSeverity::Warning, Tr(L"Not an importable playlist", L"가져올 수 있는 재생목록이 아닙니다"),
                               hls ? Tr(L"HLS playlists are video streams and cannot be imported as playlists.", L"HLS 재생목록은 동영상 스트림이므로 재생목록으로 가져올 수 없습니다.") : Tr(L"The selected URL is not a playlist.", L"선택한 주소는 재생목록이 아닙니다."));
                return;
            }
            const std::string name = c->Svc().playlists.AddOrUpdateReference(url, title, items, true);
            if (name.empty()) {
                c->ShowInfoBar(InfoSeverity::Error, Tr(L"Could not import playlist", L"재생목록을 가져올 수 없습니다"), Tr(L"The playlist could not be saved.", L"재생목록을 저장할 수 없습니다."));
                return;
            }
            c->PlaylistsChanged();
            if (onAdded) onAdded(name);
            c->ShowToast(TrW(L"Added " + wvp::ToWide(name), wvp::ToWide(name) + L" 추가함"));
        });
    }).detach();
}

void ImportReferenceAsEditableCopy(AppContext& ctx, const std::string& name,
                                   std::function<void(const std::string&)> onImported) {
    auto& store = ctx.Svc().playlists;
    if (!store.IsReference(name)) {
        ctx.ShowInfoBar(InfoSeverity::Error, Tr(L"Could not import playlist", L"재생목록을 가져올 수 없습니다"), Tr(L"This playlist no longer references a source.", L"이 재생목록은 더 이상 원본을 참조하지 않습니다."));
        return;
    }
    AppContext* c = &ctx;
    std::thread([c, name, onImported = std::move(onImported)]() mutable {
        auto result = c->Svc().playlists.RefreshReference(name, wvp::PlaylistLoader());
        const bool usingFallback = result.usedOfflineFallback;
        const bool canImport = (result.refreshed && result.isPlaylist && !result.isHlsStream) || usingFallback;
        std::string baseName = result.title.empty() ? name : result.title;
        std::string importedName;
        if (canImport) importedName = c->Svc().playlists.ImportEditableCopy(baseName, result.items, true, false);
        c->PostToUi([c, result = std::move(result), usingFallback, canImport, importedName = std::move(importedName),
                     onImported = std::move(onImported)]() mutable {
            if (!result.error.empty()) {
                c->ShowInfoBar(usingFallback ? InfoSeverity::Warning : InfoSeverity::Error,
                               usingFallback ? Tr(L"Playlist refresh failed", L"재생목록을 새로 고치지 못했습니다") : Tr(L"Could not read playlist", L"재생목록을 읽을 수 없습니다"),
                               usingFallback ? std::wstring(Tr(L"Using the last fetched items as an offline fallback. ", L"마지막으로 가져온 항목을 오프라인 대체본으로 사용합니다. ")) + wvp::ToWide(result.error) : wvp::ToWide(result.error));
            }
            if (!canImport) return;
            if (importedName.empty()) {
                c->ShowInfoBar(InfoSeverity::Error, Tr(L"Could not import playlist", L"재생목록을 가져올 수 없습니다"), Tr(L"The editable copy could not be saved.", L"편집 가능한 사본을 저장할 수 없습니다."));
                return;
            }
            if (onImported) onImported(importedName);
            c->ShowToast(TrW(L"Imported " + wvp::ToWide(importedName), wvp::ToWide(importedName) + L" 가져옴"));
        });
    }).detach();
}

void OpenPaths(AppContext& ctx, const std::vector<std::wstring>& pathsOrUrls, std::function<void()> onDone) {
    Dispatch(ctx, pathsOrUrls, DispatchTarget::VideoView, {}, false, std::move(onDone));
}

void DropPathsToPlaylist(AppContext& ctx, const std::vector<std::wstring>& pathsOrUrls, const std::string& playlistName,
                         std::function<void()> onDone) {
    if (ctx.IsPlaylistReadOnly(playlistName)) {
        ctx.ShowInfoBar(InfoSeverity::Warning, Tr(L"Playlist is read-only", L"재생목록이 읽기 전용입니다"), Tr(L"Import it to make an editable copy.", L"가져오기로 편집 가능한 사본을 만드세요."));
        if (onDone) onDone();
        return;
    }
    Dispatch(ctx, pathsOrUrls, DispatchTarget::SpecificPlaylist, playlistName, true, std::move(onDone));
}

void DropPathsOnVideoView(AppContext& ctx, const std::vector<std::wstring>& pathsOrUrls, std::function<void()> onDone) {
    Dispatch(ctx, pathsOrUrls, DispatchTarget::VideoView, {}, true, std::move(onDone));
}

void LoadSubtitleFile(AppContext& ctx, const std::vector<std::wstring>& paths) {
    auto* p = ctx.Player();
    if (!p || !p->GetMediaInfo()) return;
    if (!paths.empty()) AttachSubtitleFiles(ctx, paths, true);
}

void LoadSubtitleFile(AppContext& ctx) {
    auto* p = ctx.Player();
    if (!p || !p->GetMediaInfo()) return;
    LoadSubtitleFile(ctx, PickSubtitleFiles(ctx));
}

bool DispatchDroppedPaths(AppContext& ctx, DropRoute route, const std::vector<std::wstring>& pathsOrUrls,
                          std::function<void()> onDone) {
    if (pathsOrUrls.empty()) return false;
    switch (route) {
    case DropRoute::VideoView:
        if (std::all_of(pathsOrUrls.begin(), pathsOrUrls.end(), [](const std::wstring& s) { return open::IsSubtitleFile(s); })) {
            auto* p = ctx.Player();
            if (!p || !p->GetMediaInfo()) return false;   // nothing open: a subtitle has nothing to attach to
            AttachSubtitleFiles(ctx, pathsOrUrls, true);
            if (onDone) onDone();
            return true;
        }
        DropPathsOnVideoView(ctx, pathsOrUrls, std::move(onDone));
        return true;
    case DropRoute::PlaylistPanel:   // handled by App::HandleDropAtClient (needs the panel's viewed playlist)
    case DropRoute::Rejected:
        return false;
    }
    return false;
}

void OpenFiles(AppContext& ctx) { OpenPaths(ctx, PickMediaFiles(ctx)); }

void OpenFolder(AppContext& ctx) {
    bool cancelled = false;
    auto files = PickFolderMedia(ctx, cancelled);
    OpenPaths(ctx, files);
}

void AddFilesToPlaylist(AppContext& ctx, const std::string& playlistName, std::function<void()> onDone) {
    auto files = PickMediaFiles(ctx);
    if (!files.empty()) DropPathsToPlaylist(ctx, files, playlistName, std::move(onDone));
}

void AddFolderToPlaylist(AppContext& ctx, const std::string& playlistName, std::function<void()> onDone) {
    bool cancelled = false;
    auto files = PickFolderMedia(ctx, cancelled);
    if (!files.empty()) DropPathsToPlaylist(ctx, files, playlistName, std::move(onDone));
}

void AddUrlToPlaylist(AppContext& ctx, const std::string& playlistName, std::function<void()> onDone) {
    TextPromptOptions o;
    o.title = Tr(L"Add URL", L"주소 추가");
    o.label = Tr(L"Enter the address of a video or stream", L"동영상 또는 스트림 주소를 입력하세요");
    o.placeholder = L"https://";
    o.okText = Tr(L"Add", L"추가");
    o.validate = [](const std::wstring& t) { return open::ValidateUrl(t); };
    auto url = PromptText(ctx, o);
    if (!url) return;
    size_t b = url->find_first_not_of(L" \t\r\n"), e = url->find_last_not_of(L" \t\r\n");
    if (b == std::wstring::npos) return;
    DropPathsToPlaylist(ctx, {url->substr(b, e - b + 1)}, playlistName, std::move(onDone));
}

void OpenUrl(AppContext& ctx) {
    TextPromptOptions o;
    o.title = Tr(L"Open URL", L"주소 열기");
    o.label = Tr(L"Enter the address of a video, stream or playlist", L"동영상, 스트림 또는 재생목록의 주소를 입력하세요");
    o.placeholder = L"https://";
    o.okText = Tr(L"Open", L"열기");
    o.validate = [](const std::wstring& t) { return open::ValidateUrl(t); };
    if (auto url = PromptText(ctx, o)) {
        std::wstring u = *url;
        size_t b = u.find_first_not_of(L" \t\r\n"), e = u.find_last_not_of(L" \t\r\n");
        u = b == std::wstring::npos ? std::wstring() : u.substr(b, e - b + 1);
        OpenPaths(ctx, {u});
    }
}


namespace {
std::wstring PromptToW(NSString* s) {
    if (!s) return {};
    NSData* d = [s dataUsingEncoding:NSUTF32LittleEndianStringEncoding];
    return std::wstring(static_cast<const wchar_t*>(d.bytes), d.length / sizeof(wchar_t));
}
NSString* PromptToNS(const std::wstring& w) {
    return [[NSString alloc] initWithBytes:w.data() length:w.size() * sizeof(wchar_t) encoding:NSUTF32LittleEndianStringEncoding];
}
}

// --- TextInputDialog.h: the modal name prompt ----------------------------------------------------------------------

std::optional<std::wstring> PromptText(AppContext&, const TextPromptOptions& options) {
    if (OsDialogsBlocked()) return std::nullopt;
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = PromptToNS(options.title);
    alert.informativeText = PromptToNS(options.label);
    [alert addButtonWithTitle:PromptToNS(options.okText)];
    [alert addButtonWithTitle:PromptToNS(Tr(L"Cancel", L"취소"))];
    NSTextField* field = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 260, 24)];
    field.stringValue = PromptToNS(options.initial);
    field.placeholderString = PromptToNS(options.placeholder);
    alert.accessoryView = field;
    alert.window.initialFirstResponder = field;
    for (;;) {
        if ([alert runModal] != NSAlertFirstButtonReturn) return std::nullopt;
        std::wstring text = PromptToW(field.stringValue);
        std::wstring err = options.validate ? options.validate(text) : (text.empty() ? std::wstring(L"-") : std::wstring());
        if (err.empty()) return text;
        alert.informativeText = PromptToNS(err == L"-" ? options.label : err);
    }
}

}
