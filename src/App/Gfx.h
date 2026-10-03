#pragma once
#include <d2d1_1.h>
#include <d3d11_4.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

#include <functional>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

struct Surf {
    ComPtr<IDCompositionVisual2> visual;
    ComPtr<IDCompositionVisual3> visual3;
    ComPtr<IDCompositionSurface> surface;
    int w = 0, h = 0;
};

using StepFn = std::function<void(const char* step, HRESULT hr)>;

/// The video GPU libffplay plays on (App::SelectVideoGpuNow; the UI device stays on Windows' default adapter). luid "" = none:
/// software decoding + SDL. note says why.
struct PlaybackAdapter {
    std::string luid;
    std::wstring name;
    std::string note;
};

class Gfx {
public:
    ComPtr<ID3D11Device> d3d;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID2D1Factory1> d2dFactory;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<ID2D1DeviceContext> dc;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<IDCompositionDesktopDevice> desk;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual2> root;
    bool warp = false;
    // The UI device's adapter (as Windows assigned it, honouring the per-app graphics preference). adapterLuid is
    // "HHHHHHHH:LLLLLLLL" (LUID HighPart:LowPart, hex). adapterHardware is false for WARP, software adapters and the Microsoft
    // Basic Render Driver.
    std::string adapterLuid;
    std::wstring adapterName;
    unsigned adapterVendor = 0;
    bool adapterHardware = false;

    // Creates devices and a topmost composition target for hwnd. Each stage is reported through step().
    // forceWarp skips the hardware device (the device-loss fallback).
    HRESULT Init(HWND hwnd, const StepFn& step, bool forceWarp = false);
    /// Releases every device object (DComp target -> D2D -> D3D). The caller drops its Surf objects first.
    void Shutdown();

    /// True for the HRESULTs that mean "this device is gone / must be recreated".
    static bool IsDeviceLoss(HRESULT hr);
    /// Called (on the calling thread) with the call site and HRESULT whenever a Gfx call fails.
    std::function<void(const char* where, HRESULT hr)> onFailure;
    /// The next Commit/DrawSurf fails with hr (test hook: simulated device loss); consumed by that call.
    void InjectFailure(HRESULT hr) { injected_ = hr; }
    HRESULT DeviceRemovedReason() const { return d3d ? d3d->GetDeviceRemovedReason() : E_POINTER; }
    /// Waits for ID3D11Device4::RegisterDeviceRemovedEvent on a thread-pool thread and calls onRemoved there (it must only
    /// post to the UI thread). No-op when the interface is missing. Stopped by StopRemovalWatch / Shutdown.
    void StartRemovalWatch(std::function<void()> onRemoved);
    void StopRemovalWatch();
    /// Adds an empty visual ABOVE every visual created before it (explicit reference visual; see LayerOrder.h for the
    /// NULL-reference AddVisual pitfall).
    HRESULT CreateVisual(Surf& s);
    /// Root children bottom -> top in creation/insertion order (what CreateVisual asked DComp for).
    const std::vector<IDCompositionVisual2*>& ZOrder() const { return zorder_; }
    HRESULT ResizeSurf(Surf& s, int w, int h);            // (re)creates the premultiplied surface and sets it as content
    void ClearSurf(Surf& s);
    HRESULT DrawSurf(Surf& s, const std::function<void(ID2D1DeviceContext*)>& fn);   // clears to transparent first
    HRESULT Commit();

    // IDCompositionAnimation: v(t) = v0 + (v1-v0)*ease(t/dur) as one exact cubic segment (ease-out 1-(1-s)^3, ease-in s^3).
    ComPtr<IDCompositionAnimation> MakeAnim(float v0, float v1, float durSec, bool easeOut);

private:
    HRESULT Report(const char* where, HRESULT hr);
    std::vector<IDCompositionVisual2*> zorder_;
    HRESULT injected_ = S_OK;
    struct Watch;
    Watch* watch_ = nullptr;
    HANDLE removedEvent_ = nullptr;
    DWORD removedCookie_ = 0;
};

struct HwAdapter {
    std::string luid;
    std::wstring name;
    unsigned vendorId = 0, deviceId = 0;
    bool hasPci = false;                   // PCI bus/device/function from D3DKMT (KMTQAITYPE_ADAPTERADDRESS)
    unsigned bus = 0, device = 0, function = 0;
    bool hasOutputs = false;               // drives at least one monitor
};
/// Hardware adapters in DXGI order (no software adapters, no Microsoft Basic Render Driver). No device is created.
std::vector<HwAdapter> EnumHardwareAdapters();
/// LUID of the adapter whose DXGI output holds the monitor the window is on; "" when no adapter reports that monitor.
std::string DisplayAdapterLuid(HWND hwnd);
