#pragma once
// The Settings window: a separate, modeless, single-instance top-level window owned by a hidden app-wide owner window (no taskbar button;
// not tied to the player, so the player can cover it: it is raised above the player once when the player is activated from
// elsewhere, and hidden while the player is minimized). Standard system frame (WS_OVERLAPPEDWINDOW caption, min / max / close, snap,
// system menu, resize borders all native) with Mica behind the client (DWMWA_SYSTEMBACKDROP_TYPE + extended frame) and the
// immersive dark caption following the app theme.
//
// Rendering shares the App's UI device (Gfx: one D3D11 / D2D device context / DComp device) but has its own DirectComposition
// target for this HWND with two visuals (content = the SettingsPage tree, popup = ComboBox flyouts + tooltip), its own
// D2dRenderer + LayerRenderer (own DPI scale, hash-gated repaint like the main layers) and its own pui::UiHost (normal keyboard
// focus, Tab traversal) and UIA bridge. Draw / commit failures report through Gfx::onFailure, so a device loss in either
// window runs the one App rebind path, which calls ReleaseDevice() before the devices go and AttachDevice() + Render(true)
// after they are rebuilt.
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

class SettingsWindow {
public:
    static constexpr float kDefaultWidthDip = 900, kDefaultHeightDip = 700, kMinWidthDip = 480, kMinHeightDip = 360;
    static constexpr UINT WM_SETTINGS_REDRAW = WM_APP + 20;

    SettingsWindow(AppContext& ctx, Gfx& gfx, IDWriteFactory* dw);
    ~SettingsWindow();
    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;

    /// Creates the window (remembered placement, else centred over the main window) or brings the open one to the front.
    /// hidden (--headless-check): created, laid out and rendered but never shown or activated. attachDevice: the shared UI
    /// device is usable (false while a failed rebind left none: the next successful rebind attaches it).
    bool Open(bool hidden, bool attachDevice);
    /// Persists the placement (AppSettings::settingsWindow) and destroys the window. No-op when closed.
    void Close();
    bool IsOpen() const { return hwnd_ != nullptr; }
    HWND Hwnd() const { return hwnd_; }
    // Z-order: Settings sits directly above the player, in its always-on-top band, until the user presses a mouse button on the
    // player; the player then stays in front until the next focus cycle (the app coming back from another application).
    // None of these activate the window, and all do nothing while it is closed or hidden for a minimized player.
    /// A new focus cycle (or Settings was just opened): above the player again.
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
    pui::Widget* Page() const { return page_; }

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

private:
    static LRESULT CALLBACK StaticProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Proc(HWND, UINT, WPARAM, LPARAM);
    void OnDestroyed();
    void SavePlacement();
    void Relayout();
    void RequestRedraw();
    void Tick();
    void UpdateTimer();
    unsigned Mods() const;
    pui::PointerEvent Pointer(LPARAM lp, pui::PointerButton b, int clicks = 1) const;
    bool RenderLayer(Surf& s, uint64_t& lastHash, bool force, const std::function<void(LayerRenderer&)>& paint);
    void ApplyCaptionTheme();
    void PushTheme();
    void EditFilters();
    void RaiseAbove(HWND player);
    bool Shown() const;

    AppContext& ctx_;
    Gfx& gfx_;
    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;   // hidden owner: gives the window no taskbar button without tying it to the player
    bool hiddenForPlayer_ = false;   // hidden because the player is minimized
    bool inFront_ = false;           // the player was pressed: it stays in front until the next focus cycle
    bool hidden_ = false;
    float scale_ = 1.f;
    int cw_ = 1, ch_ = 1;
    pui::Theme theme_{};
    bool light_ = false;
    pui::UiHost host_;
    class Root;
    Root* root_ = nullptr;
    pui::Widget* page_ = nullptr;
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
