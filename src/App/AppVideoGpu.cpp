// Video GPU glue: per-adapter capability probes in child processes, the display adapter, the selection triggers (startup, open,
// drag end, display change, device loss) and playback restore after a device loss. The decision is wvp::SelectVideoGpu.
#include "App.h"
#include "Localize.h"

#include <atomic>
#include <cstdio>

#include "features/FiltersPanel.h"
#include "features/Logging.h"
#include "features/SettingsService.h"
#include "wvp/AppSettings.h"
#include "wvp/FfPlayer.h"
#include "wvp/Utf.h"

namespace app {

namespace {

std::atomic<int> g_probeSeq{0};

// Runs "<this exe> --probe-gpu <temp file> [device switches]" (no window) and reads its JSON line, the GPU capability probe.
// false (nothing read) on any failure or after 30 s.
bool ProbeGpuInChild(const std::wstring& switches, std::string& capsJson) {
    wchar_t exe[MAX_PATH * 2], tmp[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, (DWORD)std::size(exe)) || !GetTempPathW((DWORD)std::size(tmp), tmp)) return false;
    const std::wstring out = std::wstring(tmp) + L"aribplayer-gpu-probe-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                             std::to_wstring(g_probeSeq++) + L".txt";
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --probe-gpu \"" + out + L"\"" + switches;
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, 30000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);   // our own probe child, hung in a driver
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    std::string text;
    if (code == 0) {
        if (FILE* f = _wfopen(out.c_str(), L"rb")) {
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
            fclose(f);
        }
    }
    DeleteFileW(out.c_str());
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    capsJson = std::move(text);
    return !capsJson.empty();
}

std::string CostText(const wvp::PlanCost& c) {
    return std::to_string(c.Weighted()) + " host, " + std::to_string(c.deviceCopies) + " device";
}

}

std::wstring ProbeSwitches(const std::wstring& adapterLuid, const AppOptions& o) {
    std::wstring s;
    if (!adapterLuid.empty()) s += L" --adapter-luid=" + adapterLuid;
    if (o.d3d11Debug) s += L" --d3d11-debug";
    if (!o.d3d11ForceCaps.empty()) s += L" \"--d3d11-force-caps=" + o.d3d11ForceCaps + L"\"";
    return s;
}

std::vector<wvp::AdapterCandidate> ProbeAdapters(const std::vector<HwAdapter>& adapters, const AppOptions& o) {
    struct Answer { std::string caps; bool ok = false; };
    std::vector<Answer> answers(adapters.size());
    std::vector<std::thread> threads;
    for (size_t i = 0; i < adapters.size(); i++)
        threads.emplace_back([&, i] {
            const std::wstring luid(adapters[i].luid.begin(), adapters[i].luid.end());
            answers[i].ok = ProbeGpuInChild(ProbeSwitches(luid, o), answers[i].caps);
        });
    for (auto& t : threads) t.join();
    const wvp::CapabilitySet build = wvp::BuildCapabilities();
    std::vector<wvp::AdapterCandidate> out;
    for (size_t i = 0; i < adapters.size(); i++) {
        const Answer& a = answers[i];
        if (!a.ok || a.caps.empty()) continue;
        std::optional<wvp::CapabilitySet> rt = wvp::ParseCapabilities(a.caps);
        if (!rt || !rt->DeviceOk("d3d11va")) continue;   // no D3D11 video on this adapter: not a video GPU
        wvp::AdapterCandidate c;
        c.luid = adapters[i].luid;
        c.name = wvp::ToUtf8(adapters[i].name);
        c.vendorId = static_cast<int>(adapters[i].vendorId);
        c.deviceId = static_cast<int>(adapters[i].deviceId);
        c.caps = wvp::MergeBuildCapabilities(*rt, build);
        out.push_back(std::move(c));
    }
    return out;
}

