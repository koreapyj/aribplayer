// Pause / resume latency: from the API call (ffp_pause / ffp_play / ffp_toggle_pause) to the last presented frame and the last
// decoded audio handed to the audio device (pause), and to the first presented frame / first audio (resume). libffplay reports
// the stages in the debug info "pause_timing" object; the audible moment is that hand-over plus the device's
// in-flight audio ("audio_device_ms", two device buffers). Medians over several cycles keep the gate robust against load.
//
// PauseLatency_loopback_wasapi is opt-in (WVP_PAUSE_LOOPBACK=1, run alone in its process: it selects SDL's wasapi driver): it plays
// the 440 Hz test tone at a very low volume on the default output and measures when the tone really stops / starts in a WASAPI
// process-loopback capture (this process only, so other applications' sound cannot interfere).
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <audioclientactivationparams.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "wvp/DebugInfo.h"
#include "wvp/FfPlayer.h"
#include "wvp/JsonLite.h"
#include "wvp/Utf.h"
#include "wvptest.h"

#pragma comment(lib, "mmdevapi.lib")

using namespace wvp;
namespace fs = std::filesystem;

namespace {

constexpr UINT kPauseNotifyMsg = WM_APP + 61;

LRESULT CALLBACK PauseHostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == kPauseNotifyMsg) { FfPlayer::HandleNotify(w, l); return 0; }
    return DefWindowProcW(h, m, w, l);
}

struct Host {
    HWND parent = nullptr, child = nullptr;
    Host() {
        static ATOM a = [] {
            WNDCLASSW wc{};
            wc.lpfnWndProc = PauseHostProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"WvpPauseLatencyHost";
            return RegisterClassW(&wc);
        }();
        (void)a;
        HINSTANCE inst = GetModuleHandleW(nullptr);
        parent = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"WvpPauseLatencyHost", L"host", WS_POPUP, -32000, -32000, 960, 540,
                                 nullptr, nullptr, inst, nullptr);
        child = CreateWindowExW(0, L"STATIC", L"video", WS_CHILD | WS_VISIBLE, 0, 0, 960, 540, parent, nullptr, inst, nullptr);
    }
    ~Host() { if (parent) DestroyWindow(parent); }
};

fs::path RepoRoot() {
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

void Pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}

template <class F>
bool WaitFor(F cond, double seconds) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds((long long)(seconds * 1000));
    while (std::chrono::steady_clock::now() < end) {
        Pump();
        if (cond()) return true;
        Sleep(5);
    }
    Pump();
    return cond();
}

void Delay(double seconds) { WaitFor([] { return false; }, seconds); }

double QpcUs() {
    static const double freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return (double)f.QuadPart; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart * 1e6 / freq;
}

struct PauseTiming {
    bool valid = false;
    int want = -1;
    double nowUs = 0, apiUs = 0, applyUs = 0, lastFrameUs = 0, firstFrameUs = 0, audioLastEndUs = 0, audioFirstUs = 0;
    int framesAfter = 0;
    double audioAfterMs = NAN, audioDeviceMs = NAN;
};

PauseTiming ReadTiming(FfPlayer& p) {
    PauseTiming t;
    auto js = p.GetDebugInfoJson();
    if (!js) return t;
    auto v = json::Parse(*js);
    if (!v) return t;
    const json::Value* pt = v->Find("pause_timing");
    if (!pt || !pt->IsObject()) return t;
    t.valid = true;
    t.want = pt->Int("want");
    t.nowUs = pt->Dbl("now_us");
    t.apiUs = pt->Dbl("api_us");
    t.applyUs = pt->Dbl("apply_us");
    t.lastFrameUs = pt->Dbl("last_frame_us");
    t.framesAfter = pt->Int("frames_after");
    t.firstFrameUs = pt->Dbl("first_frame_us");
    t.audioLastEndUs = pt->Dbl("audio_last_end_us");
    t.audioFirstUs = pt->Dbl("audio_first_us");
    const json::Value* a = pt->Find("audio_after_ms");
    if (a && a->kind == json::Value::Kind::Number) t.audioAfterMs = a->n;
    const json::Value* d = pt->Find("audio_device_ms");
    if (d && d->kind == json::Value::Kind::Number) t.audioDeviceMs = d->n;
    return t;
}

