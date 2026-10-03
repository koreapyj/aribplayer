#include <d2d1helper.h>

#include <algorithm>
#include <cmath>

#include "App.h"
#include "features/MiniLogic.h"
#include "features/PlaybackExtras.h"
#include "pui/ToolTipStyle.h"

namespace app {

using pui::RectF;

namespace {
bool AcrylicTransparencyEffectsEnabled() {
    DWORD value = 1, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"EnableTransparency",
                     RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS)
        return value != 0;
    return true;  // absent on older Windows builds means the system default (enabled)
}

bool AcrylicBatterySaverEnabled() {
    SYSTEM_POWER_STATUS s{};
    return GetSystemPowerStatus(&s) && s.SystemStatusFlag != 0;
}

bool AcrylicRemoteSession() { return GetSystemMetrics(SM_REMOTESESSION) != 0; }

bool AcrylicHighContrastEnabled() {
    HIGHCONTRASTW hc{};
    hc.cbSize = sizeof(hc);
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

pui::Color AcrylicHighContrastWindowColor() {
    const COLORREF c = GetSysColor(COLOR_WINDOW);
    return pui::Color(0xFF000000u | (uint32_t(GetRValue(c)) << 16) | (uint32_t(GetGValue(c)) << 8) | uint32_t(GetBValue(c)));
}

class NoticeWidget final : public pui::Widget {
public:
    NoticeWidget(InfoSeverity sev, std::wstring title, std::wstring message, bool toast, std::function<void(NoticeWidget*)> dismiss)
        : pui::Widget(pui::Role::StatusBar), sev_(sev), title_(std::move(title)), message_(std::move(message)), toast_(toast),
          dismiss_(std::move(dismiss)) {
        SetName(toast ? message_ : title_ + L". " + message_);
    }
    void Paint(pui::IRenderer& r, const pui::Theme& t) override {
        pui::Color bg = t.flyoutSurface, accent = t.accent;
        if (!toast_) {
            switch (sev_) {
            case InfoSeverity::Error: bg = t.criticalBg; accent = t.critical; break;
            case InfoSeverity::Warning: bg = t.cautionBg; accent = t.caution; break;
            case InfoSeverity::Success: bg = t.successBg; accent = t.success; break;
            default: bg = t.flyoutSurface; break;
            }
        }
        r.FillRoundRect(Bounds(), t.overlayRadius, bg);
        r.StrokeRoundRect(Bounds(), t.overlayRadius, t.flyoutStroke, 1);
        if (toast_) {
            pui::TextStyle ts;
            ts.size = 14;
            ts.align = pui::TextAlign::Center;
            r.Text(message_, Bounds().Deflate(16, 0, 16, 0), ts, t.textPrimary);
            return;
        }
        r.FillRoundRect(RectF::XYWH(Bounds().l + 8, Bounds().t + 14, 4, Bounds().h() - 28), 2, accent);
        pui::TextStyle a;
        a.size = 14;
        a.weight = 600;
        pui::TextStyle b;
        b.size = 14;
        r.Text(title_, RectF{Bounds().l + 24, Bounds().t + 8, Bounds().r - 16, Bounds().t + 32}, a, t.textPrimary);
        r.Text(message_, RectF{Bounds().l + 24, Bounds().t + 32, Bounds().r - 16, Bounds().b - 8}, b, t.textSecondary);
    }
    bool OnPointerDown(const pui::PointerEvent&) override { return true; }
    bool OnPointerUp(const pui::PointerEvent& e) override {
        if (e.button == pui::PointerButton::Left && dismiss_) dismiss_(this);
        return true;
    }
private:
    InfoSeverity sev_;
    std::wstring title_, message_;
    bool toast_;
    std::function<void(NoticeWidget*)> dismiss_;
};
}

void PaintAudioContentLayer(pui::IRenderer& r, const pui::Theme& t, const pui::RectF& area, const std::wstring& title) {
    // Opaque black first: the (translucent) bar and the title area must not show the SDL video child's own audio
    // visualisation (waveform / spectrum) underneath the placeholder.
    if (!area.Empty()) r.FillRect(area, pui::Color(0xFF000000));
    PaintAudioOnlyPlaceholder(r, t, area, title);
}

ShellLayout App::ComputeLayout() const {
    if (modes_.Mode() == ShellMode::MiniPlayer) return ComputeMiniShellLayout(cw_, ch_, scale_);
    ShellLayoutIn in;
    in.cwPx = cw_; in.chPx = ch_; in.scale = scale_; in.mode = modes_.Mode();
    in.panelWanted = svc_->settings.playlistPanel;
    in.panelWidthDip = (float)svc_->settings.playlistPanelWidthDip;
    return ComputeShellLayout(in);
}

void App::Relayout() {
    if (!hwnd_ || !shell_ || IsIconic(hwnd_)) return;
    RECT c{}; GetClientRect(hwnd_, &c);
    cw_ = std::max<LONG>(1, c.right); ch_ = std::max<LONG>(1, c.bottom);
    host_.SetSize({cw_ / scale_, ch_ / scale_});
    ApplyMode();
}

void App::ApplyMode() {
    if (!shell_ || cw_ <= 0) return;
    ShellMode mode = modes_.Mode();
    layout_ = ComputeLayout();
    host_.CloseAllPopups();   // flyouts/sidebar are positioned in the old geometry
    shell_->ApplyLayout(layout_, mode);
    if (video_) {
        if (layout_.video.Empty()) {
            ShowWindow(video_, SW_HIDE);
        } else {
            SetWindowPos(video_, nullptr, layout_.video.l, layout_.video.t, layout_.video.w(), layout_.video.h(), SWP_NOZORDER | SWP_NOACTIVATE);
            ShowWindow(video_, SW_SHOWNA);
        }
    }
    if (debugVisible_) RefreshDebugOverlay();
    // A mode change shows the bar, except a key-driven switch between video modes keeps a hidden bar hidden.
    // Only the overlay bars auto-hide: full screen and the mini player. The windowed player and pages dock the bar.
    const bool overlayBar = mode == ShellMode::FullScreen || mode == ShellMode::MiniPlayer;
    // Full screen is edge-driven: bar, title strip and playlist panel start hidden and each appears when the cursor enters its edge.
    const bool fullscreen = mode == ShellMode::FullScreen;
    bool keepHidden = fullscreen || (keyInput_ && overlayBar && autoHide_.Enabled() && autoHide_.Hidden());
    autoHide_.SetEnabled(overlayBar, Now());
    autoHide_.SetEdge(fullscreen);
    autoHide_.Snap(keepHidden ? 1.f : 0.f);
    if (!keepHidden) autoHide_.Touch(Now());
    titleHide_.SetEnabled(fullscreen, Now());
    titleHide_.SetEdge(true);
    titleHide_.Snap(fullscreen ? 1.f : 0.f);
    SetCursorHidden(false);
    SnapBar();
    layersDirtyAll_ = true;
    SyncModel();
    Flush();
    if (fullscreen) UpdateEdgeReveal(Now());   // the cursor may already sit in an edge zone
}

void App::SnapBar() {
    if (!gfxOk_) return;
    float p = autoHide_.P(Now());
    float barTop = (float)layout_.barPx.t, slide = (float)S(kBarSlideDip), strip = (float)S(kStripDip);
    sBar_.visual->SetOffsetX((float)layout_.barPx.l);
    sBar_.visual->SetOffsetY(barTop + slide * p);
    sBar_.visual3->SetOpacity(1.f - p);
    float tp = layout_.titleSlides ? titleHide_.P(Now()) : 0.f;
    sTitle_.visual->SetOffsetX(0.f);
    sTitle_.visual->SetOffsetY(-strip * tp);
    sTitle_.visual3->SetOpacity(1.f - tp);
    lastCommitHr_ = gfx_.Commit();
}

void App::StartBarAnim(const AutoHide::Action& a) {
    if (a == AutoHide::Action::None || !gfxOk_) return;
    if (a == AutoHide::Action::Snapped) { SnapBar(); return; }
    const AutoHide::Anim& an = autoHide_.LastAnim();
    float d = an.durMs / 1000.f, slide = (float)S(kBarSlideDip), top = (float)layout_.barPx.t;
    auto oy = gfx_.MakeAnim(top + slide * an.p0, top + slide * an.p1, d, an.easeOut);
    auto op = gfx_.MakeAnim(1 - an.p0, 1 - an.p1, d, an.easeOut);
    if (oy && op) { sBar_.visual->SetOffsetY(oy.Get()); sBar_.visual3->SetOpacity(op.Get()); } else SnapBar();
    lastCommitHr_ = gfx_.Commit();
    SetTimerMs(16);
}

void App::StartTitleAnim(const AutoHide::Action& a) {
    if (a == AutoHide::Action::None || !gfxOk_ || !layout_.titleSlides) return;
    if (a == AutoHide::Action::Snapped) { SnapBar(); return; }
    const AutoHide::Anim& an = titleHide_.LastAnim();
    float d = an.durMs / 1000.f, strip = (float)S(kStripDip);
    auto ty = gfx_.MakeAnim(-strip * an.p0, -strip * an.p1, d, an.easeOut);
    auto to = gfx_.MakeAnim(1 - an.p0, 1 - an.p1, d, an.easeOut);
    if (ty && to) { sTitle_.visual->SetOffsetY(ty.Get()); sTitle_.visual3->SetOpacity(to.Get()); } else SnapBar();
    lastCommitHr_ = gfx_.Commit();
    SetTimerMs(16);
}

void App::SetCursorHidden(bool hide) {
    if (hide == cursorHidden_) return;
    cursorHidden_ = hide;
    ShowCursor(hide ? FALSE : TRUE);
}

void App::CheckPointerLeft(uint64_t now) {
    if (!hwnd_ || hidden_ || minimized_) return;
    POINT p; GetCursorPos(&p);
    HWND at = WindowFromPoint(p);
    bool inside = at && GetAncestor(at, GA_ROOT) == hwnd_;
    if (pointerInside_ && !inside) {
        StartBarAnim(autoHide_.PointerLeft(now, BarInteractionActive()));
        StartTitleAnim(titleHide_.PointerLeft(now, TitleInteractionActive()));
        if (panelPopup_ && modes_.fullscreen && !PanelInteractionActive()) host_.ClosePopup(panelPopup_);
    }
    pointerInside_ = inside;
}

void App::AutoHideTick(uint64_t now) {
    CheckPointerLeft(now);   // the video child sends no leave event: poll (100 ms idle tick)
    if (modes_.fullscreen) {
        UpdateEdgeReveal(now);   // also hides each overlay the pointer is off
    } else {
        bool playing = state_ == wvp::PlayerState::Playing;
        StartBarAnim(autoHide_.Tick(now, playing, BarInteractionActive(), minimized_));
        StartTitleAnim(titleHide_.Tick(now, playing, TitleInteractionActive(), minimized_));
    }
    if (!hidden_) {
        bool fullscreen = modes_.fullscreen;   // the cursor auto-hides only in full screen
        POINT p; GetCursorPos(&p);
        // ShowCursor hides the cursor for every window of this thread (the Settings window too): only over the player
        const HWND at = WindowFromPoint(p);
        const bool overPlayer = at && GetAncestor(at, GA_ROOT) == hwnd_;
        ScreenToClient(hwnd_, &p);
        pui::PointF cursorDip{p.x / scale_, p.y / scale_};
        bool overChrome = (autoHide_.Hittable(now) && layout_.bar.Contains(cursorDip.x, cursorDip.y)) ||
                          (layout_.titleSlides && titleHide_.Hittable(now) && layout_.title.Contains(cursorDip.x, cursorDip.y)) ||
                          (panelPopup_ && panelPopup_->Bounds().Contains(cursorDip.x, cursorDip.y));
        bool blocked = minimized_ || !IsWindowEnabled(hwnd_) || !overPlayer;   // a modal dialog (file picker, text input) disables us
        SetCursorHidden(cursorIdle_.ShouldHide(now, fullscreen, overChrome, NonPanelPopups() > 0, blocked));
    }
}

void App::PointerMoved(uint64_t now) {
    cursorIdle_.Moved(now);
    SetCursorHidden(false);                          // the cursor comes back at once, whatever the bar is doing
    if (modes_.fullscreen) UpdateEdgeReveal(now);
    else StartBarAnim(autoHide_.Activity(now));
}

void App::UpdateEdgeReveal(uint64_t now) {
    if (!hwnd_ || !modes_.fullscreen || minimized_ || !gfxOk_) return;
    if (hidden_) return;   // --headless-check: the physical cursor is not the test's; the steps drive the reveal state explicitly
    POINT p; GetCursorPos(&p); ScreenToClient(hwnd_, &p);
    const pui::PointF dip{p.x / scale_, p.y / scale_};
    const EdgeZones z = EdgeZonesAt(dip.x, dip.y, cw_ / scale_, ch_ / scale_);
    if (z.bottom) StartBarAnim(autoHide_.Enter(now));
    if (z.top) StartTitleAnim(titleHide_.Enter(now));
    if (z.right && !panelPopup_ && !host_.Captured()) OpenPanelOverlay();
    // Each overlay hides the moment the pointer is off its own rect (unless a menu / drag / search box keeps it).
    StartBarAnim(autoHide_.EdgeTick(now, layout_.bar.Contains(dip.x, dip.y), BarInteractionActive(), minimized_));
    StartTitleAnim(titleHide_.EdgeTick(now, layout_.title.Contains(dip.x, dip.y), TitleInteractionActive(), minimized_));
    if (panelPopup_ && !PanelInteractionActive()) host_.ClosePopup(panelPopup_);
}

bool App::RenderLayer(Surf& s, uint64_t& lastHash, int x, int y, int w, int h, const std::function<void(LayerRenderer&)>& paint, bool force) {
    w = std::max(1, w); h = std::max(1, h);
    bool realloc = !s.surface || s.w != w || s.h != h;
    pui::PointF origin{x / scale_, y / scale_};
    if (!realloc && !force) {
        lr_->BeginHash();
        paint(*lr_);
        uint64_t hv = lr_->EndHash();
        if (hv == lastHash) return false;
        lastHash = hv;
    } else {
        lr_->BeginHash();
        paint(*lr_);
        lastHash = lr_->EndHash();
        if (realloc) {
            lastDrawHr_ = gfx_.ResizeSurf(s, w, h);
            if (FAILED(lastDrawHr_)) return false;
        }
    }
    lastDrawHr_ = gfx_.DrawSurf(s, [&](ID2D1DeviceContext* dc) {
        lr_->BeginDraw(dc, origin);
        paint(*lr_);
        lr_->EndDraw();
    });
    return true;
}

void App::UpdateAcrylicState(bool forceSystemCheck) {
    const uint64_t now = Now();
    if (forceSystemCheck || now >= acrylicSystemCheckNextMs_) {
        acrylicTransparencyEffects_ = AcrylicTransparencyEffectsEnabled();
        acrylicBatterySaver_ = AcrylicBatterySaverEnabled();
        acrylicRemoteSession_ = AcrylicRemoteSession();
        acrylicHighContrast_ = AcrylicHighContrastEnabled();
        acrylicSystemCheckNextMs_ = now + 1000;
    }
    if (forceSystemCheck || now >= acrylicPresenterCheckNextMs_) {
        if (player_ && Loaded() && acrylicMediaHasVideo_) {
            if (auto json = player_->GetDebugInfoJson()) {
                const wvp::DebugInfo debug = wvp::ParseDebugInfo(*json);
                debugCache_ = debug;
                debugCacheMs_ = now;
                if (debug.Valid) {
                    acrylicD3D11_ = debug.Presenter.Present && debug.Presenter.Name == "d3d11";
                    if (debug.Presenter.Acrylic.Failures.value_or(0) > 0)
                        acrylicPresenterFailed_ = true;
                }
            }
        } else {
            acrylicD3D11_ = false;
        }
        acrylicPresenterCheckNextMs_ = now + 250;
    }

    acrylic::Conditions conditions;
    conditions.videoShowing = Loaded() && acrylicMediaHasVideo_ && !layout_.video.Empty();
    conditions.d3d11Presenter = acrylicD3D11_;
    conditions.modalDialog = host_.HasModalPopup();
    conditions.transparencyEffects = acrylicTransparencyEffects_;
    conditions.batterySaver = acrylicBatterySaver_;
    conditions.remoteSession = acrylicRemoteSession_;
    conditions.highContrast = acrylicHighContrast_;
    conditions.presenterFailure = acrylicPresenterFailed_;
    acrylic::FallbackReason reason = acrylic::DecideFallback(conditions);
    if (reason == acrylic::FallbackReason::None && acrylicApiUnsupported_)
        reason = acrylic::FallbackReason::PresenterUnavailable;

    std::vector<wvp::BackdropRegion> desired;
    if (reason == acrylic::FallbackReason::None) {
        const auto popupSurfaces = host_.AcrylicPopupSurfaces();
        const bool hasTooltip = host_.ToolTipVisible();
        if (popupSurfaces.size() + (hasTooltip ? 1u : 0u) > 64) {
            reason = acrylic::FallbackReason::RegionLimit;
        } else {
            const auto insideVideo = [&](const RectF& r) {
                const int l = (int)std::lround(r.l * scale_), t = (int)std::lround(r.t * scale_);
                const int rr = (int)std::lround(r.r * scale_), bb = (int)std::lround(r.b * scale_);
                return !r.Empty() && l >= layout_.video.l && t >= layout_.video.t &&
                       rr <= layout_.video.r && bb <= layout_.video.b;
            };
            bool allInside = true;
            int z = 0;
            const pui::Theme& theme = host_.GetTheme();
            const bool barShown = !modes_.mini && autoHide_.P(now) < 1.f;
            const bool titleShown = !layout_.title.Empty() && !(layout_.titleSlides && titleHide_.P(now) >= 1.f);
            auto addSurface = [&](const RectF& bounds, float radius) {
                if (!insideVideo(bounds)) { allInside = false; return; }
                if (acrylic::OverlapsVisibleChrome(bounds, layout_.bar, barShown, layout_.title, titleShown)) {
                    allInside = false;
                    return;
                }
                auto region = acrylic::RegionFromDip(bounds, layout_.video.l, layout_.video.t,
                                                      layout_.video.w(), layout_.video.h(), scale_, radius, theme, z++);
                if (!region) { allInside = false; return; }
                desired.push_back(*region);
            };
            for (const auto& surface : popupSurfaces)
                addSurface(surface.bounds, surface.cornerRadius);
            if (hasTooltip) {
                d2d_.SetScale(scale_);
                const RectF tooltip = pui::ToolTipBounds(d2d_, host_.ToolTipText(), host_.ToolTipAnchor(),
                                                         host_.ToolTipPoint(), host_.Size());
                addSurface(tooltip, theme.controlRadius);
            }
            if (!allInside) {
                desired.clear();
                reason = acrylic::FallbackReason::SurfaceOutsideVideo;
            }
        }
    }

    if (reason == acrylic::FallbackReason::None && player_ && !acrylicApiUnsupported_ &&
        (acrylicNeedsApply_ || !acrylicApiReady_ || !acrylic::SameRegions(acrylicAppliedRegions_, desired))) {
        const int rc = player_->SetBackdropRegions(desired.empty() ? nullptr : desired.data(), (int)desired.size());
        if (rc == 0) {
            acrylicApiReady_ = true;
            acrylicNeedsApply_ = false;
            acrylicAppliedRegions_ = desired;
        } else {
            acrylicApiReady_ = false;
            acrylicApiUnsupported_ = true;
            acrylicNeedsApply_ = false;
            acrylicAppliedRegions_.clear();
            desired.clear();
            reason = acrylic::FallbackReason::PresenterUnavailable;
        }
    } else if (reason != acrylic::FallbackReason::None) {
        if (player_ && acrylicApiReady_ && !acrylicAppliedRegions_.empty())
            if (player_->SetBackdropRegions(nullptr, 0) != 0) acrylicApiReady_ = false;
        acrylicAppliedRegions_.clear();
        acrylicNeedsApply_ = false;
    }

    const pui::Theme currentTheme = host_.GetTheme();
    const bool enableAcrylic = reason == acrylic::FallbackReason::None && acrylicApiReady_ && !acrylicAppliedRegions_.empty();
    const pui::Color acrylicFallback = acrylicHighContrast_ ? AcrylicHighContrastWindowColor() : currentTheme.flyoutSurface;
    if (currentTheme.acrylicEnabled != enableAcrylic || currentTheme.acrylicFallbackColor != acrylicFallback) {
        pui::Theme theme = currentTheme;
        theme.acrylicEnabled = enableAcrylic;
        theme.acrylicFallbackColor = acrylicFallback;
        host_.SetTheme(theme);
        layersDirtyAll_ = true;
    }
    if (reason != acrylicFallbackReason_) {
        acrylicFallbackReason_ = reason;
        if (debugVisible_) RefreshDebugOverlay();
    }
}

void App::RenderLayers(bool force) {
    if (!gfxOk_) return;
    const pui::Theme& th = host_.GetTheme();
    d2d_.SetScale(scale_);
    bool changed = false;
    ShellMode mode = modes_.Mode();
    int strip = S(kStripDip), W = cw_;

    if (AudioOnly().audioOnly) {
        // The placeholder covers exactly the video rect (the docked bar and the gutters keep the window backdrop).
        const RectPx& v = layout_.video;
        int w = std::max(1, v.w()), h = std::max(1, v.h());
        sContent_.visual->SetOffsetX((float)v.l);
        sContent_.visual->SetOffsetY((float)v.t);
        std::wstring title = AudioOnly().title;
        changed |= RenderLayer(sContent_, hContent_, v.l, v.t, w, h, [&](LayerRenderer& r) {
            PaintAudioContentLayer(r, th, pui::RectF::XYWH(0, 0, w / scale_, h / scale_), title);
        }, force);
    } else if (mode == ShellMode::FullScreen && !MediaActive()) {
        // Idle full screen: the video child has nothing to render and its GDI black is transparent over the extended DWM
        // frame (windows behind show through the translucent bar), so cover the video rect with opaque black.
        const RectPx& v = layout_.video;
        int w = std::max(1, v.w()), h = std::max(1, v.h());
        sContent_.visual->SetOffsetX((float)v.l);
        sContent_.visual->SetOffsetY((float)v.t);
        changed |= RenderLayer(sContent_, hContent_, v.l, v.t, w, h, [&](LayerRenderer& r) {
            r.FillRect(pui::RectF::XYWH(0, 0, w / scale_, h / scale_), pui::Color(0xFF000000));
        }, force);
    } else if (sContent_.surface) {
        gfx_.ClearSurf(sContent_); hContent_ = 0; changed = true;
    }

    if (mode == ShellMode::NowPlaying && !layout_.panelPx.Empty() && shell_->Panel()->Visible()) {
        const RectPx& p = layout_.panelPx;
        sPanel_.visual->SetOffsetX((float)p.l);
        sPanel_.visual->SetOffsetY((float)p.t);
        changed |= RenderLayer(sPanel_, hPanel_, p.l, p.t, p.w(), p.h(), [&](LayerRenderer& r) { shell_->Panel()->PaintTree(r, th); }, force);
    } else if (sPanel_.surface) {
        gfx_.ClearSurf(sPanel_); hPanel_ = 0; changed = true;
    }

    changed |= RenderDebugLayer(force);

    if (mode != ShellMode::MiniPlayer && !layout_.title.Empty()) {
        sTitle_.visual->SetOffsetX(0.f);
        changed |= RenderLayer(sTitle_, hTitle_, 0, 0, W, strip, [&](LayerRenderer& r) { shell_->Title()->PaintTree(r, th); }, force);
    } else if (sTitle_.surface) {
        gfx_.ClearSurf(sTitle_); hTitle_ = 0; changed = true;
    }

    pui::Widget* barW = mode == ShellMode::MiniPlayer ? static_cast<pui::Widget*>(shell_->Mini()) : shell_->Bar();
    changed |= RenderLayer(sBar_, hBar_, layout_.barPx.l, layout_.barPx.t, layout_.barPx.w(), layout_.barPx.h(), [&](LayerRenderer& r) { barW->PaintTree(r, th); }, force);

    bool anyPopup = host_.PopupCount() > 0 || host_.ToolTipVisible();
    bool acrylicPopup = th.acrylicEnabled && (!host_.AcrylicPopupSurfaces().empty() || host_.ToolTipVisible());
    if (anyPopup) {
        sPopup_.visual->SetOffsetX(0.f);
        sPopup_.visual->SetOffsetY(0.f);
        if ((!popupWasVisible_ || (acrylicPopup && !popupAcrylicWasActive_)) && sPopup_.visual3) {
            if (acrylicPopup) {
                auto fade = gfx_.MakeAnim(0.f, 1.f, 0.12f, true);  // short fade; Acrylic stays at its settled rect
                if (fade) sPopup_.visual3->SetOpacity(fade.Get());
                else sPopup_.visual3->SetOpacity(1.f);
            } else {
                sPopup_.visual3->SetOpacity(1.f);
            }
        }
        changed |= RenderLayer(sPopup_, hPopup_, 0, 0, W, ch_, [&](LayerRenderer& r) {
            shell_->SetPopupOnly(true);
            host_.Paint(r);
            shell_->SetPopupOnly(false);
        }, force);
        popupWasVisible_ = true;
        popupAcrylicWasActive_ = acrylicPopup;
    } else {
        if (sPopup_.surface) {
            gfx_.ClearSurf(sPopup_); hPopup_ = 0; changed = true;
        }
        if (sPopup_.visual3) sPopup_.visual3->SetOpacity(1.f);
        popupWasVisible_ = false;
        popupAcrylicWasActive_ = false;
    }
    if (changed) lastCommitHr_ = gfx_.Commit();
}

bool App::RenderDebugLayer(bool force) {
    if (!gfxOk_ || !sDebug_.visual) return false;
    dbgPainter_.SetScale(scale_);
    dbg::OverlayGeometry g = dbg::ComputeOverlayGeometry(cw_, ch_, scale_, modes_.Mode(), dbgPainter_.CharW(), dbgPainter_.LineH(), layout_.barPx.h());
    if (!debugVisible_ || !g.visible) {
        hDebug_ = 0;
        if (!sDebug_.surface) return false;
        gfx_.ClearSurf(sDebug_);
        return true;
    }
    debugCols_ = g.cols;
    std::vector<dbg::OverlayLine> rows = dbg::ClipRows(dbg::WrapLines(debugLines_, g.cols), g.rows);
    float pad = (float)std::lround(dbg::kPadDip * scale_);
    int w = (int)g.w, h = (int)std::ceil((float)rows.size() * dbgPainter_.LineH() + 2 * pad);
    const pui::Theme& th = host_.GetTheme();
    uint64_t hv = 1469598103934665603ull ^ ((uint64_t)w << 20) ^ (uint64_t)h ^ ((uint64_t)th.caution.v << 40) ^ th.accent.v;
    auto mix = [&](unsigned char ch) { hv = (hv ^ ch) * 1099511628211ull; };
    for (const auto& r : rows) { mix((unsigned char)r.kind); for (unsigned char ch : r.text) mix(ch); mix('\n'); }
    bool realloc = !sDebug_.surface || sDebug_.w != w || sDebug_.h != h;
    if (!force && !realloc && hv == hDebug_) return false;
    hDebug_ = hv;
    debugRows_ = rows;
    if (realloc) {
        lastDrawHr_ = gfx_.ResizeSurf(sDebug_, w, h);
        if (FAILED(lastDrawHr_)) return false;
    }
    lastDrawHr_ = gfx_.DrawSurf(sDebug_, [&](ID2D1DeviceContext* dc) { dbgPainter_.Paint(dc, rows, th, w, h); });
    sDebug_.visual->SetOffsetX(g.x);
    sDebug_.visual->SetOffsetY(g.y);
    return true;
}

void App::Flush() {
    if (!gfxOk_ || destroyed_ || minimized_ || !shell_) return;
    UpdateAcrylicState();
    if (!host_.IsDirty() && !layersDirtyAll_ && !debugDirty_) return;
    host_.ClearDirty();
    bool force = layersDirtyAll_;
    layersDirtyAll_ = false;
    debugDirty_ = false;
    RenderLayers(force);
}

void App::Tick() {
    if (!shell_ || destroyed_) return;
    uint64_t now = Now();
    EnsureVideoDropHook();
    bool more = host_.Tick();
    AutoHideTick(now);
    ChipsTick(now);
    for (size_t i = 0; i < notices_.size();) {
        if (now >= notices_[i].until) { pui::Widget* w = notices_[i].w; CloseNotice(w); i = 0; } else i++;
    }
    SyncModel();
    if (debugVisible_ && now >= debugNextMs_) {   // ~4 Hz while shown, nothing while hidden
        debugNextMs_ = now + 250;
        RefreshDebugOverlay();
    }
    PipelinePoll(now);
    if (opt_.testUiLossMs >= 0 && !testLossFired_ && state_ == wvp::PlayerState::Playing) {   // --test-ui-device-loss=<ms>
        if (!testLossArmedMs_) testLossArmedMs_ = now;
        else if (now - testLossArmedMs_ >= (uint64_t)opt_.testUiLossMs) { testLossFired_ = true; SimulateUiDeviceLoss(); }
    }
    if (uia_) {   // report the auto-hidden bar/title as off-screen to UI Automation
        bool off = IsAutoHiddenChrome(shell_->Bar()) || IsAutoHiddenChrome(shell_->Mini());
        if (off != chromeOffscreen_) {
            chromeOffscreen_ = off;
            uia_->NotifyOffscreenChanged(modes_.mini ? static_cast<pui::Widget*>(shell_->Mini()) : shell_->Bar());
            if (layout_.titleSlides) uia_->NotifyOffscreenChanged(shell_->Title());
        }
    }
    bool animating = more || autoHide_.Animating(now) || titleHide_.Animating(now);
    SetTimerMs(animating ? 16 : (debugVisible_ ? 50 : 100));   // idle = 100 ms (50 ms while the debug overlay refreshes at 4 Hz)
    Flush();
}

bool App::IsAutoHiddenChrome(pui::Widget* w) const {
    if (!shell_ || !w) return false;
    if (w == shell_->Title())
        return layout_.titleSlides && titleHide_.Enabled() && titleHide_.Hidden() && !titleHide_.Animating(Now());
    if (!autoHide_.Enabled() || !autoHide_.Hidden() || autoHide_.Animating(Now())) return false;
    return w == shell_->Bar() || w == shell_->Mini();
}

void App::CloseNotice(pui::Widget* w) {
    for (size_t i = 0; i < notices_.size(); i++)
        if (notices_[i].w == w) { notices_.erase(notices_.begin() + (long)i); break; }
    host_.ClosePopup(w);
}

void App::ShowInfoBar(InfoSeverity severity, const std::wstring& title, const std::wstring& message) {
    if (!shell_) return;
    const float clientW = cw_ / scale_, clientH = ch_ / scale_;
    float w = std::max(0.f, std::min(560.f, clientW - 16.f));
    auto nw = std::make_unique<NoticeWidget>(severity, title, message, false, [this](NoticeWidget* n) { CloseNotice(n); });
    NoticeWidget* raw = nw.get();
    float y = modes_.fullscreen || modes_.mini ? 8.f : kStripDip + 8.f;
    float h = std::max(0.f, std::min(64.f, clientH - y - 8.f));
    float x = std::clamp(std::round((clientW - w) / 2), 0.f, std::max(0.f, clientW - w));
    y = std::clamp(y, 0.f, std::max(0.f, clientH - h));
    raw->SetBounds(RectF::XYWH(x, y, w, h));
    pui::PopupOptions po;
    po.lightDismiss = false;
    po.takeFocus = false;
    po.onClosed = [this, raw] {
        for (size_t i = 0; i < notices_.size(); i++) if (notices_[i].w == raw) { notices_.erase(notices_.begin() + (long)i); break; }
    };
    notices_.push_back({raw, Now() + 12000});
    host_.OpenPopup(std::move(nw), po);
}

void App::ShowToast(const std::wstring& text) {
    if (!shell_ || text.empty()) return;
    pui::TextStyle ts;
    ts.size = 14;
    ts.ellipsis = false;
    const float clientW = cw_ / scale_, clientH = ch_ / scale_;
    float h = std::min(40.f, clientH);
    float w = std::min(std::max(0.f, clientW - 16.f), d2d_.MeasureText(text, ts).w + 40.f);
    auto nw = std::make_unique<NoticeWidget>(InfoSeverity::Informational, L"", text, true, [this](NoticeWidget* n) { CloseNotice(n); });
    NoticeWidget* raw = nw.get();
    float x = std::clamp(std::round((clientW - w) / 2), 0.f, std::max(0.f, clientW - w));
    float y = std::clamp(layout_.bar.t - 16 - h, 0.f, std::max(0.f, clientH - h));
    raw->SetBounds(RectF::XYWH(x, y, w, h));
    pui::PopupOptions po;
    po.lightDismiss = false;
    po.takeFocus = false;
    po.onClosed = [this, raw] {
        for (size_t i = 0; i < notices_.size(); i++) if (notices_[i].w == raw) { notices_.erase(notices_.begin() + (long)i); break; }
    };
    notices_.push_back({raw, Now() + 2500});
    host_.OpenPopup(std::move(nw), po);
}

}