void App::OnAdaptersProbed(std::vector<wvp::AdapterCandidate> candidates, const std::string& why) {
    capsProbed_ = true;
    adapterProbeBusy_ = false;
    for (const auto& c : candidates) {
        log::Event("caps", "video GPU candidate " + c.name + " (luid " + c.luid + ")");
        LogCaps(c.caps);
    }
    if (candidates.empty()) log::Event("caps", "unavailable");
    adapters_ = std::move(candidates);
    if (!opt_.plannerOff) {
        const std::string before = playAdapter_.luid;
        wvp::FilterChange fc;
        SelectVideoGpuNow(why, false, &fc);
        // At startup an open may already run on libffplay's default adapter: reopen only when the plan needs it.
        if (Loaded()) {
            if (fc.reopen || (why != "startup" && before != playAdapter_.luid)) ctl_->ReopenCurrentHere();
            else SendFilterChange(*this, fc);
        }
    }
    if (!adapterProbePending_.empty()) {
        std::string next = std::move(adapterProbePending_);
        adapterProbePending_.clear();
        StartAdapterProbe(next);
    }
    if (debugVisible_) RefreshDebugOverlay();
}

std::vector<HwAdapter> App::PhysicalAdapters(const std::string& why) {
    std::vector<HwAdapter> all = EnumHardwareAdapters();
    std::vector<wvp::AdapterEntry> entries;
    probedLuids_.clear();
    for (const HwAdapter& h : all) {
        probedLuids_.push_back(h.luid);
        wvp::AdapterEntry e;
        e.luid = h.luid; e.name = wvp::ToUtf8(h.name); e.vendorId = (int)h.vendorId; e.deviceId = (int)h.deviceId;
        e.hasPci = h.hasPci; e.bus = (int)h.bus; e.device = (int)h.device; e.function = (int)h.function; e.hasOutputs = h.hasOutputs;
        entries.push_back(std::move(e));
    }
    physGpus_ = wvp::GroupPhysicalGpus(entries);
    std::vector<HwAdapter> out;
    for (const HwAdapter& h : all) {
        char pci[32] = "none";
        if (h.hasPci) snprintf(pci, sizeof pci, "%u:%u.%u", h.bus, h.device, h.function);
        const std::string canon = wvp::CanonicalLuid(physGpus_, h.luid);
        log::Event("adapter", why + ": " + wvp::ToUtf8(h.name) + " luid " + h.luid + ", pci " + pci + ", outputs " +
                                  (h.hasOutputs ? "yes" : "no") + " -> " +
                                  (canon.empty() ? std::string("not a candidate") : canon == h.luid ? std::string("GPU") : "alias of " + canon));
        if (canon == h.luid) out.push_back(h);
    }
    return out;
}

void App::StartAdapterProbe(const std::string& why) {
    if (opt_.plannerOff) return;
    if (adapterProbeBusy_) { adapterProbePending_ = why; return; }
    if (adapterProbeThread_.joinable()) adapterProbeThread_.join();
    adapterProbeBusy_ = true;
    std::vector<HwAdapter> hw = PhysicalAdapters(why);
    log::Event("caps", "re-probing " + std::to_string(hw.size()) + " adapter(s): " + why);
    adapterProbeThread_ = std::thread([hw, o = opt_, why] {
        std::vector<wvp::AdapterCandidate> cands = ProbeAdapters(hw, o);
        if (App* a = App::Instance()) a->PostToUi([a, cands = std::move(cands), why]() mutable { a->OnAdaptersProbed(std::move(cands), why); });
    });
}

