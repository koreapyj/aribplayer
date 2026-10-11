#include "wvp/PlaylistStore.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
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
#include "wvp/Platform.h"
#include "wvp/Utf8.h"

namespace wvp {

namespace {

constexpr char kExt[] = ".m3u8";
constexpr size_t kMaxName = 100;

std::string TrimWs(const std::string& s) { return std::string(utf8::Trim(s)); }

bool ReadFile(const std::string& path, std::string& out) {
    return platform::ReadAllBytes(path, out, (1ull << 30) - 1) == platform::ReadResult::Ok;
}

// Names are limited in UTF-16 code units, the unit of the file system's name length limit.
void TruncateUtf16(std::string& s, size_t units) {
    size_t i = 0, n = 0;
    while (i < s.size()) {
        size_t next = i;
        n += utf8::Next(s, next) >= 0x10000 ? 2 : 1;
        if (n > units) break;
        i = next;
    }
    s.resize(i);
}

void TrimDotsAndSpaceEnd(std::string& s) {
    for (;;) {
        if (!s.empty() && s.back() == '.') { s.pop_back(); continue; }
        std::string_view t = utf8::Trim(s);
        size_t end = (size_t)(t.data() - s.data()) + t.size();
        if (t.empty() || end == s.size()) { if (t.empty()) s.clear(); return; }
        s.resize(end);
    }
}

bool IsReserved(const std::string& n) {
    std::string base = utf8::ToLowerAscii(n.substr(0, n.find('.')));
    while (!base.empty() && base.back() == ' ') base.pop_back();
    static const char* const names[] = { "con", "prn", "aux", "nul" };
    for (auto* r : names) if (base == r) return true;
    if (base.size() == 4 && (base.compare(0, 3, "com") == 0 || base.compare(0, 3, "lpt") == 0) && base[3] >= '1' && base[3] <= '9') return true;
    return false;
}

bool SameSource(const std::string& a, const std::string& b) {
    if (a == b) return true;
    auto x = MediaUri::Parse(a), y = MediaUri::Parse(b);
    if (x.IsLocal() && y.IsLocal()) {
        const std::string& px = x.localPath ? *x.localPath : a;
        const std::string& py = y.localPath ? *y.localPath : b;
        return utf8::ToLowerAscii(platform::FullPath(px)) == utf8::ToLowerAscii(platform::FullPath(py));
    }
    return false;
}

struct StoredItemState { std::string key; double position = 0, duration = 0; bool played = false, seekable = false; };
struct StoredPlaylistState { std::string name, item; double position = 0; int index = -1; };

void ReadPlaybackHistory(const std::string& path, std::vector<StoredItemState>& items, std::vector<StoredPlaylistState>& playlists) {
    items.clear(); playlists.clear();
    std::string bytes;
    if (!ReadFile(path, bytes)) return;
    auto root = json::Parse(bytes);
    if (!root || !root->IsObject()) return;
    if (const auto* arr = root->Find("items"); arr && arr->IsArray()) {
        for (const auto& v : arr->arr) {
            if (!v.IsObject() || !v.HasString("key")) continue;
            StoredItemState s;
            s.key = v.Str("key");
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
            playlists.push_back({v.Str("name"), v.Str("item"), std::max(0.0, v.Dbl("position")), v.Int("index")});
        }
    }
}

bool WritePlaybackHistory(const std::string& path, const std::vector<StoredItemState>& items,
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
    return platform::AtomicWriteFile(path, w.Str(), false);
}

}

std::string PlaylistStore::DefaultDir() { return platform::Combine(platform::AppDataDir(), "Playlists"); }

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
    void Post(std::string key, std::function<void()> job) {
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
    std::deque<std::pair<std::string, std::function<void()>>> queue_;
    bool busy_ = false, stop_ = false;
    std::thread thread_;
};

PlaylistStore::PlaylistStore(std::string dir)
    : dir_(dir.empty() ? DefaultDir() : std::move(dir)), writer_(std::make_shared<HistoryWriter>()) {
    while (dir_.size() > 3 && (dir_.back() == '\\' || dir_.back() == '/')) dir_.pop_back();
}

std::string PlaylistStore::ReferenceIndexPath() const { return platform::Combine(dir_, "references.json"); }
std::string PlaylistStore::PlaybackHistoryPath() const { return platform::Combine(dir_, "playback-history.json"); }

bool PlaylistStore::LoadReferences(std::vector<Reference>& out) const {
    out.clear();
    try {
        std::string bytes;
        if (!ReadFile(ReferenceIndexPath(), bytes)) return true;
        auto root = json::Parse(bytes);
        if (!root || !root->IsObject()) return false;
        const auto* refs = root->Find("references");
        if (!refs || !refs->IsArray()) return true;
        for (const auto& v : refs->arr) {
            if (!v.IsObject() || !v.HasString("name") || !v.HasString("source")) continue;
            Reference r;
            r.name = v.Str("name");
            r.source = v.Str("source");
            r.isRemote = v.Bool("remote");
            const auto* items = v.Find("items");
            if (items && items->IsArray()) {
                for (const auto& m : items->arr) {
                    if (!m.IsObject() || !m.HasString("url")) continue;
                    MediaItem item(m.Str("url"), m.Str("title"));
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
        return platform::AtomicWriteFile(ReferenceIndexPath(), w.Str());
    } catch (...) { return false; }
}

std::string PlaylistStore::ItemKey(const std::string& url) {
    const MediaUriInfo uri = MediaUri::Parse(url);
    if (uri.IsLocal()) {
        const std::string path = uri.localPath ? *uri.localPath : url;
        return "file:" + utf8::ToLowerAscii(platform::FullPath(path));
    }
    return "url:" + url;
}

std::string PlaylistStore::SanitizeName(const std::string& name) {
    std::string s;
    for (char ch : name) {
        unsigned char c = (unsigned char)ch;
        if (c < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') s += '_';
        else s += ch;
    }
    s = TrimWs(s);
    while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();
    if (utf8::Utf16Length(s) > kMaxName) { TruncateUtf16(s, kMaxName); TrimDotsAndSpaceEnd(s); }
    if (s.empty()) return s;
    if (IsReserved(s)) s += '_';
    return s;
}

bool PlaylistStore::IsValidName(const std::string& name) { return !name.empty() && SanitizeName(name) == name; }

std::string PlaylistStore::PathFor(const std::string& name) const { return platform::Combine(dir_, name + kExt); }

bool PlaylistStore::Exists(const std::string& name) const {
    if (!IsValidName(name)) return false;
    if (FindReferenceByName(name)) return true;
    return platform::Exists(PathFor(name));
}

bool PlaylistStore::IsReference(const std::string& name) const { return FindReferenceByName(name).has_value(); }

std::optional<PlaylistStore::Reference> PlaylistStore::FindReferenceByName(const std::string& name) const {
    if (!IsValidName(name)) return std::nullopt;
    std::vector<Reference> refs;
    if (!LoadReferences(refs)) return std::nullopt;
    auto it = std::find_if(refs.begin(), refs.end(), [&](const Reference& r) { return platform::NamesEqualIgnoreCase(r.name, name); });
    return it == refs.end() ? std::nullopt : std::optional<Reference>(*it);
}

std::optional<PlaylistStore::Reference> PlaylistStore::FindReferenceBySource(const std::string& source) const {
    if (source.empty()) return std::nullopt;
    std::vector<Reference> refs;
    if (!LoadReferences(refs)) return std::nullopt;
    auto it = std::find_if(refs.begin(), refs.end(), [&](const Reference& r) { return SameSource(r.source, source); });
    return it == refs.end() ? std::nullopt : std::optional<Reference>(*it);
}

std::string PlaylistStore::AddOrUpdateReference(const std::string& source, const std::string& suggestedName,
                                                 const std::vector<MediaItem>& items, bool isRemote) {
    if (source.empty()) return {};
    std::vector<Reference> refs;
    if (!LoadReferences(refs)) refs.clear();
    for (auto& r : refs) {
        if (!SameSource(r.source, source)) continue;
        r.items = items;
        r.isRemote = isRemote;
        return SaveReferences(refs) ? r.name : std::string();
    }
    std::string base = SanitizeName(suggestedName);
    if (base.empty()) base = "Playlist";
    std::string name = CreateUniqueName(base);
    refs.push_back({name, source, isRemote, items});
    return SaveReferences(refs) ? name : std::string();
}

bool PlaylistStore::UpdateReferenceItems(const std::string& name, const std::vector<MediaItem>& items) {
    std::vector<Reference> refs;
    if (!LoadReferences(refs)) return false;
    auto it = std::find_if(refs.begin(), refs.end(), [&](const Reference& r) { return platform::NamesEqualIgnoreCase(r.name, name); });
    if (it == refs.end()) return false;
    it->items = items;
    return SaveReferences(refs);
}

PlaylistStore::ReferenceRefreshResult PlaylistStore::RefreshReference(const std::string& name, const PlaylistLoader& loader) {
    ReferenceRefreshResult result;
    auto ref = FindReferenceByName(name);
    if (!ref) { result.error = "The referenced playlist no longer exists."; return result; }
    try {
        PlaylistLoadResult loaded = loader.Load(ref->source);
        result.title = std::move(loaded.title);
        result.isPlaylist = loaded.isPlaylist;
        result.isHlsStream = loaded.isHlsStream;
        if (!loaded.isPlaylist || loaded.isHlsStream) {
            result.items = ref->items;
            result.usedOfflineFallback = !result.items.empty();
            result.error = loaded.isHlsStream ? "The source is an HLS video stream, not a playlist." : "The source no longer contains a playlist.";
        } else {
            result.items = std::move(loaded.items);
            result.refreshed = true;
            UpdateReferenceItems(name, result.items);
        }
    } catch (const std::exception& ex) {
        result.items = ref->items;
        result.usedOfflineFallback = !result.items.empty();
        result.error = ex.what();
    }
    return result;
}

std::vector<PlaylistStore::Info> PlaylistStore::List() const {
    std::vector<Info> out;
    for (const auto& f : platform::ListFiles(dir_, kExt)) {
        Info i;
        i.name = f.name.substr(0, f.name.size() - (sizeof kExt - 1));
        i.path = platform::Combine(dir_, f.name);
        i.modifiedMs = f.modifiedMs;
        try {
            std::string bytes;
            if (ReadFile(i.path, bytes))
                i.itemCount = (int)M3uParser::Parse(M3uParser::Decode((const std::uint8_t*)bytes.data(), bytes.size(), false), i.path).items.size();
        } catch (...) {}
        out.push_back(std::move(i));
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
        std::string la = utf8::ToLowerAscii(a.name), lb = utf8::ToLowerAscii(b.name);
        return la != lb ? la < lb : a.name < b.name;
    });
    return out;
}

std::vector<MediaItem> PlaylistStore::Load(const std::string& name) const {
    if (!IsValidName(name)) return {};
    if (auto ref = FindReferenceByName(name)) return ref->items;
    try {
        std::string bytes;
        std::string path = PathFor(name);
        if (!ReadFile(path, bytes)) return {};
        return M3uParser::Parse(M3uParser::Decode((const std::uint8_t*)bytes.data(), bytes.size(), false), path).items;
    } catch (...) {
        return {};
    }
}

bool PlaylistStore::Save(const std::string& name, const std::vector<MediaItem>& items) const {
    if (!IsValidName(name) || IsReference(name)) return false;
    try { return M3uWriter::Save(PathFor(name), items, false); }
    catch (...) { return false; }
}

std::string PlaylistStore::CreateUniqueName(const std::string& base) const {
    std::string b = SanitizeName(base);
    if (b.empty()) b = "Untitled playlist";
    if (!Exists(b)) return b;
    for (int n = 2;; n++) {
        std::string c = b + " " + std::to_string(n);
        if (!Exists(c)) return c;
    }
}

std::string PlaylistStore::CreateUniqueImportName(const std::string& base) const {
    std::string b = SanitizeName(base);
    if (b.empty()) b = "Playlist";
    if (!Exists(b)) return b;
    for (int n = 2;; ++n) {
        const std::string suffix = " (" + std::to_string(n) + ")";
        std::string stem = b;
        if (utf8::Utf16Length(stem) + suffix.size() > kMaxName) TruncateUtf16(stem, kMaxName - suffix.size());
        TrimDotsAndSpaceEnd(stem);
        std::string candidate = stem + suffix;
        if (!Exists(candidate)) return candidate;
    }
}

std::string PlaylistStore::ImportEditableCopy(const std::string& baseName, const std::vector<MediaItem>& items,
                                               bool isPlaylist, bool isHlsStream) {
    if (!isPlaylist || isHlsStream) return {};
    const std::string name = CreateUniqueImportName(baseName);
    return Save(name, items) ? name : std::string();
}

PlaylistStore::ItemPlaybackState PlaylistStore::GetItemPlaybackState(const std::string& url) const {
    FlushPlaybackWrites();
    return ReadItemPlaybackState(url);
}

void PlaylistStore::FlushPlaybackWrites() const {
    if (writer_) writer_->Flush();
}

void PlaylistStore::SavePlaybackStateAsync(const std::string& playlistName, const std::string& itemUrl, double position,
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
    writer_->Post(playlistName + "\n" + itemUrl, std::move(job));
}

PlaylistStore::ItemPlaybackState PlaylistStore::ReadItemPlaybackState(const std::string& url) const {
    ItemPlaybackState result;
    if (url.empty()) return result;
    std::vector<StoredItemState> items;
    std::vector<StoredPlaylistState> playlists;
    ReadPlaybackHistory(PlaybackHistoryPath(), items, playlists);
    const std::string key = ItemKey(url);
    auto it = std::find_if(items.begin(), items.end(), [&](const StoredItemState& s) { return s.key == key; });
    if (it != items.end()) result = {it->position, it->duration, it->played, it->seekable};
    return result;
}

std::vector<bool> PlaylistStore::GetPlayedFlags(const std::vector<std::string>& urls) const {
    FlushPlaybackWrites();
    std::vector<bool> out(urls.size(), false);
    if (urls.empty()) return out;
    std::vector<StoredItemState> items;
    std::vector<StoredPlaylistState> playlists;
    ReadPlaybackHistory(PlaybackHistoryPath(), items, playlists);
    std::vector<std::string> keys;
    for (const auto& s : items) if (s.played) keys.push_back(s.key);
    std::sort(keys.begin(), keys.end());
    for (size_t i = 0; i < urls.size(); i++)
        if (!urls[i].empty()) out[i] = std::binary_search(keys.begin(), keys.end(), ItemKey(urls[i]));
    return out;
}

PlaylistStore::PlaylistPlaybackState PlaylistStore::GetPlaylistPlaybackState(const std::string& name) const {
    FlushPlaybackWrites();
    PlaylistPlaybackState result;
    std::vector<StoredItemState> items;
    std::vector<StoredPlaylistState> playlists;
    ReadPlaybackHistory(PlaybackHistoryPath(), items, playlists);
    auto it = std::find_if(playlists.begin(), playlists.end(), [&](const StoredPlaylistState& s) { return s.name == name; });
    if (it != playlists.end()) result = {it->item, it->position, it->index};
    return result;
}

bool PlaylistStore::SavePlaybackState(const std::string& playlistName, const std::string& itemUrl,
                                      double position, double duration, bool played, bool seekable, int index) const {
    if (itemUrl.empty()) return false;
    FlushPlaybackWrites();   // in order with the asynchronous writes (no-op on the writer's own copy)
    std::vector<StoredItemState> items;
    std::vector<StoredPlaylistState> playlists;
    ReadPlaybackHistory(PlaybackHistoryPath(), items, playlists);
    position = std::isfinite(position) ? std::max(0.0, position) : 0.0;
    duration = std::isfinite(duration) ? std::max(0.0, duration) : 0.0;
    const std::string key = ItemKey(itemUrl);
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

bool PlaylistStore::Rename(const std::string& from, const std::string& to, std::string* err) {
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    if (!IsValidName(from) || !Exists(from)) return fail("The playlist no longer exists.");
    if (!IsValidName(to)) return fail("The name is empty or contains characters that are not allowed: < > : \" / \\ | ? *");
    if (from == to) return true;
    bool caseOnly = platform::NamesEqualIgnoreCase(from, to);
    if (!caseOnly && Exists(to)) return fail("A playlist with that name already exists.");
    if (auto ref = FindReferenceByName(from)) {
        std::vector<Reference> refs;
        if (!LoadReferences(refs)) return fail("The playlist could not be renamed.");
        auto it = std::find_if(refs.begin(), refs.end(), [&](const Reference& r) { return platform::NamesEqualIgnoreCase(r.name, from); });
        if (it == refs.end()) return fail("The playlist no longer exists.");
        it->name = to;
        if (!SaveReferences(refs)) return fail("The playlist could not be renamed.");
        std::vector<StoredItemState> items;
        std::vector<StoredPlaylistState> playlists;
        ReadPlaybackHistory(PlaybackHistoryPath(), items, playlists);
        for (auto& p : playlists) if (p.name == from) p.name = to;
        WritePlaybackHistory(PlaybackHistoryPath(), items, playlists);
        return true;
    }
    if (!platform::RenameNoReplace(PathFor(from), PathFor(to))) return fail("The playlist could not be renamed.");
    std::vector<StoredItemState> items;
    std::vector<StoredPlaylistState> playlists;
    ReadPlaybackHistory(PlaybackHistoryPath(), items, playlists);
    for (auto& p : playlists) if (p.name == from) p.name = to;
    WritePlaybackHistory(PlaybackHistoryPath(), items, playlists);
    return true;
}

bool PlaylistStore::Delete(const std::string& name) {
    if (!IsValidName(name)) return false;
    bool deleted = false;
    std::vector<Reference> refs;
    if (LoadReferences(refs)) {
        auto before = refs.size();
        refs.erase(std::remove_if(refs.begin(), refs.end(), [&](const Reference& r) { return platform::NamesEqualIgnoreCase(r.name, name); }), refs.end());
        if (refs.size() != before) deleted = SaveReferences(refs);
    }
    if (!deleted) deleted = platform::RemoveFile(PathFor(name));
    if (!deleted) return false;
    std::vector<StoredItemState> items;
    std::vector<StoredPlaylistState> playlists;
    ReadPlaybackHistory(PlaybackHistoryPath(), items, playlists);
    playlists.erase(std::remove_if(playlists.begin(), playlists.end(), [&](const StoredPlaylistState& p) { return p.name == name; }), playlists.end());
    WritePlaybackHistory(PlaybackHistoryPath(), items, playlists);
    return true;
}

}
