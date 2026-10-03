// Hardware decoding per codec/profile through the real libffplay (hidden host window, SDL_AUDIODRIVER=dummy).
// Samples live in testmedia\hw (generated with ffmpeg).
// Whether the GPU can decode a sample is decided by probing the D3D11 video decoder profiles in the test itself.
#include <d3d11.h>
#include <d3d11_4.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "wvp/DebugInfo.h"
#include "wvp/FfPlayer.h"
#include "wvptest.h"

#pragma comment(lib, "d3d11.lib")

using namespace wvp;
namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kHwNotifyMsg = WM_APP + 58;

LRESULT CALLBACK HwHostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == kHwNotifyMsg) { FfPlayer::HandleNotify(w, l); return 0; }
    return DefWindowProcW(h, m, w, l);
}

void EnsureHwClass() {
    static ATOM a = [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = HwHostProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"WvpHwTestHost";
        return RegisterClassW(&wc);
    }();
    (void)a;
}

fs::path HwRepoRoot() {
    wchar_t buf[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    fs::path d = fs::path(buf).parent_path();
    while (!d.empty()) {
        if (fs::exists(d / L"CMakePresets.json")) return d;
        auto parent = d.parent_path();
        if (parent == d) break;
        d = parent;
    }
    return fs::current_path();
}

void HwPump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}

template <class F>
bool HwWaitFor(F cond, double seconds) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds((long long)(seconds * 1000));
    while (std::chrono::steady_clock::now() < end) {
        HwPump();
        if (cond()) return true;
        Sleep(15);
    }
    HwPump();
    return cond();
}

struct D3dCaps {
    bool ok = false;
    std::string adapter;
    std::vector<GUID> profiles;
    ComPtr<ID3D11VideoDevice> vdev;
    bool Has(const GUID& g, DXGI_FORMAT fmt) const {
        bool found = false;
        for (auto& p : profiles) if (p == g) found = true;
        if (!found || !vdev) return false;
        BOOL sup = FALSE;
        return SUCCEEDED(vdev->CheckVideoDecoderFormat(&g, fmt, &sup)) && sup;
    }
};

const D3dCaps& Caps() {
    static D3dCaps c = [] {
        D3dCaps r;
        ComPtr<ID3D11Device> dev;
        D3D_FEATURE_LEVEL fl;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                                     D3D11_SDK_VERSION, &dev, &fl, nullptr)))
            return r;
        if (FAILED(dev.As(&r.vdev))) return r;
        UINT n = r.vdev->GetVideoDecoderProfileCount();
        for (UINT i = 0; i < n; i++) {
            GUID g;
            if (SUCCEEDED(r.vdev->GetVideoDecoderProfile(i, &g))) r.profiles.push_back(g);
        }
        r.ok = true;
        return r;
    }();
    return c;
}

// profile GUIDs (d3d11.h / dxva)
const GUID kH264 = {0x1b81be68, 0xa0c7, 0x11d3, {0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5}};
const GUID kMpeg2 = {0xee27417f, 0x5e28, 0x4e65, {0xbe, 0xea, 0x1d, 0x26, 0xb5, 0x08, 0xad, 0xc9}};
const GUID kHevc8 = {0x5b11d51b, 0x2f4c, 0x4452, {0xbc, 0xc3, 0x09, 0xf2, 0xa1, 0x16, 0x0c, 0xc0}};
const GUID kHevc10 = {0x107af0e0, 0xef1a, 0x4d19, {0xab, 0xa8, 0x67, 0xa1, 0x63, 0x07, 0x3d, 0x13}};
const GUID kVp9p0 = {0x463707f8, 0xa1d0, 0x4585, {0x87, 0x6d, 0x83, 0xaa, 0x6d, 0x60, 0xb8, 0x9e}};
const GUID kAv1p0 = {0xb8be4ccb, 0xcf53, 0x46ba, {0x8d, 0x59, 0xd6, 0xb8, 0xa6, 0xda, 0x5d, 0x2a}};

enum class Codec { H264, Mpeg2, Hevc8, Hevc10, Vp9, Av1 };

bool GpuSupports(Codec c) {
    const D3dCaps& k = Caps();
    switch (c) {
    case Codec::H264: return k.Has(kH264, DXGI_FORMAT_NV12);
    case Codec::Mpeg2: return k.Has(kMpeg2, DXGI_FORMAT_NV12);
    case Codec::Hevc8: return k.Has(kHevc8, DXGI_FORMAT_NV12);
    case Codec::Hevc10: return k.Has(kHevc10, DXGI_FORMAT_P010);
    case Codec::Vp9: return k.Has(kVp9p0, DXGI_FORMAT_NV12);
    case Codec::Av1: return k.Has(kAv1p0, DXGI_FORMAT_NV12);
    }
    return false;
}