// One measured transition, in ms after the API call.
struct Transition {
    double applyMs = NAN;    // loop thread applied the command
    double videoMs = NAN;    // pause: last frame presented (apply when none came after it); resume: first frame presented
    double audioMs = NAN;    // pause: last decoded audio handed over + device in-flight; resume: first audio + device in-flight
    int framesAfter = 0;     // pause: frames presented after the apply
};

Transition Evaluate(const PauseTiming& t, bool pause) {
    Transition r;
    if (!t.valid || t.apiUs <= 0) return r;
    r.applyMs = (t.applyUs - t.apiUs) / 1000.0;
    const double dev = std::isfinite(t.audioDeviceMs) ? t.audioDeviceMs : 0.0;
    if (pause) {
        r.framesAfter = t.framesAfter;
        r.videoMs = t.framesAfter > 0 ? (t.lastFrameUs - t.apiUs) / 1000.0 : std::max(0.0, r.applyMs);
        if (t.audioLastEndUs > 0) r.audioMs = std::max(0.0, (t.audioLastEndUs - t.apiUs) / 1000.0) + dev;
    } else {
        if (t.firstFrameUs > 0) r.videoMs = (t.firstFrameUs - t.apiUs) / 1000.0;
        if (t.audioFirstUs > 0) r.audioMs = (t.audioFirstUs - t.apiUs) / 1000.0 + dev;
    }
    return r;
}

