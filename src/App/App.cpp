#include "App.h"
#include "WindowSizing.h"
#include "WindowSnap.h"

#include <dwmapi.h>
#include <ole2.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <new>

#include "features/FiltersPanel.h"
#include "features/Logging.h"
#include "features/LoggingFormat.h"
#include "features/MiniLogic.h"
#include "features/MiniPlayer.h"
#include "features/Menus.h"
#include "features/NativeMenu.h"
#include "features/OpenFlows.h"
#include "features/PlaybackExtras.h"
#include "features/SettingsLogic.h"
#include "features/SettingsService.h"
#include "features/SingleInstance.h"
#include "Localize.h"
#include "pui/Buttons.h"
#include "pui/TextBox.h"
#include "pui/ToolTipStyle.h"
#include "shell/SettingsWindow.h"
#include "wvp/Utf.h"

namespace app {

namespace {
App* g_app = nullptr;
HWND g_postHwnd = nullptr;
std::mutex g_qmu;
std::deque<std::function<void()>> g_queue;

bool IsMainOrProcessWindow(HWND candidate, HWND main) {
    if (!candidate || candidate == main || candidate == HWND_TOPMOST || candidate == HWND_BOTTOM || candidate == HWND_NOTOPMOST) return false;
    DWORD pid = 0;
    if (GetWindowThreadProcessId(candidate, &pid) && pid == GetCurrentProcessId()) return true;
    HWND owner = GetWindow(candidate, GW_OWNER);
    for (int depth = 0; owner && depth < 64; depth++) {
        if (owner == main) return true;
        HWND next = GetWindow(owner, GW_OWNER);
        if (next == owner) break;
        owner = next;
    }
    return false;
}

bool HighContrastOn() {
    HIGHCONTRASTW hc{};
    hc.cbSize = sizeof(hc);
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

pui::Color SysColorArgb(int index) {
    const COLORREF c = GetSysColor(index);
    return pui::Color(0xFF000000u | (uint32_t(GetRValue(c)) << 16) | (uint32_t(GetGValue(c)) << 8) | uint32_t(GetBValue(c)));
}

/// Windows high contrast: the translucent 4 % panel fill / 8 % borders vanish, so the panel, chips and dividers use the
/// system window and text colours (opaque).
void ApplyHighContrastTokens(pui::Theme& t) {
    t.panelFill = SysColorArgb(COLOR_WINDOW);
    t.panelStroke = t.chipStroke = t.dividerStroke = SysColorArgb(COLOR_WINDOWTEXT);
    t.textPrimary = t.textSecondary = t.textTertiary = SysColorArgb(COLOR_WINDOWTEXT);
    t.textDisabled = SysColorArgb(COLOR_GRAYTEXT);   // played rows in the playlist panel
}

bool ReadLightTheme() {
    DWORD v = 1, sz = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                     RRF_RT_REG_DWORD, nullptr, &v, &sz) != ERROR_SUCCESS) return false;
    return v != 0;
}

// AccentPalette entry 1 (Light2, dark theme) / entry 4 (Dark1, light theme); Zest fallback.
uint32_t ReadAccent(bool light) {
    BYTE buf[32] = {};
    DWORD sz = sizeof buf;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent", L"AccentPalette",
                     RRF_RT_REG_BINARY, nullptr, buf, &sz) == ERROR_SUCCESS && sz >= 20) {
        int i = light ? 4 : 1;
        return 0xFF000000u | ((uint32_t)buf[i * 4] << 16) | ((uint32_t)buf[i * 4 + 1] << 8) | buf[i * 4 + 2];
    }
    return light ? 0xFFD03F0A : 0xFFFF8232;
}

Microsoft::WRL::ComPtr<IDWriteFactory> MakeDw() {
    Microsoft::WRL::ComPtr<IDWriteFactory> f;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(f.GetAddressOf()));
    return f;
}

const char* ModeName(ShellMode m) {
    switch (m) {
    case ShellMode::NowPlaying: return "now-playing";
    case ShellMode::FullScreen: return "full-screen";
    default: return "mini-player";
    }
}

// Normal window style: resizable and with a system menu / snap support, but no WS_CAPTION (the title strip draws the caption
// buttons). Fullscreen exit and the mini player restore this one.
constexpr LONG_PTR kCaptionlessStyle = ((WS_OVERLAPPEDWINDOW & ~WS_CAPTION) | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX |
                                        WS_MAXIMIZEBOX | WS_CLIPCHILDREN);

std::wstring HeadlessDataDir() {
    wchar_t t[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, t);
    return std::wstring(t, n) + L"aribplayer-headless-data";
}

void DisableLegacyFileDrops(HWND h) {
    if (h && (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_ACCEPTFILES)) DragAcceptFiles(h, FALSE);
}
}

class AppDropTarget final : public IDropTarget {
public:
    explicit AppDropTarget(App& app) : app_(app) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IDropTarget)) {
            *out = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG refs = (ULONG)InterlockedDecrement(&refs_);
        if (!refs) delete this;
        return refs;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL point, DWORD* effect) override {
        if (!effect) return E_POINTER;
        hasFiles_ = HasFileDrop(data);
        const DWORD requested = *effect;
        *effect = hasFiles_ && (requested & DROPEFFECT_COPY) && AcceptsAt(point) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL point, DWORD* effect) override {
        if (!effect) return E_POINTER;
        const DWORD requested = *effect;
        *effect = hasFiles_ && (requested & DROPEFFECT_COPY) && AcceptsAt(point) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override { hasFiles_ = false; return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL point, DWORD* effect) override {
        if (!effect) return E_POINTER;
        const DWORD requested = *effect;
        *effect = DROPEFFECT_NONE;
        hasFiles_ = false;
        if (!data || !(requested & DROPEFFECT_COPY) || !HasFileDrop(data)) return S_OK;
        POINT client{};
        if (!ToClient(point, client) || app_.RouteDropAtClient(client) == DropRoute::Rejected) return S_OK;

        FORMATETC format = HdropFormat();
        STGMEDIUM medium{};
        if (FAILED(data->GetData(&format, &medium))) return S_OK;
        HDROP drop = reinterpret_cast<HDROP>(medium.hGlobal);
        std::vector<std::wstring> paths;
        UINT count = drop ? DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0) : 0;
        paths.reserve(count);
        for (UINT i = 0; i < count; ++i) {
            UINT length = DragQueryFileW(drop, i, nullptr, 0);
            std::vector<wchar_t> buffer((size_t)length + 1);
            UINT copied = DragQueryFileW(drop, i, buffer.data(), (UINT)buffer.size());
            if (copied) paths.emplace_back(buffer.data(), copied);
        }
        ReleaseStgMedium(&medium);
        if (app_.HandleDropAtClient(client, paths)) *effect = DROPEFFECT_COPY;
        return S_OK;
    }

private:
    static FORMATETC HdropFormat() { return {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}; }
    static bool HasFileDrop(IDataObject* data) {
        if (!data) return false;
        FORMATETC format = HdropFormat();
        return SUCCEEDED(data->QueryGetData(&format));
    }
    bool ToClient(POINTL screen, POINT& client) const {
        if (!app_.hwnd_) return false;
        client = {screen.x, screen.y};
        return ScreenToClient(app_.hwnd_, &client) != FALSE;
    }
    bool AcceptsAt(POINTL screen) const {
        POINT client{};
        return ToClient(screen, client) && app_.RouteDropAtClient(client) != DropRoute::Rejected;
    }

    App& app_;
    LONG refs_ = 1;
    bool hasFiles_ = false;
};

App* App::Instance() { return g_app; }

App::App(const AppOptions& o) : opt_(o), dw_(MakeDw()), d2d_p_(std::make_unique<pui::D2dRenderer>(dw_.Get())), d2d_(*d2d_p_), dbgPainter_(dw_.Get()) {
    dataDir_ = o.headless ? HeadlessDataDir() : wvp::SettingsStore::AppDataDir();
    svc_ = o.headless ? std::make_unique<Services>(dataDir_) : std::make_unique<Services>();
    lr_ = std::make_unique<LayerRenderer>(d2d_);
}

App::~App() {
    RevokeDropTargets();
    if (probeThread_.joinable()) probeThread_.join();
    if (adapterProbeThread_.joinable()) adapterProbeThread_.join();
    if (pui::DefaultMeasurer() == &d2d_) pui::SetDefaultMeasurer(nullptr);
    if (g_app == this) g_app = nullptr;
}

void App::Step(const char* name, HRESULT hr) {
    steps_.emplace_back(name, hr);
    if (opt_.headless) { fprintf(stderr, "STEP %s %08X\n", name, (unsigned)hr); fflush(stderr); }
}

void App::PostToUi(std::function<void()> fn) {
    { std::lock_guard<std::mutex> lk(g_qmu); g_queue.push_back(std::move(fn)); }
    if (g_postHwnd) PostMessageW(g_postHwnd, WM_RUN_QUEUE, 0, 0);
}

void App::RequestRedraw() {
    if (redrawPosted_ || !hwnd_ || destroyed_) return;
    redrawPosted_ = true;
    PostMessageW(hwnd_, WM_REDRAW, 0, 0);
}

void App::SetTimerMs(UINT ms) {
    if (ms == timerMs_ || !hwnd_) return;
    timerMs_ = ms;
    SetTimer(hwnd_, 1, ms, nullptr);
}

