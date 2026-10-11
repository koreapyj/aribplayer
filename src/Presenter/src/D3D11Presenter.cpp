// The D3D11 presenter as libffplay's FFPPresenter: the C core (presenter_core.h) behind the presenter table, device
// recovery in open(), the sink, the view subclass that follows resizes, and the backdrop / test readback API.
#include <windows.h>
#include <commctrl.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>

#include "wvp/win32/WindowsVideoPlatform.h"

#include "PresenterInternal.h"
#include "presenter_core.h"

extern "C" {
#include <libavutil/bprint.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
#include <libavutil/time.h>
}

namespace wvp::win32 {

namespace {

constexpr UINT_PTR kSubclassId = 0x77767044;   // "wvpD"
constexpr int kFramesHeld = 1;                // the shown picture, for redraws

class SharedLock {
public:
    explicit SharedLock(SRWLOCK& l) : l_(l) { AcquireSRWLockShared(&l_); }
    ~SharedLock() { ReleaseSRWLockShared(&l_); }
    SharedLock(const SharedLock&) = delete;
    SharedLock& operator=(const SharedLock&) = delete;
private:
    SRWLOCK& l_;
};

class ExclusiveLock {
public:
    explicit ExclusiveLock(SRWLOCK& l) : l_(l) { AcquireSRWLockExclusive(&l_); }
    ~ExclusiveLock() { ReleaseSRWLockExclusive(&l_); }
    ExclusiveLock(const ExclusiveLock&) = delete;
    ExclusiveLock& operator=(const ExclusiveLock&) = delete;
private:
    SRWLOCK& l_;
};

PrBackdropRegion ToCore(const BackdropRegion& r) {
    PrBackdropRegion c{};
    c.x = r.x; c.y = r.y; c.width = r.width; c.height = r.height;
    c.corner_radius = r.cornerRadius;
    c.tint_color = r.tintColor;
    c.tint_opacity = r.tintOpacity;
    c.luminosity_color = r.luminosityColor;
    c.luminosity_opacity = r.luminosityOpacity;
    c.noise = r.noise ? 1 : 0;
    c.z = r.z;
    return c;
}

}

struct D3D11Presenter::Impl {
    HWND view = nullptr;
    PrConfig cfg{};
    FFPPresenter table{};
    bool subclassed = false;

    // The core is replaced only in open() (device recovery), under the exclusive lock; the optional presenter
    // functions, the UI-thread API and the view subclass read it under the shared lock.
    SRWLOCK coreLock = SRWLOCK_INIT;
    PrD3D11* core = nullptr;
    int failMode = PR_FAIL_NONE;           // handed to the next core (test injection, once)
    bool failInit = false;
    std::atomic<bool> failed{false};       // the current open failed: the next open recreates the core

    SRWLOCK sinkLock = SRWLOCK_INIT;       // sink valid between open and close
    FFPPresenterSink sink{};
    bool sinkActive = false;
    std::atomic<int> shownSerial{0};
    std::atomic<double> shownPts{NAN};

    std::mutex extrasMu;                   // re-applied to a recreated core
    std::vector<PrBackdropRegion> regions;
    bool composedReadback = false;

    std::vector<FFPPixFormat> formats;     // FFPPresenterCaps of the last open
    std::vector<AVBufferRef*> devices;

    ~Impl() {
        if (subclassed) RemoveWindowSubclass(view, &Impl::ViewProc, kSubclassId);
        pr_d3d11_destroy(core);
    }

    PrD3D11* NewCore(char* err, size_t size) {
        PrD3D11* c = pr_d3d11_create(view, &cfg, failMode, &Impl::OnRedraw, this, err, size);
        if (!c) return nullptr;
        failMode = PR_FAIL_NONE;
        std::lock_guard<std::mutex> g(extrasMu);
        if (!regions.empty()) pr_d3d11_set_backdrop_regions(c, regions.data(), static_cast<int>(regions.size()), err, size);
        if (composedReadback) pr_d3d11_readback_test_enable(c, 1);
        return c;
    }

