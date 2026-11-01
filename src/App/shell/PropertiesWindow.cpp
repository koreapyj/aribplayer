#include "shell/PropertiesWindow.h"

#include "AppContext.h"
#include "Localize.h"
#include "features/PropertiesDialog.h"

namespace app {

namespace {
// no minimize / maximize boxes: the window has no taskbar button (hidden owner), so a minimized one would be a stub on the desktop
constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME;
}

PropertiesWindow::PropertiesWindow(AppContext& ctx, Gfx& gfx, IDWriteFactory* dw) : HostWindow(ctx, gfx, dw) {}

PropertiesWindow::~PropertiesWindow() { DestroyHost(); }

void PropertiesWindow::SetContent(std::vector<fx::PropSection> sections) {
    // Closing from the button runs inside the page that closing tears down: posted, and only for this window instance.
    const HWND self = Hwnd();
    PropertiesPage p = CreatePropertiesPage(std::move(sections), &ctx_.Measurer(), [this, self] {
        ctx_.PostToUi([this, self] { if (Hwnd() == self) Close(); });
    });
    page_ = root_->SetPage(std::move(p.root));
    host_.SetFocus(p.initialFocus);
}

bool PropertiesWindow::Show(std::vector<fx::PropSection> sections, bool hidden, bool attachDevice) {
    if (IsOpen()) {
        SetContent(std::move(sections));
        Render(false);
        return BringToFront();
    }
    Spec spec;
    spec.title = Tr(L"Properties", L"재생 정보");
    spec.style = kStyle;
    spec.widthDip = kDefaultWidthDip;
    spec.heightDip = kDefaultHeightDip;
    spec.minWidthDip = kMinWidthDip;
    spec.minHeightDip = kMinHeightDip;
    if (!CreateHost(spec, hidden)) return false;
    BeginHost();
    InstallRoot(Tr(L"Properties", L"재생 정보"), L"PropertiesWindow");
    SetContent(std::move(sections));
    EndHost(attachDevice);
    return true;
}

}
