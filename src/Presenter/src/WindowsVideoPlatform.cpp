#include "wvp/win32/WindowsVideoPlatform.h"

#include <windows.h>
#include <dxgi1_2.h>
#include <winternl.h>   // NTSTATUS for d3dkmthk.h
#include <d3dkmthk.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>

#include "wvp/FfPlayer.h"

#include "PresenterInternal.h"
#include "presenter_core.h"

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/log.h>
}

namespace wvp::win32 {

using Microsoft::WRL::ComPtr;

namespace {

std::string LuidText(const LUID& l) {
    char b[24];
    std::snprintf(b, sizeof b, "%08lx:%08lx", static_cast<unsigned long>(l.HighPart), static_cast<unsigned long>(l.LowPart));
    return b;
}

std::string Utf8(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::atomic<int> g_probeSeq{0};

enum class HelperResult { Answered, NoAnswer, Hung };

// Runs "<exe> --probe-gpu <temp file> <switches>" without a window and reads the line it writes ("" when the adapter cannot
// do video). NoAnswer: it could not start or failed; Hung: it ran over 30 s and was terminated (a probe hung in a driver;
// it is our own helper process).
HelperResult RunProbeHelper(const std::wstring& exePath, const std::wstring& switches, std::string& out) {
    wchar_t self[MAX_PATH * 2], tmp[MAX_PATH];
    std::wstring exe = exePath;
    if (exe.empty()) {
        if (!GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)))) return HelperResult::NoAnswer;
        exe = self;
    }
    if (!GetTempPathW(static_cast<DWORD>(std::size(tmp)), tmp)) return HelperResult::NoAnswer;
    const std::wstring file = std::wstring(tmp) + L"aribplayer-gpu-probe-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                              std::to_wstring(g_probeSeq++) + L".txt";
    std::wstring cmd = L"\"" + exe + L"\" --probe-gpu \"" + file + L"\"" + switches;
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return HelperResult::NoAnswer;
    DWORD code = 1;
    const bool hung = WaitForSingleObject(pi.hProcess, 30000) != WAIT_OBJECT_0;
    if (hung) TerminateProcess(pi.hProcess, 1);
    else GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    std::string text;
    bool answered = false;
    if (code == 0) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, file.c_str(), L"rb") == 0 && f) {
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
            answered = ferror(f) == 0;
            fclose(f);
        }
    }
    DeleteFileW(file.c_str());
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    out = std::move(text);
    return hung ? HelperResult::Hung : answered ? HelperResult::Answered : HelperResult::NoAnswer;
}

PresenterCap PresenterCapOf(const PrPresenterCaps& c) {
    PresenterCap p;
    p.name = "d3d11";
    p.ok = c.ok != 0;
    p.error = c.error;
    p.inputs = {FrameDomain::D3D11, FrameDomain::System};
    p.vpP010 = c.p010 != 0;
    p.decoderSrvNv12 = c.nv12_srv != 0;
    p.decoderSrvP010 = c.p010_srv != 0;
    if (c.deint_blend) p.vpDeinterlace.push_back("blend");
    if (c.deint_bob) p.vpDeinterlace.push_back("bob");
    if (c.deint_adaptive) p.vpDeinterlace.push_back("adaptive");
    if (c.deint_motion) p.vpDeinterlace.push_back("motion");
    p.vpTelecine = c.telecine != 0;
    p.shader = c.shader != 0;
    for (int i = 0; const char* name = pr_force_name(c.forced, i); i++) p.forced.push_back(name);
    return p;
}

// The engine's decoder entries, verified on the adapter: D3D11 decoder profiles / output formats.
DecoderCap Verified(const DecoderCap& engine, AVBufferRef* d3d11) {
    DecoderCap d = engine;
    if (!d.ok || d.hwaccel != "d3d11va") return d;
    int depths[2] = {0, 0};
    const int n = pr_d3d11_decoder_depths(d3d11, d.codec.c_str(), depths);
    d.bitDepths.assign(depths, depths + n);
    d.ok = n > 0 && depths[0] == 8;
    if (!d.ok) d.error = "the GPU has no 8-bit " + d.codec + " decoder profile";
    return d;
}

