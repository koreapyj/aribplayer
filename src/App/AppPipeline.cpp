// The decisions live in wvp::PipelineSession / wvp::FilterManager (unit-tested); this file only connects them to the app.
#include "App.h"

#include <cstdio>

#include "features/FiltersPanel.h"
#include "features/Logging.h"
#include "features/SettingsService.h"
#include "wvp/AppSettings.h"
#include "wvp/Utf.h"

namespace app {

namespace {
std::string Join(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& e : v) s += (s.empty() ? "" : ", ") + e;
    return s.empty() ? "-" : s;
}
}

std::vector<std::pair<std::wstring, std::wstring>> LibffplayDeviceOptions(const std::wstring& adapterLuid, const AppOptions& o) {
    std::vector<std::pair<std::wstring, std::wstring>> v;
    if (!adapterLuid.empty()) v.emplace_back(L"d3d11_adapter_luid", adapterLuid);
    if (o.d3d11Debug) v.emplace_back(L"d3d11_debug", L"1");
    if (!o.d3d11ForceCaps.empty()) v.emplace_back(L"d3d11_force_caps", o.d3d11ForceCaps);
    if (!o.d3d11Route.empty()) v.emplace_back(L"d3d11_route", o.d3d11Route);
    return v;
}

std::wstring ProbeOptions(const std::wstring& adapterLuid, const AppOptions& o) {
    std::wstring s;
    for (const auto& [k, v] : LibffplayDeviceOptions(adapterLuid, o))
        if (k != L"d3d11_route") s += (s.empty() ? L"" : L"\n") + k + L"=" + v;
    return s;
}

wvp::CapabilitySet WithoutCuda(wvp::CapabilitySet caps) {
    for (auto& d : caps.devices)
        if (d.type == "cuda") { d.ok = false; d.error = "disabled by --no-cuda"; }
    std::erase_if(caps.decoders, [](const wvp::DecoderCap& d) { return d.hwaccel == "cuda"; });
    for (auto& p : caps.presenters) { p.importCuda = false; p.importCudaZeroCopy = false; }
    return caps;
}

void LogCaps(const wvp::CapabilitySet& c) {
    const wvp::CapabilitySet* caps = &c;
    {
        std::vector<std::string> ok, bad, dec, flt, hwf, io, pr;
        for (const auto& d : caps->devices) (d.ok ? ok : bad).push_back(d.type);
        int decOk = 0;
        for (const auto& d : caps->decoders) if (d.ok) decOk++;
        for (const auto& f : caps->filters) {
            auto dom = wvp::DomainFromName(f.pixFmts.empty() ? "" : f.pixFmts.front());
            if (dom && wvp::IsHardware(*dom)) hwf.push_back(f.name);
        }
        for (const auto& i : caps->interop)
            io.push_back(std::string(wvp::DomainName(i.from)) + ">" + wvp::DomainName(i.to) + (i.method.empty() ? "" : " " + i.method) + (i.ok ? " ok" : " FAILED"));
        for (const auto& p : caps->presenters) pr.push_back(p.name + (p.ok ? " ok" : " FAILED"));
        log::Event("caps", "source: " + caps->source + ", ffmpeg " + caps->ffmpegVersion +
                               ", adapter: " + caps->adapter.name + ", features: " + Join(caps->features));
        log::Event("caps", "devices ok: " + Join(ok) + "; failed: " + Join(bad));
        log::Event("caps", "decoders: " + std::to_string(decOk) + " usable of " + std::to_string(caps->decoders.size()));
        log::Event("caps", "filters: " + std::to_string(caps->filters.size()) + " (hardware: " + Join(hwf) + ")");
        log::Event("caps", "interop: " + Join(io));
        log::Event("caps", "presenters: " + Join(pr));
    }
}

void App::LogOpenPlan() {
    const wvp::Plan* pl = svc_->filters.CurrentPlan();
    if (!pl) return;
    pipeOpenMs_ = Now();
    pipePollNextMs_ = pipeOpenMs_ + 1000;
    log::Event("pipeline", pl->Summary());
    for (const auto& r : pl->rejected) log::Event("pipeline", "rejected: " + r.summary + " (" + r.reason + ")", log::kVerbose);
}

