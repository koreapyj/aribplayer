#pragma once
// The Windows VideoPlatform: DXGI adapters, the per-adapter capability probe (run in a helper process) and the D3D11
// presenter that draws into the App's video view.
//
// The D3D11 presenter owns a child window of the view (on its own thread) with a flip-model swap chain, and its D3D11VA
// device on the adapter (decoder pools on it are shader resources where the driver allows, so decoded frames are shown
// without a copy). It follows the view's size itself (it subclasses the view) and redraws the shown picture on resizes
// and backdrop changes. A lost device is recreated at the next FFPPresenter.open.
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "wvp/CapabilitySet.h"
#include "wvp/VideoGpu.h"
#include "wvp/VideoPlatform.h"

namespace wvp::win32 {

// Diagnostic and test switches of the D3D11 presenter and of the probe (the App's --d3d11-* switches).
struct PresenterOptions {
    bool debug = false;              // D3D11 debug layer; its messages go to the FFmpeg log ("d3d11-debug:")
    std::string forceCaps;           // "nv12_srv=0,p010_srv=0,decoder_srv=0,p010=0,shader=0": capabilities masked off
    std::string route;               // "" (automatic), "vp" or "shader": the route of every frame where possible
    // Test only.
    std::string fail;                // "", "display", "init" (the first open fails), "device_lost": one injected failure
    int srvFail = 0;                 // this many decoder pools fail to get SHADER_RESOURCE
};

// An Acrylic backdrop region in client pixels of the video view; colours are 0xAARRGGBB (alpha ignored).
struct BackdropRegion {
    int x = 0, y = 0, width = 0, height = 0;
    float cornerRadius = 0;
    std::uint32_t tintColor = 0;
    float tintOpacity = 0;           // 0..1
    std::uint32_t luminosityColor = 0;
    float luminosityOpacity = 0;     // 0..1
    bool noise = false;
    int z = 0;                       // drawn in ascending z
};
constexpr int kMaxBackdropRegions = 64;

// The back buffer as composed (video, overlays, backdrop) right before Present, and the video rectangle in it.
struct ComposedFrame {
    int width = 0, height = 0, stride = 0;
    int videoX = 0, videoY = 0, videoWidth = 0, videoHeight = 0;
    std::vector<std::uint8_t> bgra;
};

class D3D11Presenter final : public Presenter {
public:
    ~D3D11Presenter() override;
    D3D11Presenter(const D3D11Presenter&) = delete;
    D3D11Presenter& operator=(const D3D11Presenter&) = delete;

    const FFPPresenter* Native() const override;
    std::vector<AVBufferRef*> Devices() const override { return {}; }
    const std::string& AdapterId() const override { return adapterId_; }

    // Replaces the backdrop regions (none: no blur) and redraws the shown picture. UI thread. false with *error for an
    // invalid region (size, opacity, more than kMaxBackdropRegions).
    bool SetBackdropRegions(const std::vector<BackdropRegion>& regions, std::string* error = nullptr);

    // Test only: keep a copy of every composed back buffer (costs a GPU->CPU copy per frame).
    void EnableComposedReadback(bool on);
    // The latest composed back buffer; nullopt before one was captured.
    std::optional<ComposedFrame> ComposedReadback();

private:
    friend class WindowsVideoPlatform;
    struct Impl;
    D3D11Presenter(std::unique_ptr<Impl> impl, std::string adapterId);
    static PresenterHandle Create(void* view, const std::string& adapterId, const PresenterOptions& options,
                                  std::string* error);
    std::unique_ptr<Impl> impl_;
    std::string adapterId_;
};

class WindowsVideoPlatform final : public VideoPlatform {
public:
    explicit WindowsVideoPlatform(PresenterOptions options = {});

    std::vector<AdapterEntry> Adapters() override;
    // view: the HWND of the video area.
    std::string DisplayAdapter(void* view) override;
    // Runs the probe in a helper process: "<probe executable> --probe-gpu <file> --adapter-luid=<id>" plus
    // "--d3d11-debug" / "--d3d11-force-caps=<caps>" from the options. The helper calls ProbeChildJson with them and
    // writes its result into <file>. When it gives no answer (it could not start or crashed) the probe runs once more in
    // this process (ProbeInProcess); a helper that runs over 30 s is terminated and the adapter is not used (its driver
    // hangs the probe). nullopt when the adapter cannot do video.
    std::optional<CapabilitySet> Probe(const AdapterEntry& adapter) override;
    // view: the HWND of the video area; the presenter subclasses it. A D3D11Presenter (static_cast is safe).
    PresenterHandle CreatePresenter(const AdapterEntry& adapter, void* view, std::string* error) override;

    // The probe of Probe(), in this process.
    std::optional<CapabilitySet> ProbeInProcess(const AdapterEntry& adapter) const;
    // The helper process run by Probe() (default: the executable of this process).
    void SetProbeExecutable(std::wstring path) { probeExe_ = std::move(path); }

    // The helper side of Probe(): the capabilities of the adapter as one JSON line (CapabilitiesToJson), "" when the
    // adapter cannot do video.
    static std::string ProbeChildJson(const std::string& adapterId, const PresenterOptions& options);

private:
    PresenterOptions options_;
    std::wstring probeExe_;
};

}
