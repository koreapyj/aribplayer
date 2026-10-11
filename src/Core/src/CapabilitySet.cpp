#include "wvp/CapabilitySet.h"

#include <algorithm>
#include <cmath>

#include "wvp/JsonLite.h"
#include "wvp/JsonWriter.h"

namespace wvp {

namespace {

using json::Value;

struct DomName { FrameDomain d; const char* name; };
constexpr DomName kDomains[] = {
    {FrameDomain::System, "system"}, {FrameDomain::Cuda, "cuda"}, {FrameDomain::D3D11, "d3d11"}, {FrameDomain::D3D12, "d3d12"},
    {FrameDomain::OpenCl, "opencl"}, {FrameDomain::Qsv, "qsv"}, {FrameDomain::Dxva2, "dxva2"}, {FrameDomain::Vulkan, "vulkan"},
    {FrameDomain::VideoToolbox, "videotoolbox"}, {FrameDomain::Any, "any"},
};

std::vector<std::string> StrArray(const Value& o, const char* key) {
    std::vector<std::string> r;
    const Value* v = o.Find(key);
    if (v && v->IsArray())
        for (auto& e : v->arr)
            if (e.kind == Value::Kind::String) r.push_back(e.s);
    return r;
}
bool IsNum(const Value& v) { return v.kind == Value::Kind::Number && std::isfinite(v.n); }
int ClampInt(double d) {
    if (d > 2147483647.0) return 2147483647;
    if (d < -2147483648.0) return -2147483647 - 1;
    return (int)d;
}
int IntOf(const Value& o, const char* key, int def) {
    const Value* v = o.Find(key);
    return v && IsNum(*v) ? ClampInt(v->n) : def;
}
const Value* ObjectOf(const Value& o, const char* key) {
    const Value* v = o.Find(key);
    return v && v->IsObject() ? v : nullptr;
}
template <class F> void EachObject(const Value& o, const char* key, F&& f) {
    const Value* v = o.Find(key);
    if (v && v->IsArray())
        for (auto& e : v->arr)
            if (e.IsObject()) f(e);
}

const char* FormatsName(FilterCap::Formats f) {
    switch (f) {
    case FilterCap::Formats::List: return "list";
    case FilterCap::Formats::Any: return "any";
    case FilterCap::Formats::Dynamic: return "dynamic";
    default: return "unknown";
    }
}
FilterCap::Formats FormatsFromName(const std::string& s) {
    if (s == "list") return FilterCap::Formats::List;
    if (s == "any") return FilterCap::Formats::Any;
    if (s == "dynamic") return FilterCap::Formats::Dynamic;
    return FilterCap::Formats::Unknown;
}

void WriteStrArray(JsonWriter& w, const char* key, const std::vector<std::string>& v) {
    w.Key(key).BeginArray();
    for (auto& s : v) w.String(std::string_view(s));
    w.EndArray();
}

}

const char* DomainName(FrameDomain d) {
    for (auto& e : kDomains) if (e.d == d) return e.name;
    return "system";
}

std::optional<FrameDomain> DomainFromName(const std::string& name) {
    if (name.empty()) return std::nullopt;
    if (name == "d3d11va") return FrameDomain::D3D11;
    if (name == "d3d12va") return FrameDomain::D3D12;
    if (name == "dxva2_vld") return FrameDomain::Dxva2;
    if (name == "videotoolbox_vld") return FrameDomain::VideoToolbox;
    for (auto& e : kDomains) if (name == e.name) return e.d;
    for (char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return std::nullopt;
    return FrameDomain::System;
}

std::optional<FrameDomain> DomainOfDeviceType(const std::string& t) {
    if (t == "d3d11va") return FrameDomain::D3D11;
    if (t == "d3d12va") return FrameDomain::D3D12;
    if (t == "dxva2") return FrameDomain::Dxva2;
    if (t == "qsv") return FrameDomain::Qsv;
    if (t == "opencl") return FrameDomain::OpenCl;
    if (t == "vulkan") return FrameDomain::Vulkan;
    if (t == "videotoolbox") return FrameDomain::VideoToolbox;
    return std::nullopt;
}

std::string DeviceTypeOfDomain(FrameDomain d) {
    switch (d) {
    case FrameDomain::D3D11: return "d3d11va";
    case FrameDomain::D3D12: return "d3d12va";
    case FrameDomain::Dxva2: return "dxva2";
    case FrameDomain::Qsv: return "qsv";
    case FrameDomain::OpenCl: return "opencl";
    case FrameDomain::Vulkan: return "vulkan";
    case FrameDomain::VideoToolbox: return "videotoolbox";
    default: return "";
    }
}

bool CapabilitySet::HasFeature(const std::string& f) const {
    return std::find(features.begin(), features.end(), f) != features.end();
}
bool CapabilitySet::DeviceOk(const std::string& type) const {
    for (auto& d : devices) if (d.type == type && d.ok) return true;
    return false;
}
const FilterCap* CapabilitySet::Filter(const std::string& name) const {
    for (auto& f : filters) if (f.name == name) return &f;
    return nullptr;
}
const PresenterCap* CapabilitySet::Presenter(const std::string& name) const {
    for (auto& p : presenters) if (p.name == name) return &p;
    return nullptr;
}
const DecoderCap* CapabilitySet::Decoder(const std::string& codec, const std::string& hwaccel) const {
    if (codec.empty()) return nullptr;
    for (auto& d : decoders) if (d.codec == codec && d.hwaccel == hwaccel) return &d;
    return nullptr;
}
bool CapabilitySet::InteropOk(FrameDomain from, FrameDomain to, const std::string& method) const {
    for (auto& i : interop)
        if (i.from == from && i.to == to && i.ok && (method.empty() || i.method == method)) return true;
    return false;
}

CapabilitySet MergeBuildCapabilities(const CapabilitySet& runtime, const CapabilitySet& build) {
    CapabilitySet c = runtime;
    c.filters = build.filters;
    c.features = build.features;
    for (const std::string& f : runtime.features)
        if (std::find(c.features.begin(), c.features.end(), f) == c.features.end()) c.features.push_back(f);
    c.decoders.clear();
    for (const DecoderCap& d : runtime.decoders) {
        const DecoderCap* s = build.Decoder(d.codec, d.hwaccel);
        if (s && s->decoder == d.decoder) c.decoders.push_back(d);
    }
    return c;
}

CapabilitySet MergePlatformCapabilities(const CapabilitySet& platform, const CapabilitySet& engine) {
    CapabilitySet c = platform;
    if (c.ffmpegVersion.empty()) c.ffmpegVersion = engine.ffmpegVersion;
    for (const HwDeviceCap& e : engine.devices) {
        auto it = std::find_if(c.devices.begin(), c.devices.end(), [&](const HwDeviceCap& d) { return d.type == e.type; });
        if (it == c.devices.end()) { c.devices.push_back(e); continue; }
        if (it->ok && !e.ok) { it->ok = false; it->error = e.error; }
    }
    for (DecoderCap& d : c.decoders)
        if (d.decoder.empty())
            if (const DecoderCap* e = engine.Decoder(d.codec, d.hwaccel)) d.decoder = e->decoder;
    c.interop = engine.interop;
    for (const std::string& f : engine.features)
        if (std::find(c.features.begin(), c.features.end(), f) == c.features.end()) c.features.push_back(f);
    return c;
}

std::optional<CapabilitySet> ParseCapabilities(const std::string& text) {
    auto root = json::Parse(text);
    if (!root || !root->IsObject()) return std::nullopt;
    const Value& r = *root;
    CapabilitySet c;
    c.schema = IntOf(r, "schema", 1);
    c.source = r.HasString("source") ? r.Str("source") : "probe";
    c.ffmpegVersion = r.Str("ffmpeg");
    if (const Value* a = ObjectOf(r, "adapter")) {
        c.adapter.name = a->Str("name"); c.adapter.luid = a->Str("luid"); c.adapter.driver = a->Str("driver");
        c.adapter.vendorId = IntOf(*a, "vendor_id", 0);
    }
    c.features = StrArray(r, "features");
    EachObject(r, "devices", [&](const Value& e) {
        HwDeviceCap d; d.type = e.Str("type"); d.ok = e.Bool("ok"); d.name = e.Str("name"); d.error = e.Str("error");
        c.devices.push_back(std::move(d));
    });
    EachObject(r, "decoders", [&](const Value& e) {
        DecoderCap d; d.codec = e.Str("codec"); d.hwaccel = e.Str("hwaccel"); d.decoder = e.Str("decoder"); d.ok = e.Bool("ok");
        const Value* b = e.Find("bit_depths");
        if (b && b->IsArray())
            for (auto& x : b->arr) if (IsNum(x)) d.bitDepths.push_back(ClampInt(x.n));
        d.maxWidth = IntOf(e, "max_width", 0); d.maxHeight = IntOf(e, "max_height", 0); d.error = e.Str("error");
        c.decoders.push_back(std::move(d));
    });
    EachObject(r, "filters", [&](const Value& e) {
        FilterCap f; f.name = e.Str("name"); f.formats = FormatsFromName(e.Str("formats"));
        f.pixFmts = StrArray(e, "pix_fmts");
        f.optionsKnown = e.Find("options") != nullptr;
        f.options = StrArray(e, "options");
        f.hwDevice = e.Bool("hw_device");
        f.flagsCombed = e.Bool("flags_combed");
        c.filters.push_back(std::move(f));
    });
    EachObject(r, "interop", [&](const Value& e) {
        auto from = DomainFromName(e.Str("from")), to = DomainFromName(e.Str("to"));
        if (!from || !to) return;
        InteropCap i; i.from = *from; i.to = *to; i.method = e.Str("method"); i.ok = e.Bool("ok"); i.error = e.Str("error");
        c.interop.push_back(std::move(i));
    });
    EachObject(r, "presenters", [&](const Value& e) {
        PresenterCap p; p.name = e.Str("name"); p.ok = e.Bool("ok"); p.error = e.Str("error");
        for (auto& s : StrArray(e, "inputs"))
            if (auto d = DomainFromName(s)) p.inputs.push_back(*d);
        if (const Value* vp = ObjectOf(e, "vp")) {
            p.vpTelecine = vp->Bool("telecine");
            p.vpDeinterlace = StrArray(*vp, "deinterlace");
            if (vp->Find("p010")) p.vpP010 = vp->Bool("p010");
        }
        if (const Value* ds = ObjectOf(e, "decoder_srv")) {
            if (ds->Find("nv12")) p.decoderSrvNv12 = ds->Bool("nv12");
            if (ds->Find("p010")) p.decoderSrvP010 = ds->Bool("p010");
        }
        p.forced = StrArray(e, "forced");
        p.shader = e.Bool("shader");
        c.presenters.push_back(std::move(p));
    });
    return c;
}

std::string CapabilitiesToJson(const CapabilitySet& c) {
    JsonWriter w(true);
    w.BeginObject();
    w.Prop("schema", c.schema).Prop("source", c.source).Prop("ffmpeg", c.ffmpegVersion);
    w.Key("adapter").BeginObject();
    w.Prop("name", c.adapter.name).Prop("vendor_id", c.adapter.vendorId).Prop("luid", c.adapter.luid).Prop("driver", c.adapter.driver);
    w.EndObject();
    WriteStrArray(w, "features", c.features);
    w.Key("devices").BeginArray();
    for (auto& d : c.devices) { w.BeginObject(); w.Prop("type", d.type).Prop("ok", d.ok).Prop("name", d.name).Prop("error", d.error); w.EndObject(); }
    w.EndArray();
    w.Key("decoders").BeginArray();
    for (auto& d : c.decoders) {
        w.BeginObject();
        w.Prop("codec", d.codec).Prop("hwaccel", d.hwaccel).Prop("decoder", d.decoder).Prop("ok", d.ok);
        w.Key("bit_depths").BeginArray();
        for (int b : d.bitDepths) w.Int(b);
        w.EndArray();
        w.Prop("max_width", d.maxWidth).Prop("max_height", d.maxHeight).Prop("error", d.error);
        w.EndObject();
    }
    w.EndArray();
    w.Key("filters").BeginArray();
    for (auto& f : c.filters) {
        w.BeginObject();
        w.Prop("name", f.name).Prop("formats", FormatsName(f.formats));
        WriteStrArray(w, "pix_fmts", f.pixFmts);
        if (f.optionsKnown) WriteStrArray(w, "options", f.options);
        w.Prop("hw_device", f.hwDevice);
        if (f.flagsCombed) w.Prop("flags_combed", true);
        w.EndObject();
    }
    w.EndArray();
    w.Key("interop").BeginArray();
    for (auto& i : c.interop) {
        w.BeginObject();
        w.Prop("from", DomainName(i.from)).Prop("to", DomainName(i.to)).Prop("method", i.method).Prop("ok", i.ok).Prop("error", i.error);
        w.EndObject();
    }
    w.EndArray();
    w.Key("presenters").BeginArray();
    for (auto& p : c.presenters) {
        w.BeginObject();
        w.Prop("name", p.name).Prop("ok", p.ok).Prop("error", p.error);
        w.Key("inputs").BeginArray();
        for (auto d : p.inputs) w.String(std::string_view(DomainName(d)));
        w.EndArray();
        w.Key("vp").BeginObject();
        WriteStrArray(w, "deinterlace", p.vpDeinterlace);
        w.Prop("telecine", p.vpTelecine).Prop("p010", p.vpP010);
        w.EndObject();
        w.Key("decoder_srv").BeginObject().Prop("nv12", p.decoderSrvNv12).Prop("p010", p.decoderSrvP010).EndObject();
        if (!p.forced.empty()) WriteStrArray(w, "forced", p.forced);
        w.Prop("shader", p.shader);
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    return w.Str();
}

namespace fakecaps {
namespace {

using Fm = FilterCap::Formats;
const std::vector<std::string> kSw = {"yuv420p", "nv12", "yuv420p10le", "p010le"};

FilterCap F(const char* name, Fm fm, std::vector<std::string> pix = {}, std::vector<std::string> opts = {}, bool optionsKnown = false, bool hwDevice = false) {
    FilterCap f; f.name = name; f.formats = fm; f.pixFmts = std::move(pix); f.options = std::move(opts); f.optionsKnown = optionsKnown; f.hwDevice = hwDevice;
    return f;
}

void AddSoftwareFilters(CapabilitySet& c) {
    for (const char* n : {"fieldmatch", "decimate", "bwdif", "yadif", "transpose", "hflip", "vflip", "eq", "scale"})
        c.filters.push_back(F(n, Fm::List, kSw));
    c.filters.push_back(F("setpts", Fm::Any));
    c.filters.push_back(F("null", Fm::Any));
    c.filters.push_back(F("format", Fm::Dynamic));
    c.filters.push_back(F("hwupload", Fm::Dynamic, {}, {}, false, true));
    c.filters.push_back(F("hwdownload", Fm::Dynamic));
    c.filters.push_back(F("hwmap", Fm::Dynamic, {}, {}, false, true));
}

const std::vector<std::string> kIvtcOpts = {"tff", "cthresh", "dupthresh", "scthresh"};

// The D3D11 compute filters and the driver post-processors of this FFmpeg build (ffmpeg-caps.inc), on every GPU vendor.
void AddD3D11Filters(CapabilitySet& c) {
    const std::vector<std::string> d = {"d3d11"};
    const std::vector<std::string> deint = {"mode", "parity", "deint", "force_output_copy"};
    c.filters.push_back(F("bwdif_d3d11", Fm::List, d, deint, true, true));
    c.filters.push_back(F("yadif_d3d11", Fm::List, d, deint, true, true));
    c.filters.push_back(F("ivtc_d3d11", Fm::List, d, {"tff", "cthresh", "dupthresh", "scthresh", "verbose", "force_output_copy"}, true, true));
    c.filters.push_back(F("scale_d3d11", Fm::List, d, {"w", "width", "h", "height", "format", "force_output_copy"}, true, true));
    c.filters.push_back(F("transpose_d3d11", Fm::List, d, {"dir", "passthrough"}, true, true));
    c.filters.push_back(F("overlay_d3d11", Fm::List, d, {"x", "y", "w", "h", "alpha", "force_output_copy"}, true, true));
    c.filters.push_back(F("tonemap_d3d11", Fm::Dynamic, {}, {"tonemap", "format", "force_output_copy"}, true, true));
    c.filters.push_back(F("deinterlace_qsv", Fm::List, {"qsv"}, {"mode", "async_depth"}, true, true));
    c.filters.push_back(F("vpp_qsv", Fm::Dynamic, {}, {"deinterlace", "denoise", "procamp", "transpose"}, true, true));
    c.filters.push_back(F("vpp_amf", Fm::Dynamic, {}, {"w", "h", "format"}, true, true));
}

void AddDevice(CapabilitySet& c, const char* type, bool ok, const std::string& name = "") {
    HwDeviceCap d; d.type = type; d.ok = ok; d.name = name;
    c.devices.push_back(std::move(d));
}

void AddDecoders(CapabilitySet& c, const char* hwaccel, bool dxva2Only) {
    struct Cd { const char* codec; std::vector<int> depths; int maxDim; };
    std::vector<Cd> list = {{"h264", {8}, 8192}, {"hevc", {8, 10}, 8192}, {"mpeg2video", {8}, 4096}};
    if (!dxva2Only) { list.push_back({"vp9", {8, 10}, 8192}); list.push_back({"av1", {8, 10}, 8192}); }
    for (auto& e : list) {
        DecoderCap d; d.codec = e.codec; d.hwaccel = hwaccel; d.decoder = e.codec; d.ok = true; d.bitDepths = e.depths;
        d.maxWidth = d.maxHeight = e.maxDim;
        c.decoders.push_back(std::move(d));
    }
}

// The D3D11 presenter without a hardware device (WARP): system frames, shader route.
PresenterCap SystemPresenter() {
    PresenterCap p; p.name = "d3d11"; p.ok = true; p.inputs = {FrameDomain::System};
    p.shader = true;
    return p;
}

PresenterCap D3D11Presenter() {
    PresenterCap p; p.name = "d3d11"; p.ok = true; p.inputs = {FrameDomain::D3D11, FrameDomain::System};
    p.vpTelecine = false;
    p.vpDeinterlace = {"bob", "adaptive"};
    p.shader = true;
    return p;
}

// Intel / AMD: VideoProcessor with P010 input (also motion-compensated deinterlacing), the shader route, decoder SRV for
// NV12 and P010.
PresenterCap D3D11PresenterIntelAmd() {
    PresenterCap p = D3D11Presenter();
    p.vpDeinterlace = {"bob", "adaptive", "motion"};
    return p;
}

CapabilitySet Base(const char* source) {
    CapabilitySet c;
    c.source = source; c.ffmpegVersion = "n7.1.4-3+3";
    return c;
}

void CommonHw(CapabilitySet& c) {
    c.features = {"hwframes_keep"};
}

// CUDA filters an FFmpeg build may carry: their frames live in a domain no device, decoder, interop or presenter of the
// player provides, so they are never planned.
void AddCudaFilters(CapabilitySet& c) {
    const std::vector<std::string> cu = {"cuda"};
    c.filters.push_back(F("bwdif_cuda", Fm::List, cu, {"mode", "parity", "deint"}, true, true));
    c.filters.push_back(F("yadif_cuda", Fm::List, cu, {"mode", "parity", "deint"}, true, true));
    c.filters.push_back(F("scale_cuda", Fm::List, cu, {}, false, true));
    c.filters.push_back(F("ivtc_cuda", Fm::List, cu, kIvtcOpts, true, true));
    c.filters.push_back(F("scale_npp", Fm::Dynamic, {}, {}, false, true));
}

}

CapabilitySet Nvidia(bool presenterD3D11) {
    CapabilitySet c = Base("fake:nvidia");
    c.adapter.name = "NVIDIA GeForce RTX 5070"; c.adapter.luid = "00000000:0001d2c4"; c.adapter.driver = "32.0.15.8129"; c.adapter.vendorId = 0x10DE;
    CommonHw(c);
    for (const char* t : {"d3d11va", "d3d12va", "dxva2"}) AddDevice(c, t, true, c.adapter.name);
    AddDecoders(c, "d3d11va", false); AddDecoders(c, "d3d12va", false); AddDecoders(c, "dxva2", true);
    AddSoftwareFilters(c);
    AddD3D11Filters(c);
    AddCudaFilters(c);
    c.presenters.push_back(presenterD3D11 ? D3D11Presenter() : SystemPresenter());
    return c;
}

CapabilitySet Intel() {
    CapabilitySet c = Base("fake:intel");
    c.adapter.name = "Intel(R) Arc(TM) Graphics"; c.adapter.luid = "00000000:0000a1b2"; c.adapter.driver = "32.0.101.6000"; c.adapter.vendorId = 0x8086;
    CommonHw(c);
    for (const char* t : {"d3d11va", "d3d12va", "dxva2", "qsv"}) AddDevice(c, t, true, c.adapter.name);
    AddDecoders(c, "d3d11va", false); AddDecoders(c, "d3d12va", false); AddDecoders(c, "dxva2", true);
    AddSoftwareFilters(c);
    AddD3D11Filters(c);
    c.filters.push_back(F("scale_qsv", Fm::List, {"qsv"}, {"w", "h", "format", "mode", "async_depth"}, true, true));
    InteropCap b; b.from = FrameDomain::D3D11; b.to = FrameDomain::Qsv; b.method = "map"; b.ok = true;
    c.interop = {b};
    c.presenters.push_back(D3D11PresenterIntelAmd());
    return c;
}

CapabilitySet Amd() {
    CapabilitySet c = Base("fake:amd");
    c.adapter.name = "AMD Radeon RX 7800 XT"; c.adapter.luid = "00000000:0000c3d4"; c.adapter.driver = "32.0.12033.1030"; c.adapter.vendorId = 0x1002;
    CommonHw(c);
    for (const char* t : {"d3d11va", "d3d12va", "dxva2"}) AddDevice(c, t, true, c.adapter.name);
    AddDecoders(c, "d3d11va", false); AddDecoders(c, "d3d12va", false); AddDecoders(c, "dxva2", true);
    AddSoftwareFilters(c);
    AddD3D11Filters(c);
    c.presenters.push_back(D3D11PresenterIntelAmd());
    return c;
}

CapabilitySet None() {
    CapabilitySet c = Base("fake:none");
    AddSoftwareFilters(c);
    c.presenters.push_back(SystemPresenter());
    return c;
}

CapabilitySet Mac() {
    CapabilitySet c = Base("fake:mac");
    c.adapter.name = "Apple M3"; c.adapter.luid = "apple-m3";
    c.features = {"hwframes_keep"};
    AddDevice(c, "videotoolbox", true, c.adapter.name);
    AddDecoders(c, "videotoolbox", false);
    AddSoftwareFilters(c);
    PresenterCap p; p.name = "metal"; p.ok = true; p.inputs = {FrameDomain::VideoToolbox, FrameDomain::System};
    p.shader = true;
    c.presenters.push_back(p);
    return c;
}

}
}