std::optional<CapabilitySet> ProbeAdapter(const std::string& adapterId, const PresenterOptions& options) {
    PrConfig cfg{};
    std::string error;
    if (!MakeConfig(adapterId, options, cfg, &error)) return std::nullopt;
    char err[256] = "";
    AVBufferRef* d3d11 = nullptr;
    if (pr_create_d3d11_device(&cfg, &d3d11, err, sizeof err) < 0) return std::nullopt;

    CapabilitySet platform;
    platform.source = "probe";
    char luid[24], name[256], driver[64];
    unsigned vendor = 0;
    pr_d3d11_adapter_info(d3d11, luid, sizeof luid, name, sizeof name, &vendor, driver, sizeof driver);
    platform.adapter.name = name;
    platform.adapter.luid = luid;
    platform.adapter.driver = driver;
    platform.adapter.vendorId = static_cast<int>(vendor);
    platform.devices.push_back({"d3d11va", true, name, ""});

    AVBufferRef* devices[1] = {d3d11};
    const std::optional<CapabilitySet> probed = FfPlayer::ProbeEngine(devices, 1);
    const CapabilitySet engine = probed ? *probed : CapabilitySet{};
    for (const DecoderCap& d : engine.decoders) platform.decoders.push_back(Verified(d, d3d11));

    PrPresenterCaps pc{};
    pr_probe_presenter(&cfg, &pc);
    platform.presenters.push_back(PresenterCapOf(pc));

    av_buffer_unref(&d3d11);
    return MergePlatformCapabilities(platform, engine);
}

std::wstring ProbeSwitches(const std::string& adapterId, const PresenterOptions& o) {
    std::wstring s;
    if (!adapterId.empty()) s += L" --adapter-luid=" + Wide(adapterId);
    if (o.debug) s += L" --d3d11-debug";
    if (!o.forceCaps.empty()) s += L" \"--d3d11-force-caps=" + Wide(o.forceCaps) + L"\"";
    return s;
}

}

bool MakeConfig(const std::string& adapterId, const PresenterOptions& options, PrConfig& cfg, std::string* error) {
    char err[256] = "";
    cfg = PrConfig{};
    if (!adapterId.empty() && pr_parse_luid(adapterId.c_str(), &cfg) < 0) {
        if (error) *error = "invalid adapter id '" + adapterId + "'";
        return false;
    }
    if (!options.forceCaps.empty() && pr_parse_force(options.forceCaps.c_str(), &cfg.force, err, sizeof err) < 0) {
        if (error) *error = err;
        return false;
    }
    if (options.route == "vp") cfg.route = PR_ROUTE_VP;
    else if (options.route == "shader") cfg.route = PR_ROUTE_SHADER;
    else if (!options.route.empty()) {
        if (error) *error = "invalid presenter route '" + options.route + "' (vp, shader)";
        return false;
    }
    cfg.debug = options.debug ? 1 : 0;
    cfg.srv_fail = options.srvFail;
    return true;
}

WindowsVideoPlatform::WindowsVideoPlatform(PresenterOptions options) : options_(std::move(options)) {}