    void Fail(const char* err, int lost, unsigned reason) {
        failed = true;
        char removed[16];
        std::snprintf(removed, sizeof removed, "0x%08x", reason);
        if (!TryAcquireSRWLockShared(&sinkLock)) return;   // close() is running: nobody to tell
        if (sinkActive) sink.failed(sink.engine, err && *err ? err : "d3d11 presenter failed", lost, lost ? removed : nullptr);
        ReleaseSRWLockShared(&sinkLock);
    }

    // Window thread of the core: the held picture was redrawn on its own (view resized, backdrop changed).
    static void OnRedraw(void* opaque, int ret, int videoWidth, int videoHeight, const char* err, int lost, unsigned reason) {
        auto* self = static_cast<Impl*>(opaque);
        if (self->failed) return;
        if (ret < 0) {
            self->Fail(err, lost, reason);
            return;
        }
        if (!TryAcquireSRWLockShared(&self->sinkLock)) return;
        if (self->sinkActive) {
            FFPPresentedInfo info{};
            info.serial = self->shownSerial.load();
            info.pts = self->shownPts.load();
            info.shown_us = av_gettime_relative();
            info.copies = 0;
            info.video_width = videoWidth;
            info.video_height = videoHeight;
            self->sink.presented(self->sink.engine, &info);
        }
        ReleaseSRWLockShared(&self->sinkLock);
    }

    static LRESULT CALLBACK ViewProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
        auto* self = reinterpret_cast<Impl*>(ref);
        switch (msg) {
        case WM_SIZE:
            self->HostResized();
            break;
        case WM_WINDOWPOSCHANGED:
            if (!(reinterpret_cast<const WINDOWPOS*>(lp)->flags & SWP_NOSIZE)) self->HostResized();
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(h, &Impl::ViewProc, kSubclassId);
            self->subclassed = false;
            break;
        }
        return DefSubclassProc(h, msg, wp, lp);
    }

    void HostResized() {
        // never waits for open()'s device recovery on the UI thread: the next present follows the size anyway
        if (!TryAcquireSRWLockShared(&coreLock)) return;
        if (core) pr_d3d11_host_resized(core);
        ReleaseSRWLockShared(&coreLock);
    }

    int Open(const FFPPresenterSink* s, FFPPresenterCaps* caps) {
        char err[256] = "";
        if (failInit) {
            failInit = false;
            av_log(nullptr, AV_LOG_ERROR, "d3d11 presenter: forced failure (presenter test option)\n");
            return AVERROR_EXTERNAL;
        }
        unsigned reason = 0;
        if (core && (failed || pr_d3d11_device_lost(core, &reason))) {
            ExclusiveLock g(coreLock);
            pr_d3d11_destroy(core);
            core = nullptr;
        }
        failed = false;
        if (!core) {
            PrD3D11* c = NewCore(err, sizeof err);
            if (!c) {
                av_log(nullptr, AV_LOG_ERROR, "d3d11 presenter: %s\n", err);
                return AVERROR_EXTERNAL;
            }
            ExclusiveLock g(coreLock);
            core = c;
        }
        const bool p010 = pr_d3d11_p010_ok(core) != 0;

        formats.clear();
        formats.push_back({AV_PIX_FMT_D3D11, AV_PIX_FMT_NV12});
        if (p010) formats.push_back({AV_PIX_FMT_D3D11, AV_PIX_FMT_P010});
        formats.push_back({AV_PIX_FMT_NV12, AV_PIX_FMT_NONE});
        if (p010) formats.push_back({AV_PIX_FMT_P010LE, AV_PIX_FMT_NONE});
        formats.push_back({AV_PIX_FMT_BGRA, AV_PIX_FMT_NONE});
        devices.clear();
        devices.push_back(pr_d3d11_device(core));
        caps->formats = formats.data();
        caps->nb_formats = static_cast<int>(formats.size());
        caps->devices = devices.data();
        caps->nb_devices = static_cast<int>(devices.size());
        caps->frames_held = kFramesHeld;

        ExclusiveLock g(sinkLock);
        sink = *s;
        sinkActive = true;
        return 0;
    }

