#pragma once
// Machine and libffplay capabilities: the runtime probe ffp_probe_caps_opts (JSON schema below) merged with
// BuildCapabilities (filters and hwaccel decoders fixed by the FFmpeg build), or built by the fake builders for unit tests.
// Pure data + parsing: no Win32, no libffplay calls. Used by PipelineTable (filter discovery) and PipelinePlanner.
//
// Caps JSON (UTF-8, schema version 1). Every member is optional; missing members give the defaults below.
// {
//   "schema": 1, "ffmpeg": "n7.1.4-3+3",
//   "adapter": {"name": "NVIDIA GeForce RTX 5070", "vendor_id": 4318, "luid": "00000000:0001d2c4", "driver": "32.0.15.8129"},
//   "features": ["hwframes_keep", "presenter_d3d11", "presenter_params"],          // libffplay runtime features
//   "devices": [{"type": "cuda", "ok": true, "name": "NVIDIA GeForce RTX 5070", "error": ""}, ...],
//   "decoders": [{"codec": "h264", "hwaccel": "cuda", "decoder": "h264", "ok": true,
//                 "bit_depths": [8], "max_width": 4096, "max_height": 4096, "error": ""}, ...],
//   "filters": [{"name": "bwdif_cuda", "formats": "list", "pix_fmts": ["cuda"], "options": ["mode", "parity", "deint"],
//                "hw_device": true}, ...],                // formats: "list" | "any" (passthrough) | "dynamic" (query func)
//   "interop": [{"from": "d3d11", "to": "qsv", "method": "map", "ok": true, "error": ""}, ...],
//   "presenters": [{"name": "d3d11", "ok": true, "error": "",
//                   "inputs": ["d3d11", "system"],                     // frame domains accepted without a host copy chain
//                   "import": {"cuda": true, "cuda_zero_copy": true}, // cuda pool registration: 0 copies; otherwise 1 device copy
//                   "vp": {"rotate": true, "mirror": true, "procamp": true,
//                          "brightness": [-100, 100], "contrast": [0, 10], "saturation": [0, 10],
//                          "deinterlace": ["bob", "adaptive"], "telecine": false},
//                   "decoder_srv": {"nv12": true, "p010": true}, "forced": ["p010=0"],   // vp also has "p010": bool
//                   "shader": true},
//                  {"name": "sdl", "ok": true, "inputs": ["system"]}]
// }
// Domains and device types use libavutil names: "cuda", "d3d11" (frames) / "d3d11va" (device), "d3d12" / "d3d12va", "opencl" (only
// as the domain of FFmpeg's own *_opencl filters: nothing probes or plans OpenCL devices), "qsv", "dxva2", "vulkan"; "system" = software frames in system memory.
#include <optional>
#include <string>
#include <vector>

namespace wvp {

/// Where a frame lives. Any = a filter that passes every pixel format through (setpts, null, ...), never a frame location.
enum class FrameDomain { System, Cuda, D3D11, D3D12, OpenCl, Qsv, Dxva2, Vulkan, Any };

/// "system", "cuda", "d3d11", "d3d12", "opencl", "qsv", "dxva2", "vulkan", "any".
const char* DomainName(FrameDomain d);
/// Accepts frame pixel-format / domain names ("cuda", "d3d11", "d3d11va" (alias), "opencl", "system", "yuv420p" -> System, ...).
/// nullopt for "" and for names that are neither a hw format nor a plausible software format.
std::optional<FrameDomain> DomainFromName(const std::string& name);
/// hwaccel / device type ("d3d11va", "cuda", "d3d12va", "dxva2", "qsv", "opencl", "vulkan") -> frame domain of its frames.
std::optional<FrameDomain> DomainOfDeviceType(const std::string& deviceType);
/// Inverse of DomainOfDeviceType for hardware domains ("d3d11va" for D3D11, ...), "" for System / Any.
std::string DeviceTypeOfDomain(FrameDomain d);
inline bool IsHardware(FrameDomain d) { return d != FrameDomain::System && d != FrameDomain::Any; }

struct AdapterCap {
    std::string name, luid, driver;
    int vendorId = 0;          // 0x10DE NVIDIA, 0x8086 Intel, 0x1002 AMD
};

struct HwDeviceCap {
    std::string type;          // device type: "cuda", "d3d11va", "qsv", ...
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
    std::string name;          // "sdl" | "d3d11"
    bool ok = false;
    std::string error;
    std::vector<FrameDomain> inputs;    // presented directly (System = one upload, a hw domain = no copy)
    bool importCuda = false;                         // device-side import; per-plane CUDA costs 1 device copy
    bool importCudaZeroCopy = false;               // registered CUDA pool: 0 device copies when importCuda; absent JSON defaults false
    bool vpRotate = false, vpMirror = false, vpProcamp = false, vpTelecine = false;
    double vpBrightnessMin = 0, vpBrightnessMax = 0, vpContrastMin = 0, vpContrastMax = 0, vpSaturationMin = 0, vpSaturationMax = 0;
    std::vector<std::string> vpDeinterlace;   // "bob", "adaptive", "motion"...
    bool shader = false;                      // D3D11/system pixel-shader path (full rotate/flip/eq); CUDA imports always have their own shader
    bool vpP010 = true;                       // VP takes P010 input ("vp":{"p010"}; absent = true); otherwise the shader route or a download
    bool decoderSrvNv12 = true, decoderSrvP010 = true;   // decoder pools bindable as SRV ("decoder_srv"; absent = true); otherwise a scratch copy
    std::vector<std::string> forced;          // capabilities masked by d3d11_force_caps ("p010=0", ...)
};

struct CapabilitySet {
    int schema = 1;
    std::string source;        // "probe" | "build" | "fake:<name>" | "legacy"
    std::string ffmpegVersion;
    AdapterCap adapter;
    std::vector<std::string> features;   // "hwframes_keep", "presenter_d3d11", "presenter_params", ...
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
/// Serializes to the same schema (round-trips through ParseCapabilities; used for the app-side cache and the overlay).
std::string CapabilitiesToJson(const CapabilitySet& caps);

/// Fake capability sets for unit tests and for running without a probe. All of them include the software filter set of this
/// build (fieldmatch, decimate, bwdif, yadif, transpose, hflip, vflip, eq, setpts, scale, format, hwupload, hwdownload, hwmap, ...).
namespace fakecaps {
/// ivtc_cuda is ALWAYS present (upstream builder-ffmpeg ships it): pix_fmts ["cuda"], options
/// tff/cthresh/dupthresh/scthresh.
CapabilitySet Nvidia(bool presenterD3D11 = true, bool presenterCudaImport = true, bool presenterCudaZeroCopy = false);
/// Nvidia() plus a made-up future IVTC variant "ivtc_npp" (suffix-only discovery: formats Dynamic, no pix_fmts, options unknown).
/// Only for generic discovery tests; ivtc_cuda stays present.
CapabilitySet NvidiaFutureIvtcVariant();
/// Intel / AMD: d3d11va (+ d3d12va, dxva2; Intel qsv) decoders, the D3D11 compute filters (ivtc_d3d11, bwdif_d3d11, ...) and
/// the driver post-processors of the build (denied by PipelineTable), no CUDA, a D3D11 presenter with VP rotate/mirror/procamp,
/// P010 input, decoder SRV and the shader route.
CapabilitySet Intel();
CapabilitySet Amd();
CapabilitySet None();
/// Decoders are not listed (decoder choice stays the user's setting).
CapabilitySet Legacy();
}

}
