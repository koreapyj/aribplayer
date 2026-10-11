#include "wvp/win32/FrameCapture.h"
#include "wvp/win32/Utf.h"

#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>


namespace wvp::FrameCapture {

using Microsoft::WRL::ComPtr;

namespace {

constexpr std::uint32_t BI_BITFIELDS_V = 3;
constexpr std::uint32_t LCS_sRGB_V = 0x73524742;   // 'sRGB'
constexpr std::uint32_t LCS_GM_IMAGES_V = 4;

void Put32(std::uint8_t* p, std::uint32_t v) { std::memcpy(p, &v, 4); }
void Put16(std::uint8_t* p, std::uint16_t v) { std::memcpy(p, &v, 2); }

void Check(HRESULT hr, const char* what) {
    if (FAILED(hr)) throw std::runtime_error(std::string(what) + " failed (HRESULT 0x" + [&] {
        char b[16]; sprintf_s(b, "%08X", (unsigned)hr); return std::string(b); }() + ")");
}

struct ComScope {
    bool owned = false;
    ComScope() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owned = SUCCEEDED(hr);           // S_OK / S_FALSE both need CoUninitialize; RPC_E_CHANGED_MODE does not
    }
    ~ComScope() { if (owned) CoUninitialize(); }
};

ComPtr<IWICImagingFactory> CreateFactory() {
    ComPtr<IWICImagingFactory> f;
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)), "CoCreateInstance(WIC)");
    return f;
}

bool OpenClipboardWithRetry(HWND owner) {
    for (int i = 0; i < 20; i++) {
        if (OpenClipboard(owner)) return true;
        Sleep(25 + i * 5);
    }
    return false;
}

void SetData(UINT format, const std::vector<std::uint8_t>& data) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, data.size());
    if (!h) throw std::bad_alloc();
    void* p = GlobalLock(h);
    if (!p) { GlobalFree(h); throw std::runtime_error("GlobalLock failed"); }
    std::memcpy(p, data.data(), data.size());
    GlobalUnlock(h);
    if (!SetClipboardData(format, h)) {
        DWORD err = GetLastError();
        GlobalFree(h);
        throw std::runtime_error("SetClipboardData(" + std::to_string(format) + ") failed: " + std::to_string(err));
    }
}

struct OwnerWindow {
    HWND hwnd = nullptr;
    OwnerWindow() { hwnd = CreateWindowExW(0, L"STATIC", nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr); }
    ~OwnerWindow() { if (hwnd) DestroyWindow(hwnd); }
};

}

void ValidateFrame(const BgraFrame& f) {
    if (f.Width <= 0 || f.Height <= 0 || f.Stride < f.Width * 4 || (long long)f.Data.size() < (long long)f.Stride * f.Height)
        throw std::invalid_argument("Invalid BGRA frame dimensions or buffer size.");
}

std::vector<std::uint8_t> BuildDibV5(const BgraFrame& f) {
    ValidateFrame(f);
    const int rowBytes = f.Width * 4;
    const int imageSize = rowBytes * f.Height;
    std::vector<std::uint8_t> buf((size_t)BitmapV5HeaderSize + (size_t)imageSize, 0);
    std::uint8_t* h = buf.data();
    Put32(h + 0, BitmapV5HeaderSize);
    Put32(h + 4, (std::uint32_t)f.Width);
    Put32(h + 8, (std::uint32_t)f.Height);   // positive = bottom-up
    Put16(h + 12, 1);
    Put16(h + 14, 32);
    Put32(h + 16, BI_BITFIELDS_V);
    Put32(h + 20, (std::uint32_t)imageSize);
    Put32(h + 40, 0x00FF0000);
    Put32(h + 44, 0x0000FF00);
    Put32(h + 48, 0x000000FF);
    Put32(h + 52, 0xFF000000);
    Put32(h + 56, LCS_sRGB_V);
    Put32(h + 108, LCS_GM_IMAGES_V);
    for (int y = 0; y < f.Height; y++) {
        const std::uint8_t* src = f.Data.data() + (size_t)(f.Height - 1 - y) * f.Stride;
        std::memcpy(buf.data() + BitmapV5HeaderSize + (size_t)y * rowBytes, src, (size_t)rowBytes);
    }
    return buf;
}

