#include "Gfx.h"

#include <d2d1helper.h>

#include <cstdio>

namespace {
std::string LuidText(const LUID& l) {
    char b[24];
    snprintf(b, sizeof b, "%08lx:%08lx", (unsigned long)l.HighPart, (unsigned long)l.LowPart);
    return b;
}
}

HRESULT Gfx::Init(HWND hwnd, const StepFn& step, bool forceWarp) {
    HRESULT hr;
    D3D_FEATURE_LEVEL fl;
    warp = false;
    adapterLuid.clear(); adapterName.clear(); adapterVendor = 0; adapterHardware = false;
    injected_ = S_OK;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    hr = forceWarp ? E_FAIL
                   : D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
                                       D3D11_SDK_VERSION, &d3d, &fl, nullptr);
    if (FAILED(hr)) {
        warp = true;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
                               D3D11_SDK_VERSION, &d3d, &fl, nullptr);
    }
    step(warp ? "d3d11_device(warp)" : "d3d11_device", hr);
    if (FAILED(hr)) return hr;
    hr = d3d.As(&dxgi);
    step("dxgi_device", hr);
    if (FAILED(hr)) return hr;
    {
        ComPtr<IDXGIAdapter> ad;
        ComPtr<IDXGIAdapter1> ad1;
        DXGI_ADAPTER_DESC1 desc{};
        HRESULT ahr = dxgi->GetAdapter(&ad);
        if (SUCCEEDED(ahr)) ahr = ad.As(&ad1);
        if (SUCCEEDED(ahr)) ahr = ad1->GetDesc1(&desc);
        step("dxgi_adapter", ahr);
        if (SUCCEEDED(ahr)) {
            adapterLuid = LuidText(desc.AdapterLuid);
            adapterName = desc.Description;
            adapterVendor = desc.VendorId;
            adapterHardware = !warp && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && desc.VendorId != 0x1414;
        }
    }

    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr, reinterpret_cast<void**>(d2dFactory.GetAddressOf()));
    if (SUCCEEDED(hr)) hr = d2dFactory->CreateDevice(dxgi.Get(), &d2dDevice);
    if (SUCCEEDED(hr)) hr = d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);
    step("d2d_device_context", hr);
    if (FAILED(hr)) return hr;
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);   // required on premultiplied transparent targets
    dc->SetUnitMode(D2D1_UNIT_MODE_PIXELS);

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()));
    step("dwrite_factory", hr);
    if (FAILED(hr)) return hr;

    hr = DCompositionCreateDevice3(dxgi.Get(), __uuidof(IDCompositionDesktopDevice), reinterpret_cast<void**>(desk.GetAddressOf()));
    step("dcomp_device3", hr);
    if (FAILED(hr)) return hr;
    hr = desk->CreateTargetForHwnd(hwnd, TRUE, &target);   // topmost: composes above child windows (the presenter's video)
    step("dcomp_target_for_hwnd_topmost", hr);
    if (FAILED(hr)) return hr;
    hr = desk->CreateVisual(&root);
    if (SUCCEEDED(hr)) hr = target->SetRoot(root.Get());
    step("dcomp_root_visual", hr);
    return hr;
}

bool Gfx::IsDeviceLoss(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG ||
           hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR || hr == D2DERR_RECREATE_TARGET;
}

HRESULT Gfx::Report(const char* where, HRESULT hr) {
    if (FAILED(hr) && onFailure) onFailure(where, hr);
    return hr;
}

HRESULT Gfx::Commit() {
    if (!desk) return E_FAIL;
    if (injected_ != S_OK) { HRESULT hr = injected_; injected_ = S_OK; return Report("commit(injected)", hr); }
    return Report("commit", desk->Commit());
}

void Gfx::Shutdown() {
    StopRemovalWatch();
    zorder_.clear();
    root.Reset(); target.Reset(); desk.Reset();
    if (dc) dc->SetTarget(nullptr);
    dc.Reset(); d2dDevice.Reset(); d2dFactory.Reset();
    dxgi.Reset(); d3d.Reset();
}

struct Gfx::Watch {
    std::function<void()> fn;
    HANDLE wait = nullptr;
};

void Gfx::StartRemovalWatch(std::function<void()> onRemoved) {
    StopRemovalWatch();
    ComPtr<ID3D11Device4> d4;
    if (!d3d || FAILED(d3d.As(&d4))) return;
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ev) return;
    DWORD cookie = 0;
    if (FAILED(d4->RegisterDeviceRemovedEvent(ev, &cookie))) { CloseHandle(ev); return; }
    auto* w = new Watch{std::move(onRemoved), nullptr};
    if (!RegisterWaitForSingleObject(&w->wait, ev,
            [](PVOID ctx, BOOLEAN) { static_cast<Watch*>(ctx)->fn(); }, w, INFINITE, WT_EXECUTEONLYONCE | WT_EXECUTEINWAITTHREAD)) {
        d4->UnregisterDeviceRemoved(cookie);
        CloseHandle(ev);
        delete w;
        return;
    }
    watch_ = w; removedEvent_ = ev; removedCookie_ = cookie;
}

