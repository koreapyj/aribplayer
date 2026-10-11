#include "wvp/FfPlayer.h"

#include <cmath>
#include <unordered_map>
#include <vector>

#include "ffplay.h"
#include "wvp/JsonLite.h"

namespace wvp {

namespace {

std::mutex g_mu;
std::unordered_map<std::uint64_t, FfPlayer*> g_players;
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

// The FFPOutput of a presenter; the vector keeps the device list alive for the ffp_create / ffp_set_output call.
struct Output {
    FFPOutput out{};
    std::vector<AVBufferRef*> devices;
    explicit Output(const Presenter* p) {
        if (!p) return;
        devices = p->Devices();
        out.presenter = p->Native();
        out.devices = devices.empty() ? nullptr : devices.data();
        out.nb_devices = static_cast<int>(devices.size());
        out.only_given_devices = p->OnlyGivenDevices() ? 1 : 0;
    }
    const FFPOutput* Get() const { return out.presenter ? &out : nullptr; }
};

}

FfPlayer::FfPlayer(PresenterHandle presenter, UiPost post)
    : presenter_(std::move(presenter)), id_(g_nextId.fetch_add(1)), post_(std::move(post)) {
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
    presenter_.reset();              // libffplay released the presenter and its devices in ffp_destroy
    std::lock_guard<std::mutex> lk(qmu_);
    queue_.clear();
}

std::unique_ptr<FfPlayer> FfPlayer::Create(PresenterHandle presenter, UiPost post, int* err, const std::string& audioDriver) {
    if (err) *err = 0;
    if (audioDriver.find_first_of("=\r\n") != std::string::npos) {
        if (err) *err = FFP_EINVAL;
        return nullptr;
    }
    std::unique_ptr<FfPlayer> pl(new FfPlayer(std::move(presenter), std::move(post)));
    const std::string kv = audioDriver.empty() ? std::string() : "audio_driver=" + audioDriver;
    const Output out(pl->presenter_.get());
    pl->p_ = ffp_create(out.Get(), kv.empty() ? nullptr : kv.c_str(), &FfPlayer::OnNativeEvent, pl.get());
    if (!pl->p_) {
        if (err) *err = -1;
        return nullptr;
    }
    return pl;
}

int FfPlayer::SetPresenter(PresenterHandle presenter) {
    if (!p_) return FFP_EINVAL;
    const Output out(presenter.get());
    const int rc = ffp_set_output(p_, out.Get());
    if (rc != 0) return rc;
    presenter_.swap(presenter);      // the old presenter is destroyed here: ffp_set_output released it
    return 0;
}

void FfPlayer::OnNativeEvent(void* user, int ev, const char* json) {
    try {
        static_cast<FfPlayer*>(user)->HandleNative(ev, json);
    } catch (...) {
        // never let an exception cross into native code
    }
}

void FfPlayer::Enqueue(std::function<void()> fn) {
    if (closing_) return;
    {
        std::lock_guard<std::mutex> lk(qmu_);
        queue_.push_back(std::move(fn));
    }
    if (notifyPending_.exchange(true) || !post_) return;
    post_([id = id_] {
        FfPlayer* p = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            auto it = g_players.find(id);
            if (it != g_players.end()) p = it->second;
        }
        if (p) p->DispatchPendingEvents();
    });
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
            i.Title = r.Str("title");
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
            e.Code = r.Int("code"); e.Message = r.Str("message"); e.Url = r.Str("url");
            e.DeviceLost = r.Bool("device_lost"); e.RemovedReason = r.Str("removed_reason");
            Enqueue([this, e = std::move(e)] { Error.raise(e); });
        }
        break;
    case FFP_EVENT_FILTER_ERROR:
        if (has) {
            FilterErrorInfo f;
            f.Type = r.Str("type"); f.Filter = r.Str("filter");
            f.Code = r.Int("code"); f.Message = r.Str("message");
            f.HwframesFallback = r.Bool("hwframes_fallback");
            Enqueue([this, f = std::move(f)] { FilterError.raise(f); });
        }
        break;
    case FFP_EVENT_PRESENTER_ERROR:
        if (has) {
            PresenterErrorInfo e;
            e.Presenter = r.Str("presenter"); e.Error = r.Str("error");
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

std::optional<CapabilitySet> FfPlayer::ProbeEngine(AVBufferRef* const* devices, int count) {
    char* s = ffp_probe_caps(devices, count < 0 ? 0 : count);
    if (!s) return std::nullopt;
    std::string text(s);
    ffp_free(s);
    if (text.empty()) return std::nullopt;
    return ParseCapabilities(text);
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
        if (k.empty() || k.find_first_of("=\r\n") != std::string::npos) return false;
        if (v.find_first_of("\r\n") != std::string::npos) return false;
        if (!out.empty()) out.push_back('\n');
        out += k;
        out.push_back('=');
        out += v;
    }
    return true;
}

int FfPlayer::Open(const std::string& url, const OpenOptions& options) {
    if (url.empty() || !p_) return FFP_EINVAL;
    std::string kv;
    if (!BuildOptionsString(options, kv)) return FFP_EINVAL;
    audioPref_ = 0;                       // every file starts on audio 1; nothing is remembered
    ffp_set_audio_preference(p_, FFP_ARIB_AUDIO_1);
    return ffp_open(p_, url.c_str(), kv.empty() ? nullptr : kv.c_str());
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

int FfPlayer::SetVideoFilter(const std::string& vf) {
    if (!p_) return FFP_EINVAL;
    return ffp_set_video_filter(p_, vf.c_str());
}

int FfPlayer::SetAudioFilter(const std::string& af) {
    if (!p_) return FFP_EINVAL;
    return ffp_set_audio_filter(p_, af.c_str());
}

int FfPlayer::SendFilterCommand(const std::string& target, const std::string& command, const std::string& argument) {
    if (!p_) return FFP_EINVAL;
    return ffp_filter_command(p_, target.c_str(), command.c_str(), argument.c_str(), nullptr, 0);
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
