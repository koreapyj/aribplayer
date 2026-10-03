#include "wvp/FfPlayer.h"

#include <cmath>
#include <thread>
#include <unordered_map>

#include "ffplay.h"
#include "wvp/JsonLite.h"
#include "wvp/Utf.h"

namespace wvp {

namespace {

std::mutex g_mu;
std::unordered_map<std::uint64_t, FfPlayer*> g_players;

struct Completion {
    FfPlayer::CreateDone done;
    std::unique_ptr<FfPlayer> player;
    int err = 0;
};
std::unordered_map<std::uint64_t, std::unique_ptr<Completion>> g_creates;
std::atomic<std::uint64_t> g_nextId{1};

std::optional<PlayerState> ParseState(const std::string& s) {
    if (s == "idle") return PlayerState::Idle;
    if (s == "opening") return PlayerState::Opening;
    if (s == "playing") return PlayerState::Playing;
    if (s == "paused") return PlayerState::Paused;
    if (s == "stopped") return PlayerState::Stopped;
    if (s == "ended") return PlayerState::Ended;
    if (s == "error") return PlayerState::Error;
    return std::nullopt;
}

std::optional<InputEventArgs> ParseInput(const json::Value& r) {
    std::string t = r.Str("type");
    InputEventArgs e;
    if (t == "move") e.Kind = InputKind::Move;
    else if (t == "down") e.Kind = InputKind::Down;
    else if (t == "up") e.Kind = InputKind::Up;
    else if (t == "dblclick") e.Kind = InputKind::DoubleClick;
    else if (t == "wheel") e.Kind = InputKind::Wheel;
    else if (t == "keydown") e.Kind = InputKind::KeyDown;
    else if (t == "keyup") e.Kind = InputKind::KeyUp;
    else return std::nullopt;
    e.X = r.Int("x"); e.Y = r.Int("y");
    e.Button = r.Int("button"); e.Buttons = r.Int("buttons");
    e.Delta = r.Int("delta"); e.VirtualKey = r.Int("vk");
    e.Modifiers = (InputModifiers)r.Int("mods");
    return e;
}

}

FfPlayer::FfPlayer(HWND notifyHwnd, UINT notifyMsg)
    : id_(g_nextId.fetch_add(1)), notifyHwnd_(notifyHwnd), notifyMsg_(notifyMsg) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_players[id_] = this;
}

FfPlayer::~FfPlayer() {
    closing_ = true;                 // callbacks stop enqueuing
    *alive_ = false;                 // an in-progress DispatchPendingEvents stops touching this
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_players.erase(id_);
    }
    if (p_) {
        ffp_destroy(p_);             // joins the player thread: no native callbacks after it returns
        p_ = nullptr;
    }
    std::lock_guard<std::mutex> lk(qmu_);
    queue_.clear();
}

void FfPlayer::CreateOnWorker(HWND videoHwnd, HWND notifyHwnd, UINT notifyMsg,
                              std::function<void(std::unique_ptr<FfPlayer>, int)> workerDone) {
    std::thread([=, workerDone = std::move(workerDone)]() mutable {
        std::unique_ptr<FfPlayer> pl(new FfPlayer(notifyHwnd, notifyMsg));
        ::FFPlayer* h = ffp_create(videoHwnd, &FfPlayer::OnNativeEvent, pl.get());
        if (!h) {
            pl.reset();
            workerDone(nullptr, -1);
            return;
        }
        pl->p_ = h;
        workerDone(std::move(pl), 0);
    }).detach();
}

void FfPlayer::CreateAsync(HWND videoHwnd, HWND notifyHwnd, UINT notifyMsg, CreateDone done) {
    CreateOnWorker(videoHwnd, notifyHwnd, notifyMsg,
        [notifyHwnd, notifyMsg, done = std::move(done)](std::unique_ptr<FfPlayer> pl, int err) mutable {
            auto c = std::make_unique<Completion>();
            c->done = std::move(done);
            c->player = std::move(pl);
            c->err = err;
            std::uint64_t id = g_nextId.fetch_add(1);
            {
                std::lock_guard<std::mutex> lk(g_mu);
                g_creates[id] = std::move(c);
            }
            if (!notifyHwnd || !PostMessageW(notifyHwnd, notifyMsg, kNotifyCreateDone, (LPARAM)id)) {
                // Window is gone: drop the completion (destroys the player here, on the worker thread).
                std::unique_ptr<Completion> dead;
                {
                    std::lock_guard<std::mutex> lk(g_mu);
                    auto it = g_creates.find(id);
                    if (it != g_creates.end()) { dead = std::move(it->second); g_creates.erase(it); }
                }
                dead.reset();
            }
        });
}