std::vector<std::uint8_t> EncodePng(const BgraFrame& f) {
    ValidateFrame(f);
    ComScope com;
    auto factory = CreateFactory();
    ComPtr<IStream> stream;
    Check(CreateStreamOnHGlobal(nullptr, TRUE, &stream), "CreateStreamOnHGlobal");
    ComPtr<IWICBitmapEncoder> enc;
    Check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc), "CreateEncoder");
    Check(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache), "Encoder.Initialize");
    ComPtr<IWICBitmapFrameEncode> frame;
    Check(enc->CreateNewFrame(&frame, nullptr), "CreateNewFrame");
    Check(frame->Initialize(nullptr), "Frame.Initialize");
    Check(frame->SetSize((UINT)f.Width, (UINT)f.Height), "SetSize");
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    Check(frame->SetPixelFormat(&fmt), "SetPixelFormat");
    if (fmt != GUID_WICPixelFormat32bppBGRA) throw std::runtime_error("WIC PNG encoder does not accept 32bppBGRA");
    Check(frame->WritePixels((UINT)f.Height, (UINT)f.Stride, (UINT)((size_t)f.Stride * f.Height), const_cast<BYTE*>(f.Data.data())), "WritePixels");
    Check(frame->Commit(), "Frame.Commit");
    Check(enc->Commit(), "Encoder.Commit");

    STATSTG st{};
    Check(stream->Stat(&st, STATFLAG_NONAME), "Stream.Stat");
    std::vector<std::uint8_t> out((size_t)st.cbSize.QuadPart);
    LARGE_INTEGER zero{};
    Check(stream->Seek(zero, STREAM_SEEK_SET, nullptr), "Stream.Seek");
    ULONG got = 0;
    Check(stream->Read(out.data(), (ULONG)out.size(), &got), "Stream.Read");
    out.resize(got);
    return out;
}

std::optional<BgraFrame> DecodePng(const std::vector<std::uint8_t>& png) {
    if (png.empty()) return std::nullopt;
    try {
        ComScope com;
        auto factory = CreateFactory();
        ComPtr<IWICStream> ws;
        Check(factory->CreateStream(&ws), "CreateStream");
        Check(ws->InitializeFromMemory(const_cast<BYTE*>(png.data()), (DWORD)png.size()), "InitializeFromMemory");
        ComPtr<IWICBitmapDecoder> dec;
        Check(factory->CreateDecoderFromStream(ws.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec), "CreateDecoderFromStream");
        ComPtr<IWICBitmapFrameDecode> fr;
        Check(dec->GetFrame(0, &fr), "GetFrame");
        ComPtr<IWICFormatConverter> conv;
        Check(factory->CreateFormatConverter(&conv), "CreateFormatConverter");
        Check(conv->Initialize(fr.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom),
              "Converter.Initialize");
        UINT w = 0, h = 0;
        Check(conv->GetSize(&w, &h), "GetSize");
        BgraFrame out;
        out.Width = (int)w; out.Height = (int)h; out.Stride = (int)w * 4;
        out.Data.resize((size_t)out.Stride * h);
        Check(conv->CopyPixels(nullptr, (UINT)out.Stride, (UINT)out.Data.size(), out.Data.data()), "CopyPixels");
        return out;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::uint32_t PngFormat() {
    static const UINT id = RegisterClipboardFormatW(L"PNG");
    return id;
}

void RunOnSta(const std::function<void()>& fn) {
    std::exception_ptr error;
    std::thread t([&] {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        try { fn(); } catch (...) { error = std::current_exception(); }
        if (SUCCEEDED(hr)) CoUninitialize();
    });
    t.join();
    if (error) std::rethrow_exception(error);
}

void CopyToClipboard(const BgraFrame& frame) {
    auto dib = BuildDibV5(frame);
    auto png = EncodePng(frame);
    RunOnSta([&] {
        OwnerWindow owner;
        if (!OpenClipboardWithRetry(owner.hwnd))
            throw std::runtime_error("Could not open the clipboard (it is locked by another process).");
        struct Closer { ~Closer() { CloseClipboard(); } } closer;
        if (!EmptyClipboard()) throw std::runtime_error("EmptyClipboard failed: " + std::to_string(GetLastError()));
        SetData(CF_DIBV5, dib);
        SetData(PngFormat(), png);
    });
}

}