void Gfx::StopRemovalWatch() {
    if (!watch_) return;
    UnregisterWaitEx(watch_->wait, INVALID_HANDLE_VALUE);   // blocks until a running callback returns
    ComPtr<ID3D11Device4> d4;
    if (d3d && SUCCEEDED(d3d.As(&d4))) d4->UnregisterDeviceRemoved(removedCookie_);
    CloseHandle(removedEvent_);
    delete watch_;
    watch_ = nullptr; removedEvent_ = nullptr; removedCookie_ = 0;
}

HRESULT Gfx::CreateVisual(Surf& s) {
    HRESULT hr = desk->CreateVisual(&s.visual);
    if (FAILED(hr)) return Report("create_visual", hr);
    hr = s.visual.As(&s.visual3);
    if (FAILED(hr)) return hr;
    // insertAbove=TRUE with a NULL reference would put the visual at the BOTTOM of the root's children. Insert above the
    // current topmost visual instead (the first one has no siblings, so FALSE+NULL = top is used).
    IDCompositionVisual2* top = zorder_.empty() ? nullptr : zorder_.back();
    hr = top ? root->AddVisual(s.visual.Get(), TRUE, top) : root->AddVisual(s.visual.Get(), FALSE, nullptr);
    if (SUCCEEDED(hr)) zorder_.push_back(s.visual.Get());
    return hr;
}

HRESULT Gfx::ResizeSurf(Surf& s, int w, int h) {
    w = w < 1 ? 1 : w; h = h < 1 ? 1 : h;
    s.surface.Reset();
    HRESULT hr = desk->CreateSurface((UINT)w, (UINT)h, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &s.surface);
    if (FAILED(hr)) { s.w = s.h = 0; return Report("create_surface", hr); }
    s.w = w; s.h = h;
    return Report("set_content", s.visual->SetContent(s.surface.Get()));
}

void Gfx::ClearSurf(Surf& s) {
    if (s.visual) s.visual->SetContent(nullptr);
    s.surface.Reset(); s.w = s.h = 0;
}

HRESULT Gfx::DrawSurf(Surf& s, const std::function<void(ID2D1DeviceContext*)>& fn) {
    if (!s.surface) return E_POINTER;
    if (injected_ != S_OK) { HRESULT hr = injected_; injected_ = S_OK; return Report("draw(injected)", hr); }
    RECT rc{0, 0, s.w, s.h};
    POINT off{};
    ComPtr<IDXGISurface> dxs;
    HRESULT hr = s.surface->BeginDraw(&rc, IID_PPV_ARGS(&dxs), &off);
    if (FAILED(hr)) return Report("surface_begin_draw", hr);
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
    ComPtr<ID2D1Bitmap1> bmp;
    hr = dc->CreateBitmapFromDxgiSurface(dxs.Get(), &bp, &bmp);
    if (SUCCEEDED(hr)) {
        dc->SetTarget(bmp.Get());
        dc->BeginDraw();
        dc->SetTransform(D2D1::Matrix3x2F::Translation((float)off.x, (float)off.y));
        dc->PushAxisAlignedClip(D2D1::RectF(0, 0, (float)s.w, (float)s.h), D2D1_ANTIALIAS_MODE_ALIASED);
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        fn(dc.Get());
        dc->PopAxisAlignedClip();
        hr = Report("d2d_end_draw", dc->EndDraw());
        dc->SetTarget(nullptr);
    } else {
        Report("create_bitmap_from_surface", hr);
    }
    HRESULT hr2 = Report("surface_end_draw", s.surface->EndDraw());
    return FAILED(hr) ? hr : hr2;
}

ComPtr<IDCompositionAnimation> Gfx::MakeAnim(float v0, float v1, float dur, bool easeOut) {
    ComPtr<IDCompositionAnimation> a;
    if (FAILED(Report("create_animation", desk->CreateAnimation(&a)))) return nullptr;
    float d = v1 - v0;
    if (easeOut) a->AddCubic(0, v0, 3.f * d / dur, -3.f * d / (dur * dur), d / (dur * dur * dur));
    else a->AddCubic(0, v0, 0.f, 0.f, d / (dur * dur * dur));
    a->End(dur, v1);
    return a;
}