bool App::Create(bool hidden) {
    hidden_ = hidden;
    g_app = this;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_DBLCLKS;   // WM_LBUTTONDBLCLK -> clickCount 2 (list rows invoke on double click)
    wc.lpfnWndProc = StaticProc; wc.hInstance = inst; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);   // black = transparent over the extended DWM frame (Mica)
    // app.rc icon 1: window caption / Alt+Tab (large) and taskbar / title bar (small)
    wc.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                             GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR | LR_SHARED));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR | LR_SHARED));
    wc.lpszClassName = L"aribplayer.Main";
    RegisterClassExW(&wc);
    WNDCLASSEXW vc{sizeof vc};
    vc.style = CS_DBLCLKS; vc.lpfnWndProc = VideoProc; vc.hInstance = inst; vc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    vc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH); vc.lpszClassName = L"aribplayer.Video";
    RegisterClassExW(&vc);

    scale_ = GetDpiForSystem() / 96.f;
    RECT wa{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int w = S(1280), h = S(720);
    int x = wa.left + std::max(0, ((int)(wa.right - wa.left) - w) / 2), y = wa.top + std::max(0, ((int)(wa.bottom - wa.top) - h) / 2);
    // hidden (--headless-check / --logictest): never activatable, so a test run cannot take the user's focus / keystrokes
    hwnd_ = CreateWindowExW(hidden ? WS_EX_NOACTIVATE : 0, L"aribplayer.Main", L"ARIB Player", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            x, y, w, h, nullptr, nullptr, inst, this);
    Step("create_main_window", hwnd_ ? S_OK : HRESULT_FROM_WIN32(GetLastError()));
    if (!hwnd_) return false;
    g_postHwnd = hwnd_;
    float real = GetDpiForWindow(hwnd_) / 96.f;
    if (std::fabs(real - scale_) > 0.001f) {
        scale_ = real;
        SetWindowPos(hwnd_, nullptr, 0, 0, S(1280), S(720), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    d2d_.SetScale(scale_);

    int mica = 2;
    DwmSetWindowAttribute(hwnd_, 38, &mica, sizeof mica);
    MARGINS m{-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(hwnd_, &m);
    // Own caption buttons: no WS_CAPTION (WM_NCCALCSIZE keeps the resize borders, drops the caption band), Mica stays.
    SetWindowLongPtrW(hwnd_, GWL_STYLE, (GetWindowLongPtrW(hwnd_, GWL_STYLE) & ~(LONG_PTR)WS_CAPTION) | kCaptionlessStyle);
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    video_ = CreateWindowExW(0, L"aribplayer.Video", nullptr, WS_CHILD | WS_CLIPSIBLINGS, 0, S(kStripDip), w, h - S(kStripDip), hwnd_, nullptr, inst, nullptr);
    Step("create_video_child", video_ ? S_OK : HRESULT_FROM_WIN32(GetLastError()));
    if (!video_) return false;
    DragAcceptFiles(hwnd_, FALSE);
    DragAcceptFiles(video_, FALSE);

    HRESULT hr = gfx_.Init(hwnd_, [this](const char* n, HRESULT r) { Step(n, r); });
    gfxOk_ = SUCCEEDED(hr);
    if (gfxOk_) {   // z-order: kLayerStack
        for (Layer l : kLayerStack)
            if (SUCCEEDED(hr)) hr = gfx_.CreateVisual(LayerSurf(l));
        Step("dcomp_visuals", hr);
        if (SUCCEEDED(hr)) Step("dcomp_layer_zorder", LayerStackMatches() ? S_OK : E_FAIL);
        gfxOk_ = SUCCEEDED(hr);
    }
    if (gfxOk_) ArmGfx();

    auto root = std::make_unique<ShellRoot>(*this);
    shell_ = static_cast<ShellRoot*>(host_.SetRoot(std::move(root)));
    host_.focusFilter = [this](const pui::Widget* w) { return AllowsFocus(w); };
    pui::SetDefaultMeasurer(&d2d_);
    pui::InstallToolTipStyle(host_);
    ApplyTheme(pui::ThemeMode::System, 0);
    host_.onInvalidate = [this] { RequestRedraw(); };
    TitleBar* title = shell_->Title();
    title->onMainMenu = [this] {
        GlyphButton* b = shell_->Title()->MainMenuButton();
        ShowMainMenu(*this, b, {[b] { b->SetMenuOpen(true); }, [b] { b->SetMenuOpen(false); }});
    };
    title->onPin = [this] { Execute(CommandId::CycleAlwaysOnTop); };
    title->onMinimize = [this] { if (hwnd_) ShowWindow(hwnd_, SW_MINIMIZE); };
    title->onMaximize = [this] { if (hwnd_) ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE); };
    title->onExitFullscreen = [this] { Execute(CommandId::ToggleFullscreen); };
    title->onClose = [this] { Execute(CommandId::Exit); };
    title->SetScale(scale_);
    PlayerBar::Actions act;
    act.command = [this](CommandId c) { Execute(c); };
    act.seekTo = [this](double f) {
        if (player_ && model_.dur > 0) {
            double t = std::clamp(f, 0.0, 1.0) * model_.dur;
            LogSeek("bar", t, false); player_->Seek(t); SavePlaybackHistory(true, false, t);
        }
    };
    act.setVolume = [this](int v) { SetVolume(v); };
    act.subtitleMenu = [this](pui::Widget* a) { ShowSubtitleMenu(*this, a); };
    shell_->Bar()->SetActions(act);
    shell_->Bar()->SetScale(scale_);
    MiniBar::Actions ma;
    ma.command = [this](CommandId c) { Execute(c); };
    ma.seekTo = act.seekTo;
    shell_->Mini()->SetActions(ma);
    shell_->Mini()->SetScale(scale_);
    uia_ = std::make_unique<pui::UiaBridge>(hwnd_, host_, [this] { return scale_; });
    uia_->SetOffscreenPredicate([this](pui::Widget* w) { return IsAutoHiddenChrome(w); });

    LoadSettingsAtStartup(*this);
    RestoreCurrentPlaylist();      // load the saved current list before command-line inputs use it
    OnModeChanged.add([](ShellMode m) { log::Event("mode", ModeName(m)); });
    log::ApplyDebugLogging(svc_->settings.debugLogging, log::LogsDir(svc_->settingsStore.Path()));
    log::InstallEngineHooks();
    log::Event("app", std::string("start: libffplay ") + wvp::FfPlayer::Version() + ", debug overlay: Tab");
    const bool plannerOff = opt_.plannerOff;
    char vendor[8];
    snprintf(vendor, sizeof vendor, "%04x", gfx_.adapterVendor);
    log::Event("adapter", "UI device: " + wvp::ToUtf8(gfx_.adapterName) + " (vendor 0x" + vendor + ", luid " + gfx_.adapterLuid + ")");
    wvp::FfPlayer::SetProbeOptions(wvp::ToUtf8(ProbeOptions(L"", opt_)));
    std::vector<HwAdapter> hw = PhysicalAdapters("startup");
    adapterProbeBusy_ = !plannerOff;
    probeThread_ = std::thread([plannerOff, hw, o = opt_] {
        // The probes run in short-lived child processes (--probe-gpu, one per hardware adapter, in parallel): the GPU drivers
        // keep tens of MB of per-process state after probing (~50-70 MB private here) even with every device released.
        // In-process probing of the default adapter stays the fallback when no child answers.
        std::vector<wvp::AdapterCandidate> cands = ProbeAdapters(plannerOff ? std::vector<HwAdapter>{} : hw, o);
        if (plannerOff) {
            if (App* a = App::Instance()) a->PostToUi([] { log::Event("caps", "planner disabled (--planner=off)"); });
            return;
        }
        if (cands.empty()) {
            if (std::optional<wvp::CapabilitySet> caps = wvp::FfPlayer::ProbeCaps()) {
                wvp::AdapterCandidate c;
                c.luid = caps->adapter.luid; c.name = caps->adapter.name; c.vendorId = caps->adapter.vendorId; c.caps = std::move(*caps);
                cands.push_back(std::move(c));
            }
        }
        if (App* a = App::Instance()) a->PostToUi([a, cands = std::move(cands)]() mutable { a->OnAdaptersProbed(std::move(cands), "startup"); });
    });
    autoHide_.SetEnabled(false, Now());
    Relayout();
    ApplyWindowTopmost();
    SyncModel();
    InitPlayer();
    OnSystemCaptionSettingsChanged(*this);
    StartSystemCaptionSettingsWatch(*this);
    InitPlaybackExtras(*this);
    RegisterPrimaryInstance(hwnd_);
    if (FAILED(RegisterDropTargets())) return false;
    if (!hidden) {
        ShowWindow(hwnd_, svc_->settings.window.valid && svc_->settings.window.maximized ? SW_SHOWMAXIMIZED : SW_SHOW);
        SetTimerMs(100);
        SetFocus(hwnd_);
        if (!opt_.inputs.empty()) OpenPaths(*this, opt_.inputs);
    }
    return true;
}

Surf& App::LayerSurf(Layer l) {
    switch (l) {
    case Layer::Content: return sContent_;
    case Layer::Panel: return sPanel_;
    case Layer::DebugOverlay: return sDebug_;
    case Layer::Title: return sTitle_;
    case Layer::Bar: return sBar_;
    default: return sPopup_;
    }
}

HRESULT App::RegisterDropTargets() {
    if (!hwnd_ || !video_) return E_HANDLE;
    if (dropTarget_) return S_FALSE;
    auto* target = new (std::nothrow) AppDropTarget(*this);
    if (!target) return E_OUTOFMEMORY;

    HRESULT hr = RegisterDragDrop(hwnd_, target);
    Step("register_drop_target_main", hr);
    if (FAILED(hr)) { target->Release(); return hr; }
    dropTargetMainRegistered_ = true;

    hr = RegisterDragDrop(video_, target);
    Step("register_drop_target_video", hr);
    if (FAILED(hr)) {
        RevokeDragDrop(hwnd_);
        dropTargetMainRegistered_ = false;
        target->Release();
        return hr;
    }
    dropTargetVideoRegistered_ = true;
    dropTarget_ = target;
    return S_OK;
}

void App::RevokeDropTargets() {
    if (dropTargetVideoRegistered_ && video_) RevokeDragDrop(video_);
    dropTargetVideoRegistered_ = false;
    if (dropTargetMainRegistered_ && hwnd_) RevokeDragDrop(hwnd_);
    dropTargetMainRegistered_ = false;
    if (dropTarget_) { dropTarget_->Release(); dropTarget_ = nullptr; }
}

void App::Destroy() {
    RevokeDropTargets();
    destroyed_ = true;
    { std::lock_guard<std::mutex> lk(g_qmu); g_queue.clear(); }
    g_postHwnd = nullptr;
    gfx_.StopRemovalWatch();
    settingsWin_.reset();   // destroys its window and hidden owner; drops its composition objects
    sPopup_ = {}; sBar_ = {}; sTitle_ = {}; sDebug_ = {}; sPanel_ = {}; sContent_ = {};
    gfx_.root.Reset(); gfx_.target.Reset();
}

int App::RunGui() {
    if (!Create(false)) return 1;
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    Destroy();
    return (int)msg.wParam;
}

void App::ApplyTheme(pui::ThemeMode mode, uint32_t accent) {
    themeMode_ = mode;
    accentOverride_ = accent;
    bool light = mode == pui::ThemeMode::Light ? true : mode == pui::ThemeMode::Dark ? false : ReadLightTheme();
    uint32_t acc = accent ? accent : ReadAccent(light);
    pui::Theme theme = pui::Theme::Make(light, pui::Color(acc));
    theme.hairline = 1.f / std::max(0.25f, scale_);
    themeHighContrast_ = HighContrastOn();
    if (themeHighContrast_) ApplyHighContrastTokens(theme);
    themeLight_ = light;
    host_.SetTheme(theme);
    if (settingsWin_) settingsWin_->ApplyTheme(SettingsTheme(), light);
    nativemenu::ApplyTheme(mode);   // native popup menus: uxtheme app mode (System follows the OS, Dark / Light force)
    UpdateAcrylicState(true);
    if (hwnd_) {
        BOOL dark = light ? FALSE : TRUE;
        DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof dark);
        // The 1 px DWM frame is translucent: it takes the tint of whatever lies behind it (Mica in the strip, the dark body below),
        // so it looked bright around the title strip and dark under it. One opaque colour keeps it even along the whole edge.
        const COLORREF border = themeHighContrast_ ? 0xFFFFFFFFu /*DWMWA_COLOR_DEFAULT*/ : (light ? RGB(0xA0, 0xA0, 0xA0) : RGB(0x3C, 0x3C, 0x3C));
        DwmSetWindowAttribute(hwnd_, 34 /*DWMWA_BORDER_COLOR*/, &border, sizeof border);
    }
    OnThemeChanged.raise(host_.GetTheme());
    RequestRedraw();
}

pui::Theme App::SettingsTheme() const {
    pui::Theme t = host_.GetTheme();
    t.acrylicEnabled = false;   // in-video Acrylic is the main window's; flyouts there use the opaque fallback surface
    t.acrylicFallbackColor = themeHighContrast_ ? SysColorArgb(COLOR_WINDOW) : t.flyoutSurface;
    return t;
}

void App::ShowSettings() {
    if (!hwnd_ || destroyed_) return;
    if (!settingsWin_) settingsWin_ = std::make_unique<SettingsWindow>(*this, gfx_, dw_.Get());
    if (!settingsWin_->IsOpen()) settingsWin_->ApplyTheme(SettingsTheme(), themeLight_);
    if (!settingsWin_->Open(hidden_, gfxOk_)) log::Event("settings", "cannot create the Settings window", log::kError);
    else settingsWin_->StartFocusCycle(hwnd_);
}

void App::ApplyWindowFullscreen(bool on) {
    const DWORD vis = hidden_ ? 0 : WS_VISIBLE;   // a hidden-window run must never become visible
    if (on) {
        wasMax_ = IsZoomed(hwnd_) != 0;
        if (wasMax_) ShowWindow(hwnd_, SW_RESTORE);
        GetWindowRect(hwnd_, &restoreRect_);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_POPUP | vis | WS_CLIPCHILDREN);
        SetWindowPos(hwnd_, nullptr, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        SetWindowLongPtrW(hwnd_, GWL_STYLE, kCaptionlessStyle | vis);
        SetWindowPos(hwnd_, nullptr, restoreRect_.left, restoreRect_.top, restoreRect_.right - restoreRect_.left,
                     restoreRect_.bottom - restoreRect_.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
        if (wasMax_ && !hidden_) ShowWindow(hwnd_, SW_MAXIMIZE);
    }
    ApplyWindowTopmost();   // Always on top "While fullscreen": modes_ is already updated by every caller; enter and exit both land here
}

void App::SetFullscreen(bool on) {
    ModeState ns = WithFullscreen(modes_, on);
    if (ns == modes_) return;
    ModeState old = modes_;
    modes_ = ns;
    if (old.mini != ns.mini) { ExitMiniPlayer(*this); ApplyWindowTopmost(); }   // leave mini BEFORE the full-screen style swap
    if (old.fullscreen != ns.fullscreen) ApplyWindowFullscreen(ns.fullscreen);
    ApplyMode();
    OnModeChanged.raise(modes_.Mode());
}

void App::SetMiniPlayer(bool on) {
    if (on && !MediaActive()) return;
    ModeState ns = WithMini(modes_, on);
    if (ns == modes_) return;
    ModeState old = modes_;
    modes_ = ns;
    if (old.fullscreen != ns.fullscreen) ApplyWindowFullscreen(ns.fullscreen);
    if (on) EnterMiniPlayer(*this); else ExitMiniPlayer(*this);
    ApplyWindowTopmost();
    ApplyMode();
    OnModeChanged.raise(modes_.Mode());
}

void App::NotePlayerPressed() {
    if (skipActivationPress_) { skipActivationPress_ = false; return; }
    if (settingsWin_) settingsWin_->OnPlayerPressed(hwnd_);
}

void App::NoteTopmostReasserted() {
    if (settingsWin_) settingsWin_->KeepAbove(hwnd_);
}

void App::ApplyWindowTopmost() {
    if (!hwnd_) return;
    if (modes_.mini) { topmostWanted_ = true; return; }   // the mini player sets its own topmost; leaving it re-applies the setting
    bool want = sl::ShouldBeTopmost(svc_->settings.alwaysOnTop, state_ == wvp::PlayerState::Playing, modes_.fullscreen);
    topmostWanted_ = want;
    bool is = (GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    // Wanted: always re-assert HWND_TOPMOST (the style bit alone does not prove the window is still in the topmost band).
    // Not wanted: only leave the band when we are in it (never toggle through NOTOPMOST needlessly).
    if (want) SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    else if (is) SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    NoteTopmostReasserted();
}

namespace {
struct SnapEnum { HWND self; std::vector<PxRect>* wins; std::vector<PxRect>* areas; };
bool VisibleFrame(HWND h, RECT& vis) {
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &vis, sizeof vis))) return true;
    return GetWindowRect(h, &vis) != 0;
}
}