double Median(std::vector<double> v) {
    v.erase(std::remove_if(v.begin(), v.end(), [](double x) { return !std::isfinite(x); }), v.end());
    if (v.empty()) return NAN;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

double Max(const std::vector<double>& v) {
    double m = NAN;
    for (double x : v) if (std::isfinite(x)) m = std::isfinite(m) ? std::max(m, x) : x;
    return m;
}

struct Config {
    const char* name;
    fs::path file;
    OpenOptions options;
    double frameMs;   // output frame interval
};

struct Summary {
    std::vector<double> pauseApply, pauseVideo, pauseAudio, resumeApply, resumeVideo, resumeAudio;
    std::vector<double> pauseHandoff, pauseAfter, resumeHandoff;   // device hand-over without the in-flight estimate
    int framesAfterPauseMax = 0;
    double avDiffAfterResumeMs = NAN;
    std::string audioDevice;
    double deviceMs = NAN;
};

Summary MeasureCycles(FfPlayer& player, int cycles, const char* config) {
    Summary s;
    for (int i = 0; i < cycles; ++i) {
        const int api = i % 3;   // 0: ffp_pause / ffp_play, 1: ffp_toggle_pause, 2: mixed (pause via toggle, play via ffp_play)
        Delay(0.45 + 0.05 * (i % 4));   // play a while (also lets the previous resume settle)
        REQUIRE_MSG((api == 0 ? player.Pause() : player.TogglePause()) == 0, "pause request failed");
        Delay(0.6);
        PauseTiming tp = ReadTiming(player);
        Transition p = Evaluate(tp, true);
        CHECK_MSG(tp.want == 1, std::string(config) + ": pause_timing does not describe a pause");
        REQUIRE_MSG(player.State() == PlayerState::Paused, std::string(config) + ": player did not pause");
        REQUIRE_MSG((api == 1 ? player.TogglePause() : player.Play()) == 0, "resume request failed");
        Delay(0.6);
        PauseTiming tr = ReadTiming(player);
        Transition r = Evaluate(tr, false);
        CHECK_MSG(tr.want == 0, std::string(config) + ": pause_timing does not describe a resume");
        s.pauseApply.push_back(p.applyMs); s.pauseVideo.push_back(p.videoMs); s.pauseAudio.push_back(p.audioMs);
        s.resumeApply.push_back(r.applyMs); s.resumeVideo.push_back(r.videoMs); s.resumeAudio.push_back(r.audioMs);
        s.pauseHandoff.push_back(p.audioMs - (std::isfinite(tp.audioDeviceMs) ? tp.audioDeviceMs : 0.0));
        s.pauseAfter.push_back(tp.audioAfterMs);
        s.resumeHandoff.push_back(r.audioMs - (std::isfinite(tr.audioDeviceMs) ? tr.audioDeviceMs : 0.0));
        s.framesAfterPauseMax = std::max(s.framesAfterPauseMax, p.framesAfter);
        s.deviceMs = tr.audioDeviceMs;
        std::printf("PAUSELAT | %s | cycle %d api %s | pause apply %.1f video %.1f (frames after %d) audio %.1f (after %.1f ms) | "
                    "resume apply %.1f video %.1f audio %.1f | device %.1f ms\n",
                    config, i, api == 0 ? "pause/play" : api == 1 ? "toggle" : "toggle/play", p.applyMs, p.videoMs, p.framesAfter,
                    p.audioMs, tp.audioAfterMs, r.applyMs, r.videoMs, r.audioMs, tr.audioDeviceMs);
        std::fflush(stdout);
    }
    Delay(1.0);
    if (auto js = player.GetDebugInfoJson()) {
        DebugInfo d = ParseDebugInfo(*js);
        s.avDiffAfterResumeMs = d.Sync.AvDiffMs.value_or(NAN);
        s.audioDevice = d.Audio.OutDevice;
    }
    return s;
}

bool OpenPlaying(Host& host, std::unique_ptr<FfPlayer>& player, const Config& c, std::string& why) {
    int err = 0;
    player = FfPlayer::CreateSync(host.child, host.parent, kPauseNotifyMsg, &err);
    if (!player) { why = "CreateSync failed " + std::to_string(err); return false; }
    auto prepared = std::make_shared<int>(0);
    auto errors = std::make_shared<int>(0);
    player->Prepared.add([prepared](const MediaPreparedInfo&) { ++*prepared; });
    player->Error.add([errors](const PlayerErrorInfo&) { ++*errors; });
    if (player->Open(c.file.wstring(), c.options) != 0) { why = "open rejected"; return false; }
    if (!WaitFor([&] { return *prepared > 0 || *errors > 0; }, 20) || *errors) { why = "did not prepare"; return false; }
    if (!WaitFor([&] {
            auto js = player->GetDebugInfoJson();
            return js && ParseDebugInfo(*js).Fps.FramesDisplayed.value_or(0) > 10 && player->State() == PlayerState::Playing;
        }, 10)) { why = "no frames displayed"; return false; }
    Delay(1.0);
    return true;
}

std::vector<Config> Configs() {
    std::vector<Config> v;
    const fs::path tone = RepoRoot() / L"testmedia" / L"testsrc2-1080p-60s.mp4";   // 25 fps + continuous 440 Hz sine
    v.push_back({"sdl-software", tone, {{L"presenter", L"sdl"}, {L"stats", L"1"}}, 40.0});
    auto caps = FfPlayer::ProbeCaps();
    const PresenterCap* d3 = caps ? caps->Presenter("d3d11") : nullptr;
    if (d3 && d3->ok) v.push_back({"d3d11-software", tone, {{L"presenter", L"d3d11"}, {L"stats", L"1"}}, 40.0});
    // the user's pipeline: CUDA decode + ivtc_cuda + d3d11 with ARIB captions (broadcast fixture), when the machine has it
    const fs::path tbs = RepoRoot() / L"testmedia" / L"arib" / L"tbs-captions.ts";
    const DecoderCap* dc = caps ? caps->Decoder("h264", "cuda") : nullptr;
    if (fs::exists(tbs) && d3 && d3->ok && d3->importCuda && caps->DeviceOk("cuda") && dc && dc->ok && caps->Filter("ivtc_cuda"))
        v.push_back({"user-cuda-ivtc-d3d11-captions", tbs,
                     {{L"hwaccel", L"cuda"}, {L"hwframes", L"keep"}, {L"presenter", L"d3d11"}, {L"vf", L"ivtc_cuda"}, {L"stats", L"1"}},
                     1001.0 / 24.0});
    const DecoderCap* dv = caps ? caps->Decoder("mpeg2video", "d3d11va") : nullptr;
    if (fs::exists(tbs) && d3 && d3->ok && caps->DeviceOk("d3d11va") && dv && dv->ok && caps->Filter("ivtc_d3d11"))
        v.push_back({"user-d3d11va-ivtc-d3d11-captions", tbs,
                     {{L"hwaccel", L"d3d11va"}, {L"hwframes", L"keep"}, {L"presenter", L"d3d11"}, {L"vf", L"ivtc_d3d11"}, {L"stats", L"1"}},
                     1001.0 / 24.0});
    return v;
}

}