struct Result {
    DebugInfo d;
    bool opened = false;
};

Result RunSample(const char* file, const wchar_t* mode, Codec codec) {
    EnsureHwClass();
    Result res;
    fs::path p = HwRepoRoot() / L"testmedia" / L"hw" / file;
    REQUIRE_MSG(fs::exists(p), std::string("missing sample ") + file + " (generate: see report / ffmpeg\\README.md)");

    HINSTANCE inst = GetModuleHandleW(nullptr);
    HWND parent = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"WvpHwTestHost", L"host", WS_POPUP, -32000, -32000, 640, 360, nullptr,
                                  nullptr, inst, nullptr);
    HWND child = CreateWindowExW(0, L"STATIC", L"video", WS_CHILD | WS_VISIBLE, 0, 0, 640, 360, parent, nullptr, inst, nullptr);
    int err = 0;
    {
        auto player = FfPlayer::CreateSync(child, parent, kHwNotifyMsg, &err);
        if (!player) { DestroyWindow(parent); REQUIRE_MSG(false, "CreateSync failed: " + std::to_string(err)); }
        int prepared = 0, errors = 0;
        player->Prepared.add([&](const MediaPreparedInfo&) { prepared++; });
        player->Error.add([&](const PlayerErrorInfo&) { errors++; });
        OpenOptions o;
        if (mode && *mode) o.push_back({L"hwaccel", mode});
        int rc = player->Open(p.wstring(), o);
        bool ok = rc == 0 && HwWaitFor([&] { return prepared > 0 || errors > 0; }, 10) && errors == 0;
        if (ok) {
            res.opened = true;
            HwWaitFor([&] {
                auto js = player->GetDebugInfoJson();
                if (!js) return false;
                res.d = ParseDebugInfo(*js);
                std::int64_t n = res.d.Video.FramesHwDirect.value_or(0) + res.d.Video.FramesHwDownload.value_or(0) + res.d.Video.FramesSoftware.value_or(0);
                return res.d.Valid && res.d.HasVideo && !res.d.Video.FramePath.empty() && n >= 10;
            }, 12);
        }
        player.reset();
    }
    DestroyWindow(parent);

    const DebugVideo& v = res.d.Video;
    std::printf("HWROW | %-28s | req=%-8ls | decoder=%-10s | device=%-8s | path=%-11s | decoded=%-8s | after=%-6s | gpu=%s | err=%s\n", file,
                (mode && *mode) ? mode : L"-", v.Decoder.c_str(), v.HwaccelDevice.c_str(), v.FramePath.c_str(), v.PixFmtDecoded.c_str(),
                v.PixFmtAfterTransfer.c_str(), GpuSupports(codec) ? "yes" : "no", v.HwaccelError.c_str());
    std::fflush(stdout);
    return res;
}

// The contract for every sample: when the GPU supports the codec, the requested hwaccel is the device really used and frames are
// hardware decoded; otherwise a readable error and software frames.
void CheckSample(const char* file, const wchar_t* mode, Codec codec) {
    Result r = RunSample(file, mode, codec);
    REQUIRE_MSG(r.opened, "open failed");
    const DebugVideo& v = r.d.Video;
    REQUIRE(r.d.Valid);
    REQUIRE(r.d.HasVideo);
    bool hwFrames = v.FramePath == "hw_download" || v.FramePath == "hw_direct";
    if (GpuSupports(codec)) {
        CHECK_MSG(hwFrames, "frame_path=" + v.FramePath + " error=" + v.HwaccelError);
        CHECK_EQ(v.HwaccelDevice, std::string("d3d11va"));
        CHECK_MSG(v.HwaccelError.empty(), v.HwaccelError);
        CHECK_EQ(v.PixFmtDecoded, std::string("d3d11"));
        CHECK(v.FramesSoftware.value_or(0) == 0);
    } else {
        CHECK_EQ(v.FramePath, std::string("software"));
        CHECK_EQ(v.HwaccelDevice, std::string("none"));
        CHECK(!v.HwaccelError.empty());
    }
}

}