std::vector<AdapterEntry> WindowsVideoPlatform::Adapters() {
    std::vector<AdapterEntry> out;
    ComPtr<IDXGIFactory1> f;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return out;
    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++, a.Reset()) {
        DXGI_ADAPTER_DESC1 d{};
        if (FAILED(a->GetDesc1(&d)) || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || d.VendorId == 0x1414) continue;
        AdapterEntry e;
        e.luid = LuidText(d.AdapterLuid);
        e.name = Utf8(d.Description);
        e.vendorId = static_cast<int>(d.VendorId);
        e.deviceId = static_cast<int>(d.DeviceId);
        D3DKMT_OPENADAPTERFROMLUID open{};
        open.AdapterLuid = d.AdapterLuid;
        if (D3DKMTOpenAdapterFromLuid(&open) == 0) {
            D3DKMT_ADAPTERADDRESS addr{};
            D3DKMT_QUERYADAPTERINFO q{};
            q.hAdapter = open.hAdapter;
            q.Type = KMTQAITYPE_ADAPTERADDRESS;
            q.pPrivateDriverData = &addr;
            q.PrivateDriverDataSize = sizeof addr;
            // a virtual adapter (indirect display) answers with all-ones: no PCI location
            if (D3DKMTQueryAdapterInfo(&q) == 0 && addr.BusNumber != 0xFFFFFFFFu) {
                e.hasPci = true;
                e.bus = static_cast<int>(addr.BusNumber);
                e.device = static_cast<int>(addr.DeviceNumber);
                e.function = static_cast<int>(addr.FunctionNumber);
            }
            D3DKMT_CLOSEADAPTER close{};
            close.hAdapter = open.hAdapter;
            D3DKMTCloseAdapter(&close);
        }
        ComPtr<IDXGIOutput> o;
        e.hasOutputs = a->EnumOutputs(0, &o) != DXGI_ERROR_NOT_FOUND;
        out.push_back(std::move(e));
    }
    return out;
}

std::string WindowsVideoPlatform::DisplayAdapter(void* view) {
    HMONITOR mon = MonitorFromWindow(static_cast<HWND>(view), MONITOR_DEFAULTTONEAREST);
    ComPtr<IDXGIFactory1> f;
    if (!mon || FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return {};
    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++, a.Reset()) {
        ComPtr<IDXGIOutput> o;
        for (UINT k = 0; a->EnumOutputs(k, &o) != DXGI_ERROR_NOT_FOUND; k++, o.Reset()) {
            DXGI_OUTPUT_DESC od{};
            if (SUCCEEDED(o->GetDesc(&od)) && od.Monitor == mon) {
                DXGI_ADAPTER_DESC1 d{};
                if (SUCCEEDED(a->GetDesc1(&d))) return LuidText(d.AdapterLuid);
            }
        }
    }
    return {};
}

std::optional<CapabilitySet> WindowsVideoPlatform::Probe(const AdapterEntry& adapter) {
    std::string json;
    switch (RunProbeHelper(probeExe_, ProbeSwitches(adapter.luid, options_), json)) {
    case HelperResult::Answered:
        if (json.empty()) return std::nullopt;
        return ParseCapabilities(json);
    case HelperResult::NoAnswer:
        av_log(nullptr, AV_LOG_WARNING, "GPU probe helper gave no answer for %s (luid %s): probing in this process\n",
               adapter.name.c_str(), adapter.luid.c_str());
        return ProbeInProcess(adapter);
    case HelperResult::Hung:
        // the driver hangs the probe: running it here would hang this process
        av_log(nullptr, AV_LOG_ERROR, "GPU probe helper hung for %s (luid %s): the adapter is not used for video\n",
               adapter.name.c_str(), adapter.luid.c_str());
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<CapabilitySet> WindowsVideoPlatform::ProbeInProcess(const AdapterEntry& adapter) const {
    return ProbeAdapter(adapter.luid, options_);
}

std::string WindowsVideoPlatform::ProbeChildJson(const std::string& adapterId, const PresenterOptions& options) {
    const std::optional<CapabilitySet> caps = ProbeAdapter(adapterId, options);
    if (!caps) return {};
    std::string json = CapabilitiesToJson(*caps);
    for (char& c : json)
        if (c == '\r' || c == '\n') c = ' ';
    return json;
}

PresenterHandle WindowsVideoPlatform::CreatePresenter(const AdapterEntry& adapter, void* view, std::string* error) {
    if (!view || !IsWindow(static_cast<HWND>(view))) {
        if (error) *error = "the d3d11 presenter needs the HWND of the video view";
        return nullptr;
    }
    return D3D11Presenter::Create(view, adapter.luid, options_, error);
}

}
