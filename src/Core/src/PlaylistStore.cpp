#include "wvp/PlaylistStore.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include "wvp/AppSettings.h"
#include "wvp/JsonLite.h"
#include "wvp/JsonWriter.h"
#include "wvp/M3uParser.h"
#include "wvp/M3uWriter.h"
#include "wvp/PlaylistLoader.h"
#include "wvp/MediaUri.h"
#include "wvp/Utf.h"

namespace wvp {

namespace {

constexpr wchar_t kExt[] = L".m3u8";
constexpr size_t kMaxName = 100;

std::wstring TrimWs(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) a++;
    while (b > a && iswspace(s[b - 1])) b--;
    return s.substr(a, b - a);
}

bool IsReserved(const std::wstring& n) {
    std::wstring base = WinPath::ToLower(n.substr(0, n.find(L'.')));
    while (!base.empty() && base.back() == L' ') base.pop_back();
    static const wchar_t* const names[] = { L"con", L"prn", L"aux", L"nul" };
    for (auto* r : names) if (base == r) return true;
    if (base.size() == 4 && (base.compare(0, 3, L"com") == 0 || base.compare(0, 3, L"lpt") == 0) && base[3] >= L'1' && base[3] <= L'9') return true;
    return false;
}

std::int64_t FileTimeToUnixMs(const FILETIME& ft) {
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    return (std::int64_t)(u.QuadPart / 10000ULL) - 11644473600000LL;
}

bool SameSource(const std::wstring& a, const std::wstring& b) {
    if (a == b) return true;
    auto x = MediaUri::Parse(a), y = MediaUri::Parse(b);
    if (x.IsLocal() && y.IsLocal()) {
        const std::wstring& px = x.localPath ? *x.localPath : a;
        const std::wstring& py = y.localPath ? *y.localPath : b;
        return WinPath::ToLower(WinPath::GetFullPath(px)) == WinPath::ToLower(WinPath::GetFullPath(py));
    }
    return false;
}

struct StoredItemState { std::wstring key; double position = 0, duration = 0; bool played = false, seekable = false; };
struct StoredPlaylistState { std::wstring name, item; double position = 0; int index = -1; };

void ReadPlaybackHistory(const std::wstring& path, std::vector<StoredItemState>& items, std::vector<StoredPlaylistState>& playlists) {
    items.clear(); playlists.clear();
    std::string bytes;
    if (!fileio::ReadAllBytes(path, bytes)) return;
    auto root = json::Parse(bytes);
    if (!root || !root->IsObject()) return;
    if (const auto* arr = root->Find("items"); arr && arr->IsArray()) {
        for (const auto& v : arr->arr) {
            if (!v.IsObject() || !v.HasString("key")) continue;
            StoredItemState s;
            s.key = ToWide(v.Str("key"));
            s.position = std::max(0.0, v.Dbl("position"));
            s.duration = std::max(0.0, v.Dbl("duration"));
            s.played = v.Bool("played");
            s.seekable = v.Bool("seekable");
            if (!s.key.empty()) items.push_back(std::move(s));
        }
    }
    if (const auto* arr = root->Find("playlists"); arr && arr->IsArray()) {
        for (const auto& v : arr->arr) {
            if (!v.IsObject() || !v.HasString("name")) continue;
            playlists.push_back({ToWide(v.Str("name")), ToWide(v.Str("item")), std::max(0.0, v.Dbl("position")), v.Int("index")});
        }
    }
}

bool WritePlaybackHistory(const std::wstring& path, const std::vector<StoredItemState>& items,
                          const std::vector<StoredPlaylistState>& playlists) {
    JsonWriter w;
    w.BeginObject().Key("items").BeginArray();
    for (const auto& s : items) {
        w.BeginObject().Prop("key", s.key).Prop("position", s.position).Prop("duration", s.duration)
            .Prop("played", s.played).Prop("seekable", s.seekable).EndObject();
    }
    w.EndArray().Key("playlists").BeginArray();
    for (const auto& s : playlists) {
        w.BeginObject().Prop("name", s.name).Prop("item", s.item).Prop("position", s.position).Prop("index", s.index).EndObject();
    }
    w.EndArray().EndObject();
    // Frequent pause / seek / item-change writes skip FlushFileBuffers and WRITE_THROUGH, which can stall for
    // hundreds of ms. The replace stays atomic.
    return fileio::AtomicWriteFile(path, w.Str(), false);
}

}