TEST_CASE_INTEGRATION(PauseLatency_api_pause_resume_stages) {
    if (!std::getenv("SDL_AUDIODRIVER")) _putenv_s("SDL_AUDIODRIVER", "dummy");
    for (const Config& c : Configs()) {
        REQUIRE_MSG(fs::exists(c.file), "missing fixture " + ToUtf8(c.file.wstring()));
        Host host;
        std::unique_ptr<FfPlayer> player;
        std::string why;
        REQUIRE_MSG(OpenPlaying(host, player, c, why), std::string(c.name) + ": " + why);
        REQUIRE_MSG(ReadTiming(*player).valid, "libffplay without pause_timing (patch 1025)");
        Summary s = MeasureCycles(*player, 6, c.name);
        const double pv = Median(s.pauseVideo), pa = Median(s.pauseAudio), rv = Median(s.resumeVideo), ra = Median(s.resumeAudio);
        std::printf("PAUSELAT | %s | MEDIAN pause apply %.1f video %.1f audio %.1f | resume apply %.1f video %.1f audio %.1f | "
                    "MAX pause video %.1f audio %.1f resume video %.1f audio %.1f | frames after pause max %d | device %.1f ms (%s) | "
                    "A-V after resume %.1f ms\n",
                    c.name, Median(s.pauseApply), pv, pa, Median(s.resumeApply), rv, ra, Max(s.pauseVideo), Max(s.pauseAudio),
                    Max(s.resumeVideo), Max(s.resumeAudio), s.framesAfterPauseMax, s.deviceMs, s.audioDevice.c_str(),
                    s.avDiffAfterResumeMs);
        std::fflush(stdout);
        // Target: about one frame interval. The command itself is applied at once; video stops with the next refresh at
        // the latest and resumes at the next frame's due time (at most one frame interval later); the audio hand-over
        // stops / starts with the next device callback plus the device's in-flight buffer.
        CHECK_MSG(Median(s.pauseApply) <= 15.0, std::string(c.name) + ": pause command applied late");
        CHECK_MSG(Median(s.resumeApply) <= 15.0, std::string(c.name) + ": resume command applied late");
        CHECK_MSG(std::isfinite(pv) && pv <= 25.0, std::string(c.name) + ": video kept presenting after pause");
        CHECK_MSG(s.framesAfterPauseMax <= 1, std::string(c.name) + ": more than one frame presented after a pause");
        CHECK_MSG(std::isfinite(rv) && rv <= c.frameMs + 25.0, std::string(c.name) + ": video resumed later than one frame interval");
        // Audio, relative to the device's callback period (half of audio_device_ms): a real device runs at its ~10 ms period,
        // SDL's sleep-paced dummy driver (the tests) keeps ~43 ms buffers. The pause takes effect in the next callback with a
        // 5 ms fade; the resume starts in the next callback.
        const double period = std::isfinite(s.deviceMs) ? s.deviceMs / 2.0 : 43.0;
        const double ph = Median(s.pauseHandoff), pf = Median(s.pauseAfter), rh = Median(s.resumeHandoff);
        std::printf("PAUSELAT | %s | AUDIO hand-over: pause last %.1f ms after the call (decoded audio after the apply %.1f ms), "
                    "resume first %.1f ms | callback period %.1f ms\n", c.name, ph, pf, rh, period);
        std::fflush(stdout);
        CHECK_MSG(std::isfinite(pf) && pf <= 8.0, std::string(c.name) + ": more than the 5 ms fade of audio after a pause");
        CHECK_MSG(std::isfinite(ph) && ph <= period + 10.0, std::string(c.name) + ": audio hand-over continued after the next callback");
        CHECK_MSG(std::isfinite(rh) && rh <= period + 10.0, std::string(c.name) + ": audio hand-over resumed after the next callback");
        // load sensitive single samples: bound loosely
        CHECK_MSG(Max(s.pauseHandoff) <= 2 * period + 40.0 && Max(s.resumeHandoff) <= 2 * period + 40.0,
                  std::string(c.name) + ": an audio transition exceeded two callback periods + 40 ms");
        CHECK_MSG(!std::isfinite(s.avDiffAfterResumeMs) || std::fabs(s.avDiffAfterResumeMs) <= 60.0,
                  std::string(c.name) + ": A-V drifted after the resumes");
        player->Stop();
        player.reset();
    }
}

