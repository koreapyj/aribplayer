#pragma once
// The macOS wvp::VideoPlatform (src/Core/include/wvp/VideoPlatform.h): Metal devices as adapters, VideoToolbox as the
// hardware decoder, mac/presenter (libffpmetalpresenter, FFPPresenter "metal") as the presenter. Objective-C++ inside;
// this header is plain C++ so Core-side code and tests include it without Objective-C.
//
// Adapter ids (AdapterEntry::luid): the Metal device's IORegistry entry id (MTLDevice.registryID) as "HHHHHHHH:LLLLLLLL"
// (high:low 32 bits) - the same text shape as a Windows LUID; unique per device, stable while the device is attached.
// PCI location: AdapterEntry::hasPci is always true with bus = high 32 bits of the registryID, device = low 32 bits,
// function = 0. Apple silicon GPUs have no PCI location, but VideoGpu.cpp's GroupPhysicalGpus() drops an entry without one
// unless it aliases a PCI GPU of the same vendor / device id; a synthetic, stable location keeps every Metal device a
// candidate.
//
// Views: `void* view` is an NSView* (as the Mac app creates it on the main thread, e.g. mac/app's VideoView)
// whose layer is a CAMetalLayer. CreatePresenter uses view.layer (and sets layer.device to the adapter's MTLDevice when
// the layer has none); DisplayAdapter uses view.window.screen (the main display when the view has no window).
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "wvp/VideoPlatform.h"

struct AVBufferRef;
struct FFPPresenter;

namespace wvp::mac {

// wvp::Presenter over mac/presenter: owns the FFPPresenter from ffp_metal_presenter_create and the VideoToolbox
// AVHWDeviceContext it was created with. Devices() is empty: the presenter hands its VideoToolbox device to the engine
// through FFPPresenter.open (FFPPresenterCaps.devices), like the D3D11 presenter's own device on Windows.
class MetalPresenter final : public wvp::Presenter {
public:
    // Takes ownership of `presenter` (destroyed with ffp_metal_presenter_destroy) and of the `vtDevice` reference.
    MetalPresenter(FFPPresenter* presenter, AVBufferRef* vtDevice, std::string adapterId);
    ~MetalPresenter() override;
    MetalPresenter(const MetalPresenter&) = delete;
    MetalPresenter& operator=(const MetalPresenter&) = delete;

    const FFPPresenter* Native() const override { return presenter_; }
    std::vector<AVBufferRef*> Devices() const override { return {}; }
    bool OnlyGivenDevices() const override { return true; }
    const std::string& AdapterId() const override { return adapterId_; }

    // The VideoToolbox device the presenter was created with (borrowed; valid for the object's lifetime).
    AVBufferRef* VideoToolboxDevice() const { return vtDevice_; }

    // --- Mac extras (the host knows the concrete type; see mac/presenter/include/ffp_metal_presenter.h) ---
    // Main thread, after the host updated the layer's drawableSize / contentsScale: redraws the held picture.
    void ViewChanged();

private:
    FFPPresenter* presenter_;
    AVBufferRef* vtDevice_;
    std::string adapterId_;
};

class MacVideoPlatform final : public wvp::VideoPlatform {
public:
    MacVideoPlatform() = default;

    // Every Metal device (MTLCopyAllDevices order; the system default device when that list is empty). Any thread.
    std::vector<AdapterEntry> Adapters() override;
    // Adapter id of the Metal device that drives the display of `view` (NSView*; its window's screen, else the main
    // display). "" when unknown.
    std::string DisplayAdapter(void* view) override;
    // Blocking, any thread: VideoToolbox device, VTIsHardwareDecodeSupported per codec (h264, hevc, mpeg2video, vp9,
    // av1; bit depths not reported), the "metal" presenter (verified by creating one on a throwaway CAMetalLayer).
    // nullopt for an unknown adapter. Merged with libffplay's probe on the same device (FfPlayer::ProbeEngine +
    // MergePlatformCapabilities: interop, features) when built with libffplay (WVP_MAC_HAVE_LIBFFPLAY).
    std::optional<CapabilitySet> Probe(const AdapterEntry& adapter) override;
    // `view`: NSView* with a CAMetalLayer. UI thread. nullptr with *error on a bad view, an unknown adapter, a
    // VideoToolbox device or presenter failure. The returned Presenter is a MetalPresenter.
    PresenterHandle CreatePresenter(const AdapterEntry& adapter, void* view, std::string* error) override;

    // Adapter id text of a Metal registryID ("HHHHHHHH:LLLLLLLL"); the inverse returns false for malformed text.
    static std::string AdapterIdOf(std::uint64_t registryId);
    static bool RegistryIdOf(const std::string& adapterId, std::uint64_t& registryId);
};

// What the macOS FFmpeg build fixes, for Core's MergeBuildCapabilities (the host's step after Probe, as App's
// ProbeAdapters does it): wvp::BuildCapabilities() over the darwin dist's ffmpeg-caps.inc (filters, build
// features, the hwaccel decoders ffcapsdump lists) plus the "videotoolbox" decoder lines. ffcapsdump only enumerates
// d3d11va / d3d12va / dxva2, so without them MergeBuildCapabilities drops every VideoToolbox decoder of the probe and
// the planner decodes in software; they are computed here from the linked libavcodec with ffcapsdump's own rule (the
// codec's default decoder when it has an AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX config for the device type, else
// another decoder of the codec that has one; "" when none). Without the darwin ffmpeg-caps.inc (no wvp_core_buildcaps)
// only the videotoolbox lines are returned (source "build", no filters).
CapabilitySet BuildCapabilities();

}
