#pragma once
// Machine and libffplay capabilities: the platform's probe (VideoPlatform::Probe: adapter, devices, verified decoders,
// presenters) merged with libffplay's portable probe ffp_probe_caps (interop, features; MergePlatformCapabilities) and with
// BuildCapabilities (filters and hwaccel decoders fixed by the FFmpeg build), or built by the fake builders for unit tests.
// Pure data + parsing: no Win32, no libffplay calls. Used by PipelineTable (filter discovery) and PipelinePlanner.
//
// Caps JSON (UTF-8, schema version 1). Every member is optional; missing members give the defaults below.
// {
//   "schema": 1, "ffmpeg": "n7.1.4-3+3",
//   "adapter": {"name": "NVIDIA GeForce RTX 5070", "vendor_id": 4318, "luid": "00000000:0001d2c4", "driver": "32.0.15.8129"},
//   "features": ["hwframes_keep"],                                                 // libffplay runtime features
//   "devices": [{"type": "d3d11va", "ok": true, "name": "NVIDIA GeForce RTX 5070", "error": ""}, ...],
//   "decoders": [{"codec": "h264", "hwaccel": "d3d11va", "decoder": "h264", "ok": true,
//                 "bit_depths": [8], "max_width": 4096, "max_height": 4096, "error": ""}, ...],
//   "filters": [{"name": "bwdif_d3d11", "formats": "list", "pix_fmts": ["d3d11"], "options": ["mode", "parity", "deint"],
//                "hw_device": true}, ...],                // formats: "list" | "any" (passthrough) | "dynamic" (query func)
//   "interop": [{"from": "d3d11", "to": "qsv", "method": "map", "ok": true, "error": ""}, ...],
//   "presenters": [{"name": "d3d11", "ok": true, "error": "",
//                   "inputs": ["d3d11", "system"],                     // frame domains accepted without a host copy chain
//                   "vp": {"deinterlace": ["bob", "adaptive"], "telecine": false},
//                   "decoder_srv": {"nv12": true, "p010": true}, "forced": ["p010=0"],   // vp also has "p010": bool
//                   "shader": true}]
// }
// Domains and device types use libavutil names: "d3d11" (frames) / "d3d11va" (device), "d3d12" / "d3d12va", "opencl" (only
// as the domain of FFmpeg's own *_opencl filters: nothing probes or plans OpenCL devices), "qsv", "dxva2", "vulkan",
// "videotoolbox" (frames and device); "system" = software frames in system memory. "cuda" is a frame domain only: it
// classifies the CUDA filters an FFmpeg build may carry (pix_fmt "cuda", _cuda / _npp / _cuvid). It has no device type, so
// no device, decoder, upload, interop or presenter input reaches it and such filters are never planned.
#include <optional>
#include <string>
#include <vector>

namespace wvp {

/// Where a frame lives. Any = a filter that passes every pixel format through (setpts, null, ...), never a frame location.
enum class FrameDomain { System, Cuda, D3D11, D3D12, OpenCl, Qsv, Dxva2, Vulkan, VideoToolbox, Any };

/// "system", "cuda", "d3d11", "d3d12", "opencl", "qsv", "dxva2", "vulkan", "videotoolbox", "any".
const char* DomainName(FrameDomain d);
/// Accepts frame pixel-format / domain names ("cuda", "d3d11", "d3d11va" (alias), "videotoolbox_vld" (alias), "opencl", "system",
/// "yuv420p" -> System, ...).
/// nullopt for "" and for names that are neither a hw format nor a plausible software format.
std::optional<FrameDomain> DomainFromName(const std::string& name);
/// hwaccel / device type ("d3d11va", "d3d12va", "dxva2", "qsv", "opencl", "vulkan", "videotoolbox") -> frame domain of its
/// frames; nullopt for any other type (also "cuda": the player has no CUDA device).
std::optional<FrameDomain> DomainOfDeviceType(const std::string& deviceType);
/// Inverse of DomainOfDeviceType for hardware domains ("d3d11va" for D3D11, ...), "" for System / Any / Cuda.
std::string DeviceTypeOfDomain(FrameDomain d);
inline bool IsHardware(FrameDomain d) { return d != FrameDomain::System && d != FrameDomain::Any; }

struct AdapterCap {
    std::string name, luid, driver;
    int vendorId = 0;          // 0x10DE NVIDIA, 0x8086 Intel, 0x1002 AMD
};

struct HwDeviceCap {
    std::string type;          // device type: "d3d11va", "qsv", ...
    bool ok = false;
    std::string name, error;
};

struct DecoderCap {
    std::string codec;         // codec id name: "h264", "hevc", "mpeg2video", "av1", "vp9", ...
    std::string hwaccel;       // device type
    std::string decoder;       // decoder name that carries the hwaccel ("h264", "av1", ...)
    bool ok = false;
    std::vector<int> bitDepths;    // empty = not reported (treated as "8 and 10 may work"; runtime fallback catches it)
    int maxWidth = 0, maxHeight = 0;   // 0 = not reported
    std::string error;
};

struct FilterCap {
    enum class Formats { Unknown, List, Any, Dynamic };
    std::string name;
    Formats formats = Formats::Unknown;
    std::vector<std::string> pixFmts;   // input pad pixel formats when formats == List
    std::vector<std::string> options;   // AVOption names; empty = not reported (every option is assumed to exist)
    bool optionsKnown = false;          // true when "options" was present in the JSON (even if empty)
    bool hwDevice = false;              // AVFILTER_FLAG_HWDEVICE
    /// ivtc filters: residual-combed output frames are marked AV_FRAME_FLAG_INTERLACED (JSON "flags_combed": true, default false),
    /// so a following bwdif deint=interlaced cleans only those. GPU ivtc filters without it get no cleanup.
    bool flagsCombed = false;
};

struct InteropCap {
    FrameDomain from = FrameDomain::System, to = FrameDomain::System;
    std::string method;        // "map" (hwmap derive, no copy) | "copy" (device-side copy)
    bool ok = false;
    std::string error;
};

struct PresenterCap {
    std::string name;          // the platform presenter: "d3d11" (Windows), "metal" (macOS)
    bool ok = false;
    std::string error;
    std::vector<FrameDomain> inputs;    // presented directly (System = one upload, a hw domain = no copy)
    bool vpTelecine = false;
    std::vector<std::string> vpDeinterlace;   // "bob", "adaptive", "motion"...
    bool shader = false;                      // D3D11/system pixel-shader route
    bool vpP010 = true;                       // VP takes P010 input ("vp":{"p010"}; absent = true); otherwise the shader route or a download
    bool decoderSrvNv12 = true, decoderSrvP010 = true;   // decoder pools bindable as SRV ("decoder_srv"; absent = true); otherwise a scratch copy
    std::vector<std::string> forced;          // capabilities masked by d3d11_force_caps ("p010=0", ...)
};

struct CapabilitySet {
    int schema = 1;
    std::string source;        // "probe" | "build" | "fake:<name>"
    std::string ffmpegVersion;
    AdapterCap adapter;
    std::vector<std::string> features;   // "hwframes_keep", "upload_hw_sink", ...
    std::vector<HwDeviceCap> devices;
    std::vector<DecoderCap> decoders;
    std::vector<FilterCap> filters;
    std::vector<InteropCap> interop;
    std::vector<PresenterCap> presenters;

