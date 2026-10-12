// --logictest cases of the playlist management (panel menus, open / drop flows, helpers). Headless: no window, no clipboard, temp dirs only.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>

#pragma comment(lib, "Ws2_32.lib")

#include "LogicTest.h"
#include "wvp/FilterManager.h"
#include "wvp/CapabilitySet.h"
#include "features/NativeMenu.h"
#include "features/OpenFlows.h"
#include "features/ListsCommon.h"
#include "shell/PlaylistPanel.h"
#include "pui/Glyphs.h"
#include "wvp/M3uWriter.h"
#include "wvp/PlaylistLoader.h"
#include "wvp/win32/Utf.h"

using namespace app;
using namespace app::lists;

namespace {

std::string TempDir(const char* tag) {
    static unsigned n = 0;
    std::filesystem::path p = std::filesystem::temp_directory_path() /
                              (L"aribplayer-lists-" + wvp::ToWide(tag) + L"-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++n));
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return wvp::ToUtf8(p.wstring());
}

void RemoveDir(const std::string& dir) { std::filesystem::remove_all(std::filesystem::path(wvp::ToWide(dir))); }

std::vector<wvp::MediaItem> MakeItems(int n, const char* prefix = "C:\\media\\clip") {
    std::vector<wvp::MediaItem> v;
    for (int i = 0; i < n; i++) v.emplace_back(std::string(prefix) + std::to_string(i) + ".mp4", "Clip " + std::to_string(i), 10.0 + i);
    return v;
}

std::vector<std::string> Urls(const std::vector<wvp::MediaItem>& v) {
    std::vector<std::string> o;
    for (auto& i : v) o.push_back(i.url);
    return o;
}

class FakePlayer final : public wvp::IMediaPlayer {
public:
    int opens = 0, stops = 0;
    std::vector<std::string> openedUrls;
    std::vector<wvp::OpenOptions> openedOptions;
    wvp::PlayerState State() const override { return wvp::PlayerState::Playing; }
    int Volume() const override { return 100; }
    void SetVolume(int) override {}
    bool IsMuted() const override { return false; }
    void SetMuted(bool) override {}
    double Position() override { return 0; }
    double Duration() override { return 0; }
    int Open(const std::string& url, const wvp::OpenOptions& options = {}) override {
        opens++;
        openedUrls.push_back(url);
        openedOptions.push_back(options);
        return 0;
    }
    int Stop() override { stops++; return 0; }
    int Play() override { return 0; }
    int Pause() override { return 0; }
    int TogglePause() override { return 0; }
    int Seek(double, bool) override { return 0; }
    int CycleStream(wvp::StreamType) override { return 0; }
    int SetVideoFilter(const std::string&) override { return 0; }
    int SetAudioFilter(const std::string&) override { return 0; }
    int SendFilterCommand(const std::string&, const std::string&, const std::string&) override { return 0; }
    std::optional<wvp::MediaInfo> GetMediaInfo() override { return std::nullopt; }
    std::optional<wvp::BgraFrame> Snapshot() override { return std::nullopt; }
};

class LocalHttpServer {
public:
    LocalHttpServer(std::string body, int status = 200) : body_(std::move(body)), status_(status) {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("WSAStartup failed");
        started_ = true;
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == INVALID_SOCKET) { WSACleanup(); started_ = false; throw std::runtime_error("socket failed"); }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR || listen(listener_, SOMAXCONN) == SOCKET_ERROR) {
            closesocket(listener_); listener_ = INVALID_SOCKET; WSACleanup(); started_ = false;
            throw std::runtime_error("bind/listen failed");
        }
        int len = sizeof(addr);
        if (getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len) == SOCKET_ERROR) {
            closesocket(listener_); listener_ = INVALID_SOCKET; WSACleanup(); started_ = false;
            throw std::runtime_error("getsockname failed");
        }
        port_ = ntohs(addr.sin_port);
        worker_ = std::thread([this] { Serve(); });
    }
    ~LocalHttpServer() {
        stopping_ = true;
        if (listener_ != INVALID_SOCKET) { shutdown(listener_, SD_BOTH); closesocket(listener_); }
        if (worker_.joinable()) worker_.join();
        if (started_) WSACleanup();
    }
    std::string Url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/list.m3u"; }
    void SetResponse(int status, std::string body) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = status;
        body_ = std::move(body);
    }
    int Requests() const { return requests_.load(); }
private:
    void Serve() {
        while (!stopping_) {
            SOCKET client = accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) break;
            char req[2048];
            recv(client, req, sizeof(req), 0);
            std::string body;
            int status;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                body = body_;
                status = status_;
            }
            requests_++;
            const std::string reason = status == 200 ? "OK" : "Service Unavailable";
            std::string response = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n" +
                "Content-Type: application/vnd.apple.mpegurl\r\nContent-Length: " + std::to_string(body.size()) +
                "\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n" + body;
            size_t sent = 0;
            while (sent < response.size()) {
                int n = send(client, response.data() + sent, (int)std::min<size_t>(response.size() - sent, INT_MAX), 0);
                if (n <= 0) break;
                sent += (size_t)n;
            }
            shutdown(client, SD_BOTH);
            closesocket(client);
        }
    }
    SOCKET listener_ = INVALID_SOCKET;
    unsigned short port_ = 0;
    std::string body_;
    int status_ = 200;
    std::mutex mutex_;
    std::atomic<int> requests_{0};
    std::atomic<bool> stopping_{false};
    bool started_ = false;
    std::thread worker_;
};

template<class Fn>
bool WaitFor(Fn&& fn, DWORD timeoutMs = 10000) {
    const ULONGLONG end = GetTickCount64() + timeoutMs;
    do {
        if (fn()) return true;
        Sleep(10);
    } while (GetTickCount64() < end);
    return fn();
}

bool WritePlaylistText(const std::string& path, const std::string& text) {
    std::ofstream file(std::filesystem::path(wvp::ToWide(path)), std::ios::binary | std::ios::trunc);
    file.write(text.data(), (std::streamsize)text.size());
    return file.good();
}

// The panel hands its menu descriptors to the native menu, which only records them in tests (it never shows anything).
struct MenuProbe {
    std::vector<pui::MenuItemDesc> items;
    int ItemCount() const { return (int)items.size(); }
    const pui::MenuItemDesc& Item(int i) const { return items[(size_t)i]; }
    void ActivateIndex(int i) { nativemenu::InvokeItem(items, items[(size_t)i].text); }
};
MenuProbe* ShownMenu() {
    static MenuProbe probe;
    probe.items = nativemenu::LastShown();
    nativemenu::ClearLastShown();
    return probe.items.empty() ? nullptr : &probe;
}