    int Present(const FFPPicture* pic) {
        if (!core) return AVERROR(EINVAL);
        char err[256] = "";
        int copies = 0, w = 0, h = 0, lost = 0;
        unsigned reason = 0;
        const int r = pr_d3d11_present(core, pic->frame, pic->overlays, &copies, &w, &h, err, sizeof err, &lost, &reason);
        if (r < 0) {
            Fail(err, lost, reason);
            return AVERROR_EXTERNAL;
        }
        shownSerial = pic->serial;
        shownPts = pic->pts;
        FFPPresentedInfo info{};
        info.serial = pic->serial;
        info.pts = pic->pts;
        info.shown_us = av_gettime_relative();
        info.copies = copies;
        info.video_width = w;
        info.video_height = h;
        SharedLock g(sinkLock);
        if (sinkActive) sink.presented(sink.engine, &info);
        return 0;
    }

    void Clear() {
        if (core) pr_d3d11_clear(core);
    }

    void Close() {
        if (core) pr_d3d11_release_held(core);
        ExclusiveLock g(sinkLock);
        sinkActive = false;
        sink = {};
    }

    int Accepts(const AVFrame* frame) {
        SharedLock g(coreLock);
        return core && pr_d3d11_accepts(core, frame);
    }

    int SetupFrames(AVBufferRef* frames) {
        auto* fc = reinterpret_cast<AVHWFramesContext*>(frames->data);
        if (fc->initial_pool_size > 0) fc->initial_pool_size += kFramesHeld;
        return 0;
    }

    int Readback(const AVFrame* frame, uint8_t* bgra, int stride, int width, int height) {
        char err[256] = "";
        SharedLock g(coreLock);
        if (!core) return AVERROR(EINVAL);
        if (pr_d3d11_readback(core, frame, bgra, stride, width, height, err, sizeof err) < 0) {
            av_log(nullptr, AV_LOG_WARNING, "d3d11 presenter readback failed: %s\n", err);
            return AVERROR_EXTERNAL;
        }
        return 0;
    }

    int StatsJson(char* buf, size_t size) {
        AVBPrint bp;
        av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
        {
            SharedLock g(coreLock);
            if (core) pr_d3d11_stats_json(core, &bp);
            else av_bprintf(&bp, "{}");
        }
        const int n = av_bprint_is_complete(&bp) ? static_cast<int>(bp.len) : 0;
        if (buf && size) std::snprintf(buf, size, "%s", n ? bp.str : "");
        av_bprint_finalize(&bp, nullptr);
        return n;
    }

    int DeviceLost(char* reason, size_t size) {
        SharedLock g(coreLock);
        unsigned hr = 0;
        if (!core || !pr_d3d11_device_lost(core, &hr)) return 0;
        if (reason && size) std::snprintf(reason, size, "0x%08x", hr);
        return 1;
    }

    static Impl* Self(void* opaque) { return static_cast<Impl*>(opaque); }
    static int FFP_CALL COpen(void* o, const FFPStreamInfo*, const FFPPresenterSink* s, FFPPresenterCaps* c) {
        return Self(o)->Open(s, c);
    }
    static int FFP_CALL CPresent(void* o, const FFPPicture* p) { return Self(o)->Present(p); }
    static void FFP_CALL CClear(void* o) { Self(o)->Clear(); }
    static void FFP_CALL CClose(void* o) { Self(o)->Close(); }
    static int FFP_CALL CAccepts(void* o, const AVFrame* f) { return Self(o)->Accepts(f); }
    static int FFP_CALL CSetupFrames(void* o, AVBufferRef* f) { return Self(o)->SetupFrames(f); }
    static int FFP_CALL CReadback(void* o, const AVFrame* f, uint8_t* b, int s, int w, int h) {
        return Self(o)->Readback(f, b, s, w, h);
    }
    static int FFP_CALL CStatsJson(void* o, char* b, size_t n) { return Self(o)->StatsJson(b, n); }
    static int FFP_CALL CDeviceLost(void* o, char* r, size_t n) { return Self(o)->DeviceLost(r, n); }
};