#define HW_CASE(id, file, codec)                                                           \
    TEST_CASE_INTEGRATION(Hwaccel_d3d11va_##id) { CheckSample(file, L"d3d11va", codec); }  \
    TEST_CASE_INTEGRATION(Hwaccel_auto_##id) { CheckSample(file, L"auto", codec); }

HW_CASE(h264_1080p_mp4, "h264-1080p.mp4", Codec::H264)
HW_CASE(h264_1080p_ts, "h264-1080p.ts", Codec::H264)
HW_CASE(h264_1440x1080i_2997_ts, "h264-1440x1080i-2997.ts", Codec::H264)
HW_CASE(h264_1920x1080i_2997_ts, "h264-1920x1080i-2997.ts", Codec::H264)
HW_CASE(h264_1440x1080_fakeinterlaced_ts, "h264-1440x1080-fakeint.ts", Codec::H264)
HW_CASE(mpeg2_1440x1080i_ts, "mpeg2-1440x1080i.ts", Codec::Mpeg2)
HW_CASE(mpeg2_1920x1080i_ts, "mpeg2-1920x1080i.ts", Codec::Mpeg2)
HW_CASE(hevc_8bit, "hevc-8bit-1080p.mp4", Codec::Hevc8)
HW_CASE(hevc_main10, "hevc-10bit-1080p.mp4", Codec::Hevc10)
HW_CASE(vp9, "vp9-1080p.webm", Codec::Vp9)
HW_CASE(av1, "av1-1080p.mp4", Codec::Av1)

// An unsupported combination must fall back visibly: mpeg4 part 2 has no D3D11VA hwaccel in any build.
TEST_CASE_INTEGRATION(Hwaccel_unsupported_codec_reports_readable_error_or_other_device) {
    fs::path clip = HwRepoRoot() / L"testmedia" / L"mpeg4-240p-short.mp4";
    REQUIRE_MSG(fs::exists(clip), "mpeg4-240p-short.mp4 missing (generated by the DebugInfo tests)");
    for (const wchar_t* mode : {L"d3d11va", L"auto"}) {
        EnsureHwClass();
        HINSTANCE inst = GetModuleHandleW(nullptr);
        HWND parent = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"WvpHwTestHost", L"host", WS_POPUP, -32000, -32000, 640, 360, nullptr,
                                      nullptr, inst, nullptr);
        HWND child = CreateWindowExW(0, L"STATIC", L"video", WS_CHILD | WS_VISIBLE, 0, 0, 640, 360, parent, nullptr, inst, nullptr);
        DebugInfo d;
        {
            int err = 0;
            auto player = FfPlayer::CreateSync(child, parent, kHwNotifyMsg, &err);
            REQUIRE(player != nullptr);
            int prepared = 0;
            player->Prepared.add([&](const MediaPreparedInfo&) { prepared++; });
            REQUIRE(player->Open(clip.wstring(), {{L"hwaccel", mode}}) == 0);
            REQUIRE(HwWaitFor([&] { return prepared > 0; }, 10));
            HwWaitFor([&] {
                auto js = player->GetDebugInfoJson();
                if (!js) return false;
                d = ParseDebugInfo(*js);
                return d.Valid && d.Video.FramesSoftware.value_or(0) >= 5;
            }, 10);
            player.reset();
        }
        DestroyWindow(parent);
        if (std::wstring(mode) == L"auto" && d.Video.HwaccelDevice != "none") {
            // auto may legitimately find another device type (NVDEC/CUDA decodes MPEG-4 part 2): then it must really be hardware
            CHECK(d.Video.FramePath == "hw_download" || d.Video.FramePath == "hw_direct");
            CHECK(d.Video.HwaccelDevice != "d3d11va");
            continue;
        }
        CHECK_EQ(d.Video.HwaccelDevice, std::string("none"));
        CHECK_EQ(d.Video.FramePath, std::string("software"));
        CHECK(!d.Video.HwaccelError.empty());
        CHECK(d.Video.HwaccelError.find("mpeg4") != std::string::npos);
    }
}

TEST_CASE_INTEGRATION(Hwaccel_gpu_probe_lists_decoder_profiles) {
    const D3dCaps& k = Caps();
    std::printf("HWPROBE | d3d11 video device: %s, %zu decoder profiles | h264=%d mpeg2=%d hevc8=%d hevc10=%d vp9=%d av1=%d\n", k.ok ? "yes" : "no",
                k.profiles.size(), GpuSupports(Codec::H264), GpuSupports(Codec::Mpeg2), GpuSupports(Codec::Hevc8), GpuSupports(Codec::Hevc10),
                GpuSupports(Codec::Vp9), GpuSupports(Codec::Av1));
    CHECK(k.ok);
}
