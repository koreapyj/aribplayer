// Video GPU glue: per-adapter capability probes (VideoPlatform::Probe), the display adapter, the selection triggers (startup,
// open, drag end, display change, device loss), the presenter on the selected adapter and playback restore after a device
// loss or a presenter error. The decision is wvp::SelectVideoGpu.
#include "App.h"
#include "Localize.h"

#include <cstdio>

#include "features/FiltersPanel.h"
#include "features/Logging.h"
#include "features/SettingsService.h"
#include "wvp/AppSettings.h"
#include "wvp/FfPlayer.h"
#include "wvp/win32/Utf.h"

namespace app {

namespace {

std::string CostText(const wvp::PlanCost& c) {
    return std::to_string(c.Weighted()) + " host, " + std::to_string(c.deviceCopies) + " device";
}

bool AnyDeviceOk(const wvp::CapabilitySet& caps) {
    for (const auto& d : caps.devices)
        if (d.ok) return true;
    return false;
}

}

std::vector<wvp::AdapterCandidate> ProbeAdapters(wvp::VideoPlatform& platform, const std::vector<wvp::AdapterEntry>& adapters) {
    std::vector<std::optional<wvp::CapabilitySet>> answers(adapters.size());
    std::vector<std::thread> threads;
    for (size_t i = 0; i < adapters.size(); i++)
        threads.emplace_back([&, i] { answers[i] = platform.Probe(adapters[i]); });
    for (auto& t : threads) t.join();
    const wvp::CapabilitySet build = wvp::BuildCapabilities();
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

wvp::PresenterHandle App::MakePresenter(const std::string& luid) {
    if (!platform_) return nullptr;
    wvp::AdapterEntry entry;
    for (const wvp::AdapterEntry& e : platform_->Adapters())
        if (e.luid == luid) { entry = e; break; }
    std::string error;
    wvp::PresenterHandle p = platform_->CreatePresenter(entry, video_, &error);
    if (!p) log::Event("presenter", "no presenter on " + (entry.luid.empty() ? std::string("the default adapter") : entry.name) + ": " + error, log::kWarning);
    return p;
}

bool App::UsePresenterOn(const std::string& luid) {
    if (!player_) return false;
    const wvp::Presenter* cur = player_->CurrentPresenter();
    if (cur && cur->AdapterId() == luid) return false;
    wvp::PresenterHandle p = MakePresenter(luid);
    if (!p && cur) return false;   // keep the working presenter
    const int rc = player_->SetPresenter(std::move(p));
    log::Event("presenter", "presenter on " + (luid.empty() ? std::string("the default adapter") : luid) +
                                (rc == 0 ? std::string() : " failed: " + std::to_string(rc)), rc == 0 ? log::kInfo : log::kWarning);
    return rc == 0;
}

void App::OnAdaptersProbed(std::vector<wvp::AdapterCandidate> candidates, const std::string& why) {
    capsProbed_ = true;
    adapterProbeBusy_ = false;
    if (why == "startup") {
        startupProbeMs_ = Now() - startMs_;
        log::Event("caps", "startup probe done " + std::to_string(startupProbeMs_) + " ms after start");
    }
    for (const auto& c : candidates) {
        log::Event("caps", "video GPU candidate " + c.name + " (luid " + c.luid + ")");
        LogCaps(c.caps);
    }
    if (candidates.empty()) log::Event("caps", "no adapter could be probed: video cannot be played", log::kError);
    adapters_ = std::move(candidates);
    const std::string before = playAdapter_.luid;
    wvp::FilterChange fc;
    bool switched = false;
    SelectVideoGpuNow(why, false, &fc, &switched);
    if (ctl_ && ctl_->HasPendingOpen()) {
        ctl_->OpenPending();   // the open that waited for the plan
    } else if (Loaded()) {
        if (switched || fc.reopen || before != playAdapter_.luid) ctl_->ReopenCurrentHere();
        else SendFilterChange(*this, fc);
    }
    ReportPlanProblem();
    if (!adapterProbePending_.empty()) {
        std::string next = std::move(adapterProbePending_);
        adapterProbePending_.clear();
        StartAdapterProbe(next);
    }
    if (debugVisible_) RefreshDebugOverlay();
}

std::vector<wvp::AdapterEntry> App::PhysicalAdapters() {
    probedAdapters_ = platform_ ? platform_->Adapters() : std::vector<wvp::AdapterEntry>{};
    probedLuids_.clear();
    for (const wvp::AdapterEntry& e : probedAdapters_) probedLuids_.push_back(e.luid);
    physGpus_ = wvp::GroupPhysicalGpus(probedAdapters_);
    std::vector<wvp::AdapterEntry> out;
    for (const wvp::AdapterEntry& e : probedAdapters_)
        if (wvp::CanonicalLuid(physGpus_, e.luid) == e.luid) out.push_back(e);
    return out;
}

void App::LogAdapters(const std::string& why) const {
    for (const wvp::AdapterEntry& e : probedAdapters_) {
        char pci[32] = "none";
        if (e.hasPci) snprintf(pci, sizeof pci, "%d:%d.%d", e.bus, e.device, e.function);
        const std::string canon = wvp::CanonicalLuid(physGpus_, e.luid);
        log::Event("adapter", why + ": " + e.name + " luid " + e.luid + ", pci " + pci + ", outputs " + (e.hasOutputs ? "yes" : "no") +
                                  " -> " + (canon.empty() ? std::string("not a candidate") : canon == e.luid ? std::string("GPU") : "alias of " + canon));
    }
}

void App::ReportPlanProblem() {
    if (!capsProbed_) return;   // the probe is still running: opens wait for it
    std::wstring message;
    if (adapters_.empty())
        message = Tr(L"No graphics adapter could be probed for video (see the log).", L"영상용 그래픽 어댑터를 검사하지 못했습니다 (로그 참조).");
    else if (!svc_->filters.CurrentPlan())
        message = TrW(L"No video pipeline for the current settings: " + wvp::ToWide(svc_->filters.PlanError()),
                      L"현재 설정으로 영상 파이프라인을 만들 수 없습니다: " + wvp::ToWide(svc_->filters.PlanError()));
    const std::string key = wvp::ToUtf8(message);
    if (key == reportedPlanProblem_) return;
    reportedPlanProblem_ = key;
    if (message.empty()) return;
    log::Event("pipeline", key, log::kError);
    ShowInfoBar(InfoSeverity::Error, Tr(L"Video cannot be played", L"영상을 재생할 수 없습니다"), message);
}

void App::StartAdapterProbe(const std::string& why) {
    if (!platform_) return;
    if (adapterProbeBusy_) { adapterProbePending_ = why; return; }
    if (adapterProbeThread_.joinable()) adapterProbeThread_.join();
    adapterProbeBusy_ = true;
    std::vector<wvp::AdapterEntry> hw = PhysicalAdapters();
    LogAdapters(why);
    log::Event("caps", "re-probing " + std::to_string(hw.size()) + " adapter(s): " + why);
    adapterProbeThread_ = std::thread([hw, platform = platform_.get(), why] {
        std::vector<wvp::AdapterCandidate> cands = ProbeAdapters(*platform, hw);
        if (App* a = App::Instance()) a->PostToUi([a, cands = std::move(cands), why]() mutable { a->OnAdaptersProbed(std::move(cands), why); });
    });
}

bool App::SelectVideoGpuNow(const std::string& trigger, bool onlyIfCheaper, wvp::FilterChange* change, bool* presenterSwitched) {
    if (presenterSwitched) *presenterSwitched = false;
    if (adapters_.empty()) return false;
    displayRawLuid_ = platform_ ? platform_->DisplayAdapter(hwnd_) : std::string();
    displayLuid_ = wvp::CanonicalLuid(physGpus_, displayRawLuid_);
    if (displayLuid_.empty()) displayLuid_ = displayRawLuid_;
    const std::vector<wvp::AdapterCandidate>& cands = adapters_;
    const wvp::AppSettings& s = svc_->settings;
    const wvp::PlanPrefs prefs = wvp::PlanPrefsFor(s.hwDecoder);
    wvp::PlanRequest req = svc_->filters.Request();
    req.decoder = prefs.decoder;
    req.forcedDecoder = prefs.forcedDecoder;
    wvp::VideoGpuPref pref = s.videoGpu;   // a manual choice stored as an alias means its GPU
    if (!pref.Automatic())
        if (std::string c = wvp::CanonicalLuid(physGpus_, pref.luid); !c.empty()) pref.luid = c;
    wvp::VideoGpuChoice ch = wvp::SelectVideoGpu(cands, displayLuid_, pref, req);
    if (videoSoftwareFallback_) {
        ch = {};
        ch.reason = "graphics fallback after repeated device loss -> software decoding on the default adapter";
    }

    if (onlyIfCheaper && haveVideoChoice_ && videoChoice_.index >= 0 && videoChoice_.index < (int)cands.size() &&
        ch.index != videoChoice_.index) {
        const auto& cur = cands[static_cast<size_t>(videoChoice_.index)];
        wvp::PlanCost now = wvp::TotalWithCrossAdapter(wvp::PipelinePlanner(cur.caps).Make(req).cost, cur.luid != displayLuid_);
        if (!(ch.total < now)) {
            log::Event("adapter", trigger + ": keeping " + cur.name + " (" + CostText(now) + "; " +
                                      (ch.index >= 0 ? cands[static_cast<size_t>(ch.index)].name : std::string("software")) + " " + CostText(ch.total) + ")");
            return false;
        }
    }
    if (ch.manualMissing && !manualMissingNoticed_) {
        manualMissingNoticed_ = true;
        ShowToast(TrW(L"Video GPU " + wvp::ToWide(s.videoGpu.name) + L" not found: using Automatic", L"영상 GPU " + wvp::ToWide(s.videoGpu.name) + L"을(를) 찾을 수 없어 자동으로 설정합니다"));
    }
    const bool changed = !haveVideoChoice_ || ch.index != videoChoice_.index ||
                         (ch.index >= 0 && cands[static_cast<size_t>(ch.index)].luid != playAdapter_.luid);
    videoChoice_ = ch;
    haveVideoChoice_ = true;

    wvp::PlanPrefs pp = prefs;
    PlaybackAdapter pa;
    pa.note = ch.reason;
    if (ch.index >= 0) {
        const auto& c = cands[static_cast<size_t>(ch.index)];
        pa.luid = c.luid;
        pa.name = wvp::ToWide(c.name);
    } else {
        pp.decoder = wvp::DecoderPref::Software;
    }
    playAdapter_ = pa;
    svc_->filters.SetPlanPreferences(pp.decoder, pp.forcedDecoder);
    wvp::FilterChange fc = svc_->filters.SetCapabilities(cands[static_cast<size_t>(ch.index >= 0 ? ch.index : 0)].caps);
    const bool switched = UsePresenterOn(pa.luid);
    if (presenterSwitched) *presenterSwitched = switched;
    if (changed || trigger != "open") {
        std::string runner = ch.runnerUp >= 0 ? "; runner-up " + cands[static_cast<size_t>(ch.runnerUp)].name + " " + CostText(ch.runnerUpTotal) : "";
        log::Event("adapter", trigger + ": video GPU " + (pa.luid.empty() ? std::string("none") : wvp::ToUtf8(pa.name) + " (luid " + pa.luid + ")") +
                                  ", display adapter " + (displayLuid_.empty() ? std::string("unknown") : displayLuid_) + ": " + ch.reason + runner);
        if (const wvp::Plan* pl = svc_->filters.CurrentPlan()) log::Event("pipeline", "planner active: " + pl->Summary());
        else log::Event("pipeline", "planner found no valid pipeline: " + svc_->filters.PlanError(), log::kWarning);
    }
    if (change) *change = fc;
    return changed;
}

void App::OnVideoDeviceLost(const std::string& why) {
    log::Event("gpu", "video device lost: " + why, log::kWarning);
    // Failures caused by the lost device are not properties of the new one.
    pipe_.Clear();
    svc_->filters.ClearBlacklist();
    SelectVideoGpuNow("device lost", false);
    const auto now = Now();
    if (Loaded() && (lastRestoreMs_ == 0 || now - lastRestoreMs_ > 250)) {
        lastRestoreMs_ = now;
        // posted: stopping the old media blocks, and a UI rebind redraws first so the window never waits for the video;
        // the presenter makes a new device in the open
        PostToUi([this] { if (Loaded()) ctl_->ReopenCurrentHere(); });
    }
    std::vector<std::string> luids;
    if (platform_)
        for (const wvp::AdapterEntry& e : platform_->Adapters()) luids.push_back(e.luid);
    if (luids != probedLuids_) StartAdapterProbe("adapter set changed after a device loss");
}

void App::OnPresenterError(const wvp::PresenterErrorInfo& e) {
    if (e.DeviceLost) { OnVideoDeviceLost("presenter: " + e.Error + " " + e.RemovedReason); return; }
    log::Event("presenter", e.Presenter + " failed: " + e.Error, log::kWarning);
    const auto now = Now();
    if (!Loaded() || (presenterReopenMs_ != 0 && now - presenterReopenMs_ < 2000)) return;   // failing again at once: stay without video
    presenterReopenMs_ = now;
    PostToUi([this] { if (Loaded()) ctl_->ReopenCurrentHere(); });
}

void App::OnUiRebound(const std::string& reason, bool adapterChanged) {
    (void)adapterChanged;
    // The UI device and the video device are separate, but a TDR or a driver change removes every device of the adapter: the
    // presenter is reopened on a fresh device right away, in parallel with the UI redraw that follows.
    OnVideoDeviceLost("UI device " + reason);
}

std::string App::DisplayGpuLuid() const {
    const std::string raw = platform_ ? platform_->DisplayAdapter(hwnd_) : std::string();
    const std::string c = wvp::CanonicalLuid(physGpus_, raw);
    return c.empty() ? raw : c;
}

void App::OnWindowDragEnd() {
    if (!svc_->settings.videoGpu.Automatic() && FindPreferred(adapters_, svc_->settings.videoGpu) >= 0) return;
    if (adapters_.size() < 2 || DisplayGpuLuid() == displayLuid_) return;
    if (SelectVideoGpuNow("drag end", true) && Loaded()) ctl_->ReopenCurrentHere();
}

void App::OnDisplayChange() {
    std::vector<std::string> luids;
    if (platform_)
        for (const wvp::AdapterEntry& e : platform_->Adapters()) luids.push_back(e.luid);
    if (luids != probedLuids_) {
        StartAdapterProbe("adapter set changed");
        return;
    }
    if (adapters_.size() >= 2 && DisplayGpuLuid() != displayLuid_ && SelectVideoGpuNow("display change", true) && Loaded())
        ctl_->ReopenCurrentHere();
}


std::vector<wvp::VideoGpuPref> App::VideoGpus() const {
    std::vector<wvp::VideoGpuPref> v;
    for (const auto& a : adapters_) v.push_back({ a.luid, a.name, a.vendorId, a.deviceId });
    return v;
}

void App::VideoGpuSettingChanged() {
    manualMissingNoticed_ = false;
    const std::string before = playAdapter_.luid;
    wvp::FilterChange fc;
    bool switched = false;
    SelectVideoGpuNow("setting", false, &fc, &switched);
    if (!Loaded()) return;
    if (switched || fc.reopen || before != playAdapter_.luid) ctl_->ReopenCurrentHere();
    else SendFilterChange(*this, fc);
}

}
