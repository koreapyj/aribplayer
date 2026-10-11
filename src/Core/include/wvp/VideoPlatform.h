#pragma once
// The video side of the host platform, implemented by the platform library (Windows: DXGI / D3D11 next to App;
// macOS: VideoToolbox / Metal in the Mac app) and consumed by Core's shared logic: video GPU selection (VideoGpu.h), the
// planner (capabilities) and FfPlayer creation (the presenter).
//
// A C++ interface: the Windows implementation lives next to App, the Mac one in the Mac app of this repo
// (Objective-C++).
//
// Portable: no platform headers, no ffplay.h (the logic library builds without libffplay), no libavutil. libffplay types
// appear only as forward declarations; Core passes them through and never dereferences or releases them.
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "wvp/CapabilitySet.h"
#include "wvp/VideoGpu.h"

struct AVBufferRef;
struct FFPPresenter;

namespace wvp {

// A video presenter on one adapter and one view, with the hardware devices it brings: what libffplay's FFPOutput is
// made from. Created by VideoPlatform::CreatePresenter, owned by the FfPlayer it is given to (PresenterHandle).
//
// Lifetime: libffplay borrows Native() and Devices() from ffp_create / ffp_set_output until ffp_destroy / the next
// ffp_set_output returns; FfPlayer destroys the Presenter only after that. The destructor releases the presenter and
// every device reference it holds (libffplay took its own references and has dropped them by then). Created and
// destroyed on the UI thread. Resizes of the view reach the presenter through the platform (it observes its view);
// they never go through libffplay.
class Presenter {
public:
    virtual ~Presenter() = default;

    // The ffplay.h presenter table (its opaque points into this object). Valid for the object's lifetime.
    virtual const FFPPresenter* Native() const = 0;
    // FFPOutput.devices: further devices of this adapter for device types the presenter itself does not hand out (the
    // presenter's own devices, e.g. its D3D11VA device, come through FFPPresenter.open). Borrowed references,
    // valid for the object's lifetime. Usually empty.
    virtual std::vector<AVBufferRef*> Devices() const = 0;
    // FFPOutput.only_given_devices: true keeps every device on this adapter (nothing is created on a default GPU).
    virtual bool OnlyGivenDevices() const { return true; }
    // Adapter id (AdapterEntry::luid) the presenter runs on.
    virtual const std::string& AdapterId() const = 0;
};

// Owns the presenter and, through it, its hardware devices. Moved into FfPlayer::Create / FfPlayer::SetPresenter.
// Platform-specific extras (the Windows Acrylic backdrop regions, the presenter test readback) stay on the concrete
// platform type: the host that created the presenter knows it.
using PresenterHandle = std::unique_ptr<Presenter>;

// The adapter is the existing AdapterEntry (VideoGpu.h): luid = the opaque adapter id (a LUID "HHHHHHHH:LLLLLLLL" on
// Windows, any stable string elsewhere), name, vendor / device id, PCI location, whether it drives a monitor.
class VideoPlatform {
public:
    virtual ~VideoPlatform() = default;

    // Hardware adapters in platform order (Windows: DXGI order without software adapters). Cheap: no device is created.
    // Any thread. GroupPhysicalGpus() turns them into candidates.
    virtual std::vector<AdapterEntry> Adapters() = 0;
    // Id of the adapter that drives the monitor `view` is on; "" when unknown. UI thread.
    virtual std::string DisplayAdapter(void* view) = 0;
    // Runtime capabilities of one adapter, blocking (seconds: devices, decoder profiles, presenter self tests); never on
    // the UI thread. Thread-safe; Windows runs it in a helper process (a hung driver must not take the app down), and
    // the result may be cached by the caller. Returns:
    //  * adapter (AdapterCap), devices (HwDeviceCap) and verified decoders (DecoderCap with bitDepths / max size) of the
    //    adapter: Windows CheckVideoDecoderFormat profiles and bit depths; macOS VTIsHardwareDecodeSupported;
    //  * presenters (PresenterCap) of this platform on the adapter ("d3d11", "metal");
    //  * merged with libffplay's portable probe (ffp_probe_caps) run on the same devices: interop, features. The
    //    implementation calls FfPlayer::ProbeEngine with its devices and MergePlatformCapabilities() (both Core).
    // nullopt when the adapter cannot do video at all. Core then adds BuildCapabilities (MergeBuildCapabilities).
    virtual std::optional<CapabilitySet> Probe(const AdapterEntry& adapter) = 0;
    // A presenter on `adapter` that draws into `view` (Windows: the HWND of the video area; macOS: the NSView /
    // CAMetalLayer the Mac app gives). UI thread. nullptr with *error on failure (no device on the adapter, ...).
    // The presenter recovers a lost device itself at its next FFPPresenter.open; a new presenter is only needed for
    // another adapter (GPU choice changed, adapter removed).
    virtual PresenterHandle CreatePresenter(const AdapterEntry& adapter, void* view, std::string* error) = 0;
};

}