std::unique_ptr<FfPlayer> FfPlayer::CreateSync(HWND videoHwnd, HWND notifyHwnd, UINT notifyMsg, int* err, DWORD timeoutMs) {
    struct Shared {
        HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::unique_ptr<FfPlayer> pl;
        int err = 0;
        bool abandoned = false;
        std::mutex mu;
        ~Shared() { CloseHandle(ev); }
    };
    auto sh = std::make_shared<Shared>();
    CreateOnWorker(videoHwnd, notifyHwnd, notifyMsg, [sh](std::unique_ptr<FfPlayer> pl, int e) {
        std::unique_ptr<FfPlayer> drop;
        {
            std::lock_guard<std::mutex> lk(sh->mu);
            if (sh->abandoned) drop = std::move(pl);   // caller timed out: destroy on this thread
            else { sh->pl = std::move(pl); sh->err = e; }
        }
        SetEvent(sh->ev);
    });

    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        ULONGLONG now = GetTickCount64();
        if (now >= deadline) break;
        DWORD wait = MsgWaitForMultipleObjects(1, &sh->ev, FALSE, (DWORD)(deadline - now), QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0) break;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    std::lock_guard<std::mutex> lk(sh->mu);
    if (WaitForSingleObject(sh->ev, 0) != WAIT_OBJECT_0) {
        sh->abandoned = true;
        if (err) *err = -110;
        return nullptr;
    }
    if (err) *err = sh->err;
    return std::move(sh->pl);
}

void FfPlayer::HandleNotify(WPARAM wParam, LPARAM lParam) {
    if (wParam == kNotifyEvents) {
        FfPlayer* p = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            auto it = g_players.find((std::uint64_t)lParam);
            if (it != g_players.end()) p = it->second;
        }
        if (p) p->DispatchPendingEvents();
    } else if (wParam == kNotifyCreateDone) {
        std::unique_ptr<Completion> c;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            auto it = g_creates.find((std::uint64_t)lParam);
            if (it != g_creates.end()) { c = std::move(it->second); g_creates.erase(it); }
        }
        if (c && c->done) c->done(std::move(c->player), c->err);
    }
}

void FfPlayer::OnNativeEvent(void* user, int ev, const char* json) {
    try {
        static_cast<FfPlayer*>(user)->HandleNative(ev, json);
    } catch (...) {
        // never let an exception cross into native code
    }
}

void FfPlayer::Post(WPARAM wp, LPARAM lp) {
    if (!notifyHwnd_) return;
    if (!PostMessageW(notifyHwnd_, notifyMsg_, wp, lp)) notifyPending_ = false;
}

void FfPlayer::Enqueue(std::function<void()> fn) {
    if (closing_) return;
    {
        std::lock_guard<std::mutex> lk(qmu_);
        queue_.push_back(std::move(fn));
    }
    if (!notifyPending_.exchange(true)) Post(kNotifyEvents, (LPARAM)id_);
}

void FfPlayer::DispatchPendingEvents() {
    notifyPending_ = false;
    std::deque<std::function<void()>> local;
    {
        std::lock_guard<std::mutex> lk(qmu_);
        local.swap(queue_);
    }
    auto alive = alive_;
    for (auto& fn : local) {
        if (!*alive) return;         // a handler destroyed this player
        fn();
    }
}