D3D11Presenter::D3D11Presenter(std::unique_ptr<Impl> impl, std::string adapterId)
    : impl_(std::move(impl)), adapterId_(std::move(adapterId)) {
    FFPPresenter& t = impl_->table;
    t.name = "d3d11";
    t.opaque = impl_.get();
    t.open = &Impl::COpen;
    t.present = &Impl::CPresent;
    t.clear = &Impl::CClear;
    t.close = &Impl::CClose;
    t.accepts = &Impl::CAccepts;
    t.setup_frames = &Impl::CSetupFrames;
    t.readback = &Impl::CReadback;
    t.stats_json = &Impl::CStatsJson;
    t.device_lost = &Impl::CDeviceLost;
    impl_->subclassed = SetWindowSubclass(impl_->view, &Impl::ViewProc, kSubclassId,
                                          reinterpret_cast<DWORD_PTR>(impl_.get())) != FALSE;
}

D3D11Presenter::~D3D11Presenter() = default;

const FFPPresenter* D3D11Presenter::Native() const { return &impl_->table; }

bool D3D11Presenter::SetBackdropRegions(const std::vector<BackdropRegion>& regions, std::string* error) {
    if (regions.size() > static_cast<size_t>(kMaxBackdropRegions)) {
        if (error) *error = "too many backdrop regions";
        return false;
    }
    std::vector<PrBackdropRegion> core;
    for (const BackdropRegion& r : regions) core.push_back(ToCore(r));
    char err[256] = "";
    {
        SharedLock g(impl_->coreLock);
        // Validated by the core; without a core yet the regions are checked when it is made.
        if (impl_->core &&
            pr_d3d11_set_backdrop_regions(impl_->core, core.data(), static_cast<int>(core.size()), err, sizeof err) < 0) {
            if (error) *error = err;
            return false;
        }
    }
    std::lock_guard<std::mutex> g(impl_->extrasMu);
    impl_->regions = std::move(core);
    return true;
}

void D3D11Presenter::EnableComposedReadback(bool on) {
    {
        std::lock_guard<std::mutex> g(impl_->extrasMu);
        impl_->composedReadback = on;
    }
    SharedLock g(impl_->coreLock);
    if (impl_->core) pr_d3d11_readback_test_enable(impl_->core, on ? 1 : 0);
}

std::optional<ComposedFrame> D3D11Presenter::ComposedReadback() {
    SharedLock g(impl_->coreLock);
    if (!impl_->core) return std::nullopt;
    ComposedFrame f;
    uint8_t* bytes = nullptr;
    char err[256] = "";
    if (pr_d3d11_readback_last(impl_->core, &bytes, &f.width, &f.height, &f.stride, &f.videoX, &f.videoY, &f.videoWidth,
                               &f.videoHeight, err, sizeof err) < 0 || !bytes)
        return std::nullopt;
    f.bgra.assign(bytes, bytes + static_cast<size_t>(f.stride) * static_cast<size_t>(f.height));
    av_free(bytes);
    return f;
}

PresenterHandle D3D11Presenter::Create(void* view, const std::string& adapterId, const PresenterOptions& options,
                                       std::string* error) {
    auto impl = std::make_unique<Impl>();
    impl->view = static_cast<HWND>(view);
    if (!MakeConfig(adapterId, options, impl->cfg, error)) return nullptr;
    if (options.fail == "init") impl->failInit = true;
    else if (options.fail == "display") impl->failMode = PR_FAIL_DISPLAY;
    else if (options.fail == "device_lost") impl->failMode = PR_FAIL_DEVICE_LOST;
    else if (!options.fail.empty()) {
        if (error) *error = "unknown presenter failure injection '" + options.fail + "'";
        return nullptr;
    }
    char err[256] = "";
    impl->core = impl->NewCore(err, sizeof err);
    if (!impl->core) {
        if (error) *error = err;
        return nullptr;
    }
    return PresenterHandle(new D3D11Presenter(std::move(impl), adapterId));
}

}
