#include "shell/SettingsWindow.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "AppContext.h"
#include "Localize.h"
#include "features/FiltersPanel.h"
#include "features/NativeMenu.h"
#include "features/SettingsService.h"
#include "pages/SettingsPage.h"
#include "pui/ToolTipStyle.h"

namespace app {

namespace {
constexpr const wchar_t* kClassName = L"aribplayer.Settings";
constexpr const wchar_t* kOwnerClassName = L"aribplayer.SettingsOwner";
constexpr UINT_PTR kTickTimer = 1;
// no minimize box: the window has no taskbar button (hidden owner), so a minimized one would be a stub on the desktop
constexpr DWORD kStyle = WS_OVERLAPPEDWINDOW & ~WS_MINIMIZEBOX;

// Re-enables drawing after the root's children (popup pass: the root suppressed its own subtree), like ShellRoot's sentinel.
class Sentinel final : public pui::Widget {
public:
    Sentinel() { SetHitTestVisible(false); }
    void Paint(pui::IRenderer& r, const pui::Theme&) override {
        if (auto* s = dynamic_cast<ISuppressible*>(&r)) s->SetDrawEnabled(true);
    }
};

}

/// Root of the window's tree: fills the client with the page layer colour over Mica and hosts the SettingsPage. Popup pass
/// (SetPopupOnly): its own subtree is suppressed so UiHost::Paint draws only the popups and the tooltip.
class SettingsWindow::Root final : public pui::Widget {
public:
    Root() : pui::Widget(pui::Role::Window) {
        SetName(Tr(L"Settings", L"환경 설정"));
        SetAutomationId(L"SettingsWindow");
    }
    pui::Widget* SetPage(std::unique_ptr<pui::Widget> page) {
        page_ = AddChild(std::move(page));
        Add<Sentinel>();
        OnLayout();
        return page_;
    }
    void SetPopupOnly(bool on) { popupOnly_ = on; }
    void OnLayout() override { if (page_) page_->SetBounds(Bounds()); }
    void Paint(pui::IRenderer& r, const pui::Theme& t) override {
        if (popupOnly_) {
            if (auto* s = dynamic_cast<ISuppressible*>(&r)) s->SetDrawEnabled(false);
            return;
        }
        r.FillRect(Bounds(), t.layerFill);
    }
private:
    pui::Widget* page_ = nullptr;
    bool popupOnly_ = false;
};

SettingsWindow::SettingsWindow(AppContext& ctx, Gfx& gfx, IDWriteFactory* dw)
    : ctx_(ctx), gfx_(gfx), d2d_(std::make_unique<pui::D2dRenderer>(dw)) {
    lr_ = std::make_unique<LayerRenderer>(*d2d_);
    pui::InstallToolTipStyle(host_);
    host_.onInvalidate = [this] { RequestRedraw(); };
}

SettingsWindow::~SettingsWindow() {
    if (hwnd_) DestroyWindow(hwnd_);
    if (owner_) DestroyWindow(owner_);
    ReleaseDevice();
}

bool SettingsWindow::Open(bool hidden, bool attachDevice) {
    if (hwnd_) {   // single instance: bring the open one to the front
        if (!hidden_) {
            if (hiddenForPlayer_) { hiddenForPlayer_ = false; ShowWindow(hwnd_, SW_SHOW); }
            if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
            SetForegroundWindow(hwnd_);
        }
        return true;
    }
    hidden_ = hidden;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof wc};
        wc.style = CS_DBLCLKS;   // WM_LBUTTONDBLCLK -> clickCount 2
        wc.lpfnWndProc = StaticProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);   // black = transparent over the extended DWM frame (Mica)
        wc.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                                 GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR | LR_SHARED));
        wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                   GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR | LR_SHARED));
        wc.lpszClassName = kClassName;
        registered = RegisterClassExW(&wc) != 0;
    }

    if (!owner_) {   // one invisible top-level owner: the Settings window gets no taskbar button and is not owned by the player
        WNDCLASSEXW oc{sizeof oc};
        oc.lpfnWndProc = DefWindowProcW;
        oc.hInstance = inst;
        oc.lpszClassName = kOwnerClassName;
        RegisterClassExW(&oc);   // already registered on a reopen: fine
        owner_ = CreateWindowExW(WS_EX_NOACTIVATE, kOwnerClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
        if (!owner_) return false;
    }

    // Default geometry: 900 x 700 DIP (at the main window's DPI), centred over the main window, inside its monitor's work area.
    HWND owner = ctx_.MainHwnd();
    const float ownerScale = (owner ? GetDpiForWindow(owner) : GetDpiForSystem()) / 96.f;
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(owner ? MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST) : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT& wa = mi.rcWork;
    RECT orc = wa;
    if (owner) GetWindowRect(owner, &orc);
    const int w = std::min((int)std::lround(kDefaultWidthDip * ownerScale), (int)(wa.right - wa.left));
    const int h = std::min((int)std::lround(kDefaultHeightDip * ownerScale), (int)(wa.bottom - wa.top));
    const int x = std::clamp((int)(orc.left + orc.right) / 2 - w / 2, (int)wa.left, std::max((int)wa.left, (int)wa.right - w));
    const int y = std::clamp((int)(orc.top + orc.bottom) / 2 - h / 2, (int)wa.top, std::max((int)wa.top, (int)wa.bottom - h));
    // hidden (--headless-check): never activatable, so a test run cannot take the user's focus / keystrokes
    hwnd_ = CreateWindowExW(hidden ? WS_EX_NOACTIVATE : 0, kClassName, Tr(L"Settings", L"환경 설정"), kStyle,
                            x, y, w, h, owner_, nullptr, inst, this);
    if (!hwnd_) return false;
    scale_ = GetDpiForWindow(hwnd_) / 96.f;
    if (std::fabs(scale_ - ownerScale) > 0.001f)
        SetWindowPos(hwnd_, nullptr, 0, 0, (int)std::lround(w * scale_ / ownerScale), (int)std::lround(h * scale_ / ownerScale),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    bool maximized = false;
    if (auto p = RestorablePlacement(ctx_.Svc().settings.settingsWindow)) {   // remembered position / size
        SetWindowPos(hwnd_, nullptr, p->x, p->y, p->w, p->h, SWP_NOZORDER | SWP_NOACTIVATE);
        maximized = p->maximized;
    }
    scale_ = GetDpiForWindow(hwnd_) / 96.f;
    d2d_->SetScale(scale_);

    int mica = 2;   // DWMWA_SYSTEMBACKDROP_TYPE = DWMSBT_MAINWINDOW; the standard caption stays system drawn
    DwmSetWindowAttribute(hwnd_, 38, &mica, sizeof mica);
    MARGINS m{-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(hwnd_, &m);
    ApplyCaptionTheme();
    PushTheme();

    auto root = std::make_unique<Root>();
    root_ = root.get();
    host_.SetRoot(std::move(root));
    SettingsPageEnv env;
    env.host = &host_;
    env.owner = hwnd_;
    env.editFilters = [this] { EditFilters(); };
    page_ = root_->SetPage(CreateSettingsPage(ctx_, std::move(env)));
    uia_ = std::make_unique<pui::UiaBridge>(hwnd_, host_, [this] { return scale_; });

    if (attachDevice) AttachDevice();
    Relayout();
    if (!hidden_) ShowWindow(hwnd_, maximized ? SW_SHOWMAXIMIZED : SW_SHOW);
    Render(true);
    UpdateTimer();
    return true;
}

bool SettingsWindow::FromOtherApp(HWND prev) {
    DWORD pid = 0;
    if (prev) GetWindowThreadProcessId(prev, &pid);
    return pid != GetCurrentProcessId();
}

bool SettingsWindow::Shown() const { return hwnd_ && !hidden_ && !hiddenForPlayer_ && IsWindowVisible(hwnd_); }

void SettingsWindow::RaiseAbove(HWND player) {
    // Directly above the player, without activation, in the player's band (topmost while the player is always on top).
    const bool topmost = (GetWindowLongPtrW(player, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    SetWindowPos(hwnd_, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void SettingsWindow::StartFocusCycle(HWND player) {
    if (!Shown()) return;
    inFront_ = false;
    RaiseAbove(player);
}

void SettingsWindow::OnPlayerPressed(HWND player) {
    if (!Shown() || inFront_) return;
    inFront_ = true;
    if (GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) & WS_EX_TOPMOST)
        SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (!(GetWindowLongPtrW(player, GWL_EXSTYLE) & WS_EX_TOPMOST))
        SetWindowPos(hwnd_, player, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);   // directly below the player
}

void SettingsWindow::KeepAbove(HWND player) {
    if (Shown() && !inFront_) RaiseAbove(player);
}

void SettingsWindow::OnPlayerMinimized(HWND player, bool minimized) {
    if (!hwnd_ || hidden_) return;
    if (minimized) {
        if (IsWindowVisible(hwnd_)) { hiddenForPlayer_ = true; host_.CloseAllPopups(); ShowWindow(hwnd_, SW_HIDE); }
    } else if (hiddenForPlayer_) {
        hiddenForPlayer_ = false;
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        RaiseAbove(player);
    }
}

void SettingsWindow::Close() {
    if (hwnd_) DestroyWindow(hwnd_);   // WM_DESTROY -> OnDestroyed (also when the main window's destruction takes it along)
}

void SettingsWindow::OnDestroyed() {
    SavePlacement();
    KillTimer(hwnd_, kTickTimer);
    timerMs_ = 0;
    if (uia_) { uia_->Shutdown(); uia_.reset(); }   // disconnect providers before the tree goes away
    host_.CloseAllPopups();
    host_.SetRoot(nullptr);
    root_ = nullptr;
    page_ = nullptr;
    ReleaseDevice();
    gfx_.Commit();   // drops this window's composition tree now
    hwnd_ = nullptr;
    hiddenForPlayer_ = false;
    mouseTracked_ = redrawPosted_ = false;
}

void SettingsWindow::SavePlacement() {
    if (!hwnd_ || hidden_) return;   // a hidden test window never overwrites the user's placement
    if (auto p = CaptureWindowPlacement(hwnd_)) {
        ctx_.Svc().settings.settingsWindow = *p;
        ctx_.Svc().settingsStore.Save(ctx_.Svc().settings);
    }
}

void SettingsWindow::ApplyTheme(const pui::Theme& theme, bool light) {
    theme_ = theme;
    light_ = light;
    if (!hwnd_) return;
    ApplyCaptionTheme();
    PushTheme();
}

void SettingsWindow::ApplyCaptionTheme() {
    BOOL dark = light_ ? FALSE : TRUE;
    DwmSetWindowAttribute(hwnd_, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof dark);
}

void SettingsWindow::PushTheme() {
    pui::Theme t = theme_;
    t.hairline = 1.f / std::max(0.25f, scale_);   // one device pixel of THIS window
    host_.SetTheme(t);
    dirtyAll_ = true;
    RequestRedraw();
}

void SettingsWindow::EditFilters() {
    // The filters panel is a popup of the main window, which this (owned) window would cover: close Settings first. Posted:
    // the click handler runs inside the page that closing tears down.
    AppContext& ctx = ctx_;
    ctx.PostToUi([this, &ctx] {
        Close();
        ShowFiltersPanel(ctx, FiltersTab::Video);
    });
}

void SettingsWindow::ReleaseDevice() {
    // device children first: surfaces / visuals -> target -> the renderer's dc-bound brush
    content_ = {};
    popup_ = {};
    rootVisual_.Reset();
    target_.Reset();
    d2d_->ReleaseDevice();
    deviceReady_ = false;
    hContent_ = hPopup_ = 0;
}

HRESULT SettingsWindow::AttachDevice() {
    ReleaseDevice();
    if (!hwnd_ || !gfx_.desk) return E_FAIL;
    HRESULT hr = gfx_.desk->CreateTargetForHwnd(hwnd_, TRUE, &target_);
    if (SUCCEEDED(hr)) hr = gfx_.desk->CreateVisual(&rootVisual_);
    if (SUCCEEDED(hr)) hr = target_->SetRoot(rootVisual_.Get());
    // content at the bottom, popup above it (FALSE + NULL reference = top; see LayerOrder.h for the NULL-reference pitfall)
    if (SUCCEEDED(hr)) hr = gfx_.desk->CreateVisual(&content_.visual);
    if (SUCCEEDED(hr)) hr = rootVisual_->AddVisual(content_.visual.Get(), FALSE, nullptr);
    if (SUCCEEDED(hr)) hr = gfx_.desk->CreateVisual(&popup_.visual);
    if (SUCCEEDED(hr)) hr = rootVisual_->AddVisual(popup_.visual.Get(), TRUE, content_.visual.Get());
    if (FAILED(hr)) { ReleaseDevice(); return hr; }
    deviceReady_ = true;
    dirtyAll_ = true;
    return S_OK;
}

void SettingsWindow::Relayout() {
    if (!hwnd_ || IsIconic(hwnd_)) return;
    RECT c{};
    GetClientRect(hwnd_, &c);
    const int w = std::max<LONG>(1, c.right), h = std::max<LONG>(1, c.bottom);
    if (w != cw_ || h != ch_) host_.CloseAllPopups();   // flyouts are positioned in the old geometry
    cw_ = w;
    ch_ = h;
    host_.SetSize({cw_ / scale_, ch_ / scale_});
    Render(false);
}

void SettingsWindow::RequestRedraw() {
    if (redrawPosted_ || !hwnd_) return;
    redrawPosted_ = true;
    PostMessageW(hwnd_, WM_SETTINGS_REDRAW, 0, 0);
}

bool SettingsWindow::RenderLayer(Surf& s, uint64_t& lastHash, bool force, const std::function<void(LayerRenderer&)>& paint) {
    const int w = std::max(1, cw_), h = std::max(1, ch_);
    const bool realloc = !s.surface || s.w != w || s.h != h;
    lr_->BeginHash();
    paint(*lr_);
    const uint64_t hv = lr_->EndHash();
    if (!realloc && !force && hv == lastHash) return false;   // nothing visible changed
    lastHash = hv;
    if (realloc) {
        lastDrawHr_ = gfx_.ResizeSurf(s, w, h);
        if (FAILED(lastDrawHr_)) return false;
    }
    lastDrawHr_ = gfx_.DrawSurf(s, [&](ID2D1DeviceContext* dc) {
        lr_->BeginDraw(dc, {0, 0});
        paint(*lr_);
        lr_->EndDraw();
    });
    return true;
}

void SettingsWindow::Render(bool force) {
    if (!hwnd_ || !deviceReady_ || !root_ || IsIconic(hwnd_)) return;
    if (!force && !dirtyAll_ && !host_.IsDirty()) return;
    force = force || dirtyAll_;
    dirtyAll_ = false;
    host_.ClearDirty();
    d2d_->SetScale(scale_);
    const pui::Theme& th = host_.GetTheme();
    bool changed = RenderLayer(content_, hContent_, force, [&](LayerRenderer& r) { root_->PaintTree(r, th); });
    if (host_.PopupCount() > 0 || host_.ToolTipVisible()) {
        changed |= RenderLayer(popup_, hPopup_, force, [&](LayerRenderer& r) {
            root_->SetPopupOnly(true);
            host_.Paint(r);
            root_->SetPopupOnly(false);
        });
    } else if (popup_.surface) {
        gfx_.ClearSurf(popup_);
        hPopup_ = 0;
        changed = true;
    }
    if (changed) {
        renders_++;
        gfx_.Commit();
    }
}

void SettingsWindow::Tick() {
    if (!hwnd_) return;
    host_.Tick();   // tooltips, animations, closed popups' destruction
    Render(false);
    UpdateTimer();
}

void SettingsWindow::UpdateTimer() {
    if (!hwnd_) return;
    const UINT ms = host_.NeedsTick() ? 16 : 100;
    if (ms == timerMs_) return;
    timerMs_ = ms;
    SetTimer(hwnd_, kTickTimer, ms, nullptr);
}

unsigned SettingsWindow::Mods() const {
    return (GetKeyState(VK_SHIFT) < 0 ? pui::ModShift : 0u) | (GetKeyState(VK_CONTROL) < 0 ? pui::ModCtrl : 0u) |
           (GetKeyState(VK_MENU) < 0 ? pui::ModAlt : 0u);
}

pui::PointerEvent SettingsWindow::Pointer(LPARAM lp, pui::PointerButton b, int clicks) const {
    pui::PointerEvent e;
    e.pt = {GET_X_LPARAM(lp) / scale_, GET_Y_LPARAM(lp) / scale_};
    e.button = b;
    e.clickCount = clicks;
    e.mods = Mods();
    return e;
}

LRESULT CALLBACK SettingsWindow::StaticProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        if (cs->lpCreateParams) static_cast<SettingsWindow*>(cs->lpCreateParams)->hwnd_ = h;
    }
    auto* w = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCDESTROY) SetWindowLongPtrW(h, GWLP_USERDATA, 0);
    if (!w || w->hwnd_ != h) return DefWindowProcW(h, m, wp, lp);
    return w->Proc(h, m, wp, lp);
}

LRESULT SettingsWindow::Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    nativemenu::OwnerScope menuOwner(h);   // a native menu opened from here belongs to this window
    switch (m) {
    case WM_GETOBJECT:
        if (uia_) { LRESULT r = 0; if (uia_->HandleGetObject(wp, lp, &r)) return r; }
        break;
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        const UINT dpi = GetDpiForWindow(h);
        RECT r{0, 0, (LONG)std::lround(kMinWidthDip * dpi / 96.f), (LONG)std::lround(kMinHeightDip * dpi / 96.f)};
        AdjustWindowRectExForDpi(&r, kStyle, FALSE, 0, dpi);
        mm->ptMinTrackSize = {r.right - r.left, r.bottom - r.top};
        return 0;
    }
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) Relayout();
        else host_.CloseAllPopups();
        return 0;
    case WM_DPICHANGED: {
        scale_ = HIWORD(wp) / 96.f;
        auto* r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        d2d_->SetScale(scale_);
        PushTheme();   // theme.hairline follows the scale
        cw_ = ch_ = 0;
        Relayout();
        return 0;
    }
    case WM_TIMER:
        if (wp == kTickTimer) Tick();
        return 0;
    case WM_SETTINGS_REDRAW:
        redrawPosted_ = false;
        Render(false);
        UpdateTimer();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            HWND other = reinterpret_cast<HWND>(lp);
            const bool ownDialog = other && GetWindow(other, GW_OWNER) == h;   // the caption font picker keeps the popups
            if (!ownDialog) host_.CloseAllPopups();
            Render(false);
        } else if (!HIWORD(wp) && FromOtherApp(reinterpret_cast<HWND>(lp)) && ctx_.MainHwnd()) {
            StartFocusCycle(ctx_.MainHwnd());   // the app came back from another application through this window
        }
        break;
    case WM_MOUSEMOVE:
        if (!mouseTracked_) { TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0}; TrackMouseEvent(&t); mouseTracked_ = true; }
        host_.PointerMove(Pointer(lp, pui::PointerButton::None));
        Render(false);
        UpdateTimer();
        return 0;
    case WM_MOUSELEAVE:
        mouseTracked_ = false;
        host_.PointerLeave();
        Render(false);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (IsWindowVisible(h)) SetFocus(h);   // never on the hidden test window
        host_.PointerDown(Pointer(lp, pui::PointerButton::Left, m == WM_LBUTTONDBLCLK ? 2 : 1));
        if (host_.Captured()) SetCapture(h);
        Render(false);
        return 0;
    case WM_LBUTTONUP:
        host_.PointerUp(Pointer(lp, pui::PointerButton::Left));
        if (GetCapture() == h) ReleaseCapture();
        Render(false);
        return 0;
    case WM_RBUTTONDOWN:
        host_.PointerDown(Pointer(lp, pui::PointerButton::Right));
        Render(false);
        return 0;
    case WM_RBUTTONUP:
        host_.PointerUp(Pointer(lp, pui::PointerButton::Right));
        Render(false);
        return 0;
    case WM_MOUSEWHEEL: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(h, &p);
        pui::PointerEvent e;
        e.pt = {p.x / scale_, p.y / scale_};
        e.wheelDelta = (float)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        e.mods = Mods();
        host_.PointerWheel(e);
        Render(false);
        return 0;
    }
    case WM_CAPTURECHANGED:
        host_.CancelCapture();   // a lost OS capture sends no pointer-up: also clears the pressed look
        Render(false);
        return 0;
    case WM_CONTEXTMENU:
        if (lp == -1 && host_.ContextMenuKey()) { Render(false); return 0; }   // Shift+F10 / Apps key
        break;   // the caption's system menu
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        const unsigned mods = Mods();
        const bool handled = host_.KeyDown((int)wp, mods);   // Esc closes an open flyout first
        if (!handled && wp == VK_ESCAPE && mods == 0) { Close(); return 0; }
        Render(false);
        if (handled) return 0;
        break;   // Alt+F4, Alt+Space ...
    }
    case WM_KEYUP:
    case WM_SYSKEYUP:
        host_.KeyUp((int)wp, Mods());
        break;
    case WM_CHAR:
        host_.Char((wchar_t)wp);
        Render(false);
        return 0;
    case WM_CLOSE:
        Close();
        return 0;
    case WM_DESTROY:
        OnDestroyed();
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

}