int FindMenuItem(const MenuProbe& menu, const std::wstring& text) {
    for (int i = 0; i < menu.ItemCount(); ++i) if (menu.Item(i).text == text) return i;
    return -1;
}

class StubContext final : public AppContext {
public:
    explicit StubContext(const std::string& dataDir, bool withController = false) : svc(wvp::ToWide(dataDir)) {
        if (withController) {
            ctl = std::make_unique<wvp::PlaybackController>(player);
            svc.filters.SetCapabilities(wvp::fakecaps::None());
            ctl->SetFilters(svc.filters.Provider());
        }
    }
    HWND MainHwnd() const override { return nullptr; }
    pui::UiHost& Ui() override { return host; }
    wvp::PlaybackController* Controller() override { return ctl.get(); }
    wvp::FfPlayer* Player() override { return nullptr; }
    Services& Svc() override { return svc; }
    pui::IRenderer& Measurer() override { return measurer; }
    std::string CurrentPlaylistName() const override { return currentName; }
    std::vector<wvp::MediaItem> PlaylistItems(const std::string& name) override {
        if (name == currentName && ctl) return ctl->Items();
        if (name.empty()) return unnamedItems;
        return svc.playlists.Load(name);
    }
    bool SavePlaylistItems(const std::string& name, const std::vector<wvp::MediaItem>& items) override {
        bool saved = name.empty() ? (unnamedItems = items, true) : svc.playlists.Save(name, items);
        if (name == currentName && ctl) {
            std::string current = ctl->Current() ? ctl->Current()->url : std::string();
            int index = -1;
            for (size_t i = 0; i < items.size(); i++) if (!current.empty() && items[i].url == current) { index = (int)i; break; }
            ctl->SetQueue(items, index, false);
        }
        return saved;
    }
    void PlayPlaylist(const std::string& name, const std::vector<wvp::MediaItem>& items, int start) override {
        PlayPlaylistAtPosition(name, items, start, -1);
    }
    void PlayPlaylistAtPosition(const std::string& name, const std::vector<wvp::MediaItem>& items, int start, double position) override {
        currentName = name; played = items; playStart = start; playPosition = position; playReplace = true;
        if (ctl) ctl->SetQueue(items, start, true, position);
    }
    void AppendPlaylistItems(const std::string& name, const std::vector<wvp::MediaItem>& items) override {
        AppendItems(name, items, false);
    }
    void AppendItems(const std::string& name, const std::vector<wvp::MediaItem>& items, bool startIfIdle) {
        if (name == currentName) {
            enqueued.insert(enqueued.end(), items.begin(), items.end());
            if (ctl) {
                bool startFirst = startIfIdle && ctl->CurrentIndex() < 0;
                ctl->Enqueue(items);
                if (startFirst && !ctl->Items().empty()) ctl->PlayAt(0);
                if (name.empty()) unnamedItems = ctl->Items(); else svc.playlists.Save(name, ctl->Items());
            } else {
                auto all = PlaylistItems(name); all.insert(all.end(), items.begin(), items.end());
                SavePlaylistItems(name, all);
            }
        } else {
            auto all = PlaylistItems(name); all.insert(all.end(), items.begin(), items.end());
            SavePlaylistItems(name, all);
        }
    }
    void AppendToCurrentPlaylist(std::vector<wvp::MediaItem> items) override { AppendItems(currentName, items, true); }
    void ReplaceUnnamedPlaylistAtPosition(std::vector<wvp::MediaItem> items, double) override {
        currentName.clear(); unnamedItems = items; played = items; playStart = 0; playReplace = true;
        if (ctl) ctl->SetQueue(items, 0, true);
    }
    void PlaylistRenamed(const std::string& from, const std::string& to) override { if (currentName == from) currentName = to; }
    void PlaylistDeleted(const std::string& name) override { if (currentName == name) currentName.clear(); }
    void SetFullscreen(bool) override {}
    bool IsFullscreen() const override { return false; }
    void SetMiniPlayer(bool) override {}
    bool IsMiniPlayer() const override { return false; }
    ShellMode Mode() const override { return ShellMode::NowPlaying; }
    void ShowInfoBar(InfoSeverity, const std::wstring&, const std::wstring& m) override {
        infoBars.push_back(m);
        if (onInfoBar) onInfoBar();
    }
    void ShowToast(const std::wstring& t) override {
        toasts.push_back(t);
        if (onToast) onToast();
    }
    void ApplyTheme(pui::ThemeMode, uint32_t) override {}
    const pui::Theme& CurrentTheme() const override { return theme; }
    pui::ThemeMode CurrentThemeMode() const override { return pui::ThemeMode::Dark; }
    float Scale() const override { return 1.f; }
    void RequestRedraw() override {}
    void Execute(CommandId) override {}
    void PostToUi(std::function<void()> fn) override { fn(); }

    FakePlayer player;
    std::unique_ptr<wvp::PlaybackController> ctl;
    Services svc;
    pui::UiHost host;
    pui::NullMeasurer measurer;
    pui::Theme theme;
    std::string currentName;
    std::vector<wvp::MediaItem> unnamedItems;
    std::vector<wvp::MediaItem> played, enqueued;
    int playStart = -1;
    double playPosition = -1;
    bool playReplace = false;
    std::vector<std::wstring> infoBars, toasts;
    std::function<void()> onInfoBar, onToast;
};

pui::Button* PopupButton(StubContext& ctx, const std::wstring& name) {
    auto* d = dynamic_cast<ConfirmDialog*>(ctx.host.TopPopup());
    if (!d) return nullptr;
    return name == L"Cancel" ? d->CancelButton() : d->ConfirmButton();
}

}


LOGIC_TEST(lists_item_count_text) {
    LT_CHECK_EQ(ItemCountText(0), L"0 items");
    LT_CHECK_EQ(ItemCountText(1), L"1 item");
    LT_CHECK_EQ(ItemCountText(2), L"2 items");
    LT_CHECK_EQ(ItemCountText(1234), L"1234 items");
    LT_CHECK_EQ(ItemCountText(-5), L"0 items");
}

