#include "wvp/mac/MacVideoPlatform.h"

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#if WVP_MAC_HAVE_LIBFFPLAY
#include "wvp/FfPlayer.h"
#endif

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include "ffplay.h"
#include "ffp_metal_presenter.h"
}

namespace wvp::mac {

namespace {

constexpr int kVendorApple = 0x106B;

std::string Utf8(NSString* s) { return s ? std::string(s.UTF8String) : std::string(); }

std::string AvError(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(err, buf, sizeof buf);
    return buf;
}

NSArray<id<MTLDevice>>* AllDevices() {
    NSArray<id<MTLDevice>>* all = MTLCopyAllDevices();
    if (all.count) return all;
    id<MTLDevice> d = MTLCreateSystemDefaultDevice();
    return d ? @[d] : @[];
}

typedef id<MTLDevice> MetalDevice;

MetalDevice DeviceById(const std::string& adapterId) {
    std::uint64_t id = 0;
    if (!MacVideoPlatform::RegistryIdOf(adapterId, id)) return nil;
    for (MetalDevice d in AllDevices())
        if (d.registryID == id) return d;
    return nil;
}

// The registryID of the Metal device that drives a display; 0 when unknown.
std::uint64_t DisplayDeviceId(CGDirectDisplayID display) {
    id<MTLDevice> d = CGDirectDisplayCopyCurrentMetalDevice(display);
    return d ? d.registryID : 0;
}

std::vector<CGDirectDisplayID> OnlineDisplays() {
    uint32_t n = 0;
    if (CGGetOnlineDisplayList(0, nullptr, &n) != kCGErrorSuccess || n == 0) return {};
    std::vector<CGDirectDisplayID> ids(n);
    if (CGGetOnlineDisplayList(n, ids.data(), &n) != kCGErrorSuccess) return {};
    ids.resize(n);
    return ids;
}

// "vendor-id" / "device-id" of the PCI ancestor of the Metal device's IORegistry entry (Intel Macs, external GPUs).
// Apple silicon GPUs are not PCI devices: no such properties.
bool PciIds(std::uint64_t registryId, int& vendor, int& device) {
    io_service_t entry = IOServiceGetMatchingService(kIOMainPortDefault, IORegistryEntryIDMatching(registryId));
    if (!entry) return false;
    bool found = false;
    io_registry_entry_t cur = entry;
    for (int depth = 0; cur && depth < 8 && !found; depth++) {
        CFTypeRef v = IORegistryEntryCreateCFProperty(cur, CFSTR("vendor-id"), kCFAllocatorDefault, 0);
        CFTypeRef d = IORegistryEntryCreateCFProperty(cur, CFSTR("device-id"), kCFAllocatorDefault, 0);
        if (v && CFGetTypeID(v) == CFDataGetTypeID() && CFDataGetLength((CFDataRef)v) >= 2) {
            uint32_t vv = 0, dd = 0;
            CFDataGetBytes((CFDataRef)v, CFRangeMake(0, std::min<CFIndex>(4, CFDataGetLength((CFDataRef)v))), (UInt8*)&vv);
            if (d && CFGetTypeID(d) == CFDataGetTypeID() && CFDataGetLength((CFDataRef)d) >= 2)
                CFDataGetBytes((CFDataRef)d, CFRangeMake(0, std::min<CFIndex>(4, CFDataGetLength((CFDataRef)d))), (UInt8*)&dd);
            vendor = (int)(vv & 0xFFFF);
            device = (int)(dd & 0xFFFF);
            found = true;
        }
        if (v) CFRelease(v);
        if (d) CFRelease(d);
        io_registry_entry_t parent = 0;
        if (IORegistryEntryGetParentEntry(cur, kIOServicePlane, &parent) != KERN_SUCCESS) parent = 0;
        IOObjectRelease(cur);
        cur = parent;
    }
    if (cur) IOObjectRelease(cur);
    return found;
}

AdapterEntry EntryOf(id<MTLDevice> d, const std::vector<std::uint64_t>& displayDevices) {
    AdapterEntry e;
    const std::uint64_t id = d.registryID;
    e.luid = MacVideoPlatform::AdapterIdOf(id);
    e.name = Utf8(d.name);
    if ([d.name hasPrefix:@"Apple"]) {
        e.vendorId = kVendorApple;
    } else {
        int v = 0, dev = 0;
        if (PciIds(id, v, dev)) { e.vendorId = v; e.deviceId = dev; }
    }
    // Synthetic, stable PCI location (see the header): GroupPhysicalGpus() needs one to keep the entry.
    e.hasPci = true;
    e.bus = (int)(id >> 32);
    e.device = (int)(id & 0xffffffffu);
    e.function = 0;
    for (std::uint64_t dd : displayDevices) if (dd == id) e.hasOutputs = true;
    return e;
}

int CreateVideoToolboxDevice(AVBufferRef** out, std::string* error) {
    int ret = av_hwdevice_ctx_create(out, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0);
    if (ret < 0 && error) *error = "av_hwdevice_ctx_create(videotoolbox): " + AvError(ret);
    return ret;
}

struct Codec { const char* name; CMVideoCodecType type; };
constexpr Codec kCodecs[] = {
    {"h264", kCMVideoCodecType_H264},
    {"hevc", kCMVideoCodecType_HEVC},
    {"mpeg2video", kCMVideoCodecType_MPEG2Video},
    {"vp9", kCMVideoCodecType_VP9},
    {"av1", kCMVideoCodecType_AV1},
};

}

// --- ids --------------------------------------------------------------------------------------------------------------

std::string MacVideoPlatform::AdapterIdOf(std::uint64_t registryId) {
    char b[24];
    std::snprintf(b, sizeof b, "%08llx:%08llx", (unsigned long long)(registryId >> 32), (unsigned long long)(registryId & 0xffffffffu));
    return b;
}

bool MacVideoPlatform::RegistryIdOf(const std::string& adapterId, std::uint64_t& registryId) {
    if (adapterId.size() != 17 || adapterId[8] != ':') return false;
    for (size_t i = 0; i < adapterId.size(); i++) {
        if (i == 8) continue;
        if (!std::isxdigit((unsigned char)adapterId[i])) return false;
    }
    const unsigned long long hi = std::strtoull(adapterId.substr(0, 8).c_str(), nullptr, 16);
    const unsigned long long lo = std::strtoull(adapterId.substr(9).c_str(), nullptr, 16);
    registryId = (hi << 32) | lo;
    return true;
}

// --- VideoPlatform -------------------------------------------------------------------------------------------------------

std::vector<AdapterEntry> MacVideoPlatform::Adapters() {
    @autoreleasepool {
        std::vector<std::uint64_t> displayDevices;
        for (CGDirectDisplayID disp : OnlineDisplays())
            if (std::uint64_t id = DisplayDeviceId(disp)) displayDevices.push_back(id);
        std::vector<AdapterEntry> out;
        for (MetalDevice d in AllDevices()) out.push_back(EntryOf(d, displayDevices));
        return out;
    }
}

std::string MacVideoPlatform::DisplayAdapter(void* view) {
    @autoreleasepool {
        CGDirectDisplayID display = CGMainDisplayID();
        NSView* v = (__bridge NSView*)view;
        if (v && [v isKindOfClass:NSView.class] && v.window && v.window.screen) {
            NSNumber* n = v.window.screen.deviceDescription[@"NSScreenNumber"];
            if (n) display = (CGDirectDisplayID)n.unsignedIntValue;
        }
        const std::uint64_t id = DisplayDeviceId(display);
        return id ? AdapterIdOf(id) : std::string();
    }
}

std::optional<CapabilitySet> MacVideoPlatform::Probe(const AdapterEntry& adapter) {
    @autoreleasepool {
        id<MTLDevice> device = DeviceById(adapter.luid);
        if (!device) return std::nullopt;

        CapabilitySet platform;
        platform.source = "probe";
        platform.adapter.name = Utf8(device.name);
        platform.adapter.luid = AdapterIdOf(device.registryID);
        platform.adapter.driver = Utf8(NSProcessInfo.processInfo.operatingSystemVersionString);
        platform.adapter.vendorId = adapter.vendorId ? adapter.vendorId : ([device.name hasPrefix:@"Apple"] ? kVendorApple : 0);

        AVBufferRef* vt = nullptr;
        std::string vtError;
        const bool vtOk = CreateVideoToolboxDevice(&vt, &vtError) >= 0;
        platform.devices.push_back({"videotoolbox", vtOk, platform.adapter.name, vtOk ? "" : vtError});

        for (const Codec& c : kCodecs) {
            DecoderCap d;
            d.codec = c.name;
            d.hwaccel = "videotoolbox";
            d.decoder = c.name;
            d.ok = vtOk && VTIsHardwareDecodeSupported(c.type);
            if (!d.ok) d.error = vtOk ? "VTIsHardwareDecodeSupported: no hardware decoder" : vtError;
            platform.decoders.push_back(std::move(d));
        }

        PresenterCap p;
        p.name = "metal";
        p.inputs = {FrameDomain::VideoToolbox, FrameDomain::System};
        p.shader = true;
        p.vpTelecine = false;
        p.vpP010 = true;
        p.decoderSrvNv12 = p.decoderSrvP010 = true;
        if (vtOk) {
            // A presenter on a throwaway layer of this device proves the Metal route (shaders, texture cache) works here.
            CAMetalLayer* layer = [CAMetalLayer layer];
            layer.device = device;
            char* err = nullptr;
            FFPPresenter* pres = ffp_metal_presenter_create((__bridge void*)layer, vt, &err);
            p.ok = pres != nullptr;
            if (!p.ok) p.error = err ? err : "ffp_metal_presenter_create failed";
            free(err);
            ffp_metal_presenter_destroy(pres);
        } else {
            p.error = vtError;
        }
        platform.presenters.push_back(std::move(p));

#if WVP_MAC_HAVE_LIBFFPLAY
        // libffplay's portable probe on the same VideoToolbox device: interop and features come from the engine; adapter,
        // devices, decoders and presenters stay the platform's (MergePlatformCapabilities).
        std::optional<CapabilitySet> engine;
        if (vtOk) {
            AVBufferRef* devices[1] = {vt};
            engine = FfPlayer::ProbeEngine(devices, 1);
        } else {
            engine = FfPlayer::ProbeEngine(nullptr, 0);
        }
        av_buffer_unref(&vt);
        return MergePlatformCapabilities(platform, engine ? *engine : CapabilitySet{});
#else
        // Built without libffplay (no ffmpeg/dist/darwin-arm64/libffplay.dylib): the platform set as is (features and
        // interop empty).
        av_buffer_unref(&vt);
        return platform;
#endif
    }
}

PresenterHandle MacVideoPlatform::CreatePresenter(const AdapterEntry& adapter, void* view, std::string* error) {
    @autoreleasepool {
        NSView* v = (__bridge NSView*)view;
        if (!v || ![v isKindOfClass:NSView.class]) {
            if (error) *error = "the metal presenter needs the NSView of the video area";
            return nullptr;
        }
        CALayer* layer = v.layer;
        if (!layer || ![layer isKindOfClass:CAMetalLayer.class]) {
            if (error) *error = "the video view's layer is not a CAMetalLayer";
            return nullptr;
        }
        id<MTLDevice> device = DeviceById(adapter.luid);
        if (!device) {
            if (error) *error = "unknown adapter id '" + adapter.luid + "'";
            return nullptr;
        }
        CAMetalLayer* metal = (CAMetalLayer*)layer;
        if (!metal.device) metal.device = device;
        AVBufferRef* vt = nullptr;
        if (CreateVideoToolboxDevice(&vt, error) < 0) return nullptr;
        char* err = nullptr;
        FFPPresenter* pres = ffp_metal_presenter_create((__bridge void*)metal, vt, &err);
        if (!pres) {
            if (error) *error = err ? err : "ffp_metal_presenter_create failed";
            free(err);
            av_buffer_unref(&vt);
            return nullptr;
        }
        return std::make_unique<MetalPresenter>(pres, vt, AdapterIdOf(device.registryID));
    }
}

}