void FfPlayer::HandleNative(int ev, const char* jsonText) {
    if (closing_) return;
    std::optional<json::Value> doc;
    if (jsonText && *jsonText) doc = json::Parse(jsonText);
    const bool has = doc.has_value();
    const json::Value empty;
    const json::Value& r = has ? *doc : empty;

    switch (ev) {
    case FFP_EVENT_PREPARED:
        if (has) {
            MediaPreparedInfo i;
            i.Duration = r.Dbl("duration"); i.Seekable = r.Bool("seekable");
            i.HasVideo = r.Bool("has_video"); i.HasAudio = r.Bool("has_audio");
            i.Width = r.Int("width"); i.Height = r.Int("height");
            i.SarNum = r.Int("sar_num"); i.SarDen = r.Int("sar_den");
            i.Title = ToWide(r.Str("title"));
            Enqueue([this, i = std::move(i)] { Prepared.raise(i); });
        }
        break;
    case FFP_EVENT_POSITION:
        if (has) {
            double pos = r.Dbl("position"), dur = r.Dbl("duration");
            Enqueue([this, pos, dur] { PositionChanged.raise(pos, dur); });
        }
        break;
    case FFP_EVENT_STATE:
        if (has) {
            auto st = ParseState(r.Str("state"));
            if (!st) break;
            state_ = *st;
            PlayerState s = *st;
            Enqueue([this, s] { StateChanged.raise(s); });
        }
        break;
    case FFP_EVENT_BUFFERING:
        if (has) {
            bool b = r.Bool("buffering");
            Enqueue([this, b] { BufferingChanged.raise(b); });
        }
        break;
    case FFP_EVENT_EOF:
        Enqueue([this] { Ended.raise(); });
        break;
    case FFP_EVENT_ERROR:
        if (has) {
            PlayerErrorInfo e;
            e.Code = r.Int("code"); e.Message = ToWide(r.Str("message")); e.Url = ToWide(r.Str("url"));
            e.DeviceLost = r.Bool("device_lost"); e.RemovedReason = r.Str("removed_reason");
            Enqueue([this, e = std::move(e)] { Error.raise(e); });
        }
        break;
    case FFP_EVENT_INPUT:
        if (has) {
            if (auto ie = ParseInput(r)) Enqueue([this, e = *ie] { Input.raise(e); });
        }
        break;
    case FFP_EVENT_FILTER_ERROR:
        if (has) {
            FilterErrorInfo f;
            f.Type = ToWide(r.Str("type")); f.Filter = ToWide(r.Str("filter"));
            f.Code = r.Int("code"); f.Message = ToWide(r.Str("message"));
            f.HwframesFallback = r.Bool("hwframes_fallback");
            Enqueue([this, f = std::move(f)] { FilterError.raise(f); });
        }
        break;
    case FFP_EVENT_PRESENTER_ERROR:
        if (has) {
            PresenterErrorInfo e;
            e.Presenter = ToWide(r.Str("presenter")); e.Error = ToWide(r.Str("error")); e.Fallback = ToWide(r.Str("fallback"));
            e.DeviceLost = r.Bool("device_lost"); e.RemovedReason = r.Str("removed_reason");
            Enqueue([this, e = std::move(e)] { PresenterError.raise(e); });
        }
        break;
    case FFP_EVENT_STEP_DONE:
        if (has) {
            int dir = r.Int("direction"); bool moved = r.Int("moved") != 0;
            Enqueue([this, dir, moved] { FrameStepDone.raise(dir, moved); });
        }
        break;
    case FFP_EVENT_KEYSEEK_DONE:
        if (has) {
            int dir = r.Int("direction"); bool moved = r.Int("moved") != 0;
            double position = r.Dbl("position"), key = r.Dbl("key");
            Enqueue([this, dir, moved, position, key] { KeyframeSeekDone.raise(dir, moved, position, key); });
        }
        break;
    default: break;
    }
}

std::string FfPlayer::Version() { const char* v = ffp_version(); return v ? v : ""; }
void FfPlayer::SetLogLevel(int l) { ffp_set_log_level(l); }

namespace {
std::mutex g_capsMu;
bool g_capsDone = false;
std::string g_capsCached;
std::string g_probeOptions;
}

void FfPlayer::SetProbeOptions(const std::string& optionsKv) {
    std::lock_guard<std::mutex> lk(g_capsMu);
    g_probeOptions = optionsKv;
}

std::string FfPlayer::ProbeCapsJson() {
    std::mutex& mu = g_capsMu;
    bool& done = g_capsDone;
    std::string& cached = g_capsCached;
    std::lock_guard<std::mutex> lk(mu);
    if (done) return cached;
    char* s = ffp_probe_caps_opts(g_probeOptions.empty() ? nullptr : g_probeOptions.c_str());
    if (s) {
        cached = s;
        ffp_free(s);
    }
    done = true;   // an answer (even an empty one) is final for this process
    return cached;
}

std::optional<CapabilitySet> FfPlayer::ProbeCaps() {
    std::string json = ProbeCapsJson();
    if (json.empty()) return std::nullopt;
    auto runtime = ParseCapabilities(json);
    if (!runtime) return std::nullopt;
    return MergeBuildCapabilities(*runtime, BuildCapabilities());
}