/// WM_MOVING / WM_SIZING: snaps the proposed window rect (lp -> RECT) to the monitor work areas and to the other player
/// windows (class aribplayer.Main only) with hysteresis (SnapTracker), measured on the visible frame so the invisible resize borders leave no gap.
void App::SnapWindowMessage(LPARAM lp, bool move, int sizingEdge) {
    auto* r = reinterpret_cast<RECT*>(lp);
    RECT cur{}, vis{};
    if (!GetWindowRect(hwnd_, &cur) || !VisibleFrame(hwnd_, vis)) return;
    const int invL = std::max(0L, vis.left - cur.left), invT = std::max(0L, vis.top - cur.top);
    const int invR = std::max(0L, cur.right - vis.right), invB = std::max(0L, cur.bottom - vis.bottom);
    PxRect v{r->left + invL, r->top + invT, r->right - invR, r->bottom - invB};
    std::vector<PxRect> areas, wins;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hm, HDC, LPRECT, LPARAM p) -> BOOL {
        // Both the work area (taskbar edge) and the whole monitor (screen edge) are targets.
        MONITORINFO mi{sizeof mi};
        if (GetMonitorInfoW(hm, &mi)) {
            auto* out = static_cast<std::vector<PxRect>*>(reinterpret_cast<void*>(p));
            out->push_back({mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom});
            if (!EqualRect(&mi.rcWork, &mi.rcMonitor))
                out->push_back({mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom});
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&areas));
    SnapEnum se{hwnd_, &wins, &areas};
    EnumWindows([](HWND h, LPARAM p) -> BOOL {
        auto* e = reinterpret_cast<SnapEnum*>(p);
        if (h == e->self || !IsWindowVisible(h) || IsIconic(h)) return TRUE;
        wchar_t cls[32]{};
        if (GetClassNameW(h, cls, 32) == 0 || wcscmp(cls, L"aribplayer.Main") != 0) return TRUE;
        RECT o{};
        if (VisibleFrame(h, o)) e->wins->push_back({o.left, o.top, o.right, o.bottom});
        return TRUE;
    }, reinterpret_cast<LPARAM>(&se));
    SnapEdges drag;
    if (!move) {
        drag.l = sizingEdge == WMSZ_LEFT || sizingEdge == WMSZ_TOPLEFT || sizingEdge == WMSZ_BOTTOMLEFT;
        drag.r = sizingEdge == WMSZ_RIGHT || sizingEdge == WMSZ_TOPRIGHT || sizingEdge == WMSZ_BOTTOMRIGHT;
        drag.t = sizingEdge == WMSZ_TOP || sizingEdge == WMSZ_TOPLEFT || sizingEdge == WMSZ_TOPRIGHT;
        drag.b = sizingEdge == WMSZ_BOTTOM || sizingEdge == WMSZ_BOTTOMLEFT || sizingEdge == WMSZ_BOTTOMRIGHT;
    }
    POINT cp{};
    GetCursorPos(&cp);
    PxRect o = snap_.Step(cp.x, cp.y, v, move, drag, areas, wins, S(kSnapThresholdDip), S(kMinWindowWidthDip) - invL - invR, S(kMinWindowHeightDip) - invT - invB);
    *r = {o.l - invL, o.t - invT, o.r + invR, o.b + invB};
}