bool App::SelectVideoGpuNow(const std::string& trigger, bool onlyIfCheaper, wvp::FilterChange* change) {
    if (opt_.plannerOff || adapters_.empty()) return false;
    displayRawLuid_ = DisplayAdapterLuid(hwnd_);
    displayLuid_ = wvp::CanonicalLuid(physGpus_, displayRawLuid_);
    if (displayLuid_.empty()) displayLuid_ = displayRawLuid_;
    std::vector<wvp::AdapterCandidate> cands = adapters_;
    if (opt_.noCuda) for (auto& c : cands) c.caps = WithoutCuda(c.caps);
    const wvp::AppSettings& s = svc_->settings;
    const wvp::PlanPrefs prefs = wvp::PlanPrefsFor(s.hwDecoder, s.presenter);
    wvp::PlanRequest req = svc_->filters.Request();
    req.decoder = prefs.decoder;
    req.forcedDecoder = prefs.forcedDecoder;
    req.presenter = prefs.presenter;
    wvp::VideoGpuPref pref = s.videoGpu;   // a manual choice stored as an alias means its GPU
    if (!pref.Automatic())
        if (std::string c = wvp::CanonicalLuid(physGpus_, pref.luid); !c.empty()) pref.luid = c;
    wvp::VideoGpuChoice ch = wvp::SelectVideoGpu(cands, displayLuid_, pref, req);
    if (videoSoftwareFallback_) {
        ch = {};
        ch.reason = "graphics fallback after repeated device loss -> software decoding + SDL";
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
        pp.presenter = wvp::PresenterPref::Sdl;
    }
    playAdapter_ = pa;
    svc_->filters.SetPlanPreferences(pp.decoder, pp.forcedDecoder, pp.presenter);
    wvp::FilterChange fc = svc_->filters.SetCapabilities(cands[static_cast<size_t>(ch.index >= 0 ? ch.index : 0)].caps);
    if (ctl_ && !pa.luid.empty())
        for (const auto& [k, v] : LibffplayDeviceOptions(std::wstring(pa.luid.begin(), pa.luid.end()), opt_)) ctl_->SetBaseOption(k, v);
    if (changed || trigger != "open") {
        std::string runner = ch.runnerUp >= 0 ? "; runner-up " + cands[static_cast<size_t>(ch.runnerUp)].name + " " + CostText(ch.runnerUpTotal) : "";
        log::Event("adapter", trigger + ": video GPU " + (pa.luid.empty() ? std::string("none") : wvp::ToUtf8(pa.name) + " (luid " + pa.luid + ")") +
                                  ", display adapter " + (displayLuid_.empty() ? std::string("unknown") : displayLuid_) + ": " + ch.reason + runner);
        if (const wvp::Plan* pl = svc_->filters.CurrentPlan()) log::Event("pipeline", "planner active: " + pl->Summary());
        else log::Event("pipeline", "planner found no valid pipeline: staying with the legacy options", log::kWarning);
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
        // posted: stopping the old media blocks, and a UI rebind redraws first so the window never waits for the video
        PostToUi([this] { if (Loaded()) ctl_->ReopenCurrentHere(); });
    }
    std::vector<HwAdapter> hw = EnumHardwareAdapters();
    std::vector<std::string> luids;
    for (const HwAdapter& h : hw) luids.push_back(h.luid);
    if (luids != probedLuids_) StartAdapterProbe("adapter set changed after a device loss");
}

void App::OnUiRebound(const std::string& reason, bool adapterChanged) {
    (void)adapterChanged;
    // The UI device and the video device are separate, but a TDR or a driver change removes every device of the adapter: the
    // presenter is reopened on a fresh device right away, in parallel with the UI redraw that follows.
    OnVideoDeviceLost("UI device " + reason);
}

std::string App::DisplayGpuLuid() const {
    const std::string raw = DisplayAdapterLuid(hwnd_);
    const std::string c = wvp::CanonicalLuid(physGpus_, raw);
    return c.empty() ? raw : c;
}

void App::OnWindowDragEnd() {
    if (!svc_->settings.videoGpu.Automatic() && FindPreferred(adapters_, svc_->settings.videoGpu) >= 0) return;
    if (adapters_.size() < 2 || DisplayGpuLuid() == displayLuid_) return;
    if (SelectVideoGpuNow("drag end", true) && Loaded()) ctl_->ReopenCurrentHere();
}

void App::OnDisplayChange() {
    std::vector<HwAdapter> hw = EnumHardwareAdapters();
    std::vector<std::string> luids;
    for (const HwAdapter& h : hw) luids.push_back(h.luid);
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
    SelectVideoGpuNow("setting", false, &fc);
    if (!Loaded()) return;
    if (fc.reopen || before != playAdapter_.luid) ctl_->ReopenCurrentHere();
    else SendFilterChange(*this, fc);
}

}
