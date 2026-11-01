#pragma once
// A separate, modeless, single-instance top-level window hosting its own pui tree (the Settings and Properties windows). It is
// owned by a hidden app-wide owner window (no taskbar button; not tied to the player, so the player can cover it: it is raised
// above the player once when the player is activated from elsewhere, and hidden while the player is minimized). Standard system
// frame (caption, system menu, snap, resize borders all native) with Mica behind the client (DWMWA_SYSTEMBACKDROP_TYPE +
// extended frame) and the immersive dark caption following the app theme.
//
// Rendering shares the App's UI device (Gfx: one D3D11 / D2D device context / DComp device) but has its own DirectComposition
// target for this HWND with two visuals (content = the tree, popup = flyouts + tooltip), its own D2dRenderer + LayerRenderer
// (own DPI scale, hash-gated repaint like the main layers) and its own pui::UiHost (normal keyboard focus, Tab traversal) and UIA
// bridge. Draw / commit failures report through Gfx::onFailure, so a device loss in any window runs the one App rebind path,
// which calls ReleaseDevice() before the devices go and AttachDevice() + Render(true) after they are rebuilt.
//
// A subclass creates the window in its Open(): CreateHost, BeginHost, InstallRoot + its content, EndHost.
#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>

#include "Gfx.h"
#include "LayerRenderer.h"
#include "pui/D2dRenderer.h"
#include "pui/Uia.h"
#include "pui/Widget.h"

namespace app {

class AppContext;

/// Root of a host window's tree: fills the client with the page layer colour over Mica and hosts the content page. Popup pass
/// (SetPopupOnly): its own subtree is suppressed so UiHost::Paint draws only the popups and the tooltip.
class HostRoot final : public pui::Widget {
public:
    HostRoot(const std::wstring& name, const wchar_t* automationId);
    /// Replaces the current page, if any.
    pui::Widget* SetPage(std::unique_ptr<pui::Widget> page);
    void SetPopupOnly(bool on) { popupOnly_ = on; }
    void OnLayout() override;
    void Paint(pui::IRenderer& r, const pui::Theme& t) override;
private:
    pui::Widget* page_ = nullptr;
    pui::Widget* sentinel_ = nullptr;
    bool popupOnly_ = false;
};

class HostWindow {
public:
    static constexpr UINT WM_HOST_REDRAW = WM_APP + 20;

    virtual ~HostWindow();
    HostWindow(const HostWindow&) = delete;
    HostWindow& operator=(const HostWindow&) = delete;

    /// Destroys the window (the subclass saves what it keeps in BeforeDestroy). No-op when closed.
    void Close();
    bool IsOpen() const { return hwnd_ != nullptr; }
    HWND Hwnd() const { return hwnd_; }
    // Z-order: the window sits directly above the player, in its always-on-top band, until the user presses a mouse button on the
    // player; the player then stays in front until the next focus cycle (the app coming back from another application).
    // None of these activate the window, and all do nothing while it is closed or hidden for a minimized player.
    /// A new focus cycle (or the window was just opened): above the player again.
    void StartFocusCycle(HWND player);
    /// A mouse button was pressed on the player (title strip, client or video): the player goes in front. Idempotent.
    void OnPlayerPressed(HWND player);
    /// The player re-asserted its z-order / always-on-top state: stay directly above it unless it was put in front.
    void KeepAbove(HWND player);
    /// `prev` (the window that lost activation) is not a window of this process.
    static bool FromOtherApp(HWND prev);
    /// The player was minimized (hide this window) or restored (show it again without activating, above the player).
    void OnPlayerMinimized(HWND player, bool minimized);
    pui::UiHost& Ui() { return host_; }

    /// App theme (already high-contrast adjusted; acrylic off); the window sets its own hairline and the caption colours.
    void ApplyTheme(const pui::Theme& theme, bool light);

    /// Device loss: drops the DComp target, visuals, surfaces and the renderer's device brush (before Gfx::Shutdown).
    void ReleaseDevice();
    /// After Gfx::Init: new target + visuals for this HWND; everything repaints at the next Render.
    HRESULT AttachDevice();
    /// Paints the dirty layers (all with force) and commits when something changed.
    void Render(bool force);

    bool DeviceReady() const { return deviceReady_; }
    bool ContentDrawn() const { return content_.surface != nullptr; }
    HRESULT LastDrawHr() const { return lastDrawHr_; }
    int Renders() const { return renders_; }
    float Scale() const { return scale_; }

protected:
    struct Spec {
        std::wstring title;
        DWORD style = WS_OVERLAPPEDWINDOW;
        float widthDip = 600, heightDip = 400, minWidthDip = 320, minHeightDip = 240;
    };

    HostWindow(AppContext& ctx, Gfx& gfx, IDWriteFactory* dw);

    /// Single instance: brings an open window to the front (false when closed).
    bool BringToFront();
    /// Creates the (hidden: never activatable) HWND centred over the main window inside its monitor's work area. hidden is
    /// --headless-check: the window is laid out and rendered but never shown or activated.
    bool CreateHost(const Spec& spec, bool hidden);
    /// Mica, extended frame, caption and host theme: before the tree is built.
    void BeginHost();
    HostRoot* InstallRoot(const std::wstring& name, const wchar_t* automationId);
    /// UIA bridge, composition target (attachDevice: the shared UI device is usable), first layout and render, show.
    void EndHost(bool attachDevice, int showCmd = SW_SHOW);
    /// Subclass destructors call it first, while their own state still exists.
    void DestroyHost();
    /// Runs when the window is about to be destroyed (placement saving, dropping pointers into the content).
    virtual void BeforeDestroy() {}

    AppContext& ctx_;
    HostRoot* root_ = nullptr;
    pui::UiHost host_;
    bool hidden_ = false;

private:
    static LRESULT CALLBACK StaticProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Proc(HWND, UINT, WPARAM, LPARAM);
    void OnDestroyed();
    void Relayout();
    void RequestRedraw();
    void Tick();
    void UpdateTimer();
    unsigned Mods() const;
    pui::PointerEvent Pointer(LPARAM lp, pui::PointerButton b, int clicks = 1) const;
    bool RenderLayer(Surf& s, uint64_t& lastHash, bool force, const std::function<void(LayerRenderer&)>& paint);
    void ApplyCaptionTheme();
    void PushTheme();
    void RaiseAbove(HWND player);
    bool Shown() const;

    Gfx& gfx_;
    HWND hwnd_ = nullptr;
    bool ownerHeld_ = false;         // holds a reference on the shared hidden owner window
    bool hiddenForPlayer_ = false;   // hidden because the player is minimized
    bool inFront_ = false;           // the player was pressed: it stays in front until the next focus cycle
    DWORD style_ = WS_OVERLAPPEDWINDOW;
    float minWidthDip_ = 320, minHeightDip_ = 240;
    float scale_ = 1.f;
    int cw_ = 1, ch_ = 1;
    pui::Theme theme_{};
    bool light_ = false;
    std::unique_ptr<pui::UiaBridge> uia_;
    std::unique_ptr<pui::D2dRenderer> d2d_;
    std::unique_ptr<LayerRenderer> lr_;
    ComPtr<IDCompositionTarget> target_;
    ComPtr<IDCompositionVisual2> rootVisual_;
    Surf content_, popup_;
    uint64_t hContent_ = 0, hPopup_ = 0;
    bool deviceReady_ = false, dirtyAll_ = true, redrawPosted_ = false, mouseTracked_ = false;
    UINT timerMs_ = 0;
    HRESULT lastDrawHr_ = S_OK;
    int renders_ = 0;
};

}