LOGIC_TEST(lists_name_validation_messages) {
    std::string dir = TempDir("names");
    wvp::PlaylistStore store(dir);
    LT_CHECK(ValidatePlaylistName(store, "Road trip").empty());
    LT_CHECK(!ValidatePlaylistName(store, "").empty());
    LT_CHECK(!ValidatePlaylistName(store, "   ").empty());
    LT_CHECK(!ValidatePlaylistName(store, "a/b").empty());
    LT_CHECK(!ValidatePlaylistName(store, "what?").empty());
    LT_CHECK(!ValidatePlaylistName(store, std::string(101, 'x')).empty());
    LT_CHECK(store.Save("Road trip", MakeItems(2)));
    LT_CHECK_EQ(ValidatePlaylistName(store, "Road trip"), L"A playlist with this name already exists.");
    LT_CHECK_EQ(ValidatePlaylistName(store, "  road trip "), L"A playlist with this name already exists.");
    LT_CHECK(ValidatePlaylistName(store, "Road trip", "Road trip").empty());
    LT_CHECK(ValidatePlaylistName(store, "ROAD TRIP", "Road trip").empty());
    LT_CHECK(ValidatePlaylistName(store, "Other", "Road trip").empty());
    LT_CHECK_EQ(TrimName("  x y \t"), "x y");
    RemoveDir(dir);
}