void App::SizeWindowToVideo(double scale, std::optional<PxSize> knownVideo) {
    if (!hwnd_) return;
    // No video dimensions known (nothing open, or audio only): size to the nominal kFallbackVideoW x kFallbackVideoH.
    std::optional<PxSize> size;
    bool haveVideo = false;
    auto mi = (!knownVideo && player_ && MediaActive()) ? player_->GetMediaInfo() : std::nullopt;
    if (knownVideo) { size = knownVideo; haveVideo = true; }   // the open-time call: Prepared already told the display size
    if (mi) {
        const wvp::StreamInfo* vs = nullptr;
        for (const auto& st : mi->Streams)
            if (st.Type == wvp::StreamType::Video && (!vs || st.Selected)) vs = &st;
        if (vs) {
            wvp::Rotation rot = svc_->filters.Settings().rotation;
            size = VideoDisplaySize(vs->Width, vs->Height, vs->SarNum, vs->SarDen, rot == wvp::Rotation::Cw90 || rot == wvp::Rotation::Ccw90);
            haveVideo = size.has_value();
        }
    }
    if (!size) size = PxSize{kFallbackVideoW, kFallbackVideoH};
    // restore first: the video rect is the Now playing one of a normal window
    if (modes_.mini) SetMiniPlayer(false);
    if (modes_.fullscreen) SetFullscreen(false);
    if (IsZoomed(hwnd_) || IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    Relayout();
    RECT wr{}; GetWindowRect(hwnd_, &wr);
    if (haveVideo && layout_.video.Empty()) return;
    VideoWindowIn in;
    in.video = *size;
    in.scale = scale;
    in.window = {wr.left, wr.top, wr.right, wr.bottom};
    in.minW = S(kMinWindowWidthDip);
    in.minH = S(kMinWindowHeightDip);
    // Window size minus the VIDEO rect for the requested size: non-client frame + (strip, dock | gutters, panel). The panel
    // is solved for the target width (it docks, or not, depending on the window width), not read off the current layout.
    RECT cr{}; GetClientRect(hwnd_, &cr);
    const int ncW = (wr.right - wr.left) - (int)cr.right, ncH = (wr.bottom - wr.top) - (int)cr.bottom;
    const bool panelWanted = svc_->settings.playlistPanel;
    const int targetW = scale > 0 ? (int)std::lround(size->w * scale) : size->w;
    const int targetH = scale > 0 ? (int)std::lround(size->h * scale) : size->h;
    in.extraW = ncW + ClientWidthForVideo(targetW, (float)scale_, panelWanted, (float)svc_->settings.playlistPanelWidthDip) - targetW;
    in.extraH = ncH + ClientHeightForVideo(targetH, (float)scale_) - targetH;
    RECT vis{};
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd_, DWMWA_EXTENDED_FRAME_BOUNDS, &vis, sizeof vis))) {
        in.invL = std::max(0L, vis.left - wr.left); in.invT = std::max(0L, vis.top - wr.top);
        in.invR = std::max(0L, wr.right - vis.right); in.invB = std::max(0L, wr.bottom - vis.bottom);
    }
    MONITORINFO mon{sizeof mon};
    GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mon);
    in.work = {mon.rcWork.left, mon.rcWork.top, mon.rcWork.right, mon.rcWork.bottom};
    PxRect r = VideoWindowRect(in);
    SetWindowPos(hwnd_, nullptr, r.l, r.t, r.w(), r.h(), SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK App::StaticProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        if (cs->lpCreateParams) static_cast<App*>(cs->lpCreateParams)->hwnd_ = h;
        nativemenu::SetOwner(h);
    }
    auto* a = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!a) return DefWindowProcW(h, m, wp, lp);
    return a->Proc(h, m, wp, lp);
}

// The video child lets clicks near the parent's resize borders, over the (hittable) bar and while a popup is open fall
// through to the parent so the shell sees them.
LRESULT CALLBACK App::VideoProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    App* a = g_app;
    if (m == WM_NCHITTEST && a && a->hwnd_ && !a->destroyed_) {
        if (a->modes_.mini) return HTTRANSPARENT;   // mini player: the whole window is draggable, the parent decides
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(a->hwnd_, &p);
        if (!a->modes_.fullscreen && !IsZoomed(a->hwnd_)) {
            int b = a->S(6);
            if (p.x < b || p.x >= a->cw_ - b || p.y >= a->ch_ - b) return HTTRANSPARENT;
        }
        if (a->host_.PopupCount() > 0) return HTTRANSPARENT;
        const float xDip = p.x / a->scale_, yDip = p.y / a->scale_;
        if ((a->layout_.bar.Contains(xDip, yDip) || (a->layout_.titleSlides && a->layout_.title.Contains(xDip, yDip))) &&
            a->ChromeHittableAt(a->Now(), {xDip, yDip}))
            return HTTRANSPARENT;
    }
    if (a && a->hwnd_ && !a->destroyed_ &&
        (m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN || m == WM_MBUTTONDOWN || m == WM_LBUTTONDBLCLK || m == WM_RBUTTONDBLCLK || m == WM_MBUTTONDBLCLK))
        a->NotePlayerPressed();
    // Bottom of the chain: SDL's procedure (above) has already queued its button-down event when the press gets here.
    if (m == WM_LBUTTONDOWN && a && a->hwnd_ && !a->destroyed_) a->VideoPressDrag(h, lp);
    return DefWindowProcW(h, m, wp, lp);
}

// Dragging the video moves the window (native move loop: Aero Snap, drag-to-restore from maximized). The move
// loop starts synchronously inside the real WM_LBUTTONDOWN dispatch, exactly like a title-bar press: started later from
// the posted INPUT events (UI -> libffplay loop thread -> UI round trip, the loop nested in FfPlayer::DispatchPendingEvents)
// it missed presses that moved at once. DragDetect / the move loop eat the real WM_LBUTTONUP, so a posted one resyncs
// SDL (it ignores a press while it still thinks the button is down: no down / double click after that) and gives the app
// its Up event; a plain click therefore stays a click (Down + Up) and double click still reaches SDL as a second press.
void App::VideoPressDrag(HWND video, LPARAM lp) {
    if (modes_.fullscreen || modes_.mini || !IsWindowVisible(hwnd_) || !(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) return;
    POINT press{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    ClientToScreen(video, &press);
    const ULONGLONG t0 = GetTickCount64();
    const bool dragged = DragDetect(hwnd_, press) != FALSE;
    POINT cur; GetCursorPos(&cur);
    log::Event("video", "press (" + std::to_string(press.x) + "," + std::to_string(press.y) + ") " + (dragged ? "drag" : "click") +
               " after " + std::to_string(GetTickCount64() - t0) + " ms at (" + std::to_string(cur.x) + "," + std::to_string(cur.y) + ")",
               log::kDebug);
    if (dragged && !destroyed_) {
        host_.CancelCapture();
        ReleaseCapture();
        // the press point (not the cursor now) starts the loop: the window follows the full distance already travelled
        SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM((short)press.x, (short)press.y));
        log::Event("video", "move loop ended after " + std::to_string(GetTickCount64() - t0) + " ms", log::kDebug);
    }
    if (!IsWindow(video)) return;
    GetCursorPos(&cur);
    ScreenToClient(video, &cur);
    PostMessageW(video, WM_LBUTTONUP, 0, MAKELPARAM((short)cur.x, (short)cur.y));
}

namespace { WNDPROC g_videoPrevProc = nullptr; }

LRESULT CALLBACK App::VideoDropProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_DROPFILES) {   // OLE handles all accepted drops; never leak legacy drops into SDL's ignored SDL_DROPFILE path.
        DragFinish(reinterpret_cast<HDROP>(wp));
        return 0;
    }
    return g_videoPrevProc ? CallWindowProcW(g_videoPrevProc, h, m, wp, lp) : DefWindowProcW(h, m, wp, lp);
}

void App::EnsureVideoDropHook() {
    if (!video_) return;
    DisableLegacyFileDrops(video_);   // SDL may toggle WS_EX_ACCEPTFILES while attaching its Win32 drop handler.
    if (VideoDropHooked()) return;
    LONG_PTR cur = GetWindowLongPtrW(video_, GWLP_WNDPROC);
    if (cur == reinterpret_cast<LONG_PTR>(&App::VideoProc)) return;   // SDL has not subclassed yet: hooking now would sit BELOW SDL's proc
    g_videoPrevProc = reinterpret_cast<WNDPROC>(cur);
    SetWindowLongPtrW(video_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&App::VideoDropProc));
    DisableLegacyFileDrops(video_);
}

void App::UnhookVideoDrop() {
    if (video_ && g_videoPrevProc && VideoDropHooked()) SetWindowLongPtrW(video_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_videoPrevProc));
}

