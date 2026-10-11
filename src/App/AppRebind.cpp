// UI side of "rebind to a GPU adapter": loss detection, the RebindController glue and the UI rebuild.
// Decisions (debounce, attempt budget, fallback, one notice per incident) live in RebindLogic.h; this file performs them.
#include <cstdio>
#include <cstring>

#include "App.h"
#include "Localize.h"
#include "features/Logging.h"
#include "shell/SettingsWindow.h"
#include "wvp/win32/Utf.h"

namespace app {

namespace {
double QpcUs() {
    static const double f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return (double)q.QuadPart; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return c.QuadPart * 1e6 / f;
}

RebindReason ReasonFor(HRESULT hr, HRESULT removed) {
    if (FAILED(removed) || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR)
        return RebindReason::DeviceRemoved;
    if (hr == DXGI_ERROR_DEVICE_RESET) return RebindReason::DeviceReset;
    return RebindReason::RecreateTarget;
}
}

void App::ArmGfx() {
    gfx_.onFailure = [this](const char* where, HRESULT hr) { OnGfxFailure(where, hr); };
    const unsigned gen = ++gfxGen_;
    gfx_.StartRemovalWatch([gen] {
        const double t = QpcUs();   // thread-pool thread: only timestamp and post
        if (App* a = App::Instance())
            a->PostToUi([a, gen, t] { if (gen == a->gfxGen_) a->ReportGpuLoss(RebindReason::DeviceRemoved, "removal_event", t); });
    });
}

void App::OnGfxFailure(const char* where, HRESULT hr) {
    if (destroyed_) return;
    const bool injected = std::strstr(where, "(injected)") != nullptr;
    const HRESULT removed = injected ? S_OK : gfx_.DeviceRemovedReason();
    if (!Gfx::IsDeviceLoss(hr) && SUCCEEDED(removed)) return;
    if (rebuilding_) { rebuildFailed_ = true; return; }
    ReportGpuLoss(injected ? injectedReason_ : ReasonFor(hr, removed), where);
}

void App::ReportGpuLoss(RebindReason reason, const char* where, double detectedUs) {
    if (destroyed_) return;
    const double t = detectedUs > 0 ? detectedUs : QpcUs();
    if (rebind_.ReportLoss(reason)) {
        rebindTiming_ = {};
        rebindTiming_.detectedUs = t;
        rebindTiming_.reason = RebindReasonName(reason);
        rebindTiming_.where = where;
        log::Event("gpu", std::string("device loss: ") + RebindReasonName(reason) + " at " + where, log::kWarning);
    }
    if (rebindPosted_ || !rebind_.Pending()) return;
    rebindPosted_ = true;
    PostToUi([this] { rebindPosted_ = false; RunRebind(); });   // never inside the draw / commit that reported it
}

void App::SimulateUiDeviceLoss(RebindReason reason) {
    if (!gfxOk_) return;
    injectedReason_ = reason;
    gfx_.InjectFailure(DXGI_ERROR_DEVICE_REMOVED);
    layersDirtyAll_ = true;
    Flush();
    if (!rebind_.Busy()) gfx_.Commit();   // Flush drew nothing (e.g. minimized): the commit consumes the injection
}

void App::ResetRebindFallback() {
    rebind_.ResetFallback();
    ReportGpuLoss(RebindReason::AdapterChange, "reset_fallback");
}

void App::RunRebind() {
    if (destroyed_) return;
    for (RebindAction act; (act = rebind_.Next(Now())) != RebindAction::None;) {
        const bool fallback = act == RebindAction::Fallback;
        const bool ok = RebindUi(rebind_.Reason(), fallback);
        rebind_.OnRebindResult(ok, Now());
        if (!ok) log::Event("gpu", fallback ? "UI fallback rebuild failed" : "UI rebuild failed", log::kError);
    }
    RebindNotice n;
    if (rebind_.TakeNotice(n) && shell_) {
        if (n.kind == RebindNotice::Kind::Recovered) ShowToast(wvp::ToWide(n.text));
        else ShowInfoBar(n.kind == RebindNotice::Kind::Failed ? InfoSeverity::Error : InfoSeverity::Warning, Tr(L"Graphics", L"그래픽"), wvp::ToWide(n.text));
        log::Event("gpu", n.text);
    }
}

bool App::RebindUi(RebindReason reason, bool forceWarp) {
    rebindTiming_.startUs = QpcUs();
    rebindTiming_.fallback = forceWarp;
    rebuilding_ = true;
    rebuildFailed_ = false;

    // Device children first, then the devices: layers (visual + surface) -> renderer's dc-bound brush -> DComp -> D2D -> D3D.
    // The Settings window's target / visuals / surfaces live on the same devices.
    if (settingsWin_) settingsWin_->ReleaseDevice();
    for (Layer l : kLayerStack) LayerSurf(l) = {};
    d2d_.ReleaseDevice();
    gfx_.Shutdown();

    const std::string oldLuid = gfx_.adapterLuid;
    HRESULT hr = gfx_.Init(hwnd_, [](const char* n, HRESULT r) {
        if (FAILED(r)) log::Event("gpu", std::string("rebuild step failed: ") + n, log::kError);
    }, forceWarp);
    if (SUCCEEDED(hr))
        for (Layer l : kLayerStack)
            if (SUCCEEDED(hr)) hr = gfx_.CreateVisual(LayerSurf(l));
    gfxOk_ = SUCCEEDED(hr) && LayerStackMatches();
    rebindTiming_.deviceReadyUs = QpcUs();
    if (!gfxOk_) {
        rebuilding_ = false;
        return false;
    }
    ArmGfx();
    if (settingsWin_ && settingsWin_->IsOpen() && FAILED(settingsWin_->AttachDevice())) {   // its own target for its HWND
        log::Event("gpu", "rebuild step failed: settings window target", log::kError);
        rebuildFailed_ = true;
    }

    // Playback restore can start now; the hook returns without waiting for the open.
    videoSoftwareFallback_ = rebind_.FallbackActive() || forceWarp;
    if (videoSoftwareFallback_) playAdapter_ = PlaybackAdapter{{}, {}, "graphics fallback after repeated device loss -> software decoding on the default adapter"};
    log::Event("adapter", "UI rebound (" + std::string(RebindReasonName(reason)) + "): " + wvp::ToUtf8(gfx_.adapterName) + " luid " +
                              gfx_.adapterLuid);
    rebindTiming_.hookUs = QpcUs();
    OnUiRebound(RebindReasonName(reason), gfx_.adapterLuid != oldLuid);

    // Every layer starts empty: forget the paint hashes and re-apply the per-visual state the layers keep in DComp.
    hContent_ = hPanel_ = hTitle_ = hBar_ = hPopup_ = hDebug_ = 0;
    popupWasVisible_ = popupAcrylicWasActive_ = false;
    layersDirtyAll_ = true;
    if (shell_ && !minimized_) {
        SnapBar();
        Flush();
    }
    if (settingsWin_) settingsWin_->Render(true);
    const HRESULT chr = gfx_.Commit();
    rebindTiming_.uiVisibleUs = QpcUs();
    rebuilding_ = false;
    const bool ok = SUCCEEDED(chr) && SUCCEEDED(gfx_.DeviceRemovedReason()) && !rebuildFailed_;
    if (ok) rebuilds_++;
    log::Event("gpu", std::string("UI rebuilt: ") + (ok ? "ok" : "FAILED") + ", device " +
                          std::to_string((rebindTiming_.deviceReadyUs - rebindTiming_.startUs) / 1000.0) + " ms, ui visible " +
                          std::to_string(rebindTiming_.DeviceToUiMs()) + " ms after the device, " +
                          std::to_string(rebindTiming_.DetectToUiMs()) + " ms after detection");
    return ok;
}

}