namespace {
std::mutex g_logMu;
std::shared_ptr<FfPlayer::LogCallback> g_logCb;

void FFP_CALL NativeLog(void*, int level, const char* line) {
    std::shared_ptr<FfPlayer::LogCallback> cb;
    {
        std::lock_guard<std::mutex> lk(g_logMu);
        cb = g_logCb;
    }
    if (!cb || !*cb) return;
    try { (*cb)(level, line ? line : ""); } catch (...) {}
}
}

void FfPlayer::SetLogCallback(LogCallback cb) {
    if (!cb) {
        ffp_set_log_callback(nullptr, nullptr);   // restore the default logger first: no callbacks after this
        std::lock_guard<std::mutex> lk(g_logMu);
        g_logCb.reset();
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_logMu);
        g_logCb = std::make_shared<LogCallback>(std::move(cb));
    }
    ffp_set_log_callback(&NativeLog, nullptr);
}

bool FfPlayer::BuildOptionsString(const OpenOptions& options, std::string& out) {
    out.clear();
    for (auto& [k, v] : options) {
        if (k.empty() || k.find_first_of(L"=\r\n") != std::wstring::npos) return false;
        if (v.find_first_of(L"\r\n") != std::wstring::npos) return false;
        if (!out.empty()) out.push_back('\n');
        out += ToUtf8(k);
        out.push_back('=');
        out += ToUtf8(v);
    }
    return true;
}

int FfPlayer::Open(const std::wstring& url, const OpenOptions& options) {
    if (url.empty() || !p_) return FFP_EINVAL;
    std::string kv;
    if (!BuildOptionsString(options, kv)) return FFP_EINVAL;
    std::string u = ToUtf8(url);
    audioPref_ = 0;                       // every file starts on audio 1; nothing is remembered
    ffp_set_audio_preference(p_, FFP_ARIB_AUDIO_1);
    return ffp_open(p_, u.c_str(), kv.empty() ? nullptr : kv.c_str());
}

int FfPlayer::Stop() { return p_ ? ffp_stop(p_) : FFP_EINVAL; }
int FfPlayer::Play() { return p_ ? ffp_play(p_) : FFP_EINVAL; }
int FfPlayer::Pause() { return p_ ? ffp_pause(p_) : FFP_EINVAL; }
int FfPlayer::TogglePause() { return p_ ? ffp_toggle_pause(p_) : FFP_EINVAL; }

int FfPlayer::Seek(double seconds, bool relative) {
    return p_ ? ffp_seek(p_, seconds, relative ? FFP_SEEK_RELATIVE : FFP_SEEK_ABSOLUTE) : FFP_EINVAL;
}

int FfPlayer::StepFrame(int direction) {
    if (!p_ || (direction != 1 && direction != -1)) return FFP_EINVAL;
    return ffp_step_frame(p_, direction);
}

int FfPlayer::SeekKeyframe(int direction) {
    if (!p_ || (direction != 1 && direction != -1)) return FFP_EINVAL;
    return ffp_seek_keyframe(p_, direction);
}

int FfPlayer::Skip(double seconds) {
    if (!p_) return FFP_EINVAL;
    return ffp_skip(p_, seconds);
}

void FfPlayer::SetVolume(int v) {
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    volume_ = v;
    if (p_) ffp_set_volume(p_, v);
}

void FfPlayer::SetMuted(bool m) {
    muted_ = m;
    if (p_) ffp_set_mute(p_, m ? 1 : 0);
}

double FfPlayer::Position() { return p_ ? ffp_get_position(p_) : std::nan(""); }
double FfPlayer::Duration() { return p_ ? ffp_get_duration(p_) : 0.0; }

std::optional<MediaInfo> FfPlayer::GetMediaInfo() {
    if (!p_) return std::nullopt;
    char* s = ffp_get_media_info(p_);
    if (!s) return std::nullopt;
    std::string text(s);
    ffp_free(s);
    if (text.empty()) return std::nullopt;
    return MediaInfo::Parse(text);
}

int FfPlayer::CycleStream(StreamType type) {
    if (!p_) return FFP_EINVAL;
    int t;
    switch (type) {
    case StreamType::Video: t = FFP_STREAM_VIDEO; break;
    case StreamType::Audio: t = FFP_STREAM_AUDIO; break;
    case StreamType::Subtitle: t = FFP_STREAM_SUBTITLE; break;
    default: return FFP_EINVAL;
    }
    return ffp_cycle_stream(p_, t);
}

