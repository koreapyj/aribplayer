#include "shell/SettingsWindow.h"

#include "AppContext.h"
#include "Localize.h"
#include "features/FiltersPanel.h"
#include "features/SettingsService.h"
#include "pages/SettingsPage.h"

namespace app {

namespace {
// no minimize box: the window has no taskbar button (hidden owner), so a minimized one would be a stub on the desktop
constexpr DWORD kStyle = WS_OVERLAPPEDWINDOW & ~WS_MINIMIZEBOX;
}

SettingsWindow::SettingsWindow(AppContext& ctx, Gfx& gfx, IDWriteFactory* dw) : HostWindow(ctx, gfx, dw) {}

SettingsWindow::~SettingsWindow() { DestroyHost(); }

bool SettingsWindow::Open(bool hidden, bool attachDevice) {
    if (BringToFront()) return true;
    Spec spec;
    spec.title = Tr(L"Settings", L"환경 설정");
    spec.style = kStyle;
    spec.widthDip = kDefaultWidthDip;
    spec.heightDip = kDefaultHeightDip;
    spec.minWidthDip = kMinWidthDip;
    spec.minHeightDip = kMinHeightDip;
    if (!CreateHost(spec, hidden)) return false;
    bool maximized = false;
    if (auto p = RestorablePlacement(ctx_.Svc().settings.settingsWindow)) {   // remembered position / size
        SetWindowPos(Hwnd(), nullptr, p->x, p->y, p->w, p->h, SWP_NOZORDER | SWP_NOACTIVATE);
        maximized = p->maximized;
    }
    BeginHost();
    InstallRoot(Tr(L"Settings", L"환경 설정"), L"SettingsWindow");
    SettingsPageEnv env;
    env.host = &host_;
    env.owner = Hwnd();
    env.editFilters = [this] { EditFilters(); };
    page_ = root_->SetPage(CreateSettingsPage(ctx_, std::move(env)));
    EndHost(attachDevice, maximized ? SW_SHOWMAXIMIZED : SW_SHOW);
    return true;
}

void SettingsWindow::BeforeDestroy() {
    if (!hidden_) {   // a hidden test window never overwrites the user's placement
        if (auto p = CaptureWindowPlacement(Hwnd())) {
            ctx_.Svc().settings.settingsWindow = *p;
            ctx_.Svc().settingsStore.Save(ctx_.Svc().settings);
        }
    }
    page_ = nullptr;
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

}