DropRoute App::RouteDropAtClient(POINT client) const {
    const ShellMode mode = Mode();
    const pui::PointF dip{client.x / scale_, client.y / scale_};
    const bool pointInVideo = !layout_.video.Empty() && client.x >= layout_.video.l && client.x < layout_.video.r &&
                              client.y >= layout_.video.t && client.y < layout_.video.b;
    bool pointOverPopup = host_.HasModalPopup();   // modal dialogs block the underlying client even outside their card bounds
    for (pui::Widget* popup : host_.Popups()) {
        if (popup && popup->IsEffectivelyVisible() && (popup->Bounds().Contains(dip) || popup->HitTest(dip))) {
            pointOverPopup = true;
            break;
        }
    }
    if (!pointOverPopup && host_.ToolTipVisible()) {
        pui::RectF tooltip = pui::ToolTipBounds(d2d_, host_.ToolTipText(), host_.ToolTipAnchor(), host_.ToolTipPoint(), host_.Size());
        pointOverPopup = tooltip.Contains(dip);
    }
    if (!pointOverPopup && mode == ShellMode::NowPlaying && shell_ && shell_->Panel()->Visible() && layout_.panel.Contains(dip.x, dip.y))
        return shell_->Panel()->AcceptsDrops() ? DropRoute::PlaylistPanel : DropRoute::Rejected;   // docked playlist panel
    return RouteDrop(pointInVideo, pointOverPopup, layout_.bar.Contains(dip.x, dip.y), layout_.title.Contains(dip.x, dip.y));
}

bool App::HandleDropAtClient(POINT client, const std::vector<std::wstring>& files) {
    if (files.empty() || destroyed_ || !hwnd_) return false;
    const DropRoute route = RouteDropAtClient(client);
    if (route == DropRoute::Rejected) return false;
    const pui::PointF dip{client.x / scale_, client.y / scale_};
    if (route == DropRoute::PlaylistPanel) {
        if (!shell_->Panel()->DropPaths(files, dip)) return false;
        dropEvents_++;
        return true;
    }
    if (!DispatchDroppedPaths(*this, route, files)) return false;
    dropEvents_++;
    return true;
}

LRESULT App::NcHitTest(LPARAM lp) {
    POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    ScreenToClient(hwnd_, &p);
    if (modes_.mini) {
        if (!IsZoomed(hwnd_)) {
            int b = S(6), t = S(4);
            bool l = p.x < b, r = p.x >= cw_ - b, tp = p.y < t, bt = p.y >= ch_ - b;
            if (tp && l) return HTTOPLEFT;
            if (tp && r) return HTTOPRIGHT;
            if (bt && l) return HTBOTTOMLEFT;
            if (bt && r) return HTBOTTOMRIGHT;
            if (l) return HTLEFT;
            if (r) return HTRIGHT;
            if (tp) return HTTOP;
            if (bt) return HTBOTTOM;
        }
        return ClassifyMiniPoint(p.x, p.y, cw_, ch_, scale_, BarHittable(Now())) == MiniZone::Caption ? HTCAPTION : HTCLIENT;
    }
    if (modes_.fullscreen || !shell_) return HTCLIENT;
    // Title buttons: the maximize button is HTMAXBUTTON (Windows 11 snap layouts), the others plain client; the rest of the
    // strip drags the window.
    const pui::PointF dip{p.x / scale_, p.y / scale_};
    const TitleZone zone = shell_->Title()->Visible() ? shell_->Title()->Layout().Classify(dip) : TitleZone::None;
    if (zone == TitleZone::Max) return HTMAXBUTTON;
    if (zone == TitleZone::MainMenu || zone == TitleZone::Pin || zone == TitleZone::Min || zone == TitleZone::Close)
        return HTCLIENT;
    LRESULT d = DefWindowProcW(hwnd_, WM_NCHITTEST, 0, lp);
    if (d != HTCLIENT) return d;   // left / right / bottom borders are real non-client area (see WM_NCCALCSIZE)
    if (!IsZoomed(hwnd_)) {        // the top resize band lies inside the client (the caption was removed)
        const UINT dpi = GetDpiForWindow(hwnd_);
        const int by = GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        const int bx = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        if (p.y >= 0 && p.y < by) return p.x < bx ? HTTOPLEFT : p.x >= cw_ - bx ? HTTOPRIGHT : HTTOP;
    }
    if (zone == TitleZone::Drag) {
        // a flyout (menu, toast) reaching into the strip is client area, not a drag handle
        if (host_.PopupCount() > 0) {
            const pui::Widget* w = host_.HitTest(dip);
            if (w && !shell_->IsAncestorOf(w)) return HTCLIENT;
        }
        return HTCAPTION;
    }
    return HTCLIENT;
}

bool App::SplitterAt(POINT p) const {
    const RectPx& r = layout_.splitterPx;
    return shell_ && modes_.Mode() == ShellMode::NowPlaying && !layout_.panelOverlay && !r.Empty() && !UserPopupOpen() &&
           p.x >= r.l && p.x < r.r && p.y >= r.t && p.y < r.b;
}

bool App::BeginSplitterDrag(LPARAM lp, bool doubleClick) {
    const POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    if (!SplitterAt(p)) return false;
    if (doubleClick) {   // reset to the default width
        SetPanelWidth((int)kPanelDefaultDip);
        svc_->settingsStore.Save(svc_->settings);
        return true;
    }
    splitDrag_ = true;
    splitGrabPx_ = p.x - layout_.splitterPx.l;
    SetCapture(hwnd_);
    return true;
}

void App::DragSplitter(int xPx) {
    // The panel's left edge follows the splitter's left edge plus the gap; its right edge stays.
    const int panelW = layout_.panelPx.r - (xPx - splitGrabPx_ + S(kBodyGapDip));
    SetPanelWidth((int)std::lround(panelW / scale_));
}

void App::SetPanelWidth(int widthDip) {
    if (widthDip == svc_->settings.playlistPanelWidthDip) return;
    svc_->settings.playlistPanelWidthDip = widthDip;
    ApplyMode();
    if (!layout_.panelPx.Empty()) svc_->settings.playlistPanelWidthDip = (int)std::lround(layout_.panelPx.w() / scale_);   // as clamped by the layout
}

bool App::EndSplitterDrag() {
    if (!splitDrag_) return false;
    splitDrag_ = false;
    svc_->settingsStore.Save(svc_->settings);
    return true;
}

LPARAM App::NcClientLParam(LPARAM screenLp) const {
    POINT p{GET_X_LPARAM(screenLp), GET_Y_LPARAM(screenLp)};
    ScreenToClient(hwnd_, &p);
    return MAKELPARAM((short)p.x, (short)p.y);
}

void App::OnClose() {
    if (destroyed_) return;
    SavePlaybackHistory(true);
    if (settingsWin_) settingsWin_->Close();   // persists its placement before the settings file is written below
    if (modes_.mini) ExitMiniPlayer(*this);   // so settings.window is the normal placement, not the mini rect
    SaveSettingsAtShutdown(*this);
    StopSystemCaptionSettingsWatch();
    KillTimer(hwnd_, 1); KillTimer(hwnd_, 2);
    SetCursorHidden(false);
    uia_.reset();                       // disconnect providers before the tree goes away
    host_.CloseAllPopups();
    RevokeDropTargets();
    UnhookVideoDrop();
    ctl_.reset();                       // dispose controller + player before the video child HWND goes away
    player_.reset();
    log::Event("app", "closed");
    log::RemoveEngineHooks();
    DestroyWindow(hwnd_);
}

pui::PointerEvent App::MakePointer(LPARAM lp, pui::PointerButton b, int clicks) const {
    pui::PointerEvent e;
    e.pt = {GET_X_LPARAM(lp) / scale_, GET_Y_LPARAM(lp) / scale_};
    e.button = b;
    e.clickCount = clicks;
    e.mods = CurrentMods();
    return e;
}

unsigned App::CurrentMods() const {
    if (modsOverride_ >= 0) return (unsigned)modsOverride_;
    return (GetKeyState(VK_SHIFT) < 0 ? pui::ModShift : 0u) | (GetKeyState(VK_CONTROL) < 0 ? pui::ModCtrl : 0u) |
           (GetKeyState(VK_MENU) < 0 ? pui::ModAlt : 0u);
}

bool App::BarHittable(uint64_t now) const {
    if (!gfxOk_ || minimized_) return false;
    return autoHide_.Hittable(now);
}

bool App::ChromeHittableAt(uint64_t now, pui::PointF dip) const {
    if (!gfxOk_ || minimized_) return false;
    if (!modes_.fullscreen) return autoHide_.Hittable(now);
    if (panelPopup_ && panelPopup_->Bounds().Contains(dip.x, dip.y)) return true;   // the edge playlist panel over a hidden bar / title
    if (layout_.bar.Contains(dip.x, dip.y) && !autoHide_.Hittable(now)) return false;
    if (layout_.titleSlides && layout_.title.Contains(dip.x, dip.y) && !titleHide_.Hittable(now)) return false;
    return host_.PopupCount() > 0 || layout_.bar.Contains(dip.x, dip.y) || layout_.title.Contains(dip.x, dip.y);   // else the bare video: the presenter child takes it
}

/// User popups other than the full-screen edge playlist panel (dialogs, flyouts) plus an open native menu (its modal loop keeps
/// dispatching timers, so the auto-hide logic must see it).
size_t App::NonPanelPopups() const {
    size_t n = host_.PopupCount() > notices_.size() ? host_.PopupCount() - notices_.size() : 0;
    if (panelPopup_ && modes_.fullscreen && n > 0) n--;
    return n + (nativemenu::IsOpen() ? 1 : 0);
}