int FfPlayer::SetAudioPreference(int pref) {
    if (!p_ || (pref != 0 && pref != 1)) return FFP_EINVAL;
    int rc = ffp_set_audio_preference(p_, pref ? FFP_ARIB_AUDIO_2 : FFP_ARIB_AUDIO_1);
    if (rc == 0) audioPref_ = pref;
    return rc;
}

int FfPlayer::SetSubtitleVisible(SubtitleLayer layer, bool on) {
    if (!p_) return FFP_EINVAL;
    return ffp_set_subtitle_visible(p_, static_cast<int>(layer), on ? 1 : 0);
}

int FfPlayer::RefreshCaptionStyle(bool ignoreBackground, bool forceOutline) {
    if (!p_) return FFP_EINVAL;
    return ffp_refresh_caption_style(p_, ignoreBackground ? 1 : 0, forceOutline ? 1 : 0);
}

int FfPlayer::SetVideoFilter(const std::wstring& vf) {
    if (!p_) return FFP_EINVAL;
    std::string s = ToUtf8(vf);
    return ffp_set_video_filter(p_, s.c_str());
}

int FfPlayer::SetAudioFilter(const std::wstring& af) {
    if (!p_) return FFP_EINVAL;
    std::string s = ToUtf8(af);
    return ffp_set_audio_filter(p_, s.c_str());
}

int FfPlayer::SendFilterCommand(const std::wstring& target, const std::wstring& command, const std::wstring& argument) {
    if (!p_) return FFP_EINVAL;
    std::string t = ToUtf8(target), c = ToUtf8(command), a = ToUtf8(argument);
    return ffp_filter_command(p_, t.c_str(), c.c_str(), a.c_str(), nullptr, 0);
}

std::optional<std::string> FfPlayer::GetDebugInfoJson() {
    if (!p_) return std::nullopt;
    char* s = ffp_get_debug_info(p_);
    if (!s) return std::nullopt;
    std::string text(s);
    ffp_free(s);
    if (text.empty()) return std::nullopt;
    return text;
}

int FfPlayer::SetPresenterParams(const PresenterParams& pp) {
    if (!p_) return FFP_EINVAL;
    std::string j = ToUtf8(PresenterParamsJson(pp));
    return ffp_set_presenter_params(p_, j.c_str());
}

int FfPlayer::SetBackdropRegions(const BackdropRegion* regions, int count) {
    if (!p_ || count < 0 || count > FFP_BACKDROP_REGION_MAX || (count > 0 && !regions)) return FFP_EINVAL;
    if (count == 0) return ffp_set_backdrop_regions(p_, nullptr, 0);
    FFPBackdropRegion nativeRegions[FFP_BACKDROP_REGION_MAX]{};
    for (int i = 0; i < count; ++i) {
        const BackdropRegion& region = regions[i];
        nativeRegions[i] = {
            .x = region.x, .y = region.y, .width = region.width, .height = region.height,
            .corner_radius = region.corner_radius,
            .tint_color = region.tint_color, .tint_opacity = region.tint_opacity,
            .luminosity_color = region.luminosity_color, .luminosity_opacity = region.luminosity_opacity,
            .noise = region.noise, .z = region.z
        };
    }
    return ffp_set_backdrop_regions(p_, nativeRegions, count);
}

std::optional<BgraFrame> FfPlayer::Snapshot() { return Snapshot(false); }

std::optional<BgraFrame> FfPlayer::Snapshot(bool withSubtitles) {
    if (!p_) return std::nullopt;
    std::uint8_t* buf = nullptr;
    int w = 0, h = 0, stride = 0;
    int rc = FFP_EINVAL;
    if (withSubtitles) {
        rc = ffp_snapshot_ex(p_, FFP_SNAPSHOT_SUBTITLES, &buf, &w, &h, &stride);
    } else {
        rc = ffp_snapshot(p_, &buf, &w, &h, &stride);
    }
    if (rc != 0 || !buf) {
        if (buf) ffp_free(buf);
        return std::nullopt;     // FFP_EAGAIN = no frame displayed yet
    }
    BgraFrame f;
    f.Width = w; f.Height = h; f.Stride = stride;
    f.Data.assign(buf, buf + (size_t)stride * (size_t)h);
    ffp_free(buf);
    return f;
}

}
