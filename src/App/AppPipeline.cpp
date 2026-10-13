// The decisions live in wvp::PipelineSession / wvp::FilterManager (unit-tested); this file only connects them to the app.
#include "App.h"

#include <cstdio>

#include "features/FiltersPanel.h"
#include "features/Logging.h"
#include "features/SettingsService.h"
#include "wvp/AppSettings.h"
#include "wvp/win32/Utf.h"

namespace app {

namespace {
std::string Join(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& e : v) s += (s.empty() ? "" : ", ") + e;
    return s.empty() ? "-" : s;
}
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
    ctl_->beforeOpen = [this](const std::string& url) {
        // Resolve the Windows caption style and font on every open, before the final options are logged/passed to libffplay.
        ApplyCaptionOpenOptions(*ctl_, svc_->settings);
        // plan with what we know about this file (cached from an earlier open) before the options are built
        wvp::ContentInfo c = pipe_.BeforeOpen(url);
        svc_->filters.SetContent(c);
        RefreshFiltersPanel(*this);
        SelectVideoGpuNow("open", false);
        ReportPlanProblem();
    };
    player_->Prepared.add([this](const wvp::MediaPreparedInfo&) { OnPipelinePrepared(); });
    player_->MediaInfoChanged.add([this] { ApplyPipelineContent(); });
    player_->Error.add([this](const wvp::PlayerErrorInfo& e) {
        if (e.DeviceLost) OnVideoDeviceLost("decoder: " + e.Message + " " + e.RemovedReason);
    });
    player_->FilterError.add([this](const wvp::FilterErrorInfo& e) {
        const wvp::Plan* pl = svc_->filters.CurrentPlan();
        if (!pl) return;
        if (auto fb = pipe_.OnFilterError(*pl, e)) PipelineFallback(*fb);
    });
    player_->PresenterError.add([this](const wvp::PresenterErrorInfo& e) { OnPresenterError(e); });
}

void App::OnPipelinePrepared() {
    if (!player_ || !ctl_) return;
    if (!firstPreparedMs_) {
        firstPreparedMs_ = Now() - startMs_;
        log::Event("open", "first media prepared " + std::to_string(firstPreparedMs_) + " ms after start");
    }
    ApplyPipelineContent();
}

void App::ApplyPipelineContent() {
    if (!player_ || !ctl_) return;
    auto mi = player_->GetMediaInfo();
    if (!mi) return;
    const std::string url = lastOpenUrl_;
    wvp::PipelineSession::Prepared pr = pipe_.OnPrepared(url, *mi);
    if (!pr.content.Known() || !svc_->filters.PlannerActive()) {
        if (pr.content.Known()) {
            svc_->filters.SetContent(pr.content);   // remembered for when capabilities arrive
            RefreshFiltersPanel(*this);
        }
        return;
    }
    log::Event("pipeline", "content: " + pr.content.codec + " " + std::to_string(pr.content.width) + "x" + std::to_string(pr.content.height) + " " +
                               std::to_string(pr.content.bitDepth) + "-bit" + (pr.content.progressive ? " progressive" : "") + (pr.changed ? " (changed since the plan was made)" : ""));
    wvp::FilterChange ch = svc_->filters.SetContent(pr.content);
    RefreshFiltersPanel(*this);
    if (ch.reopen) {
        if (!pipe_.ClaimContentReopen(url)) return;   // once per url chain: never loop
        if (const wvp::Plan* pl = svc_->filters.CurrentPlan()) log::Event("pipeline", "replanned for the content: " + pl->Summary() + " -> reopen");
        int gen = pipe_.Generation();
        PostToUi([this, gen] {
            if (ctl_ && pipe_.Generation() == gen && Loaded()) ctl_->ReopenCurrentHere();
        });
    } else {
        SendFilterChange(*this, ch);   // graph rebuild only
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