namespace {

struct LoopbackBlock { double qpcUs; float mag; };

// Completion of ActivateAudioInterfaceAsync (must be agile: it is called on an MTA worker thread).
class ActivationHandler : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HRESULT hr = E_FAIL;
    IAudioClient* client = nullptr;
    ~ActivationHandler() { if (client) client->Release(); CloseHandle(done); }
    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IActivateAudioInterfaceCompletionHandler))
            *out = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        else if (iid == __uuidof(IAgileObject)) *out = static_cast<IAgileObject*>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override { ULONG r = --refs_; if (!r) delete this; return r; }
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        IUnknown* unk = nullptr;
        HRESULT result = E_FAIL;
        hr = op->GetActivateResult(&result, &unk);
        if (SUCCEEDED(hr)) hr = result;
        if (SUCCEEDED(hr) && unk) hr = unk->QueryInterface(__uuidof(IAudioClient), (void**)&client);
        if (unk) unk->Release();
        SetEvent(done);
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{1};
};

class Loopback {
public:
    bool Start(std::string& why) {
        thread_ = std::thread([this] { Run(); });
        for (int i = 0; i < 200 && !started_ && !failed_; ++i) Sleep(10);
        if (!started_) { why = error_.empty() ? "loopback did not start" : error_; return false; }
        return true;
    }
    ~Loopback() { stop_ = true; if (thread_.joinable()) thread_.join(); }
    std::vector<LoopbackBlock> Blocks() { std::lock_guard<std::mutex> g(mu_); return blocks_; }

private:
    void Run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IAudioClient* client = nullptr;
        IAudioCaptureClient* cap = nullptr;
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        auto fail = [&](const char* what, HRESULT hr) {
            char b[96]; std::snprintf(b, sizeof b, "%s failed 0x%08lX", what, (unsigned long)hr); error_ = b; failed_ = true;
        };
        // Process loopback (Windows 10 2004+): only the audio of this process tree, never another application's sound.
        AUDIOCLIENT_ACTIVATION_PARAMS ap{};
        ap.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        ap.ProcessLoopbackParams.TargetProcessId = GetCurrentProcessId();
        ap.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
        PROPVARIANT pv{};
        pv.vt = VT_BLOB;
        pv.blob.cbSize = sizeof ap;
        pv.blob.pBlobData = reinterpret_cast<BYTE*>(&ap);
        auto* handler = new ActivationHandler();
        IActivateAudioInterfaceAsyncOperation* op = nullptr;
        HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv, handler, &op);
        if (FAILED(hr)) fail("ActivateAudioInterfaceAsync(process loopback)", hr);
        else if (WaitForSingleObject(handler->done, 5000) != WAIT_OBJECT_0) fail("process loopback activation (timeout)", E_FAIL);
        else if (FAILED(handler->hr) || !handler->client) fail("process loopback activation", handler->hr);
        else { client = handler->client; client->AddRef(); }
        if (op) op->Release();
        handler->Release();
        WAVEFORMATEX fmt{};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = 2;
        fmt.nSamplesPerSec = 48000;
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = fmt.nChannels * fmt.wBitsPerSample / 8;
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
        WAVEFORMATEX* wf = &fmt;
        const bool isFloat = false;
        if (!failed_ && FAILED(hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                                     AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                                         AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                                     2000000, 0, wf, nullptr)))
            fail("Initialize(process loopback)", hr);
        if (!failed_ && FAILED(hr = client->SetEventHandle(ev))) fail("SetEventHandle", hr);
        if (!failed_ && FAILED(hr = client->GetService(__uuidof(IAudioCaptureClient), (void**)&cap))) fail("GetService", hr);
        if (!failed_ && FAILED(hr = client->Start())) fail("Start", hr);
        if (!failed_) {
            started_ = true;
            const int rate = (int)wf->nSamplesPerSec, ch = wf->nChannels;
            const int block = std::max(1, rate / 400);   // 2.5 ms
            const double w = 2.0 * 3.14159265358979 * 440.0 / rate, coeff = 2.0 * std::cos(w);
            std::vector<float> acc;
            double accStartUs = 0;
            while (!stop_) {
                UINT32 n = 0;
                if (FAILED(cap->GetNextPacketSize(&n))) break;
                if (!n) { WaitForSingleObject(ev, 5); continue; }
                BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0; UINT64 devPos = 0, qpcPos = 0;
                if (FAILED(cap->GetBuffer(&data, &frames, &flags, &devPos, &qpcPos))) break;
                // QPC position of the first frame in 100 ns units (0 when the stream gives none: estimate from now)
                const double startUs = qpcPos ? qpcPos / 10.0 : QpcUs() - frames * 1e6 / rate;
                if (acc.empty()) accStartUs = startUs;
                for (UINT32 i = 0; i < frames; ++i) {
                    float x = 0;
                    if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
                        x = isFloat ? reinterpret_cast<float*>(data)[i * ch] : reinterpret_cast<int16_t*>(data)[i * ch] / 32768.0f;
                    if (acc.empty()) accStartUs = startUs + i * 1e6 / rate;
                    acc.push_back(x);
                    if ((int)acc.size() == block) {
                        double s1 = 0, s2 = 0;
                        for (float v : acc) { double s0 = v + coeff * s1 - s2; s2 = s1; s1 = s0; }
                        const float mag = (float)(std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - coeff * s1 * s2)) / block);
                        { std::lock_guard<std::mutex> g(mu_); blocks_.push_back({accStartUs, mag}); }
                        acc.clear();
                    }
                }
                cap->ReleaseBuffer(frames);
            }
            client->Stop();
        }
        if (cap) cap->Release();
        if (client) client->Release();
        CloseHandle(ev);
        CoUninitialize();
    }

    std::thread thread_;
    std::atomic<bool> stop_{false}, started_{false}, failed_{false};
    std::string error_;
    std::mutex mu_;
    std::vector<LoopbackBlock> blocks_;
};

}