    bool HasFeature(const std::string& f) const;
    bool DeviceOk(const std::string& type) const;
    const FilterCap* Filter(const std::string& name) const;
    const PresenterCap* Presenter(const std::string& name) const;   // nullptr unless present (ok or not)
    /// Decoder entry for (codec, hwaccel), nullptr when none. codec "" matches nothing.
    const DecoderCap* Decoder(const std::string& codec, const std::string& hwaccel) const;
    /// True when interop from -> to is ok with the given method ("" = any method).
    bool InteropOk(FrameDomain from, FrameDomain to, const std::string& method = "") const;
};

/// Parses the caps JSON above. nullopt on invalid JSON or a non-object root. Unknown members are ignored.
std::optional<CapabilitySet> ParseCapabilities(const std::string& json);
/// What the FFmpeg build fixes (filters, the decoder of each hwaccel, libffplay features), generated at app build by
/// ffcapsdump.exe from the FFmpeg dist the app ships with.
CapabilitySet BuildCapabilities();
/// The capabilities the planner uses: BuildCapabilities plus the runtime probe (devices, adapter, decode support, interop,
/// presenters, machine-dependent features). A probed decoder is kept only when the build names the same decoder for its
/// (codec, hwaccel).
CapabilitySet MergeBuildCapabilities(const CapabilitySet& runtime, const CapabilitySet& build);
/// The platform's probe merged with libffplay's portable probe on the same devices: adapter, devices, decoders and presenters
/// from the platform; interop and features from the engine. Decoders the engine lists that the platform did not verify are
/// not added (the platform's probe decides decode support); an engine device the platform does not list is added.
CapabilitySet MergePlatformCapabilities(const CapabilitySet& platform, const CapabilitySet& engine);
/// `caps` with every hardware device unusable (error `reason`): the planner is left with software decoding and CPU filters on
/// the same presenter.
CapabilitySet WithoutHardwareDevices(CapabilitySet caps, const std::string& reason);
/// Serializes to the same schema (round-trips through ParseCapabilities; used for the app-side cache and the overlay).
std::string CapabilitiesToJson(const CapabilitySet& caps);

/// Fake capability sets for unit tests. All of them include the software filter set of this build (fieldmatch, decimate,
/// bwdif, yadif, transpose, hflip, vflip, eq, setpts, scale, format, hwupload, hwdownload, hwmap, ...).
namespace fakecaps {
/// An NVIDIA GPU: d3d11va, d3d12va and dxva2 decoders, the D3D11 compute filters (ivtc_d3d11, bwdif_d3d11, ...), the driver
/// post-processors of the build (denied by PipelineTable) and the CUDA filters a build may carry (ivtc_cuda, bwdif_cuda,
/// scale_npp, ...: domain Cuda, never planned). presenterD3D11 false: the presenter takes system frames only (like the
/// D3D11 presenter on WARP).
CapabilitySet Nvidia(bool presenterD3D11 = true);
/// Intel / AMD: d3d11va (+ d3d12va, dxva2; Intel qsv) decoders, the D3D11 compute filters and the driver post-processors of
/// the build, a D3D11 presenter with P010 VideoProcessor input, decoder SRV and the shader route.
CapabilitySet Intel();
CapabilitySet Amd();
/// No hardware device: the presenter takes system frames only (the D3D11 presenter on WARP).
CapabilitySet None();
/// A Mac: VideoToolbox device and decoders, the CPU filters of the build and a Metal presenter taking VideoToolbox and
/// system frames.
CapabilitySet Mac();
}

}