LOGIC_TEST(lists_overview_row_drop_appends_to_that_active_playlist) {
    std::string dir = TempDir("rowdrop");
    {
        StubContext ctx(dir, true);
        auto existing = MakeItems(1, "C:\\mix\\");
        LT_CHECK(ctx.svc.playlists.Save("Mix", existing));
        ctx.currentName = "Mix";
        ctx.ctl->SetQueue(existing, 0, true);
        const std::string target = "Mix";
        int opens = ctx.player.opens;
        auto done = std::make_shared<std::promise<void>>();
        auto complete = done->get_future();
        DropPathsToPlaylist(ctx, {L"C:\\drop\\added.mp4"}, target, [done] { done->set_value(); });
        LT_CHECK(complete.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
        auto saved = ctx.svc.playlists.Load("Mix");
        LT_CHECK_EQ((int)saved.size(), 2);
        LT_CHECK_EQ(saved.back().url, "C:\\drop\\added.mp4");
        LT_CHECK_EQ((int)ctx.ctl->Items().size(), 2);
        LT_CHECK_EQ(ctx.ctl->CurrentIndex(), 0);
        LT_CHECK_EQ(ctx.player.opens, opens);   // appending on the overview row does not interrupt playback
        auto done2 = std::make_shared<std::promise<void>>();
        auto complete2 = done2->get_future();
        DropPathsToPlaylist(ctx, {L"C:\\drop\\details.mp4"}, "Mix", [done2] { done2->set_value(); });
        LT_CHECK(complete2.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
        saved = ctx.svc.playlists.Load("Mix");
        LT_CHECK_EQ((int)saved.size(), 3);
        LT_CHECK_EQ(saved.back().url, "C:\\drop\\details.mp4");
        LT_CHECK_EQ(ctx.player.opens, opens);   // a details-view drop appends to the named playlist without a restart
    }
    RemoveDir(dir);
}

LOGIC_TEST(lists_playlist_view_drops_never_start_playback) {
    std::string dir = TempDir("dropnoplay");
    {
        StubContext ctx(dir, true);   // current = the unnamed playlist, nothing has played
        LT_CHECK(ctx.svc.playlists.Save("Mix", MakeItems(1, "C:\\mix\\")));
        LT_CHECK_EQ(ctx.ctl->CurrentIndex(), -1);
        auto drop = [&](const std::string& target, const wchar_t* path) {
            auto done = std::make_shared<std::promise<void>>();
            auto f = done->get_future();
            DropPathsToPlaylist(ctx, {path}, target, [done] { done->set_value(); });
            LT_CHECK(f.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
        };
        drop("", L"C:\\drop\\a.mp4");
        drop(ctx.CurrentPlaylistName(), L"C:\\drop\\b.mp4");
        drop("", L"C:\\drop\\c.mp4");
        drop("Mix", L"C:\\drop\\d.mp4");
        LT_CHECK_EQ(ctx.player.opens, 0);
        LT_CHECK_EQ(ctx.ctl->CurrentIndex(), -1);
        LT_CHECK_EQ((int)ctx.ctl->Items().size(), 3);           // a, b, c appended to the current (unnamed) playlist
        LT_CHECK_EQ((int)ctx.svc.playlists.Load("Mix").size(), 2);
        // An Open appends and, with nothing played, starts the first item
        ctx.AppendToCurrentPlaylist(MakeItems(1, "C:\\open\\"));
        LT_CHECK_EQ(ctx.ctl->CurrentIndex(), 0);
        LT_CHECK(ctx.player.opens > 0);
        // something playing: a drop on the current playlist's details keeps the current item and does not reopen
        int opens = ctx.player.opens;
        const std::string cur = ctx.ctl->Current()->url;
        drop("", L"C:\\drop\\e.mp4");
        LT_CHECK_EQ(ctx.player.opens, opens);
        LT_CHECK_EQ(ctx.ctl->Current()->url, cur);
    }
    RemoveDir(dir);
}

LOGIC_TEST(lists_video_drop_replaces_unnamed_playlist_and_plays) {
    std::string dir = TempDir("homedrop");
    {
        StubContext ctx(dir, true);
        auto named = MakeItems(2, "C:\\named\\");
        LT_CHECK(ctx.svc.playlists.Save("Mix", named));
        ctx.currentName = "Mix";
        ctx.ctl->SetQueue(named, 0, true);
        const int opens = ctx.player.opens;
        auto done = std::make_shared<std::promise<void>>();
        auto complete = done->get_future();
        LT_CHECK(DispatchDroppedPaths(ctx, DropRoute::VideoView, {L"C:\\drop\\home.mp4"},
                                      [done] { done->set_value(); }));
        LT_CHECK(complete.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
        LT_CHECK(ctx.currentName.empty());
        LT_CHECK_EQ((int)ctx.unnamedItems.size(), 1);
        LT_CHECK_EQ(ctx.unnamedItems[0].url, "C:\\drop\\home.mp4");
        LT_CHECK_EQ((int)ctx.ctl->Items().size(), 1);
        LT_CHECK_EQ(ctx.ctl->CurrentIndex(), 0);
        LT_CHECK(ctx.ctl->Current() && ctx.ctl->Current()->url == "C:\\drop\\home.mp4");
        LT_CHECK_EQ(ctx.player.opens, opens + 1);
        LT_CHECK(ctx.playStart == 0 && ctx.playReplace);
        LT_CHECK(ctx.infoBars.empty() && ctx.toasts.empty());
        LT_CHECK_EQ((int)ctx.svc.playlists.Load("Mix").size(), 2);
    }
    RemoveDir(dir);
}

LOGIC_TEST(lists_open_videos_replace_unnamed_like_video_view_drop) {
    std::string dir = TempDir("openvideoreplace");
    {
        StubContext ctx(dir, true);
        auto named = MakeItems(2, "C:\\named\\");
        auto oldUnnamed = MakeItems(3, "C:\\old\\");
        LT_CHECK(ctx.svc.playlists.Save("Mix", named));
        ctx.currentName = "Mix";
        ctx.unnamedItems = oldUnnamed;
        ctx.ctl->SetQueue(named, 1, true);
        const int opens = ctx.player.opens;
        auto done = std::make_shared<std::promise<void>>();
        auto complete = done->get_future();
        OpenPaths(ctx, {L"C:\\open\\video.mp4"}, [done] { done->set_value(); });
        LT_CHECK(complete.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
        LT_CHECK(ctx.currentName.empty());
        LT_CHECK_EQ((int)ctx.unnamedItems.size(), 1);
        LT_CHECK_EQ(ctx.unnamedItems[0].url, "C:\\open\\video.mp4");
        LT_CHECK(Urls(ctx.svc.playlists.Load("Mix")) == Urls(named));
        LT_CHECK(ctx.ctl->Current() && ctx.ctl->Current()->url == "C:\\open\\video.mp4");
        LT_CHECK_EQ(ctx.ctl->CurrentIndex(), 0);
        LT_CHECK_EQ(ctx.player.opens, opens + 1);
        LT_CHECK(ctx.playStart == 0 && ctx.playReplace);
    }
    RemoveDir(dir);
}

LOGIC_TEST(lists_mixed_open_and_video_drop_expand_playlists_in_order) {
    std::string dir = TempDir("mixedopen");
    {
        StubContext ctx(dir, true);
        const std::string source = dir + "\\source.m3u";
        auto nested = std::vector<wvp::MediaItem>{wvp::MediaItem("C:\\list\\one.mp4", "One"), wvp::MediaItem("C:\\list\\two.mp4", "Two")};
        LT_CHECK(wvp::M3uWriter::Save(source, nested, false));
        auto runMixed = [&](bool drop) {
            auto done = std::make_shared<std::promise<void>>();
            auto complete = done->get_future();
            std::vector<std::wstring> inputs{L"C:\\before.mp4", wvp::ToWide(source), L"C:\\after.mp4"};
            if (drop) DropPathsOnVideoView(ctx, inputs, [done] { done->set_value(); });
            else OpenPaths(ctx, inputs, [done] { done->set_value(); });
            LT_CHECK(complete.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
            LT_CHECK(Urls(ctx.unnamedItems) == (std::vector<std::string>{"C:\\before.mp4", "C:\\list\\one.mp4", "C:\\list\\two.mp4", "C:\\after.mp4"}));
            LT_CHECK(ctx.currentName.empty());
            LT_CHECK_EQ(ctx.ctl->CurrentIndex(), 0);
            LT_CHECK_EQ(ctx.ctl->Current()->url, "C:\\before.mp4");
            LT_CHECK(ctx.svc.playlists.List().empty());  // mixed batches are not turned into reference entries
        };
        runMixed(false);
        runMixed(true);
    }
    RemoveDir(dir);
}

LOGIC_TEST(lists_playlist_only_open_creates_and_reuses_reference_entry) {
    std::string dir = TempDir("refopen");
    {
        StubContext ctx(dir, true);
        const std::string source = dir + "\\named-source.m3u";
        const std::string secondSource = dir + "\\later.m3u8";
        auto nested = std::vector<wvp::MediaItem>{wvp::MediaItem("C:\\list\\first.mp4", "First"), wvp::MediaItem("C:\\list\\second.mp4", "Second")};
        LT_CHECK(wvp::M3uWriter::Save(source, nested, false));
        LT_CHECK(wvp::M3uWriter::Save(secondSource, {wvp::MediaItem("C:\\list\\later.mp4", "Later")}, false));
        auto open = [&] {
            auto done = std::make_shared<std::promise<void>>();
            auto complete = done->get_future();
            OpenPaths(ctx, {wvp::ToWide(source), wvp::ToWide(secondSource)}, [done] { done->set_value(); });
            LT_CHECK(complete.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
        };
        open();
        LT_CHECK_EQ(ctx.currentName, "named-source");
        auto entry = ctx.svc.playlists.FindReferenceByName(ctx.currentName);
        LT_CHECK(entry.has_value());
        LT_CHECK(entry && entry->source == source);
        LT_CHECK(ctx.ctl->Current() && ctx.ctl->Current()->url == nested[0].url);
        auto refs = ctx.svc.playlists.List();
        LT_CHECK_EQ(refs.size(), (size_t)2);
        LT_CHECK(refs[0].isReference && refs[1].isReference);
        open();
        LT_CHECK_EQ(ctx.svc.playlists.List().size(), (size_t)2);
        LT_CHECK_EQ(ctx.currentName, "named-source");
    }
    RemoveDir(dir);
}

LOGIC_TEST(lists_unnamed_playlist_persists_and_restores_without_autoplay) {
    std::string dir = TempDir("unnamedroundtrip");
    auto items = MakeItems(3, "C:\\scratch\\");
    std::string path = dir + "\\unnamed.m3u8";
    LT_CHECK(wvp::M3uWriter::Save(path, items, false));
    auto loaded = wvp::PlaylistLoader().Load(path).items;
    LT_CHECK(Urls(loaded) == Urls(items));
    FakePlayer player;
    wvp::PlaybackController ctl(player);
    wvp::FilterManager fm;
    fm.SetCapabilities(wvp::fakecaps::None());
    ctl.SetFilters(fm.Provider());
    ctl.SetQueue(loaded, -1, false);
    LT_CHECK_EQ(player.opens, 0);
    LT_CHECK_EQ(ctl.CurrentIndex(), -1);
    LT_CHECK(ctl.PlayAt(0));
    LT_CHECK_EQ(player.opens, 1);
    LT_CHECK_EQ(ctl.Current()->url, items[0].url);
    RemoveDir(dir);
}

LOGIC_TEST(openflows_playlist_resume_and_row_play_use_controller_resolution) {
    std::string dir = TempDir("playlistresume");
    {
        StubContext ctx(dir, true);
        const std::vector<wvp::MediaItem> items{wvp::MediaItem("C:\\resume\\first.mp4", "First", 120.0),
                                                wvp::MediaItem("C:\\resume\\second.mp4", "Second", 120.0)};
        LT_CHECK(ctx.svc.playlists.Save("Mix", items));
        LT_CHECK(ctx.svc.playlists.SavePlaybackState("Mix", items[1].url, 42, 120, false, true, 1));
        ctx.ctl->resolveResumePosition = [&ctx](const wvp::MediaItem& item, double requested) {
            auto state = ctx.svc.playlists.GetItemPlaybackState(item.url);
            state.position = requested >= 0 ? requested : state.position;
            if (state.duration <= 0) state.duration = item.duration.value_or(0.0);
            return wvp::PlaylistStore::ResumePosition(state);
        };
        PlayPlaylistEntry(ctx, "Mix", 0, true);       // playlist resume selects item + its saved offset
        LT_CHECK_EQ(ctx.playStart, 1);
        LT_CHECK_EQ(ctx.playPosition, 42.0);
        LT_CHECK_EQ(ctx.ctl->Current()->url, items[1].url);
        LT_CHECK_EQ(ctx.player.openedUrls.back(), items[1].url);
        auto getSs = [](const wvp::OpenOptions& options) {
            for (const auto& item : options) if (item.first == "ss") return item.second;
            return std::string();
        };
        LT_CHECK_EQ(getSs(ctx.player.openedOptions.back()), "42.000000");

        LT_CHECK(ctx.svc.playlists.SavePlaybackState("Mix", items[0].url, 30, 120, false, true, 0));
        PlayPlaylistEntry(ctx, "Mix", 0, false);      // row play supplies no preferred time; controller resolves item history
        LT_CHECK_EQ(ctx.playStart, 0);
        LT_CHECK_EQ(ctx.playPosition, -1.0);
        LT_CHECK_EQ(ctx.ctl->Current()->url, items[0].url);
        LT_CHECK_EQ(ctx.player.openedUrls.back(), items[0].url);
        LT_CHECK_EQ(getSs(ctx.player.openedOptions.back()), "30.000000");
    }
    RemoveDir(dir);
}

LOGIC_TEST(lists_name_unnamed_playlist_promotes_named_and_empties_unnamed) {
    std::string dir = TempDir("nameunnamed");
    {
        StubContext ctx(dir, true);
        auto items = MakeItems(3);
        ctx.unnamedItems = items;
        ctx.ctl->SetQueue(items, 1, true);
        int opens = ctx.player.opens;
        LT_CHECK(NameUnnamedPlaylist(ctx, "Road trip", items, 1));
        LT_CHECK_EQ(ctx.currentName, "Road trip");
        LT_CHECK(ctx.svc.playlists.Exists("Road trip"));
        LT_CHECK(Urls(ctx.svc.playlists.Load("Road trip")) == Urls(items));
        LT_CHECK(ctx.unnamedItems.empty());
        LT_CHECK_EQ(ctx.ctl->CurrentIndex(), 1);
        LT_CHECK_EQ(ctx.ctl->Current()->url, items[1].url);
        LT_CHECK_EQ(ctx.player.opens, opens + 1);
    }
    RemoveDir(dir);
}

LOGIC_TEST(lists_video_view_drop_replaces_only_unnamed_and_plays_first) {
    std::string dir = TempDir("videodrop");
    {
        StubContext ctx(dir, true);
        auto named = MakeItems(2, "C:\\named\\");
        auto oldUnnamed = MakeItems(3, "C:\\unnamed\\");
        auto dropped = MakeItems(2, "C:\\drop\\");
        LT_CHECK(ctx.svc.playlists.Save("Mix", named));
        ctx.currentName = "Mix";
        ctx.unnamedItems = oldUnnamed;
        ctx.ctl->SetQueue(named, 1, true);
        int opens = ctx.player.opens;
        ctx.ReplaceUnnamedPlaylistAtPosition(dropped, -1);
        LT_CHECK(ctx.currentName.empty());
        LT_CHECK(Urls(ctx.unnamedItems) == Urls(dropped));
        LT_CHECK(Urls(ctx.svc.playlists.Load("Mix")) == Urls(named));
        LT_CHECK(Urls(ctx.ctl->Items()) == Urls(dropped));
        LT_CHECK_EQ(ctx.ctl->CurrentIndex(), 0);
        LT_CHECK_EQ(ctx.ctl->Current()->url, dropped[0].url);
        LT_CHECK_EQ(ctx.player.opens, opens + 1);
    }
    RemoveDir(dir);
}

LOGIC_TEST(lists_confirm_dialog_escape_closes_modal) {
    std::string dir = TempDir("dlg");
    {
        StubContext ctx(dir);
        ctx.host.SetSize({800, 600});
        bool confirmed = false;
        ShowConfirmDialog(ctx.host, L"Delete playlist?", L"Sure?", L"Delete", [&] { confirmed = true; });
        LT_CHECK_EQ((int)ctx.host.PopupCount(), 1);
        LT_CHECK(ctx.host.KeyDown(pui::key::Escape, 0));
        LT_CHECK_EQ((int)ctx.host.PopupCount(), 0);
        LT_CHECK(!confirmed);
        ShowConfirmDialog(ctx.host, L"Delete playlist?", L"Sure?", L"Delete", [&] { confirmed = true; });
        PopupButton(ctx, L"Delete")->Invoke();
        LT_CHECK(confirmed);
        LT_CHECK_EQ((int)ctx.host.PopupCount(), 0);
    }
    RemoveDir(dir);
}


LOGIC_TEST(lists_rejected_drop_has_no_side_effects) {
    std::string dir = TempDir("rejecteddrop");
    {
        StubContext ctx(dir, true);
        auto beforeItems = MakeItems(3, "C:\\playing\\");
        ctx.ctl->SetQueue(beforeItems, 1, true);
        const auto current = ctx.ctl->Current() ? ctx.ctl->Current()->url : std::string();
        const int index = ctx.ctl->CurrentIndex();
        const int opens = ctx.player.opens;
        const auto route = RouteDrop(false, false, false, false);   // outside the video, bar and title
        LT_CHECK(route == DropRoute::Rejected);
        LT_CHECK(!DispatchDroppedPaths(ctx, route, {L"C:\\drop\\ignored.mp4"}));
        LT_CHECK(Urls(ctx.ctl->Items()) == Urls(beforeItems));
        LT_CHECK_EQ(ctx.ctl->CurrentIndex(), index);
        LT_CHECK(ctx.ctl->Current() && ctx.ctl->Current()->url == current);
        LT_CHECK_EQ(ctx.player.opens, opens);
        LT_CHECK(ctx.played.empty() && ctx.enqueued.empty());
        LT_CHECK(ctx.infoBars.empty() && ctx.toasts.empty());
    }
    RemoveDir(dir);
}

namespace {
PlaylistPanel* MakePanel(StubContext& ctx) {
    auto* panel = static_cast<PlaylistPanel*>(ctx.host.SetRoot(std::make_unique<PlaylistPanel>(ctx)));
    ctx.host.SetSize({420, 720});
    panel->SetBounds({0, 0, 420, 720});
    panel->Refresh();
    return panel;
}
std::vector<std::wstring> MenuTexts(const MenuProbe& m) {
    std::vector<std::wstring> t;
    for (int i = 0; i < m.ItemCount(); i++) t.push_back(m.Item(i).text);
    return t;
}
}

LOGIC_TEST(lists_panel_plus_button_opens_the_playlist_menu) {
    std::string dir = TempDir("plusmenu");
    {
        StubContext ctx(dir);
        PlaylistPanel* panel = MakePanel(ctx);
        nativemenu::ClearLastShown();
        panel->PlusButton()->onInvoke();
        auto* menu = ShownMenu();
        LT_CHECK(menu != nullptr);
        if (menu) {
            LT_CHECK(MenuTexts(*menu) == (std::vector<std::wstring>{L"New playlist\u2026", L"Import playlist file\u2026", L"Import from URL\u2026", L"", L"Open playlists folder"}));
            LT_CHECK_EQ(menu->Item(0).accelerator, L"Ctrl+N");
        }
        ctx.host.SetRoot(nullptr);
    }
    RemoveDir(dir);
}

LOGIC_TEST(lists_panel_chip_menus_follow_the_playlist_kind) {
    std::string dir = TempDir("chipmenu");
    {
        StubContext ctx(dir);
        LT_CHECK(ctx.svc.playlists.Save("Alpha", MakeItems(2)));
        const std::string reference = ctx.svc.playlists.AddOrUpdateReference("https://example.test/list.m3u", "Feed", MakeItems(1), true);
        PlaylistPanel* panel = MakePanel(ctx);
        LT_CHECK_EQ(panel->ChipCount(), 3);
        LT_CHECK(!panel->ChipReadOnly(0) && !panel->ChipReadOnly(1) && panel->ChipReadOnly(2));
        LT_CHECK_EQ(panel->ChipName(2), reference);
        // default chip: the playlist menu; Add to current is hidden for the current playlist, Save as is disabled while empty
        panel->Chip(0)->onRightClick();
        auto* menu = ShownMenu();
        LT_CHECK(menu != nullptr);
        if (menu) {
            LT_CHECK(MenuTexts(*menu) == (std::vector<std::wstring>{L"Play playlist", L"Add", L"", L"Save as playlist…"}));
            LT_CHECK(menu->Item(1).enabled && !menu->Item(3).enabled);
            LT_CHECK_EQ(menu->Item(1).sub.size(), (size_t)3);
            LT_CHECK_EQ(menu->Item(1).sub[0].accelerator, L"Ctrl+I");
            LT_CHECK_EQ(menu->Item(1).sub[1].accelerator, L"Ctrl+O");
            LT_CHECK_EQ(menu->Item(1).sub[2].accelerator, L"Ctrl+U");
        }
        // editable named playlist (right-clicking the chip views it)
        panel->Chip(1)->onRightClick();
        LT_CHECK(panel->Viewed() == panel->ChipName(1));
        menu = ShownMenu();
        LT_CHECK(menu != nullptr);
        if (menu) {
            LT_CHECK(MenuTexts(*menu) == (std::vector<std::wstring>{L"Play playlist", L"Add", L"Add to current playlist", L"", L"Rename…", L"Delete playlist"}));
            LT_CHECK(menu->Item(1).enabled);
            LT_CHECK_EQ(menu->Item(4).accelerator, L"Ctrl+R");
            LT_CHECK_EQ(menu->Item(5).accelerator, L"Ctrl+D");
        }
        // read-only reference playlist: Add disabled, playlist-level items stay
        panel->Chip(2)->onRightClick();
        menu = ShownMenu();
        LT_CHECK(menu != nullptr);
        if (menu) {
            LT_CHECK(MenuTexts(*menu) == (std::vector<std::wstring>{L"Play playlist", L"Add", L"Add to current playlist", L"", L"Rename…", L"Delete playlist", L"", L"Refresh", L"Make editable copy"}));
            LT_CHECK(!menu->Item(1).enabled);
            LT_CHECK(menu->Item(4).enabled && menu->Item(5).enabled);
        }
        // empty list space opens the same playlist menu
        panel->List()->onContextMenu(-1, pui::PointF{0, 0}, false);
        menu = ShownMenu();
        LT_CHECK(menu != nullptr);
        if (menu) LT_CHECK(FindMenuItem(*menu, L"Play playlist") == 0 && FindMenuItem(*menu, L"Play") < 0);
        ctx.host.SetRoot(nullptr);
    }
    RemoveDir(dir);
}

LOGIC_TEST(lists_panel_item_menu_has_copy_delete_and_move) {
    std::string dir = TempDir("rowmenu");
    {
        StubContext ctx(dir);
        LT_CHECK(ctx.svc.playlists.Save("Alpha", MakeItems(3)));
        PlaylistPanel* panel = MakePanel(ctx);
        panel->Chip(1)->onRightClick();
        ShownMenu();   // drop the chip menu
        panel->List()->onContextMenu(1, pui::PointF{0, 0}, false);
        auto* menu = ShownMenu();
        LT_CHECK(menu != nullptr);
        if (menu) {
            LT_CHECK(MenuTexts(*menu) == (std::vector<std::wstring>{L"Play", L"", L"Copy", L"", L"Move to top", L"Move to bottom", L"",
                                                                     L"Delete", L"Delete others", L"Delete missing files", L"Delete items above"}));
        }
        if (menu && menu->ItemCount() == 11) {
            for (int i : {0, 2, 4, 5, 7, 8, 9, 10}) LT_CHECK(menu->Item(i).enabled);   // row 1 is the topmost selected: row 0 lies above
            LT_CHECK_EQ(menu->Item(0).accelerator, L"Enter");
            LT_CHECK_EQ(menu->Item(4).accelerator, L"Ctrl+Home");
            LT_CHECK_EQ(menu->Item(5).accelerator, L"Ctrl+End");
            LT_CHECK_EQ(menu->Item(7).accelerator, L"Del");
            LT_CHECK_EQ(menu->Item(8).accelerator, L"Ctrl+Del");
            LT_CHECK_EQ(menu->Item(9).accelerator, L"Ctrl+T");
            LT_CHECK(menu->Item(10).accelerator.empty());
            // no other playlist: a disabled placeholder, then a separator and "New playlist…"
            auto copy = menu->Item(2).sub;
            LT_CHECK(MenuTexts(MenuProbe{copy}) == (std::vector<std::wstring>{L"(No playlists)", L"", L"New playlist…"}));
            if (copy.size() == 3) LT_CHECK(!copy[0].enabled && copy[2].enabled);
            // Delete items above removes row 0 only
            menu->ActivateIndex(10);
            LT_CHECK_EQ(ctx.svc.playlists.Load("Alpha").size(), (size_t)2);
        }
        // the topmost selected item is row 0: nothing above, the entry is disabled
        panel->List()->onContextMenu(0, pui::PointF{0, 0}, false);
        menu = ShownMenu();
        LT_CHECK(menu != nullptr);
        if (menu) LT_CHECK(menu->ItemCount() == 11 && !menu->Item(10).enabled);
        ctx.host.SetRoot(nullptr);
    }
    RemoveDir(dir);
}

LOGIC_TEST(lists_panel_item_menu_copy_lists_other_playlists) {
    std::string dir = TempDir("copymenu");
    {
        StubContext ctx(dir);
        LT_CHECK(ctx.svc.playlists.Save("Alpha", MakeItems(2)));
        LT_CHECK(ctx.svc.playlists.Save("Beta", MakeItems(1)));
        PlaylistPanel* panel = MakePanel(ctx);
        panel->Chip(1)->onRightClick();
        ShownMenu();
        panel->List()->onContextMenu(0, pui::PointF{0, 0}, false);
        auto* menu = ShownMenu();
        LT_CHECK(menu != nullptr);
        if (menu) {
            auto copy = menu->Item(2).sub;
            LT_CHECK(MenuTexts(MenuProbe{copy}) == (std::vector<std::wstring>{L"Beta", L"", L"New playlist…"}));
            nativemenu::InvokeItem(copy, L"Beta");
            LT_CHECK_EQ(ctx.svc.playlists.Load("Beta").size(), (size_t)2);
            LT_CHECK_EQ(ctx.svc.playlists.Load("Alpha").size(), (size_t)2);
        }
        ctx.host.SetRoot(nullptr);
    }
    RemoveDir(dir);
}

LOGIC_TEST(lists_panel_read_only_item_menu_disables_delete_and_move) {
    std::string dir = TempDir("romenu");
    {
        StubContext ctx(dir);
        ctx.svc.playlists.AddOrUpdateReference("https://example.test/list.m3u", "Feed", MakeItems(2), true);
        PlaylistPanel* panel = MakePanel(ctx);
        panel->Chip(1)->onRightClick();
        ShownMenu();
        panel->List()->onContextMenu(1, pui::PointF{0, 0}, false);
        auto* menu = ShownMenu();
        LT_CHECK(menu != nullptr);
        if (menu) {
            LT_CHECK(menu->ItemCount() == 11 && menu->Item(0).enabled && menu->Item(2).enabled);
            if (menu->ItemCount() == 11) for (int i : {4, 5, 7, 8, 9, 10}) LT_CHECK(!menu->Item(i).enabled);
        }
        ctx.host.SetRoot(nullptr);
    }
    RemoveDir(dir);
}

LOGIC_TEST(lists_panel_delete_asks_for_confirmation) {
    std::string dir = TempDir("chipdelete");
    {
        StubContext ctx(dir);
        LT_CHECK(ctx.svc.playlists.Save("Alpha", MakeItems(2)));
        PlaylistPanel* panel = MakePanel(ctx);
        panel->Chip(1)->onRightClick();
        auto* menu = ShownMenu();
        LT_CHECK(menu != nullptr);
        const int del = menu ? FindMenuItem(*menu, L"Delete playlist") : -1;
        LT_CHECK(del >= 0);
        if (menu && del >= 0) menu->ActivateIndex(del);
        LT_CHECK(ctx.host.TopPopup() != nullptr);
        auto* cancel = PopupButton(ctx, L"Cancel");
        LT_CHECK(cancel != nullptr);
        if (cancel) {
            LT_CHECK(ctx.host.Focused() == cancel);   // focus starts on the safe button
            cancel->Invoke();
        }
        LT_CHECK(ctx.svc.playlists.Exists("Alpha"));
        LT_CHECK_EQ((int)ctx.host.PopupCount(), 0);
        panel->Chip(1)->onRightClick();
        menu = ShownMenu();
        if (menu) menu->ActivateIndex(FindMenuItem(*menu, L"Delete playlist"));
        auto* confirm = PopupButton(ctx, L"Delete");
        LT_CHECK(confirm != nullptr);
        if (confirm) confirm->Invoke();
        LT_CHECK(!ctx.svc.playlists.Exists("Alpha"));
        ctx.host.SetRoot(nullptr);
    }
    RemoveDir(dir);
}

LOGIC_TEST(lists_panel_reference_menu_makes_fresh_local_copy) {
    std::string dir = TempDir("refcopylocal");
    const std::string source = dir + "\\source.m3u";
    {
        LT_CHECK(WritePlaylistText(source, "#EXTM3U\n#PLAYLIST:Old Feed\n#EXTINF:10,Old item\nold.mp4\n"));
        StubContext ctx(dir);
        const auto cached = wvp::PlaylistLoader().Load(source).items;
        const std::string reference = ctx.svc.playlists.AddOrUpdateReference(source, "Local source", cached, false);
        LT_CHECK(!reference.empty());
        LT_CHECK(WritePlaylistText(source, "#EXTM3U\n#PLAYLIST:Fresh Feed\n#EXTINF:22,Fresh item\nfresh.mp4\n"));
        PlaylistPanel* panel = MakePanel(ctx);
        LT_CHECK_EQ(panel->ChipCount(), 2);
        panel->Chip(1)->onRightClick();
        auto* menu = ShownMenu();
        LT_CHECK(menu != nullptr);
        const int copy = menu ? FindMenuItem(*menu, L"Make editable copy") : -1;
        LT_CHECK(copy >= 0);
        std::promise<void> completed;
        auto completion = completed.get_future();
        ctx.onToast = [&completed] { completed.set_value(); };
        if (menu && copy >= 0) menu->ActivateIndex(copy);
        LT_CHECK(completion.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
        LT_CHECK(WaitFor([&] { return ctx.svc.playlists.Exists("Fresh Feed"); }));
        auto imported = ctx.svc.playlists.Load("Fresh Feed");
        LT_CHECK_EQ(imported.size(), (size_t)1);
        if (!imported.empty()) {
            LT_CHECK_EQ(imported[0].title, "Fresh item");
            LT_CHECK(imported[0].url.find("fresh.mp4") != std::string::npos);
        }
        LT_CHECK(!ctx.svc.playlists.IsReference("Fresh Feed"));
        const auto refreshedReference = ctx.svc.playlists.Load(reference);
        LT_CHECK_EQ(refreshedReference.size(), (size_t)1);
        if (!refreshedReference.empty()) LT_CHECK_EQ(refreshedReference[0].title, "Fresh item"); // reference cache was freshly re-read too
        LT_CHECK(ctx.currentName.empty());
        LT_CHECK(ctx.played.empty() && ctx.enqueued.empty());
        LT_CHECK(WaitFor([&] { return panel->ChipCount() == 3 && panel->Viewed() == "Fresh Feed" && !panel->ViewedReadOnly(); }));   // the panel shows the copy
        ctx.host.SetRoot(nullptr);
    }
    RemoveDir(dir);
}

LOGIC_TEST(lists_panel_reference_menu_copies_cached_items_offline) {
    std::string dir = TempDir("refcopyoffline");
    LocalHttpServer server("offline", 503);
    {
        StubContext ctx(dir, true);
        const auto cached = std::vector<wvp::MediaItem>{wvp::MediaItem("https://cdn.test/cached.mp4", "Cached item", 45.0)};
        const std::string reference = ctx.svc.playlists.AddOrUpdateReference(server.Url(), "Remote cache", cached, true);
        PlaylistPanel* panel = MakePanel(ctx);
        panel->Chip(1)->onRightClick();
        auto* menu = ShownMenu();
        LT_CHECK(menu != nullptr);
        const int copy = menu ? FindMenuItem(*menu, L"Make editable copy") : -1;
        LT_CHECK(copy >= 0);
        std::promise<void> completed;
        auto completion = completed.get_future();
        ctx.onToast = [&completed] { completed.set_value(); };
        if (menu && copy >= 0) menu->ActivateIndex(copy);
        LT_CHECK(completion.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
        LT_CHECK(WaitFor([&] { return ctx.svc.playlists.Exists("Remote cache (2)"); }));
        const auto imported = ctx.svc.playlists.Load("Remote cache (2)");
        LT_CHECK_EQ(imported.size(), (size_t)1);
        if (!imported.empty()) {
            LT_CHECK_EQ(imported[0].url, cached[0].url);
            LT_CHECK_EQ(imported[0].title, cached[0].title);
            LT_CHECK_EQ(imported[0].duration.value_or(0), cached[0].duration.value_or(0));
        }
        LT_CHECK(!ctx.svc.playlists.IsReference("Remote cache (2)"));
        LT_CHECK(!ctx.infoBars.empty());
        LT_CHECK(ctx.infoBars.back().find(L"offline fallback") != std::wstring::npos);
        LT_CHECK_EQ(server.Requests(), 1);
        LT_CHECK(ctx.currentName.empty());
        LT_CHECK_EQ(ctx.player.opens, 0);
        LT_CHECK(WaitFor([&] { return panel->Viewed() == "Remote cache (2)"; }));
        ctx.host.SetRoot(nullptr);
    }
    RemoveDir(dir);
}

LOGIC_TEST(lists_panel_import_from_url_creates_a_read_only_reference) {
    std::string dir = TempDir("urlref");
    LocalHttpServer server("#EXTM3U\n#PLAYLIST:Weekly Mix\n#EXTINF:33,Episode One\nrelative/one.mp4\n");
    {
        StubContext ctx(dir, true);
        ctx.currentName = "Keep current";
        LT_CHECK(ctx.svc.playlists.Save(ctx.currentName, MakeItems(1)));
        std::promise<std::string> completed;
        auto result = completed.get_future();
        AddPlaylistUrl(ctx, wvp::ToWide(server.Url()), [&completed](const std::string& name) { completed.set_value(name); });
        LT_CHECK(result.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
        const std::string name = result.wait_for(std::chrono::seconds(0)) == std::future_status::ready ? result.get() : std::string();
        LT_CHECK_EQ(name, "Weekly Mix");
        LT_CHECK(ctx.svc.playlists.IsReference(name));
        auto items = ctx.svc.playlists.Load(name);
        LT_CHECK_EQ(items.size(), (size_t)1);
        if (!items.empty()) {
            LT_CHECK_EQ(items[0].title, "Episode One");
            LT_CHECK_EQ(items[0].url, server.Url().substr(0, server.Url().find_last_of('/') + 1) + "relative/one.mp4");
        }
        LT_CHECK_EQ(ctx.currentName, "Keep current");   // adding a reference never starts playback
        LT_CHECK_EQ(ctx.player.opens, 0);
        const auto ref = ctx.svc.playlists.FindReferenceByName(name);
        LT_CHECK(ref && ref->isRemote && ref->source == server.Url());
        // adding the same URL again refreshes the same reference instead of creating a second one
        std::promise<std::string> again;
        auto againResult = again.get_future();
        AddPlaylistUrl(ctx, wvp::ToWide(server.Url()), [&again](const std::string& n) { again.set_value(n); });
        LT_CHECK(againResult.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
        LT_CHECK_EQ(againResult.get(), name);
        int references = 0;
        for (const auto& info : ctx.svc.playlists.List()) references += info.isReference ? 1 : 0;
        LT_CHECK_EQ(references, 1);
    }
    RemoveDir(dir);
}

LOGIC_TEST(lists_panel_import_from_url_refuses_hls_media_and_master_playlists) {
    std::string dir = TempDir("urlhls");
    LocalHttpServer server("#EXTM3U\n#EXT-X-TARGETDURATION:10\n#EXT-X-MEDIA-SEQUENCE:0\n");
    {
        StubContext ctx(dir);
        std::promise<void> deniedMedia;
        auto mediaResult = deniedMedia.get_future();
        ctx.onInfoBar = [&deniedMedia] { deniedMedia.set_value(); };
        AddPlaylistUrl(ctx, wvp::ToWide(server.Url()));
        LT_CHECK(mediaResult.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
        LT_CHECK(ctx.infoBars.back().find(L"HLS") != std::wstring::npos);
        std::promise<void> deniedMaster;
        auto masterResult = deniedMaster.get_future();
        ctx.onInfoBar = [&deniedMaster] { deniedMaster.set_value(); };
        server.SetResponse(200, "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=100000\nvariant.m3u8\n");
        AddPlaylistUrl(ctx, wvp::ToWide(server.Url()));
        LT_CHECK(masterResult.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
        LT_CHECK(ctx.infoBars.back().find(L"HLS") != std::wstring::npos);
        LT_CHECK(ctx.svc.playlists.List().empty());
        LT_CHECK(ctx.toasts.empty());
        LT_CHECK_EQ(server.Requests(), 2);
    }
    RemoveDir(dir);
}