TEST_CASE_INTEGRATION(PauseLatency_loopback_wasapi) {
    if (!std::getenv("WVP_PAUSE_LOOPBACK")) {
        std::printf("LOOPBACK | SKIP: set WVP_PAUSE_LOOPBACK=1 and run this test alone (plays a quiet 440 Hz tone)\n");
        return;
    }
    // main() forces the dummy driver; SDL reads the variable when libffplay first initialises audio, so this test must run
    // alone in its process (filter PauseLatency_loopback).
    _putenv_s("SDL_AUDIODRIVER", "wasapi");
    Loopback lb;
    std::string why;
    REQUIRE_MSG(lb.Start(why), why);
    const fs::path tone = RepoRoot() / L"testmedia" / L"testsrc2-1080p-60s.mp4";
    std::vector<Config> configs = {
        {"sdl-wasapi", tone, {{L"presenter", L"sdl"}, {L"volume", L"3"}}, 40.0},
        {"d3d11-wasapi", tone, {{L"presenter", L"d3d11"}, {L"volume", L"3"}}, 40.0},
    };
    for (const Config& c : configs) {
        Host host;
        std::unique_ptr<FfPlayer> player;
        REQUIRE_MSG(OpenPlaying(host, player, c, why), std::string(c.name) + ": " + why);
        Delay(1.0);
        std::vector<double> stopMs, startMs, estStop, estStart;
        for (int i = 0; i < 5; ++i) {
            Delay(0.5);
            const double t0 = QpcUs();
            REQUIRE(player->Pause() == 0);
            Delay(0.7);
            Transition p = Evaluate(ReadTiming(*player), true);
            const double t1 = QpcUs();
            REQUIRE(player->Play() == 0);
            Delay(0.7);
            Transition r = Evaluate(ReadTiming(*player), false);
            auto blocks = lb.Blocks();
            std::vector<float> before;
            for (const auto& b : blocks) if (b.qpcUs > t0 - 400000 && b.qpcUs < t0 - 100000) before.push_back(b.mag);
            if (before.empty()) { std::printf("LOOPBACK | %s | cycle %d: no captured audio before the pause\n", c.name, i); continue; }
            std::sort(before.begin(), before.end());
            const float level = before[before.size() / 2], thr = level * 0.25f;
            double lastLoud = NAN, firstLoud = NAN;
            for (const auto& b : blocks) {
                if (b.qpcUs >= t0 - 100000 && b.qpcUs < t1 && b.mag > thr) lastLoud = b.qpcUs + 2500;   // end of the block
                if (b.qpcUs >= t1 && b.qpcUs < t1 + 600000 && b.mag > thr && !std::isfinite(firstLoud)) firstLoud = b.qpcUs;
            }
            const double stop = std::isfinite(lastLoud) ? std::max(0.0, (lastLoud - t0) / 1000.0) : NAN;
            const double start = std::isfinite(firstLoud) ? (firstLoud - t1) / 1000.0 : NAN;
            stopMs.push_back(stop); startMs.push_back(start); estStop.push_back(p.audioMs); estStart.push_back(r.audioMs);
            std::printf("LOOPBACK | %s | cycle %d | level %.5f | audible stop %.1f ms (estimate %.1f) | audible start %.1f ms (estimate %.1f) | "
                        "video pause %.1f resume %.1f\n", c.name, i, level, stop, p.audioMs, start, r.audioMs, p.videoMs, r.videoMs);
            std::fflush(stdout);
        }
        std::printf("LOOPBACK | %s | MEDIAN audible stop %.1f ms (estimate %.1f) start %.1f ms (estimate %.1f)\n", c.name, Median(stopMs),
                    Median(estStop), Median(startMs), Median(estStart));
        std::fflush(stdout);
        CHECK_MSG(std::isfinite(Median(stopMs)) && std::isfinite(Median(startMs)), std::string(c.name) + ": tone not detected in the loopback");
        // real device, measured: ~45 ms (Windows shared-mode engine + 2 periods)
        CHECK_MSG(Median(stopMs) <= 60.0, std::string(c.name) + ": audible audio continued more than 60 ms after the pause call");
        CHECK_MSG(Median(startMs) <= 60.0, std::string(c.name) + ": audio became audible more than 60 ms after the play call");
        player->Stop();
        player.reset();
    }
}
