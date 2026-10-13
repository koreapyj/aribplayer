#include "Engine.h"

#import <Foundation/Foundation.h>

#include <chrono>
#include <cstdio>
#include <optional>

#include "Localize.h"
#include "Services.h"
#include "features/Logging.h"
#include "features/LoggingFormat.h"
#include "features/SettingsService.h"
#include "wvp/AppSettings.h"
#include "wvp/DebugInfo.h"
#include "wvp/PipelinePlanner.h"
#include "wvp/mac/MacVideoPlatform.h"
#include "wvp/win32/Utf.h"

extern "C" {
#include "ffplay.h"
}

namespace mainview {

std::uint64_t NowMs() {
    return (std::uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/// App events: stderr ("[area] text", Console.app / `log stream` / the test logs) and the app log sink (log::Event: the
/// Debug logging file, --log-file, --console), as the Windows app's log::Event calls.
void Log(const std::string& area, const std::string& line) {
    std::fprintf(stderr, "[%s] %s\n", area.c_str(), line.c_str());
    std::fflush(stderr);
    app::log::Event(area, line);
}

namespace {

std::string CostText(const wvp::PlanCost& c) {
    return std::to_string(c.Weighted()) + " host, " + std::to_string(c.deviceCopies) + " device";
}

bool AnyDeviceOk(const wvp::CapabilitySet& caps) {
    for (const auto& d : caps.devices)
        if (d.ok) return true;
    return false;
}

std::string Join(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& e : v) s += (s.empty() ? "" : ", ") + e;
    return s.empty() ? "-" : s;
}

// AppPipeline.cpp LogCaps.
void LogCaps(const wvp::CapabilitySet& caps) {
    std::vector<std::string> ok, bad, dec, io, pr, hwf;
    for (const auto& d : caps.devices) (d.ok ? ok : bad).push_back(d.type);
    for (const auto& d : caps.decoders) if (d.ok) dec.push_back(d.codec + "/" + d.hwaccel);
    for (const auto& f : caps.filters) {
        auto dom = wvp::DomainFromName(f.pixFmts.empty() ? "" : f.pixFmts.front());
        if (dom && wvp::IsHardware(*dom)) hwf.push_back(f.name);
    }
    for (const auto& i : caps.interop)
        io.push_back(std::string(wvp::DomainName(i.from)) + ">" + wvp::DomainName(i.to) + (i.ok ? " ok" : " FAILED"));
    for (const auto& p : caps.presenters) pr.push_back(p.name + (p.ok ? " ok" : " FAILED"));
    Log("caps", "source: " + caps.source + ", ffmpeg " + caps.ffmpegVersion + ", adapter: " + caps.adapter.name + ", features: " + Join(caps.features));
    Log("caps", "devices ok: " + Join(ok) + "; failed: " + Join(bad));
    Log("caps", "decoders usable: " + Join(dec));
    Log("caps", "filters: " + std::to_string(caps.filters.size()) + " (hardware: " + Join(hwf) + ")");
    Log("caps", "interop: " + Join(io));
    Log("caps", "presenters: " + Join(pr));
}

// AppVideoGpu.cpp ProbeAdapters: every adapter probed in parallel, merged with what the build fixes.
std::vector<wvp::AdapterCandidate> ProbeAdapters(wvp::VideoPlatform& platform, const std::vector<wvp::AdapterEntry>& adapters) {
    std::vector<std::optional<wvp::CapabilitySet>> answers(adapters.size());
    std::vector<std::thread> threads;
    for (size_t i = 0; i < adapters.size(); i++)
        threads.emplace_back([&, i] { answers[i] = platform.Probe(adapters[i]); });
    for (auto& t : threads) t.join();
    const wvp::CapabilitySet build = wvp::mac::BuildCapabilities();   // Windows: wvp::BuildCapabilities()
    std::vector<wvp::AdapterCandidate> out;
    for (size_t i = 0; i < adapters.size(); i++) {
        const std::optional<wvp::CapabilitySet>& caps = answers[i];
        if (!caps || !AnyDeviceOk(*caps)) continue;   // no hardware video on this adapter: not a video GPU
        wvp::AdapterCandidate c;
        c.luid = adapters[i].luid;
        c.name = adapters[i].name;
        c.vendorId = adapters[i].vendorId;
        c.deviceId = adapters[i].deviceId;
        c.caps = wvp::MergeBuildCapabilities(*caps, build);
        out.push_back(std::move(c));
    }
    return out;
}

}

std::string Engine::FormatOptions(const wvp::OpenOptions& o) { return app::log::FormatOptionsLine(o); }

Engine::Engine(app::Services& svc, void* videoView, Post post)
    : svc_(svc), view_(videoView), post_(std::move(post)), platform_(std::make_unique<wvp::mac::MacVideoPlatform>()) {
    startMs_ = NowMs();
    UpdateCaptionFont();
}

/// The bundled caption font, registered for the process by ApplyCaptionOpenOptions.
void Engine::UpdateCaptionFont() {
    const AribFont& f = BundledCaptionFont();
    if (!font_.option.empty() && f.file == font_.file && f.option == font_.option) return;
    font_ = f;
    if (font_.Ok())
        Log("caption", "font " + font_.file + ": family '" + font_.family + "', PostScript '" + font_.postscript + "', CoreText resolves it to " +
                           font_.resolved + "; arib_font=" + font_.option);
    else
        Log("caption", "caption font: " + font_.error + "; arib_font=" + font_.option + " (libaribcaption's defaults)");
}

Engine::~Engine() {
    *alive_ = false;
    if (probeThread_.joinable()) probeThread_.join();
    ReleasePlayer();
}

void Engine::ReleasePlayer() {
    ctl_ = nullptr;
    player_.reset();
}

wvp::mac::MetalPresenter* Engine::Presenter() const {
    return player_ ? dynamic_cast<wvp::mac::MetalPresenter*>(player_->CurrentPresenter()) : nullptr;
}

void Engine::ViewChanged() {
    if (wvp::mac::MetalPresenter* p = Presenter()) p->ViewChanged();
}

std::string Engine::PresenterStats() const {
    wvp::mac::MetalPresenter* p = Presenter();
    if (!p || !p->Native() || !p->Native()->stats_json) return "{}";
    char buf[8192];
    const int n = p->Native()->stats_json(p->Native()->opaque, buf, sizeof buf);
    return n > 0 ? std::string(buf, std::min<size_t>((size_t)n, sizeof buf - 1)) : "{}";
}

bool Engine::Loaded() const { return ctl_ && ctl_->Current() != nullptr; }

void Engine::ReopenHere() {
    if (Loaded()) ctl_->ReopenCurrentHere();
}

// --- startup: probe, player ---------------------------------------------------------------------------------------------

std::vector<wvp::AdapterEntry> Engine::PhysicalAdapters() {
    probedAdapters_ = platform_->Adapters();
    probedLuids_.clear();
    for (const wvp::AdapterEntry& e : probedAdapters_) probedLuids_.push_back(e.luid);
    physGpus_ = wvp::GroupPhysicalGpus(probedAdapters_);
    std::vector<wvp::AdapterEntry> out;
    for (const wvp::AdapterEntry& e : probedAdapters_)
        if (wvp::CanonicalLuid(physGpus_, e.luid) == e.luid) out.push_back(e);
    return out;
}

void Engine::LogAdapters(const std::string& why) const {
    for (const wvp::AdapterEntry& e : probedAdapters_) {
        const std::string canon = wvp::CanonicalLuid(physGpus_, e.luid);
        Log("adapter", why + ": " + e.name + " luid " + e.luid + ", outputs " + (e.hasOutputs ? "yes" : "no") + " -> " +
                           (canon.empty() ? std::string("not a candidate") : canon == e.luid ? std::string("GPU") : "alias of " + canon));
    }
}

void Engine::StartProbe(const std::string& why) {
    if (probeBusy_) { probePending_ = why; return; }
    if (probeThread_.joinable()) probeThread_.join();
    probeBusy_ = true;
    std::vector<wvp::AdapterEntry> hw = PhysicalAdapters();
    LogAdapters(why);
    Log("caps", "probing " + std::to_string(hw.size()) + " adapter(s): " + why);
    auto alive = alive_;
    probeThread_ = std::thread([this, hw, platform = platform_.get(), why, alive] {
        std::vector<wvp::AdapterCandidate> cands = ProbeAdapters(*platform, hw);
        post_([this, alive, cands = std::move(cands), why]() mutable {
            if (*alive) OnAdaptersProbed(std::move(cands), why);
        });
    });
}

bool Engine::CreatePlayer(const std::string& audioDriver, std::string* error) {
    // AppPlayback.cpp InitPlayer: the first presenter runs on the display adapter; SelectVideoGpuNow moves it once the
    // adapters are probed.
    const std::string display = platform_->DisplayAdapter(view_);
    int err = 0;
    player_ = wvp::FfPlayer::Create(MakePresenter(display), post_, &err, audioDriver);
    if (!player_) {
        if (error) *error = "libffplay failed to start (error " + std::to_string(err) + ")";
        return false;
    }
    Log("player", "libffplay " + wvp::FfPlayer::Version() + ", presenter " + (player_->CurrentPresenter() ? "metal on " + display : std::string("none")) +
                      ", audio driver " + (audioDriver.empty() ? std::string("SDL default") : audioDriver));
    return true;
}

wvp::PresenterHandle Engine::MakePresenter(const std::string& luid) {
    wvp::AdapterEntry entry;
    bool found = false;
    // "" (no hardware adapter chosen: software decoding) still presents through Metal: the display adapter's device.
    const std::string want = luid.empty() ? platform_->DisplayAdapter(view_) : luid;
    for (const wvp::AdapterEntry& e : platform_->Adapters())
        if (e.luid == want) { entry = e; found = true; break; }
    if (!found) entry.luid = want;
    std::string error;
    wvp::PresenterHandle p = platform_->CreatePresenter(entry, view_, &error);
    if (!p) Log("presenter", "no presenter on " + (entry.name.empty() ? want : entry.name) + ": " + error);
    return p;
}

bool Engine::UsePresenterOn(const std::string& luid) {
    if (!player_) return false;
    const wvp::Presenter* cur = player_->CurrentPresenter();
    if (cur && (cur->AdapterId() == luid || luid.empty())) return false;
    wvp::PresenterHandle p = MakePresenter(luid);
    if (!p && cur) return false;   // keep the working presenter
    const int rc = player_->SetPresenter(std::move(p));
    Log("presenter", "presenter on " + luid + (rc == 0 ? std::string() : " failed: " + std::to_string(rc)));
    return rc == 0;
}

void Engine::OnAdaptersProbed(std::vector<wvp::AdapterCandidate> candidates, const std::string& why) {
    capsProbed_ = true;
    probeBusy_ = false;
    if (why == "startup") {
        startupProbeMs_ = NowMs() - startMs_;
        Log("caps", "startup probe done " + std::to_string(startupProbeMs_) + " ms after start");
    }
    for (const auto& c : candidates) {
        Log("caps", "video GPU candidate " + c.name + " (luid " + c.luid + ")");
        LogCaps(c.caps);
    }
    if (candidates.empty()) Log("caps", "no adapter could be probed: video cannot be played");
    adapters_ = std::move(candidates);
    const std::string before = playLuid_;
    wvp::FilterChange fc;
    bool switched = false;
    SelectVideoGpuNow(why, false, &fc, &switched);
    if (ctl_ && ctl_->HasPendingOpen()) {
        ctl_->OpenPending();   // the open that waited for the plan
    } else if (Loaded()) {
        if (switched || fc.reopen || before != playLuid_) ctl_->ReopenCurrentHere();
        else ctl_->ApplyFilterChange(fc);
    }
    ReportPlanProblem();
    if (!probePending_.empty()) {
        std::string next = std::move(probePending_);
        probePending_.clear();
        StartProbe(next);
    }
    if (onProbed) onProbed();
}

std::string Engine::DisplayGpuLuid() const {
    const std::string raw = platform_->DisplayAdapter(view_);
    const std::string c = wvp::CanonicalLuid(physGpus_, raw);
    return c.empty() ? raw : c;
}

bool Engine::SelectVideoGpuNow(const std::string& trigger, bool onlyIfCheaper, wvp::FilterChange* change, bool* presenterSwitched) {
    if (presenterSwitched) *presenterSwitched = false;
    if (adapters_.empty()) return false;
    displayLuid_ = DisplayGpuLuid();
    const std::vector<wvp::AdapterCandidate>& cands = adapters_;
    const wvp::AppSettings& s = svc_.settings;
    const wvp::PlanRequest req = svc_.filters.Request();
    wvp::VideoGpuPref pref = s.videoGpu;
    if (!pref.Automatic())
        if (std::string c = wvp::CanonicalLuid(physGpus_, pref.luid); !c.empty()) pref.luid = c;
    wvp::VideoGpuChoice ch = wvp::SelectVideoGpu(cands, displayLuid_, pref, req);

    if (onlyIfCheaper && haveVideoChoice_ && videoChoice_.index >= 0 && videoChoice_.index < (int)cands.size() && ch.index != videoChoice_.index) {
        const auto& cur = cands[(size_t)videoChoice_.index];
        wvp::PlanCost now = wvp::TotalWithCrossAdapter(wvp::PipelinePlanner(cur.caps).Make(req).cost, cur.luid != displayLuid_);
        if (!(ch.total < now)) {
            Log("adapter", trigger + ": keeping " + cur.name + " (" + CostText(now) + ")");
            return false;
        }
    }
    const bool changed = !haveVideoChoice_ || ch.index != videoChoice_.index || (ch.index >= 0 && cands[(size_t)ch.index].luid != playLuid_);
    videoChoice_ = ch;
    haveVideoChoice_ = true;

    wvp::CapabilitySet caps;
    std::string luid, name;
    if (ch.index >= 0) {
        const auto& c = cands[(size_t)ch.index];
        luid = c.luid;
        name = c.name;
        caps = c.caps;
    } else {
        caps = wvp::WithoutHardwareDevices(cands[0].caps, ch.reason);
    }
    playLuid_ = luid;
    wvp::FilterChange fc = svc_.filters.SetCapabilities(std::move(caps));
    const bool switched = UsePresenterOn(luid);
    if (presenterSwitched) *presenterSwitched = switched;
    if (changed || trigger != "open") {
        std::string runner = ch.runnerUp >= 0 ? "; runner-up " + cands[(size_t)ch.runnerUp].name + " " + CostText(ch.runnerUpTotal) : "";
        Log("adapter", trigger + ": video GPU " + (luid.empty() ? std::string("none") : name + " (luid " + luid + ")") + ", display adapter " +
                           (displayLuid_.empty() ? std::string("unknown") : displayLuid_) + ": " + ch.reason + runner);
        if (const wvp::Plan* pl = svc_.filters.CurrentPlan()) Log("pipeline", "planner active: " + pl->Summary());
        else Log("pipeline", "planner found no valid pipeline: " + svc_.filters.PlanError());
    }
    if (change) *change = fc;
    return changed;
}

void Engine::ReportPlanProblem() {
    if (!capsProbed_) return;   // the probe is still running: opens wait for it
    std::wstring message;
    if (adapters_.empty())
        message = app::Tr(L"No graphics adapter could be probed for video (see the log).", L"영상용 그래픽 어댑터를 검사하지 못했습니다 (로그 참조).");
    else if (!svc_.filters.CurrentPlan())
        message = app::TrW(L"No video pipeline for the current settings: " + wvp::ToWide(svc_.filters.PlanError()),
                           L"현재 설정으로 영상 파이프라인을 만들 수 없습니다: " + wvp::ToWide(svc_.filters.PlanError()));
    const std::string key = wvp::ToUtf8(message);
    if (key == reportedPlanProblem_) return;
    reportedPlanProblem_ = key;
    if (message.empty()) return;
    Log("pipeline", key);
    if (onProblem) onProblem(app::Tr(L"Video cannot be played", L"영상을 재생할 수 없습니다"), message);
}

// --- pipeline (AppPipeline.cpp) ------------------------------------------------------------------------------------------

std::string Engine::OptionOf(const wvp::OpenOptions& o, const std::string& key) {
    for (const auto& [k, v] : o) if (k == key) return v;
    return {};
}

wvp::OpenOptions Engine::EffectiveOpenOptions() const { return app::log::EffectiveOptions(lastOpenOpts_, liveVf_, liveAf_); }

/// App::BuildDebugContext's adapter note: video GPU, display adapter, why, runner-up; warn on a cross-adapter copy, a
/// missing manual GPU or no hardware adapter.
std::string Engine::AdapterNote(bool* warn) const {
    auto nameOf = [&](const std::string& luid) {
        for (const auto& a : adapters_) if (a.luid == luid) return a.name;
        return luid.empty() ? std::string("unknown") : luid;
    };
    std::string note = (playLuid_.empty() ? std::string("video GPU none") : "video GPU " + nameOf(playLuid_) + " (luid " + playLuid_ + ")") +
                       ", display " + nameOf(displayLuid_) + " - " + (haveVideoChoice_ ? videoChoice_.reason : std::string("not chosen yet"));
    if (haveVideoChoice_ && videoChoice_.runnerUp >= 0 && videoChoice_.runnerUp < (int)adapters_.size())
        note += "; runner-up " + adapters_[(size_t)videoChoice_.runnerUp].name + " [host " + std::to_string(videoChoice_.runnerUpTotal.Weighted()) +
                ", device " + std::to_string(videoChoice_.runnerUpTotal.deviceCopies) + "]";
    if (warn) *warn = haveVideoChoice_ && (videoChoice_.crossAdapter || videoChoice_.manualMissing || videoChoice_.index < 0);
    return note;
}

void Engine::ApplyCaptionOpenOptions() {
    // features/SettingsService ApplyCaptionOpenOptions (SettingsServiceMac.mm): the caption font, superimpose and the
    // macOS caption style (MediaAccessibility) mapped to arib_ignore_background / arib_force_outline.
    UpdateCaptionFont();
    app::ApplyCaptionOpenOptions(*ctl_, svc_.settings);
}

void Engine::Wire(wvp::PlaybackController& ctl) {
    ctl_ = &ctl;
    ctl.SetFilters(svc_.filters.Provider());
    ctl.beforeOpen = [this](const std::string& url) {
        ApplyCaptionOpenOptions();
        // plan with what we know about this file (cached from an earlier open) before the options are built
        wvp::ContentInfo c = pipe_.BeforeOpen(url);
        svc_.filters.SetContent(c);
        if (onPipelineContentChanged) onPipelineContentChanged();
        SelectVideoGpuNow("open", false);
        ReportPlanProblem();
    };
    ctl.onOpening = [this](const std::string& url, const wvp::OpenOptions& o) {
        lastOpenUrl_ = url;
        lastOpenOpts_ = o;
        liveVf_ = OptionOf(o, "vf");
        liveAf_ = OptionOf(o, "af");
        Log("open", "url: " + url);
        Log("open", "options: " + FormatOptions(o));
        if (const wvp::Plan* pl = svc_.filters.CurrentPlan()) {
            pipeOpenMs_ = NowMs();
            pipePollNextMs_ = pipeOpenMs_ + 1000;
            Log("pipeline", pl->Summary());
        }
    };
    ctl.onFilterChange = [this](const wvp::FilterChange& c) {
        liveVf_ = c.videoFilter;   // App::effOpts_: the open options with the live filter strings (overlay, log)
        liveAf_ = c.audioFilter;
        Log("filter", "vf" + std::string(c.videoRebuild ? " (rebuild)" : " (unchanged)") + "=" + c.videoFilter + " | af" +
                          (c.audioRebuild ? " (rebuild)" : " (unchanged)") + "=" + c.audioFilter + (c.reopen ? " (reopen)" : ""));
    };
    player_->Prepared.add([this](const wvp::MediaPreparedInfo& p) {
        Log("player", "prepared: duration " + std::to_string(p.Duration) + " s, " + std::to_string(p.Width) + "x" + std::to_string(p.Height) + ", video " +
                          (p.HasVideo ? "yes" : "no") + ", audio " + (p.HasAudio ? "yes" : "no") + ", seekable " + (p.Seekable ? "yes" : "no"));
        statsAtPrepared_ = PresenterStats();
        ApplyPipelineContent();
    });
    player_->MediaInfoChanged.add([this] { ApplyPipelineContent(); });
    player_->Error.add([this](const wvp::PlayerErrorInfo& e) {
        if (e.DeviceLost) { OnVideoDeviceLost("decoder: " + e.Message + " " + e.RemovedReason); return; }
        Log("player", "ERROR code " + std::to_string(e.Code) + ": " + e.Message + " (" + e.Url + ")");
    });
    player_->FilterError.add([this](const wvp::FilterErrorInfo& e) {
        Log("filter", "ERROR " + e.Type + " " + e.Filter + " code " + std::to_string(e.Code) + ": " + e.Message);
        const wvp::Plan* pl = svc_.filters.CurrentPlan();
        if (!pl) return;
        if (auto fb = pipe_.OnFilterError(*pl, e)) PipelineFallback(*fb);
    });
    player_->PresenterError.add([this](const wvp::PresenterErrorInfo& e) { OnPresenterError(e); });
}

void Engine::ApplyPipelineContent() {
    if (!player_ || !ctl_) return;
    auto mi = player_->GetMediaInfo();
    if (!mi) return;
    const std::string url = lastOpenUrl_;
    wvp::PipelineSession::Prepared pr = pipe_.OnPrepared(url, *mi);
    if (!pr.content.Known() || !svc_.filters.PlannerActive()) {
        if (pr.content.Known()) {
            svc_.filters.SetContent(pr.content);   // remembered for when capabilities arrive
            if (onPipelineContentChanged) onPipelineContentChanged();
        }
        return;
    }
    Log("pipeline", "content: " + pr.content.codec + " " + std::to_string(pr.content.width) + "x" + std::to_string(pr.content.height) + " " +
                        std::to_string(pr.content.bitDepth) + "-bit" + (pr.content.progressive ? " progressive" : "") + (pr.changed ? " (changed since the plan was made)" : ""));
    wvp::FilterChange ch = svc_.filters.SetContent(pr.content);
    if (onPipelineContentChanged) onPipelineContentChanged();
    if (ch.reopen) {
        if (!pipe_.ClaimContentReopen(url)) return;   // once per url chain: never loop
        if (const wvp::Plan* pl = svc_.filters.CurrentPlan()) Log("pipeline", "replanned for the content: " + pl->Summary() + " -> reopen");
        const int gen = pipe_.Generation();
        auto alive = alive_;
        post_([this, alive, gen] { if (*alive && pipe_.Generation() == gen) ReopenHere(); });
    } else {
        ctl_->ApplyFilterChange(ch);   // graph rebuild only
    }
}

void Engine::Poll(std::uint64_t now) {
    if (!player_ || player_->State() != wvp::PlayerState::Playing || !svc_.filters.PlannerActive()) return;
    if (now < pipePollNextMs_ || now > pipeOpenMs_ + 10000) return;
    pipePollNextMs_ = now + 1000;
    auto j = player_->GetDebugInfoJson();
    if (!j) return;
    wvp::DebugInfo d = wvp::ParseDebugInfo(*j);
    if (auto fb = pipe_.OnDebugInfo(*svc_.filters.CurrentPlan(), d)) PipelineFallback(*fb);
}

void Engine::PipelineFallback(const wvp::PipelineSession::Fallback& fb) {
    Log("pipeline", "fallback: " + fb.key + " (" + fb.reason + ")");
    wvp::FilterChange ch = svc_.filters.Blacklist(fb.key);
    if (const wvp::Plan* pl = svc_.filters.CurrentPlan()) Log("pipeline", "replanned: " + pl->Summary());
    if (Loaded() && (ch.reopen || ch.videoRebuild)) {
        const int gen = pipe_.Generation();
        auto alive = alive_;
        post_([this, alive, gen] { if (*alive && pipe_.Generation() == gen) ReopenHere(); });
    }
}

void Engine::ApplyFilterSettings(const wvp::FilterSettings& next) {
    wvp::FilterChange fc = svc_.filters.Apply(next);
    if (const wvp::Plan* pl = svc_.filters.CurrentPlan()) Log("pipeline", "filters changed: " + pl->Summary());
    if (ctl_) ctl_->ApplyFilterChange(fc);
}

void Engine::VideoGpuSettingChanged() {
    const std::string before = playLuid_;
    wvp::FilterChange fc;
    bool switched = false;
    SelectVideoGpuNow("setting", false, &fc, &switched);
    ReportPlanProblem();
    if (!Loaded()) return;
    if (switched || fc.reopen || before != playLuid_) ctl_->ReopenCurrentHere();
    else ctl_->ApplyFilterChange(fc);
}

// --- device loss / presenter errors / display changes (AppVideoGpu.cpp) ---------------------------------------------------

void Engine::OnVideoDeviceLost(const std::string& why) {
    Log("gpu", "video device lost: " + why);
    pipe_.Clear();
    svc_.filters.ClearBlacklist();
    SelectVideoGpuNow("device lost", false);
    const std::uint64_t now = NowMs();
    if (Loaded() && (lastRestoreMs_ == 0 || now - lastRestoreMs_ > 250)) {
        lastRestoreMs_ = now;
        auto alive = alive_;
        post_([this, alive] { if (*alive) ReopenHere(); });   // the presenter makes a new device in the open
    }
    std::vector<std::string> luids;
    for (const wvp::AdapterEntry& e : platform_->Adapters()) luids.push_back(e.luid);
    if (luids != probedLuids_) StartProbe("adapter set changed after a device loss");
}

void Engine::OnPresenterError(const wvp::PresenterErrorInfo& e) {
    if (e.DeviceLost) { OnVideoDeviceLost("presenter: " + e.Error + " " + e.RemovedReason); return; }
    Log("presenter", e.Presenter + " failed: " + e.Error);
    const std::uint64_t now = NowMs();
    if (!Loaded() || (presenterReopenMs_ != 0 && now - presenterReopenMs_ < 2000)) return;   // failing again at once: stay without video
    presenterReopenMs_ = now;
    auto alive = alive_;
    post_([this, alive] { if (*alive) ReopenHere(); });
}

void Engine::DisplayChanged() {
    std::vector<std::string> luids;
    for (const wvp::AdapterEntry& e : platform_->Adapters()) luids.push_back(e.luid);
    if (luids != probedLuids_) {
        StartProbe("adapter set changed");
        return;
    }
    if (adapters_.size() >= 2 && DisplayGpuLuid() != displayLuid_ && SelectVideoGpuNow("display change", true) && Loaded())
        ctl_->ReopenCurrentHere();
}

}