bool App::BarInteractionActive() const {
    if (NonPanelPopups() > 0) return true;
    PlayerBar* bar = shell_->Bar();
    MiniBar* mb = shell_->Mini();
    if (bar->SeekDragging() || mb->SeekDragging()) return true;
    if (const pui::Widget* hv = host_.Hovered())   // full screen goes by the pointer position instead (the video child sends no leave)
        if (!modes_.fullscreen && (bar->IsAncestorOf(hv) || mb->IsAncestorOf(hv))) return true;
    if (const pui::Widget* cp = host_.Captured())
        if (bar->IsAncestorOf(cp) || mb->IsAncestorOf(cp)) return true;
    return false;
}

bool App::TitleInteractionActive() const {
    if (NonPanelPopups() > 0) return true;
    TitleBar* title = shell_->Title();
    if (const pui::Widget* hv = host_.Hovered()) if (!modes_.fullscreen && title->IsAncestorOf(hv)) return true;
    if (const pui::Widget* cp = host_.Captured()) if (title->IsAncestorOf(cp)) return true;
    return false;
}

/// The edge playlist panel stays while the cursor is over its rect, a menu from it is open, it is being dragged, or its
/// search box has the keyboard.
bool App::PanelInteractionActive() const {
    if (!panelPopup_) return false;
    if (NonPanelPopups() > 0 || pui::TextBox::IsTextInputFocused(host_)) return true;
    if (const pui::Widget* cp = host_.Captured()) if (panelPopup_->IsAncestorOf(cp) || cp == panelPopup_) return true;
    POINT p; GetCursorPos(&p); ScreenToClient(hwnd_, &p);
    const float x = p.x / scale_, y = p.y / scale_;
    return panelPopup_->Bounds().Contains(x, y);
}

dbg::TabAction App::DecideTabNow(unsigned mods) {
    bool popup = UserPopupOpen();
    dbg::TabAction a = dbg::DecideTab(popup, (mods & pui::ModShift) != 0);
    lastTabDecision_ = std::string(a == dbg::TabAction::ToggleOverlay ? "toggle" : a == dbg::TabAction::Ignore ? "ignore" : "traversal") +
                       " mode=" + std::to_string((int)modes_.Mode()) + " popups=" + std::to_string(host_.PopupCount()) +
                       " notices=" + std::to_string(notices_.size()) + " userPopup=" + (popup ? "1" : "0") + " mods=" + std::to_string(mods);
    return a;
}

/// Player views (Now playing, full screen, mini player) have no keyboard focus: the keyboard belongs to the key table. The only
/// widgets that may hold focus are the playlist search box (while the user types in it) and the contents of popups (dialogs, flyouts;
/// except an overlay playlist panel's own widgets).
bool App::AllowsFocus(const pui::Widget* w) const {
    for (const pui::Widget* p = w; p; p = p->Parent())
        if (auto* panel = dynamic_cast<const PlaylistPanel*>(p)) return w == panel->Search();
    return !shell_ || !shell_->IsAncestorOf(w);
}

/// Every left/right press in the client area lands here first (before the host sees it). The playlist panel becomes the key target
/// (Active) when the press is inside it and stops being one on a press elsewhere in the shell (video area, bar, title); a press in
/// another popup (a menu opened from the panel) leaves it alone. A press anywhere but the search box ends text entry.
void App::NotePointerTarget(pui::PointF dip) {
    if (!shell_) return;
    const pui::Widget* hit = host_.HitTest(dip);
    const PlaylistPanel* hitPanel = nullptr;
    for (const pui::Widget* p = hit; p && !hitPanel; p = p->Parent()) hitPanel = dynamic_cast<const PlaylistPanel*>(p);
    if (pui::TextBox::IsTextInputFocused(host_) && (!hitPanel || hit != hitPanel->Search())) {
        if (const pui::Widget* f = host_.Focused(); f && dynamic_cast<const PlaylistPanel*>(f->Parent())) host_.SetFocus(nullptr);
    }
    if (hitPanel) {
        ForPanels([&](PlaylistPanel& p) { p.SetActive(&p == hitPanel); });
    } else if (!hit || hit == shell_ || shell_->IsAncestorOf(hit)) {
        ForPanels([](PlaylistPanel& p) { p.SetActive(false); });
    }
}

bool App::PanelKey(int vk, unsigned mods) {
    PlaylistPanel* p = shell_ ? shell_->Panel() : nullptr;
    return p && p->Visible() && p->Active() && !UserPopupOpen() && p->OnKeyDown(vk, mods);
}

/// A focused text box keeps plain and Shift keys (letters, digits are typed there, never shortcuts); F-keys and Esc (after the box
/// declined it) pass through; Ctrl / Alt
/// combinations still reach the app table.
bool App::TextInputOwnsKey(int vk, unsigned mods) const {
    // F-keys (F6 panel, F7 ...) never type; Esc reaches here only when the box was already empty (TextBox clears first, then bubbles)
    if (vk == VK_ESCAPE || (vk >= VK_F1 && vk <= VK_F24)) return false;
    return pui::TextBox::IsTextInputFocused(host_) && !(mods & (pui::ModCtrl | pui::ModAlt));
}

bool App::UserPopupOpen() const { return host_.PopupCount() > notices_.size(); }

bool App::SpaceIsPlayPause() const {
    const pui::Widget* f = host_.Focused();
    const pui::Role r = f ? f->GetRole() : pui::Role::Custom;
    const bool focusUsesSpace = f && (r == pui::Role::Edit || r == pui::Role::List || r == pui::Role::ListItem ||
                                      r == pui::Role::ComboBox || r == pui::Role::Tree || r == pui::Role::TreeItem);
    return SpaceTogglesPlayback(host_.PopupCount() > 0, focusUsesSpace);
}

void App::HandleGlobalKey(int vk, unsigned mods, bool repeat) {
    if (vk == VK_TAB && !(mods & (pui::ModCtrl | pui::ModAlt))) {
        const dbg::TabAction tab = DecideTabNow(mods);
        if (tab == dbg::TabAction::ToggleOverlay) {
            ToggleDebugOverlay("video-key");
        } else if (tab == dbg::TabAction::FocusTraversal) {
            host_.KeyDown(vk, mods);
            if (host_.Focused() && hwnd_ && IsWindowVisible(hwnd_)) SetFocus(hwnd_);
        }
        return;
    }
    if (vk == VK_SPACE && mods == 0 && SpaceIsPlayPause()) { Execute(CommandId::PlayPause); return; }
    if (host_.PopupCount() > 0 && host_.KeyDown(vk, mods)) return;   // a popup / menu first (Esc closes it, Enter invokes)
    if (TextInputOwnsKey(vk, mods)) return;
    if (PanelKey(vk, mods)) return;   // an active playlist panel owns its accelerators (Del, Enter, Ctrl+O/U/T/S ...) before the key table
    CommandId c = LookupKey(vk, mods);
    if (c != CommandId::None) RunKeyCommand(vk, c, repeat);
}

void App::OnVideoInput(const wvp::InputEventArgs& e) {
    using wvp::InputKind;
    uint64_t now = Now();
    switch (e.Kind) {
    case InputKind::Move: {
        POINT p; GetCursorPos(&p);
        if (p.x != lastCur_.x || p.y != lastCur_.y) {   // synthetic moves (ShowCursor) are not activity
            lastCur_ = p;
            PointerMoved(now);
        }
        host_.PointerLeave();
        break;
    }
    case InputKind::Down:
        if (e.Button == 1) {
            StartBarAnim(autoHide_.Activity(now));
            host_.SetFocus(nullptr);
            host_.CancelCapture();   // nothing from an earlier press may stay pressed / captured (the window drag: VideoPressDrag)
            ForPanels([](PlaylistPanel& p) { p.SetActive(false); });
        }
        break;
    case InputKind::Up:
        if (e.Button == 1) {
            StartBarAnim(autoHide_.Activity(now));   // a single click does nothing else (VideoClickCommand(1) == None)
        } else if (e.Button == 3) {
            StartBarAnim(autoHide_.Activity(now));
            ShowMainMenu(*this, {(e.X + layout_.video.l) / scale_, (e.Y + layout_.video.t) / scale_});
        }
        break;
    case InputKind::DoubleClick:
        if (CommandId c = VideoClickCommand(2); c != CommandId::None) Execute(c);
        break;
    case InputKind::Wheel: AdjustVolume(e.Delta); break;   // 1 % per notch
    case InputKind::KeyDown: {
        autoHide_.KeyPress();   // keys never reveal the OSD nor keep it alive
        KeyInputScope keyScope(keyInput_);
        unsigned mods = ((int)e.Modifiers & 1 ? pui::ModShift : 0u) | ((int)e.Modifiers & 2 ? pui::ModCtrl : 0u) | ((int)e.Modifiers & 4 ? pui::ModAlt : 0u);
        const bool repeat = e.VirtualKey == videoKeyVk_;   // the video child reports no repeat flag: a second keydown before keyup is one
        videoKeyVk_ = e.VirtualKey;
        HandleGlobalKey(e.VirtualKey, mods, repeat);
        break;
    }
    case InputKind::KeyUp:
        frameStep_.KeyUp(e.VirtualKey);
        if (e.VirtualKey == videoKeyVk_) videoKeyVk_ = 0;
        break;
    default: break;
    }
    Flush();
}