void App::WirePipeline() {
    ctl_->beforeOpen = [this](const std::wstring& url) {
        // Resolve the Windows caption style and font on every open, before the final options are logged/passed to libffplay.
        ApplyCaptionOpenOptions(*ctl_, svc_->settings);
        // plan with what we know about this file (cached from an earlier open) before the options are built
        wvp::ContentInfo c = pipe_.BeforeOpen(url);
        svc_->filters.SetContent(c);
        SelectVideoGpuNow("open", false);
    };
    player_->Prepared.add([this](const wvp::MediaPreparedInfo&) { OnPipelinePrepared(); });
    player_->Error.add([this](const wvp::PlayerErrorInfo& e) {
        if (e.DeviceLost) OnVideoDeviceLost("decoder: " + wvp::ToUtf8(e.Message) + " " + e.RemovedReason);
    });
    player_->FilterError.add([this](const wvp::FilterErrorInfo& e) {
        const wvp::Plan* pl = svc_->filters.CurrentPlan();
        if (!pl) return;
        if (auto fb = pipe_.OnFilterError(*pl, e)) PipelineFallback(*fb);
    });
    player_->PresenterError.add([this](const wvp::PresenterErrorInfo& e) {
        if (e.DeviceLost) { OnVideoDeviceLost("presenter: " + wvp::ToUtf8(e.Error) + " " + e.RemovedReason); return; }
        const wvp::Plan* pl = svc_->filters.CurrentPlan();
        if (!pl) return;
        if (auto fb = pipe_.OnPresenterError(*pl, e)) PipelineFallback(*fb);   // libffplay is already on SDL: replan without d3d11, reopen
    });
    ctl_->onPresenterParamsRejected = [this](int err, const wvp::PresenterParams& rejected) {
        // the presenter cannot apply what the plan absorbed: move that operation into the graph and replan
        log::Event("pipeline", "presenter refused its parameters (error " + std::to_string(err) + "): not absorbing them", log::kWarning);
        wvp::FilterChange ch = svc_->filters.PresenterParamsRejected(rejected);
        if (const wvp::Plan* pl = svc_->filters.CurrentPlan()) log::Event("pipeline", "replanned: " + pl->Summary());
        SendFilterChange(*this, ch);
    };
}

void App::OnPipelinePrepared() {
    if (!player_ || !ctl_) return;
    auto mi = player_->GetMediaInfo();
    if (!mi) return;
    const std::wstring url = lastOpenUrl_;
    wvp::PipelineSession::Prepared pr = pipe_.OnPrepared(url, *mi);
    if (!pr.content.Known() || !svc_->filters.PlannerActive()) {
        if (pr.content.Known()) svc_->filters.SetContent(pr.content);   // remembered for when capabilities arrive
        return;
    }
    log::Event("pipeline", "content: " + pr.content.codec + " " + std::to_string(pr.content.width) + "x" + std::to_string(pr.content.height) + " " +
                               std::to_string(pr.content.bitDepth) + "-bit" + (pr.changed ? " (changed since the plan was made)" : ""));
    wvp::FilterChange ch = svc_->filters.SetContent(pr.content);
    if (ch.reopen) {
        if (!pipe_.ClaimContentReopen(url)) return;   // once per url chain: never loop
        if (const wvp::Plan* pl = svc_->filters.CurrentPlan()) log::Event("pipeline", "replanned for the content: " + pl->Summary() + " -> reopen");
        int gen = pipe_.Generation();
        PostToUi([this, gen] {
            if (ctl_ && pipe_.Generation() == gen && Loaded()) ctl_->ReopenCurrentHere();
        });
    } else {
        SendFilterChange(*this, ch);   // graph rebuild / eq commands only
    }
}

void App::PipelinePoll(uint64_t now) {
    if (!player_ || state_ != wvp::PlayerState::Playing || !svc_->filters.PlannerActive()) return;
    if (now < pipePollNextMs_ || now > pipeOpenMs_ + 10000) return;
    pipePollNextMs_ = now + 1000;
    auto j = player_->GetDebugInfoJson();
    if (!j) return;
    wvp::DebugInfo d = wvp::ParseDebugInfo(*j);
    if (auto fb = pipe_.OnDebugInfo(*svc_->filters.CurrentPlan(), d)) PipelineFallback(*fb);
}

void App::PipelineFallback(const wvp::PipelineSession::Fallback& fb) {
    log::Event("pipeline", "fallback: " + fb.key + " (" + fb.reason + ")", log::kWarning);
    wvp::FilterChange ch = svc_->filters.Blacklist(fb.key);
    if (const wvp::Plan* pl = svc_->filters.CurrentPlan()) log::Event("pipeline", "replanned: " + pl->Summary());
    if (Loaded() && (ch.reopen || ch.videoRebuild)) {
        int gen = pipe_.Generation();
        PostToUi([this, gen] {
            if (ctl_ && pipe_.Generation() == gen && Loaded()) ctl_->ReopenCurrentHere();
        });
    }
    if (debugVisible_) RefreshDebugOverlay();
}

}