std::wstring PlaylistStore::DefaultDir() { return SettingsStore::AppDataDir() + L"\\Playlists"; }

// One background thread per store (started with the first job) that runs history writes in order.
class PlaylistStore::HistoryWriter {
public:
    ~HistoryWriter() {
        {
            std::lock_guard<std::mutex> g(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();   // the queue is finished first: the last position is not lost
    }
    void Post(std::wstring key, std::function<void()> job) {
        {
            std::lock_guard<std::mutex> g(mu_);
            for (auto& q : queue_)
                if (q.first == key) { q.second = std::move(job); job = nullptr; break; }   // not started yet: replace
            if (job) queue_.emplace_back(std::move(key), std::move(job));
            if (!thread_.joinable()) thread_ = std::thread([this] { Run(); });
        }
        cv_.notify_all();
    }
    void Flush() {
        std::unique_lock<std::mutex> g(mu_);
        idle_.wait(g, [&] { return queue_.empty() && !busy_; });
    }

private:
    void Run() {
        std::unique_lock<std::mutex> g(mu_);
        for (;;) {
            cv_.wait(g, [&] { return stop_ || !queue_.empty(); });
            if (queue_.empty()) return;   // stop_ and nothing left
            auto job = std::move(queue_.front().second);
            queue_.pop_front();
            busy_ = true;
            g.unlock();
            try { job(); } catch (...) {}
            g.lock();
            busy_ = false;
            if (queue_.empty()) idle_.notify_all();
        }
    }
    std::mutex mu_;
    std::condition_variable cv_, idle_;
    std::deque<std::pair<std::wstring, std::function<void()>>> queue_;
    bool busy_ = false, stop_ = false;
    std::thread thread_;
};

PlaylistStore::PlaylistStore(std::wstring dir)
    : dir_(dir.empty() ? DefaultDir() : std::move(dir)), writer_(std::make_shared<HistoryWriter>()) {
    while (dir_.size() > 3 && (dir_.back() == L'\\' || dir_.back() == L'/')) dir_.pop_back();
}

std::wstring PlaylistStore::ReferenceIndexPath() const { return dir_ + L"\\references.json"; }
std::wstring PlaylistStore::PlaybackHistoryPath() const { return dir_ + L"\\playback-history.json"; }

bool PlaylistStore::LoadReferences(std::vector<Reference>& out) const {
    out.clear();
    try {
        std::string bytes;
        if (!fileio::ReadAllBytes(ReferenceIndexPath(), bytes)) return true;
        auto root = json::Parse(bytes);
        if (!root || !root->IsObject()) return false;
        const auto* refs = root->Find("references");
        if (!refs || !refs->IsArray()) return true;
        for (const auto& v : refs->arr) {
            if (!v.IsObject() || !v.HasString("name") || !v.HasString("source")) continue;
            Reference r;
            r.name = ToWide(v.Str("name"));
            r.source = ToWide(v.Str("source"));
            r.isRemote = v.Bool("remote");
            const auto* items = v.Find("items");
            if (items && items->IsArray()) {
                for (const auto& m : items->arr) {
                    if (!m.IsObject() || !m.HasString("url")) continue;
                    MediaItem item(ToWide(m.Str("url")), ToWide(m.Str("title")));
                    double duration = m.Dbl("duration");
                    if (std::isfinite(duration) && duration > 0) item.duration = duration;
                    r.items.push_back(std::move(item));
                }
            }
            if (IsValidName(r.name) && !r.source.empty()) out.push_back(std::move(r));
        }
        return true;
    } catch (...) { out.clear(); return false; }
}

bool PlaylistStore::SaveReferences(const std::vector<Reference>& refs) const {
    try {
        JsonWriter w;
        w.BeginObject().Key("references").BeginArray();
        for (const auto& r : refs) {
            w.BeginObject().Prop("name", r.name).Prop("source", r.source).Prop("remote", r.isRemote).Key("items").BeginArray();
            for (const auto& item : r.items) {
                w.BeginObject().Prop("url", item.url).Prop("title", item.title);
                if (item.duration && std::isfinite(*item.duration) && *item.duration > 0) w.Prop("duration", *item.duration);
                w.EndObject();
            }
            w.EndArray().EndObject();
        }
        w.EndArray().EndObject();
        return fileio::AtomicWriteFile(ReferenceIndexPath(), w.Str());
    } catch (...) { return false; }
}

std::wstring PlaylistStore::ItemKey(const std::wstring& url) {
    const MediaUriInfo uri = MediaUri::Parse(url);
    if (uri.IsLocal()) {
        const std::wstring path = uri.localPath ? *uri.localPath : url;
        return L"file:" + WinPath::ToLower(WinPath::GetFullPath(path));
    }
    return L"url:" + url;
}

std::wstring PlaylistStore::SanitizeName(const std::wstring& name) {
    std::wstring s;
    for (wchar_t c : name) {
        if (c < 0x20 || c == L'<' || c == L'>' || c == L':' || c == L'"' || c == L'/' || c == L'\\' || c == L'|' || c == L'?' || c == L'*') s += L'_';
        else s += c;
    }
    s = TrimWs(s);
    while (!s.empty() && (s.back() == L'.' || s.back() == L' ')) s.pop_back();
    if (s.size() > kMaxName) { s.resize(kMaxName); while (!s.empty() && (s.back() == L'.' || iswspace(s.back()))) s.pop_back(); }
    if (s.empty()) return s;
    if (IsReserved(s)) s += L'_';
    return s;
}

bool PlaylistStore::IsValidName(const std::wstring& name) { return !name.empty() && SanitizeName(name) == name; }

std::wstring PlaylistStore::PathFor(const std::wstring& name) const { return dir_ + L"\\" + name + kExt; }

bool PlaylistStore::Exists(const std::wstring& name) const {
    if (!IsValidName(name)) return false;
    if (FindReferenceByName(name)) return true;
    DWORD a = GetFileAttributesW(PathFor(name).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool PlaylistStore::IsReference(const std::wstring& name) const { return FindReferenceByName(name).has_value(); }

std::optional<PlaylistStore::Reference> PlaylistStore::FindReferenceByName(const std::wstring& name) const {
    if (!IsValidName(name)) return std::nullopt;
    std::vector<Reference> refs;
    if (!LoadReferences(refs)) return std::nullopt;
    auto it = std::find_if(refs.begin(), refs.end(), [&](const Reference& r) { return _wcsicmp(r.name.c_str(), name.c_str()) == 0; });
    return it == refs.end() ? std::nullopt : std::optional<Reference>(*it);
}

std::optional<PlaylistStore::Reference> PlaylistStore::FindReferenceBySource(const std::wstring& source) const {
    if (source.empty()) return std::nullopt;
    std::vector<Reference> refs;
    if (!LoadReferences(refs)) return std::nullopt;
    auto it = std::find_if(refs.begin(), refs.end(), [&](const Reference& r) { return SameSource(r.source, source); });
    return it == refs.end() ? std::nullopt : std::optional<Reference>(*it);
}

std::wstring PlaylistStore::AddOrUpdateReference(const std::wstring& source, const std::wstring& suggestedName,
                                                 const std::vector<MediaItem>& items, bool isRemote) {
    if (source.empty()) return {};
    std::vector<Reference> refs;
    if (!LoadReferences(refs)) refs.clear();
    for (auto& r : refs) {
        if (!SameSource(r.source, source)) continue;
        r.items = items;
        r.isRemote = isRemote;
        return SaveReferences(refs) ? r.name : std::wstring();
    }
    std::wstring base = SanitizeName(suggestedName);
    if (base.empty()) base = L"Playlist";
    std::wstring name = CreateUniqueName(base);
    refs.push_back({name, source, isRemote, items});
    return SaveReferences(refs) ? name : std::wstring();
}

bool PlaylistStore::UpdateReferenceItems(const std::wstring& name, const std::vector<MediaItem>& items) {
    std::vector<Reference> refs;
    if (!LoadReferences(refs)) return false;
    auto it = std::find_if(refs.begin(), refs.end(), [&](const Reference& r) { return _wcsicmp(r.name.c_str(), name.c_str()) == 0; });
    if (it == refs.end()) return false;
    it->items = items;
    return SaveReferences(refs);
}

PlaylistStore::ReferenceRefreshResult PlaylistStore::RefreshReference(const std::wstring& name, const PlaylistLoader& loader) {
    ReferenceRefreshResult result;
    auto ref = FindReferenceByName(name);
    if (!ref) { result.error = L"The referenced playlist no longer exists."; return result; }
    try {
        PlaylistLoadResult loaded = loader.Load(ref->source);
        result.title = std::move(loaded.title);
        result.isPlaylist = loaded.isPlaylist;
        result.isHlsStream = loaded.isHlsStream;
        if (!loaded.isPlaylist || loaded.isHlsStream) {
            result.items = ref->items;
            result.usedOfflineFallback = !result.items.empty();
            result.error = loaded.isHlsStream ? L"The source is an HLS video stream, not a playlist." : L"The source no longer contains a playlist.";
        } else {
            result.items = std::move(loaded.items);
            result.refreshed = true;
            UpdateReferenceItems(name, result.items);
        }
    } catch (const std::exception& ex) {
        result.items = ref->items;
        result.usedOfflineFallback = !result.items.empty();
        result.error = ToWide(ex.what());
    }
    return result;
}

std::vector<PlaylistStore::Info> PlaylistStore::List() const {
    std::vector<Info> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir_ + L"\\*" + kExt).c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring file = fd.cFileName;
            // FindFirstFile also matches 8.3 aliases like *.m3u8x: require the exact extension.
            if (file.size() <= 5 || WinPath::ToLower(file.substr(file.size() - 5)) != kExt) continue;
            Info i;
            i.name = file.substr(0, file.size() - 5);
            i.path = dir_ + L"\\" + file;
            i.modifiedMs = FileTimeToUnixMs(fd.ftLastWriteTime);
            try {
                std::string bytes;
                if (fileio::ReadAllBytes(i.path, bytes))
                    i.itemCount = (int)M3uParser::Parse(M3uParser::Decode((const std::uint8_t*)bytes.data(), bytes.size(), false), i.path).items.size();
            } catch (...) {}
            out.push_back(std::move(i));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::vector<Reference> refs;
    if (LoadReferences(refs)) {
        for (const auto& r : refs) {
            Info i;
            i.name = r.name;
            i.path = r.source;
            i.itemCount = (int)r.items.size();
            i.isReference = true;
            i.isRemote = r.isRemote;
            i.source = r.source;
            out.push_back(std::move(i));
        }
    }
    std::sort(out.begin(), out.end(), [](const Info& a, const Info& b) {
        std::wstring la = WinPath::ToLower(a.name), lb = WinPath::ToLower(b.name);
        return la != lb ? la < lb : a.name < b.name;
    });
    return out;
}

std::vector<MediaItem> PlaylistStore::Load(const std::wstring& name) const {
    if (!IsValidName(name)) return {};
    if (auto ref = FindReferenceByName(name)) return ref->items;
    try {
        std::string bytes;
        std::wstring path = PathFor(name);
        if (!fileio::ReadAllBytes(path, bytes)) return {};
        return M3uParser::Parse(M3uParser::Decode((const std::uint8_t*)bytes.data(), bytes.size(), false), path).items;
    } catch (...) {
        return {};
    }
}

bool PlaylistStore::Save(const std::wstring& name, const std::vector<MediaItem>& items) const {
    if (!IsValidName(name) || IsReference(name)) return false;
    try { return M3uWriter::Save(PathFor(name), items, false); }
    catch (...) { return false; }
}

std::wstring PlaylistStore::CreateUniqueName(const std::wstring& base) const {
    std::wstring b = SanitizeName(base);
    if (b.empty()) b = L"Untitled playlist";
    if (!Exists(b)) return b;
    for (int n = 2;; n++) {
        std::wstring c = b + L" " + std::to_wstring(n);
        if (!Exists(c)) return c;
    }
}

std::wstring PlaylistStore::CreateUniqueImportName(const std::wstring& base) const {
    std::wstring b = SanitizeName(base);
    if (b.empty()) b = L"Playlist";
    if (!Exists(b)) return b;
    for (int n = 2;; ++n) {
        const std::wstring suffix = L" (" + std::to_wstring(n) + L")";
        std::wstring stem = b;
        if (stem.size() + suffix.size() > kMaxName) stem.resize(kMaxName - suffix.size());
        while (!stem.empty() && (stem.back() == L'.' || iswspace(stem.back()))) stem.pop_back();
        std::wstring candidate = stem + suffix;
        if (!Exists(candidate)) return candidate;
    }
}

std::wstring PlaylistStore::ImportEditableCopy(const std::wstring& baseName, const std::vector<MediaItem>& items,
                                               bool isPlaylist, bool isHlsStream) {
    if (!isPlaylist || isHlsStream) return {};
    const std::wstring name = CreateUniqueImportName(baseName);
    return Save(name, items) ? name : std::wstring();
}

PlaylistStore::ItemPlaybackState PlaylistStore::GetItemPlaybackState(const std::wstring& url) const {
    FlushPlaybackWrites();
    return ReadItemPlaybackState(url);
}

void PlaylistStore::FlushPlaybackWrites() const {
    if (writer_) writer_->Flush();
}

void PlaylistStore::SavePlaybackStateAsync(const std::wstring& playlistName, const std::wstring& itemUrl, double position,
                                           double duration, bool playedHint, bool seekable, int index) const {
    if (itemUrl.empty()) return;
    // the job works on a copy of the store without the writer: it never touches `this`, and its own
    // SavePlaybackState does not wait for itself
    PlaylistStore copy(*this);
    copy.writer_.reset();
    auto job = [copy, playlistName, itemUrl, position, duration, playedHint, seekable, index] {
        const bool played = playedHint || copy.ReadItemPlaybackState(itemUrl).played;
        copy.SavePlaybackState(playlistName, itemUrl, (!seekable || played) ? 0.0 : position, duration, played, seekable, index);
    };
    if (!writer_) { job(); return; }
    writer_->Post(playlistName + L"\n" + itemUrl, std::move(job));
}

PlaylistStore::ItemPlaybackState PlaylistStore::ReadItemPlaybackState(const std::wstring& url) const {
    ItemPlaybackState result;
    if (url.empty()) return result;
    std::vector<StoredItemState> items;
    std::vector<StoredPlaylistState> playlists;
    ReadPlaybackHistory(PlaybackHistoryPath(), items, playlists);
    const std::wstring key = ItemKey(url);
    auto it = std::find_if(items.begin(), items.end(), [&](const StoredItemState& s) { return s.key == key; });
    if (it != items.end()) result = {it->position, it->duration, it->played, it->seekable};
    return result;
}

std::vector<bool> PlaylistStore::GetPlayedFlags(const std::vector<std::wstring>& urls) const {
    FlushPlaybackWrites();
    std::vector<bool> out(urls.size(), false);
    if (urls.empty()) return out;
    std::vector<StoredItemState> items;
    std::vector<StoredPlaylistState> playlists;
    ReadPlaybackHistory(PlaybackHistoryPath(), items, playlists);
    std::vector<std::wstring> keys;
    for (const auto& s : items) if (s.played) keys.push_back(s.key);
    std::sort(keys.begin(), keys.end());
    for (size_t i = 0; i < urls.size(); i++)
        if (!urls[i].empty()) out[i] = std::binary_search(keys.begin(), keys.end(), ItemKey(urls[i]));
    return out;
}

PlaylistStore::PlaylistPlaybackState PlaylistStore::GetPlaylistPlaybackState(const std::wstring& name) const {
    FlushPlaybackWrites();
    PlaylistPlaybackState result;
    std::vector<StoredItemState> items;
    std::vector<StoredPlaylistState> playlists;
    ReadPlaybackHistory(PlaybackHistoryPath(), items, playlists);
    auto it = std::find_if(playlists.begin(), playlists.end(), [&](const StoredPlaylistState& s) { return s.name == name; });
    if (it != playlists.end()) result = {it->item, it->position, it->index};
    return result;
}

bool PlaylistStore::SavePlaybackState(const std::wstring& playlistName, const std::wstring& itemUrl,
                                      double position, double duration, bool played, bool seekable, int index) const {
    if (itemUrl.empty()) return false;
    FlushPlaybackWrites();   // in order with the asynchronous writes (no-op on the writer's own copy)
    std::vector<StoredItemState> items;
    std::vector<StoredPlaylistState> playlists;
    ReadPlaybackHistory(PlaybackHistoryPath(), items, playlists);
    position = std::isfinite(position) ? std::max(0.0, position) : 0.0;
    duration = std::isfinite(duration) ? std::max(0.0, duration) : 0.0;
    const std::wstring key = ItemKey(itemUrl);
    auto item = std::find_if(items.begin(), items.end(), [&](const StoredItemState& s) { return s.key == key; });
    StoredItemState itemState{key, played ? 0.0 : position, duration, played, seekable};
    if (item == items.end()) items.push_back(std::move(itemState)); else *item = std::move(itemState);
    auto pl = std::find_if(playlists.begin(), playlists.end(), [&](const StoredPlaylistState& s) { return s.name == playlistName; });
    StoredPlaylistState playlistState{playlistName, itemUrl, played ? 0.0 : position, index};
    if (pl == playlists.end()) playlists.push_back(std::move(playlistState)); else *pl = std::move(playlistState);
    return WritePlaybackHistory(PlaybackHistoryPath(), items, playlists);
}

bool PlaylistStore::IsPlayedPosition(double position, double duration, bool reachedEof) {
    if (reachedEof) return true;
    if (!std::isfinite(position) || !std::isfinite(duration) || duration <= 0) return false;
    const double remaining = std::max(0.0, duration - std::max(0.0, position));
    return remaining <= std::min(PlayedMaxSeconds, duration * PlayedDurationFraction);
}

double PlaylistStore::ResumePosition(const ItemPlaybackState& state) {
    if (state.played || !state.seekable || !std::isfinite(state.position) || !std::isfinite(state.duration) || state.duration <= 0)
        return 0;
    if (state.position <= ResumeBoundarySeconds || state.duration - state.position <= ResumeBoundarySeconds) return 0;
    return std::max(0.0, state.position);
}

bool PlaylistStore::Rename(const std::wstring& from, const std::wstring& to, std::wstring* err) {
    auto fail = [&](const wchar_t* m) { if (err) *err = m; return false; };
    if (!IsValidName(from) || !Exists(from)) return fail(L"The playlist no longer exists.");
    if (!IsValidName(to)) return fail(L"The name is empty or contains characters that are not allowed: < > : \" / \\ | ? *");
    if (from == to) return true;
    bool caseOnly = WinPath::ToLower(from) == WinPath::ToLower(to);
    if (!caseOnly && Exists(to)) return fail(L"A playlist with that name already exists.");
    if (auto ref = FindReferenceByName(from)) {
        std::vector<Reference> refs;
        if (!LoadReferences(refs)) return fail(L"The playlist could not be renamed.");
        auto it = std::find_if(refs.begin(), refs.end(), [&](const Reference& r) { return _wcsicmp(r.name.c_str(), from.c_str()) == 0; });
        if (it == refs.end()) return fail(L"The playlist no longer exists.");
        it->name = to;
        if (!SaveReferences(refs)) return fail(L"The playlist could not be renamed.");
        std::vector<StoredItemState> items;
        std::vector<StoredPlaylistState> playlists;
        ReadPlaybackHistory(PlaybackHistoryPath(), items, playlists);
        for (auto& p : playlists) if (p.name == from) p.name = to;
        WritePlaybackHistory(PlaybackHistoryPath(), items, playlists);
        return true;
    }
    if (!MoveFileExW(PathFor(from).c_str(), PathFor(to).c_str(), 0)) return fail(L"The playlist could not be renamed.");
    std::vector<StoredItemState> items;
    std::vector<StoredPlaylistState> playlists;
    ReadPlaybackHistory(PlaybackHistoryPath(), items, playlists);
    for (auto& p : playlists) if (p.name == from) p.name = to;
    WritePlaybackHistory(PlaybackHistoryPath(), items, playlists);
    return true;
}

bool PlaylistStore::Delete(const std::wstring& name) {
    if (!IsValidName(name)) return false;
    bool deleted = false;
    std::vector<Reference> refs;
    if (LoadReferences(refs)) {
        auto before = refs.size();
        refs.erase(std::remove_if(refs.begin(), refs.end(), [&](const Reference& r) { return _wcsicmp(r.name.c_str(), name.c_str()) == 0; }), refs.end());
        if (refs.size() != before) deleted = SaveReferences(refs);
    }
    if (!deleted) deleted = DeleteFileW(PathFor(name).c_str()) != 0;
    if (!deleted) return false;
    std::vector<StoredItemState> items;
    std::vector<StoredPlaylistState> playlists;
    ReadPlaybackHistory(PlaybackHistoryPath(), items, playlists);
    playlists.erase(std::remove_if(playlists.begin(), playlists.end(), [&](const StoredPlaylistState& p) { return p.name == name; }), playlists.end());
    WritePlaybackHistory(PlaybackHistoryPath(), items, playlists);
    return true;
}

}