LRESULT App::Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_GETOBJECT:
        if (uia_) { LRESULT r = 0; if (uia_->HandleGetObject(wp, lp, &r)) return r; }
        break;
    case WM_NCCALCSIZE:
        if (!wp || modes_.fullscreen) return 0;   // full screen: client area == whole window
        if (IsZoomed(h)) {
            // maximized: the client is the monitor work area (the window itself overhangs it by the invisible frame)
            MONITORINFO mi{sizeof mi};
            GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
            reinterpret_cast<NCCALCSIZE_PARAMS*>(lp)->rgrc[0] = mi.rcWork;
            return 0;
        }
        if (!modes_.mini) {
            // Custom caption: the system keeps the left/right/bottom resize borders OUTSIDE the visible frame (non-client) and
            // only the caption band is dropped, so the strip starts at the very top of the window.
            auto* np = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
            LONG top = np->rgrc[0].top;
            LRESULT r = DefWindowProcW(h, m, wp, lp);
            np->rgrc[0].top = top;
            return r;
        }
        return 0;                                // mini player: client area == whole window
    case WM_NCHITTEST: return NcHitTest(lp);
    case WM_NCACTIVATE:
        // Without WS_CAPTION the default handler repaints the top sizing frame (a light band over the title strip) when
        // focus changes; lParam -1 keeps the activation bookkeeping but skips that non-client repaint.
        return DefWindowProcW(h, m, wp, -1);
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        if (modes_.mini) { PxSize mn = MiniMinSizePx(scale_); mm->ptMinTrackSize.x = mn.w; mm->ptMinTrackSize.y = mn.h; }
        else {
            mm->ptMinTrackSize.x = S(kMinWindowWidthDip); mm->ptMinTrackSize.y = S(kMinWindowHeightDip);
            // a window without a caption would maximize over the taskbar: maximize to the work area instead
            MONITORINFO mi{sizeof mi};
            GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
            mm->ptMaxPosition = {mi.rcWork.left - mi.rcMonitor.left, mi.rcWork.top - mi.rcMonitor.top};
            mm->ptMaxSize = {mi.rcWork.right - mi.rcWork.left, mi.rcWork.bottom - mi.rcWork.top};
        }
        return 0;
    }
    case WM_MOVING:
    case WM_SIZING:
        if (m == WM_SIZING && !modes_.mini) { SnapWindowMessage(lp, false, (int)wp); return TRUE; }
        if (m == WM_MOVING) { SnapWindowMessage(lp, true, 0); return TRUE; }
        if (modes_.mini) {   // aspect-locked resize of the mini player (client == window, the video fills it)
            auto* r = reinterpret_cast<RECT*>(lp);
            PxRect o = MiniConstrainSizing({r->left, r->top, r->right, r->bottom}, (int)wp, mediaAspect_, MiniMinSizePx(scale_));
            *r = {o.l, o.t, o.r, o.b};
            return TRUE;
        }
        break;
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) {
            minimized_ = true; host_.CloseAllPopups(); SetCursorHidden(false);
            if (settingsWin_) settingsWin_->OnPlayerMinimized(h, true);
        } else if (shell_) {
            bool restored = minimized_;
            minimized_ = false;
            if (restored && settingsWin_) settingsWin_->OnPlayerMinimized(h, false);
            shell_->Title()->SetMaximized(IsZoomed(h) != 0);
            Relayout();
            RefreshZoomAspect(*this);
            if (restored) { autoHide_.Snap(modes_.fullscreen ? 1.f : 0.f); titleHide_.Snap(modes_.fullscreen ? 1.f : 0.f); autoHide_.Touch(Now()); SnapBar(); Flush(); }
        }
        return 0;
    case WM_DPICHANGED: {
        scale_ = HIWORD(wp) / 96.f;
        auto* r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        d2d_.SetScale(scale_);
        shell_->Title()->SetScale(scale_);
        shell_->Bar()->SetScale(scale_);
        shell_->Mini()->SetScale(scale_);
        ApplyTheme(themeMode_, accentOverride_);   // theme.hairline follows the scale
        layersDirtyAll_ = true;
        Relayout();
        return 0;
    }
    case WM_TIMER:
        if (wp == 1) Tick();
        return 0;
    case WM_ENTERSIZEMOVE: {
        RECT vis{};
        POINT cp{};
        if (VisibleFrame(h, vis) && GetCursorPos(&cp)) snap_.Begin({vis.left, vis.top, vis.right, vis.bottom}, cp.x, cp.y);
        break;
    }
    case WM_EXITSIZEMOVE: snap_.End(); OnWindowDragEnd(); break;
    case WM_DISPLAYCHANGE: OnDisplayChange(); break;
    case WM_SETTINGCHANGE:
        if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0 && themeMode_ == pui::ThemeMode::System)
            ApplyTheme(themeMode_, accentOverride_);
        else if (HighContrastOn() != themeHighContrast_)
            ApplyTheme(themeMode_, accentOverride_);  // high contrast toggled: panel / chip tokens change
        else
            UpdateAcrylicState(true);                 // transparency / accessibility settings also affect Acrylic
        break;
    case WM_POWERBROADCAST:
        if (wp == PBT_APMPOWERSTATUSCHANGE || wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND)
            UpdateAcrylicState(true);                 // battery saver toggles without a theme change
        return DefWindowProcW(h, m, wp, lp);
    case WM_ENABLE:
        if (!wp) SetCursorHidden(false);   // a modal dialog opened (file picker, text input, message box): it needs the cursor
        break;
    case WM_WINDOWPOSCHANGING: {
        auto* wp2 = reinterpret_cast<WINDOWPOS*>(lp);
        // Foreign insert-after values must stay in the topmost band, but an owned dialog (or any window in our process)
        // must remain above the player. This avoids promoting the owner over its own modal windows.
        const bool insertAfterIsOurs = wp2 && IsMainOrProcessWindow(wp2->hwndInsertAfter, h);
        if (wp2 && ForceTopmostInsertAfter(topmostWanted_, wp2->flags, wp2->hwndInsertAfter == HWND_TOPMOST, insertAfterIsOurs)) {
            log::Event("window", "always on top: z-order change kept in the topmost band (insertAfter=" +
                       std::to_string(reinterpret_cast<intptr_t>(wp2->hwndInsertAfter)) + " flags=" + std::to_string(wp2->flags) + ")");
            wp2->hwndInsertAfter = HWND_TOPMOST;
        }
        break;
    }
    case WM_WINDOWPOSCHANGED:
        // A NOTOPMOST request can resolve to one of our own HWNDs; let owned-window ordering through above, then restore
        // the band if the main window actually left it. The owner rule keeps its owned dialogs above it during this reassert.
        if (topmostWanted_ && !(GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
            SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            NoteTopmostReasserted();
        }
        break;
    case WM_ACTIVATEAPP:
        if (!wp) frameStep_.Cancel();   // a key released in another application never reaches us
        if (!wp && topmostWanted_ && !modes_.mini) {
            SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            NoteTopmostReasserted();
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && shell_) {
            HWND other = reinterpret_cast<HWND>(lp);
            bool ownDialog = other && GetWindow(other, GW_OWNER) == hwnd_;   // PromptText / file dialogs owned by us keep the popups
            if (!ownDialog) host_.CloseAllPopups();
        }
        else if (settingsWin_ && LOWORD(wp) != WA_INACTIVE && !HIWORD(wp) && SettingsWindow::FromOtherApp(reinterpret_cast<HWND>(lp))) {
            // The app came back from another application: a new focus cycle. The click that did it is not a press on the player.
            skipActivationPress_ = LOWORD(wp) == WA_CLICKACTIVE;
            settingsWin_->StartFocusCycle(hwnd_);
        }
        break;
    case WM_NOTIFY_PLAYER: wvp::FfPlayer::HandleNotify(wp, lp); return 0;
    case WM_RUN_QUEUE: {
        std::deque<std::function<void()>> q;
        { std::lock_guard<std::mutex> lk(g_qmu); q.swap(g_queue); }
        for (auto& f : q) f();
        return 0;
    }
    case WM_REDRAW: redrawPosted_ = false; Flush(); return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && (HWND)wp == h) {
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
            if (splitDrag_ || SplitterAt(p)) { SetCursor(LoadCursorW(nullptr, IDC_SIZEWE)); return TRUE; }
        }
        break;
    case WM_MOUSEMOVE: {
        uint64_t now = Now();
        POINT p; GetCursorPos(&p);
        if (p.x != lastCur_.x || p.y != lastCur_.y) {
            lastCur_ = p;
            PointerMoved(now);
        }
        if (splitDrag_) { DragSplitter(GET_X_LPARAM(lp)); return 0; }
        if (!mouseTracked_) { TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0}; TrackMouseEvent(&t); mouseTracked_ = true; }
        if (!ChromeHittableAt(now, {GET_X_LPARAM(lp) / scale_, GET_Y_LPARAM(lp) / scale_}) && !host_.Captured()) host_.PointerLeave();   // the hidden bar/title must not react
        else host_.PointerMove(MakePointer(lp, pui::PointerButton::None));
        Flush();
        return 0;
    }
    // mini player: the video child is transparent, so mouse over the video arrives as non-client messages
    case WM_NCMOUSEMOVE:
        if (modes_.mini) {
            POINT p; GetCursorPos(&p);
            if (p.x != lastCur_.x || p.y != lastCur_.y) { lastCur_ = p; PointerMoved(Now()); }
            if (!mouseTracked_) { TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0}; TrackMouseEvent(&t); mouseTracked_ = true; }
            Flush();
        } else if (shell_ && !modes_.fullscreen) {
            // the maximize button is non-client (HTMAXBUTTON, snap layouts): feed its hover to the widget tree
            if (wp == HTMAXBUTTON) {
                if (!ncTracked_) { TRACKMOUSEEVENT t{sizeof t, TME_LEAVE | TME_NONCLIENT, h, 0}; TrackMouseEvent(&t); ncTracked_ = true; }
                host_.PointerMove(MakePointer(NcClientLParam(lp), pui::PointerButton::None));
            } else {
                host_.PointerLeave();
            }
            Flush();
        }
        break;
    case WM_NCMOUSELEAVE:
        ncTracked_ = false;
        if (shell_ && !modes_.mini) {
            if (host_.Captured()) host_.PointerUp(MakePointer(MAKELPARAM(0xFFFF, 0xFFFF), pui::PointerButton::Left));   // released elsewhere: no click
            host_.PointerLeave();
            Flush();
        }
        break;
    case WM_NCRBUTTONDOWN:
    case WM_NCMBUTTONDOWN:
    case WM_NCRBUTTONDBLCLK:
    case WM_NCMBUTTONDBLCLK:
        NotePlayerPressed();
        break;
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
        NotePlayerPressed();
        if (modes_.mini && wp == HTCAPTION) {
            if (m == WM_NCLBUTTONDBLCLK) {   // never maximize: double click is play/pause like on the video
                if (CommandId c = VideoClickCommand(2); c != CommandId::None) Execute(c);
                return 0;
            }
            StartBarAnim(autoHide_.Activity(Now()));
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (!DragDetect(h, pt)) return 0;
        } else if (wp == HTMAXBUTTON && shell_ && !modes_.mini && !modes_.fullscreen) {
            host_.PointerDown(MakePointer(NcClientLParam(lp), pui::PointerButton::Left));
            Flush();
            return 0;
        }
        break;   // drag: DefWindowProc starts the system move loop
    case WM_NCLBUTTONUP:
        if (wp == HTMAXBUTTON && shell_ && !modes_.mini && !modes_.fullscreen) {
            host_.PointerUp(MakePointer(NcClientLParam(lp), pui::PointerButton::Left));   // the button toggles maximize / restore
            Flush();
            return 0;
        }
        break;
    case WM_NCPOINTERDOWN:
    case WM_NCPOINTERUP: {
        // touch / pen on the maximize button (mouse input still arrives as WM_NCLBUTTON*)
        POINTER_INPUT_TYPE type = PT_MOUSE;
        if (HIWORD(wp) == HTMAXBUTTON && shell_ && !modes_.mini && !modes_.fullscreen && GetPointerType(GET_POINTERID_WPARAM(wp), &type) &&
            type != PT_MOUSE) {
            const pui::PointerEvent e = MakePointer(NcClientLParam(lp), pui::PointerButton::Left);
            if (m == WM_NCPOINTERDOWN) host_.PointerDown(e); else host_.PointerUp(e);
            Flush();
            return 0;
        }
        break;
    }
    case WM_NCRBUTTONUP:
        if (modes_.mini && wp == HTCAPTION) {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; ScreenToClient(h, &pt);
            ShowMainMenu(*this, {pt.x / scale_, pt.y / scale_});
            return 0;
        }
        break;
    case WM_MOUSELEAVE: mouseTracked_ = false; host_.PointerLeave(); CheckPointerLeft(Now()); Flush(); return 0;
    case WM_LBUTTONDBLCLK:
    case WM_LBUTTONDOWN: {
        NotePlayerPressed();
        if (IsWindowVisible(h)) SetFocus(h);   // never on the hidden test window: it would take the user's keyboard focus
        if (BeginSplitterDrag(lp, m == WM_LBUTTONDBLCLK)) return 0;   // the splitter is neither video nor panel: no target change
        NotePointerTarget({GET_X_LPARAM(lp) / scale_, GET_Y_LPARAM(lp) / scale_});
        uint64_t now = Now();
        StartBarAnim(autoHide_.Activity(now));
        if (!ChromeHittableAt(now, {GET_X_LPARAM(lp) / scale_, GET_Y_LPARAM(lp) / scale_})) return 0;
        host_.PointerDown(MakePointer(lp, pui::PointerButton::Left, m == WM_LBUTTONDBLCLK ? 2 : 1));
        if (host_.Captured()) SetCapture(h);
        Flush();
        return 0;
    }
    case WM_LBUTTONUP:
        if (EndSplitterDrag()) { if (GetCapture() == h) ReleaseCapture(); return 0; }
        host_.PointerUp(MakePointer(lp, pui::PointerButton::Left));
        if (GetCapture() == h) ReleaseCapture();
        autoHide_.Touch(Now());
        Flush();
        return 0;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK:
    case WM_RBUTTONDBLCLK:
        NotePlayerPressed();
        break;
    case WM_RBUTTONDOWN:
        NotePlayerPressed();
        if (!ChromeHittableAt(Now(), {GET_X_LPARAM(lp) / scale_, GET_Y_LPARAM(lp) / scale_})) return 0;
        NotePointerTarget({GET_X_LPARAM(lp) / scale_, GET_Y_LPARAM(lp) / scale_});
        host_.PointerDown(MakePointer(lp, pui::PointerButton::Right));
        Flush();
        return 0;
    case WM_RBUTTONUP: {
        // While any popup (even a toast / InfoBar notice) is open the video child is HTTRANSPARENT, so a right-click over
        // the video lands here instead of OnVideoInput: open the video context menu unless it hit a popup or visible chrome.
        const pui::PointF dip{GET_X_LPARAM(lp) / scale_, GET_Y_LPARAM(lp) / scale_};
        const pui::Widget* hit = host_.HitTest(dip);
        const bool onPopup = hit && shell_ && hit != shell_ && !shell_->IsAncestorOf(hit);
        const bool onChrome = (layout_.bar.Contains(dip.x, dip.y) || layout_.title.Contains(dip.x, dip.y)) && ChromeHittableAt(Now(), dip);
        const bool overVideo = !modes_.mini && !layout_.video.Empty() &&
                               GET_X_LPARAM(lp) >= layout_.video.l && GET_X_LPARAM(lp) < layout_.video.r &&
                               GET_Y_LPARAM(lp) >= layout_.video.t && GET_Y_LPARAM(lp) < layout_.video.b;
        host_.PointerUp(MakePointer(lp, pui::PointerButton::Right));
        if (overVideo && !onPopup && !onChrome && !UserPopupOpen()) {
            StartBarAnim(autoHide_.Activity(Now()));
            ShowMainMenu(*this, dip);
        }
        Flush();
        return 0;
    }
    case WM_CONTEXTMENU:
        if (lp == -1) host_.ContextMenuKey();   // Shift+F10 / Apps key
        return 0;
    case WM_CAPTURECHANGED:
        EndSplitterDrag();
        host_.CancelCapture();   // also clears the pressed look: a lost OS capture sends no pointer-up
        Flush();
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        unsigned mods = CurrentMods();
        autoHide_.KeyPress();   // keys never reveal the OSD nor keep it alive
        cursorIdle_.KeyPress(); // nor an auto-hidden cursor
        KeyInputScope keyScope(keyInput_);
        bool handled = false;
        if (wp == VK_TAB && !(mods & (pui::ModCtrl | pui::ModAlt))) {
            const dbg::TabAction tab = DecideTabNow(mods);
            if (tab != dbg::TabAction::FocusTraversal) {
                if (tab == dbg::TabAction::ToggleOverlay) ToggleDebugOverlay("wm-keydown");
                Flush();
                return 0;
            }
        }
        if (wp == VK_SPACE && mods == 0 && SpaceIsPlayPause()) {   // before the focused widget (it would act on key up)
            if (!(lp & 0x40000000)) Execute(CommandId::PlayPause);   // auto-repeat of a held Space: no toggling back and forth
            Flush();
            return 0;
        }
        handled = host_.KeyDown((int)wp, mods) || PanelKey((int)wp, mods);
        if (!handled && !TextInputOwnsKey((int)wp, mods)) {
            CommandId c = LookupKey((int)wp, mods);
            if (c != CommandId::None) { RunKeyCommand((int)wp, c, (lp & 0x40000000) != 0); handled = true; }
        }
        Flush();
        if (handled && m == WM_SYSKEYDOWN) return 0;   // never swallow Alt+F4 / other system keys
        if (handled) return 0;
        break;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP: frameStep_.KeyUp((int)wp); host_.KeyUp((int)wp, CurrentMods()); break;
    case WM_KILLFOCUS: frameStep_.Cancel(); break;
    case WM_CHAR: host_.Char((wchar_t)wp); Flush(); return 0;
    case WM_MOUSEWHEEL: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(h, &p);
        pui::PointerEvent e;
        e.pt = {p.x / scale_, p.y / scale_};
        e.wheelDelta = (float)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        e.mods = CurrentMods();
        if (!host_.PointerWheel(e)) AdjustVolume(GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA);   // 1 % per notch
        Flush();
        return 0;
    }
    case WM_DROPFILES:
        DragFinish(reinterpret_cast<HDROP>(wp));   // all accepted drops use OLE; discard any unexpected legacy delivery
        return 0;
    case WM_COPYDATA:
        if (HandleCopyData(*this, reinterpret_cast<const COPYDATASTRUCT*>(lp))) return TRUE;
        break;
    case WM_CLOSE: OnClose(); return 0;
    case WM_DESTROY:
        ctl_.reset(); player_.reset();           // safety net if destroyed without WM_CLOSE
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

bool App::PumpUntil(const std::function<bool()>& cond, DWORD timeoutMs) {
    ULONGLONG end = Now() + timeoutMs;
    while (!cond()) {
        if (Now() > end) return false;
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) return cond();
            TranslateMessage(&m); DispatchMessageW(&m);
        }
        Sleep(5);
    }
    return true;
}

}
