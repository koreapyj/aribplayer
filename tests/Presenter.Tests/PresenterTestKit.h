#pragma once
// Shared by the presenter tests: a hidden host window, the D3D11 presenter of the Windows video platform on it, and a
// libffplay player that presents through it (dummy audio driver: silent). Nothing is shown on screen.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ffplay.h"
#include "wvp/JsonLite.h"
#include "wvp/win32/Utf.h"
#include "wvp/win32/WindowsVideoPlatform.h"
#include "wvptest.h"

namespace ptk {

namespace fs = std::filesystem;
using wvp::json::Value;

inline fs::path RepoRoot() {
    static const fs::path root = [] {
        wchar_t buffer[32768]{};
        GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
        fs::path dir = fs::path(buffer).parent_path();
        while (!dir.empty()) {
            if (fs::exists(dir / L"CMakePresets.json")) return dir;
            const fs::path parent = dir.parent_path();
            if (parent == dir) break;
            dir = parent;
        }
        return fs::current_path();
    }();
    return root;
}

inline std::string Utf8Path(const fs::path& p) { return wvp::ToUtf8(p.wstring()); }

inline void PumpMessages() {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

template <class P>
bool WaitFor(P&& pred, double seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<long long>(seconds * 1000.0));
    while (std::chrono::steady_clock::now() < deadline) {
        PumpMessages();
        if (pred()) return true;
        Sleep(10);
    }
    PumpMessages();
    return pred();
}

inline LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

// A popup far off screen with a child "video" view the presenter fills.
struct HiddenHost {
    HWND parent = nullptr, video = nullptr;
    explicit HiddenHost(int width = 640, int height = 360) {
        static const ATOM atom = [] {
            WNDCLASSW wc{};
            wc.lpfnWndProc = HostProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"WvpPresenterTestHost";
            return RegisterClassW(&wc);
        }();
        (void)atom;
        const HINSTANCE inst = GetModuleHandleW(nullptr);
        parent = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"WvpPresenterTestHost", L"presenter test host", WS_POPUP,
                                 -32000, -32000, width, height, nullptr, nullptr, inst, nullptr);
        if (parent)
            video = CreateWindowExW(0, L"STATIC", L"video", WS_CHILD | WS_VISIBLE, 0, 0, width, height, parent, nullptr, inst, nullptr);
    }
    ~HiddenHost() { if (parent) DestroyWindow(parent); }
    HiddenHost(const HiddenHost&) = delete;
    HiddenHost& operator=(const HiddenHost&) = delete;
    bool Resize(int width, int height) const {
        return SetWindowPos(parent, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) &&
               SetWindowPos(video, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
};

struct Events {
    std::atomic<int> prepared{0}, errors{0}, presenterErrors{0};
    std::mutex mu;
    std::string lastPresenterError;   // FFP_EVENT_PRESENTER_ERROR json
};

inline void FFP_CALL OnEvent(void* user, int event, const char* json) {
    auto* e = static_cast<Events*>(user);
    if (event == FFP_EVENT_PREPARED) ++e->prepared;
    else if (event == FFP_EVENT_ERROR) ++e->errors;
    else if (event == FFP_EVENT_PRESENTER_ERROR) {
        std::lock_guard<std::mutex> g(e->mu);
        e->lastPresenterError = json ? json : "";
        ++e->presenterErrors;
    }
}

// Puts itself between a presenter and the engine: every FFPPresenter call is forwarded, and the sink's presented() reports
// (the video size the presenter shows, also after a redraw of its own) are recorded before they reach the engine.
class PresentedWatch {
public:
    explicit PresentedWatch(const FFPPresenter& inner) : inner_(inner) {
        table_ = inner;
        table_.opaque = this;
        table_.open = &Open;
        table_.present = [](void* o, const FFPPicture* pic) { auto* w = Self(o); return w->inner_.present(w->inner_.opaque, pic); };
        table_.clear = [](void* o) { auto* w = Self(o); w->inner_.clear(w->inner_.opaque); };
        table_.close = [](void* o) { auto* w = Self(o); w->inner_.close(w->inner_.opaque); };
        if (inner.accepts)
            table_.accepts = [](void* o, const AVFrame* f) { auto* w = Self(o); return w->inner_.accepts(w->inner_.opaque, f); };
        if (inner.setup_frames)
            table_.setup_frames = [](void* o, AVBufferRef* f) { auto* w = Self(o); return w->inner_.setup_frames(w->inner_.opaque, f); };
        if (inner.readback)
            table_.readback = [](void* o, const AVFrame* f, uint8_t* b, int st, int wd, int ht) {
                auto* w = Self(o);
                return w->inner_.readback(w->inner_.opaque, f, b, st, wd, ht);
            };
        if (inner.stats_json)
            table_.stats_json = [](void* o, char* b, size_t n) { auto* w = Self(o); return w->inner_.stats_json(w->inner_.opaque, b, n); };
        if (inner.device_lost)
            table_.device_lost = [](void* o, char* r, size_t n) { auto* w = Self(o); return w->inner_.device_lost(w->inner_.opaque, r, n); };
    }
    PresentedWatch(const PresentedWatch&) = delete;
    PresentedWatch& operator=(const PresentedWatch&) = delete;

    const FFPPresenter* Table() const { return &table_; }
    size_t Reports() const {
        std::lock_guard<std::mutex> g(mu_);
        return sizes_.size();
    }
    // Blocks until a presented() report after the first `from` reports shows the video at width x height; false when none
    // came within `guard` (a hang guard, not a measurement).
    bool WaitForSize(size_t from, int width, int height, std::chrono::milliseconds guard) const {
        std::unique_lock<std::mutex> lk(mu_);
        return cv_.wait_for(lk, guard, [&] {
            for (size_t i = from; i < sizes_.size(); ++i)
                if (sizes_[i].first == width && sizes_[i].second == height) return true;
            return false;
        });
    }

private:
    static PresentedWatch* Self(void* o) { return static_cast<PresentedWatch*>(o); }
    static int FFP_CALL Open(void* o, const FFPStreamInfo* info, const FFPPresenterSink* sink, FFPPresenterCaps* caps) {
        auto* w = Self(o);
        w->engine_ = *sink;
        w->sink_.engine = w;
        w->sink_.presented = [](void* e, const FFPPresentedInfo* i) {
            auto* self = static_cast<PresentedWatch*>(e);
            {
                std::lock_guard<std::mutex> g(self->mu_);
                self->sizes_.emplace_back(i->video_width, i->video_height);
            }
            self->cv_.notify_all();
            self->engine_.presented(self->engine_.engine, i);
        };
        w->sink_.failed = [](void* e, const char* reason, int lost, const char* removed) {
            auto* self = static_cast<PresentedWatch*>(e);
            self->engine_.failed(self->engine_.engine, reason, lost, removed);
        };
        return w->inner_.open(w->inner_.opaque, info, &w->sink_, caps);
    }

    const FFPPresenter inner_;
    FFPPresenter table_{};
    FFPPresenterSink engine_{}, sink_{};
    mutable std::mutex mu_;
    mutable std::condition_variable cv_;
    std::vector<std::pair<int, int>> sizes_;
};

// The adapter that drives the host's monitor, else the first hardware adapter, else the default adapter ("").
inline wvp::AdapterEntry PickAdapter(wvp::VideoPlatform& platform, HWND view) {
    const std::vector<wvp::AdapterEntry> all = platform.Adapters();
    const std::string display = platform.DisplayAdapter(view);
    for (const auto& a : all)
        if (a.luid == display) return a;
    return all.empty() ? wvp::AdapterEntry{} : all.front();
}

// The D3D11 presenter on a hidden host and a player on it. Skips when the host has no D3D11 presenter.
struct Session {
    HiddenHost host;
    Events events;
    wvp::win32::WindowsVideoPlatform platform;
    wvp::PresenterHandle presenter;
    wvp::win32::D3D11Presenter* d3d11 = nullptr;
    std::unique_ptr<PresentedWatch> watch;   // between the presenter and the engine
    FFPlayer* player = nullptr;

    explicit Session(wvp::win32::PresenterOptions options = {}, int width = 640, int height = 360)
        : host(width, height), platform(std::move(options)) {
        REQUIRE_MSG(host.parent && host.video, "could not create the hidden test host");
        std::string error;
        presenter = platform.CreatePresenter(PickAdapter(platform, host.video), host.video, &error);
        if (!presenter) throw wvptest::SkipFailure{"no D3D11 presenter on this host: " + error};
        d3d11 = static_cast<wvp::win32::D3D11Presenter*>(presenter.get());
        watch = std::make_unique<PresentedWatch>(*presenter->Native());
        const std::vector<AVBufferRef*> devices = presenter->Devices();
        FFPOutput output{};
        output.presenter = watch->Table();
        output.devices = devices.data();
        output.nb_devices = static_cast<int>(devices.size());
        output.only_given_devices = presenter->OnlyGivenDevices() ? 1 : 0;
        player = ffp_create(&output, "audio_driver=dummy", OnEvent, &events);
        REQUIRE_MSG(player != nullptr, "ffp_create failed");
    }
    ~Session() {
        if (player) ffp_destroy(player);
        presenter.reset();
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Opens and waits for PREPARED; false on an ERROR event or a timeout.
    bool Open(const fs::path& media, const std::string& options, double seconds = 25.0) {
        if (ffp_open(player, Utf8Path(media).c_str(), options.empty() ? nullptr : options.c_str()) != 0) return false;
        const int before = events.errors.load();
        return WaitFor([&] { return events.prepared.load() > 0 || events.errors.load() > before; }, seconds) &&
               events.errors.load() == before;
    }

    std::optional<Value> Debug() const {
        char* text = ffp_get_debug_info(player);
        if (!text) return std::nullopt;
        std::string s(text);
        ffp_free(text);
        return wvp::json::Parse(s);
    }
};

inline const Value* Member(const Value* o, std::string_view key) {
    return o && o->kind == Value::Kind::Object ? o->Find(key) : nullptr;
}

inline std::optional<double> Number(const Value* o, std::string_view key) {
    const Value* v = Member(o, key);
    if (!v || v->kind != Value::Kind::Number) return std::nullopt;
    return v->n;
}

inline uint64_t Count(const Value* o, std::string_view key) {
    return static_cast<uint64_t>(std::max(0.0, Number(o, key).value_or(0.0)));
}

// debug.presenter.stats: the presenter's own statistics (FFPPresenter.stats_json).
inline const Value* Stats(const Value& debug) { return Member(Member(&debug, "presenter"), "stats"); }

inline bool PresenterFailed(const Value& debug) {
    const Value* p = Member(&debug, "presenter");
    return !p || p->Str("name") != "d3d11" || !p->Str("error").empty();
}

inline uint64_t ReadbackSeq(const Session& s) {
    auto d = s.Debug();
    return d ? Count(Member(Stats(*d), "overlay"), "readback_seq") : 0;
}

}
