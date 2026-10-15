/*
 * libffpmetalpresenter: libffplay's FFPPresenter (ffplay.h) on a CAMetalLayer.
 *
 * This file is derived from FFmpeg code and is licensed under the LGPL 2.1 or later.
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreGraphics/CoreGraphics.h>
#import <IOSurface/IOSurfaceRef.h>

#include <math.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>

#include <libavutil/avstring.h>
#include <libavutil/buffer.h>
#include <libavutil/common.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/log.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libavutil/time.h>

#include "ffplay.h"
#include "ffp_metal_presenter.h"

#if !__has_feature(objc_arc)
#error "build with -fobjc-arc"
#endif

extern const unsigned char ffp_metal_shaders_metallib[];
extern const unsigned int ffp_metal_shaders_metallib_len;

/* the layer's pixel formats: SDR bgra8, HDR (PQ / HLG) bgr10a2 */
enum { FMT_BGRA8, FMT_BGR10A2, FMT_NB };
/* what the layer is configured for */
enum { MODE_UNSET = -1, MODE_SDR = 0, MODE_PQ = 1, MODE_HLG = 2, MODE_SDR2020 = 3 };

/* frames_held = INFLIGHT_MAX command buffers that still read their picture + the newest picture kept for redraws
 * (a different one after a timeout skip). Display command buffers complete about a millisecond after commit (the GPU
 * does not wait for the display); the drawable gate, not INFLIGHT_MAX, is what normally limits present(). */
#define INFLIGHT_MAX 2
#define FRAMES_HELD (INFLIGHT_MAX + 1)
#define SLOT_COUNT (FRAMES_HELD + 1)        /* one spare: a slot is freed just after its successor is taken */

#define OVERLAY_MAX_RECTS 1024
#define OVERLAY_MAX_BYTES (64u * 1024u * 1024u)
#define MAX_DIM 16384
#define DRAIN_TIMEOUT_US (2 * 1000 * 1000)

/* the constant buffer of video_fs */
typedef struct VideoCB {
    unsigned input[4];      /* luma storage w,h */
    unsigned crop[4];       /* x,y,w,h */
    float color[4];         /* Kr, Kb, full-range, - */
    float dest[4];          /* x,y,width,height */
} VideoCB;

typedef struct OverlayCB {
    float dest[4];
    float target[4];        /* width, height, hdr mode (0 sdr, 1 pq, 2 hlg), - */
} OverlayCB;

typedef struct PRect { int left, top, right, bottom; } PRect;

/* the two planes of a picture bound for the GPU (plain C: CF references only) */
typedef struct Input {
    CVPixelBufferRef pb;            /* retained: the frame's own (direct) or the upload */
    CVMetalTextureRef ty, tuv;      /* retained */
    int p010, direct;
    int storage_w, storage_h;       /* plane 0 */
    int vis_x, vis_y, vis_w, vis_h;
} Input;

/* a held picture: one av_frame_ref + its bound planes */
typedef struct Slot {
    int used;
    AVFrame *frame;
    Input in;
    int copies;
    int serial;
    double pts;
    int inflight;                   /* display command buffers reading it (slot_lock) */
    int latest;                     /* kept for redraws (slot_lock) */
} Slot;

typedef struct OverlayRectGeom { int x, y, width, height; } OverlayRectGeom;

/* One committed drawable until it is on screen (its presented handler) or its command buffer failed: the gate in
 * front of nextDrawable counts these, because nextDrawable blocks (up to a second, not configurable) while every
 * drawable of the layer is on screen or queued behind it. */
@interface FFPDrawableTicket : NSObject
@end
@implementation FFPDrawableTicket {
@public
    atomic_int retired;
    unsigned gen;
}
@end

@interface FFPMetalPresenterObj : NSObject
@end

@interface FFPMetalPresenterObj () {
@public
    pthread_mutex_t lock;           /* open / present / clear / close / view-change redraw */
    pthread_mutex_t slot_lock;      /* slots' inflight / latest / used (completion handlers); never held while waiting */
    pthread_cond_t slot_cond;
    pthread_mutex_t rb_lock;        /* readback() (its own cache / pool) */
    pthread_mutex_t trb_lock;       /* the test readback buffer (completion handlers) */

    CAMetalLayer *layer;
    id<MTLDevice> dev;
    id<MTLCommandQueue> queue;
    id<MTLLibrary> lib;
    id<MTLRenderPipelineState> video_pso[FMT_NB];
    id<MTLRenderPipelineState> overlay_pso[FMT_NB];
    id<MTLSamplerState> sampler;
    id<NSObject> dev_observer;
    CVMetalTextureCacheRef cache;   /* display path */
    CVPixelBufferPoolRef upload_pool;
    OSType upload_fmt;
    int upload_w, upload_h;
    CVMetalTextureCacheRef rb_cache; /* readback() path */
    CVPixelBufferPoolRef rb_pool;
    OSType rb_pool_fmt;
    int rb_pool_w, rb_pool_h;

    AVBufferRef *vt_device;
    FFPPresenter table;
    FFPPixFormat formats[3];
    AVBufferRef *devices[1];
    char device_name[128];

    int64_t refresh_us;
    pthread_mutex_t gate_lock;      /* inflight_tokens (completed handlers), pending_drawables (presented handlers) */
    pthread_cond_t gate_cond;
    int inflight_tokens;            /* display command buffers not yet completed (<= INFLIGHT_MAX) */
    int pending_drawables;          /* committed, not yet on screen */
    dispatch_queue_t wake_queue;    /* the strict deadline timer: kernel timed waits are coalesced by ~50 % */
    dispatch_source_t wake_timer;
    unsigned gate_gen;              /* bumped by a staleness reset: older tickets no longer count */
    int64_t last_retire_us;
    dispatch_queue_t view_change_queue;
    atomic_int view_change_retry_pending;   /* a skipped view-change redraw is scheduled again */
    atomic_int skip_redraw_pending;         /* a redraw of a picture present() skipped is scheduled */

    /* per open */
    int is_open;
    int latest_unshown;             /* the newest picture was skipped on a timeout and is not on screen yet (lock) */
    FFPPresenterSink sink;
    int opens;
    Slot slots[SLOT_COUNT];
    int last_dw, last_dh;           /* the drawable the last picture was rendered into */
    double last_scale;
    int video_rect[4];

    /* layer state */
    int mode;
    int framebuffer_only;           /* -1 unknown */

    /* overlay caches, per layer */
    uint64_t ov_generation[FFP_OVERLAY_LAYER_COUNT];
    int ov_valid[FFP_OVERLAY_LAYER_COUNT], ov_visible[FFP_OVERLAY_LAYER_COUNT];
    int ov_canvas_w[FFP_OVERLAY_LAYER_COUNT], ov_canvas_h[FFP_OVERLAY_LAYER_COUNT];
    int ov_count[FFP_OVERLAY_LAYER_COUNT];
    OverlayRectGeom *ov_rects[FFP_OVERLAY_LAYER_COUNT];
    NSMutableArray<id<MTLTexture>> *ov_textures[FFP_OVERLAY_LAYER_COUNT];
    int overlay_frame_rects;

    /* test readback */
    atomic_int trb_enabled;
    int trb_valid, trb_w, trb_h, trb_stride;
    size_t trb_capacity;
    uint8_t *trb_data;
    uint64_t trb_seq;

    /* debug */
    atomic_int debug_frames;        /* frames still to log */
    int debug_frame_no;
    atomic_int sim_gpu_latency_us;

    /* statistics (lifetime) */
    atomic_int inflight_cbs;        /* display command buffers whose completed handler has not finished */
    atomic_uint_fast64_t frames_presented, frames_direct, frames_uploaded, copies_total;
    atomic_uint_fast64_t skipped_timeout, skipped_gpu, skipped_display, gate_resets;
    atomic_uint_fast64_t dropped_no_drawable, dropped_hidden, redraws, view_change_redraws, view_change_retries;
    atomic_uint_fast64_t skipped_redraws;
    atomic_uint_fast64_t render_encoders, blit_encoders;
    atomic_uint_fast64_t zero_copy_verified, zero_copy_mismatch;
    atomic_uint_fast64_t frames_completed, gpu_us_total, present_wait_us_max, present_wait_us_total;
    atomic_uint_fast64_t wait_sem_us_max, wait_gate_us_max, next_drawable_us_max;
    atomic_uint_fast64_t presented_count, present_latency_us_total, present_latency_count;
    atomic_uint_fast64_t ov_uploads, ov_rects_uploaded, ov_rects_drawn, ov_cache_hits, ov_draw_failures;
    atomic_int held_now, held_max;
    atomic_int device_lost;
    char lost_reason[96];
}
@end

/* ------------------------------------------------------------------------------------------------------- */
/* helpers                                                                                                   */

static void set_error(char **error, const char *what, NSError *e)
{
    char buf[512];
    if (!error)
        return;
    if (e)
        snprintf(buf, sizeof(buf), "%s: %s", what, [[e localizedDescription] UTF8String]);
    else
        snprintf(buf, sizeof(buf), "%s", what);
    *error = strdup(buf);
}

static MTLPixelFormat mtl_pixfmt(int fmt)
{
    return fmt == FMT_BGR10A2 ? MTLPixelFormatBGR10A2Unorm : MTLPixelFormatBGRA8Unorm;
}

static const char *mtl_pixfmt_name(int fmt)
{
    return fmt == FMT_BGR10A2 ? "bgr10a2Unorm" : "bgra8Unorm";
}

static int mode_fmt(int mode)
{
    return mode == MODE_PQ || mode == MODE_HLG ? FMT_BGR10A2 : FMT_BGRA8;
}

static const char *mode_name(int mode)
{
    switch (mode) {
    case MODE_PQ:      return "bt2100-pq";
    case MODE_HLG:     return "bt2100-hlg";
    case MODE_SDR2020: return "bt2020";
    case MODE_SDR:     return "srgb";
    }
    return "unset";
}

static int frame_mode(const AVFrame *f)
{
    if (f->color_trc == AVCOL_TRC_SMPTE2084)
        return MODE_PQ;
    if (f->color_trc == AVCOL_TRC_ARIB_STD_B67)
        return MODE_HLG;
    if (f->color_primaries == AVCOL_PRI_BT2020)
        return MODE_SDR2020;
    return MODE_SDR;
}

static int cv_format(OSType f, int *p010)
{
    switch (f) {
    case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
    case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
        *p010 = 0;
        return 1;
    case kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange:  /* 'x420': P010 layout */
    case kCVPixelFormatType_420YpCbCr10BiPlanarFullRange:
        *p010 = 1;
        return 1;
    }
    return 0;
}

/* a VideoToolbox NV12 / P010 CVPixelBuffer backed by an IOSurface: bindable without a copy */
static int vt_frame_ok(const AVFrame *f)
{
    CVPixelBufferRef pb;
    int p010;

    if (!f || f->format != AV_PIX_FMT_VIDEOTOOLBOX || !f->data[3])
        return 0;
    pb = (CVPixelBufferRef)f->data[3];
    return cv_format(CVPixelBufferGetPixelFormatType(pb), &p010) &&
           CVPixelBufferGetPlaneCount(pb) == 2 && CVPixelBufferGetIOSurface(pb) != NULL;
}

static void input_release(Input *in)
{
    if (in->ty)
        CFRelease(in->ty);
    if (in->tuv)
        CFRelease(in->tuv);
    if (in->pb)
        CVPixelBufferRelease(in->pb);
    memset(in, 0, sizeof(*in));
}

static int64_t display_refresh_us(void)
{
    double hz = 0;
    CGDirectDisplayID did = CGMainDisplayID();
    CGDisplayModeRef m = CGDisplayCopyDisplayMode(did);
    if (m) {
        hz = CGDisplayModeGetRefreshRate(m);
        CGDisplayModeRelease(m);
    }
    if (!(hz > 1.0 && hz < 1000.0))
        hz = 60.0;
    return (int64_t)llround(1e6 / hz);
}

/* ------------------------------------------------------------------------------------------------------- */
/* geometry (as ffplay.c calculate_display_rect)                                                             */

static PRect letterbox(int bw, int bh, int pw, int ph, AVRational sar)
{
    AVRational ar = sar;
    int64_t w, h, x, y;
    PRect r;

    if (av_cmp_q(ar, av_make_q(0, 1)) <= 0)
        ar = av_make_q(1, 1);
    ar = av_mul_q(ar, av_make_q(pw, ph));
    h = bh;
    w = av_rescale(h, ar.num, ar.den) & ~1;
    if (w > bw) {
        w = bw;
        h = av_rescale(w, ar.den, ar.num) & ~1;
    }
    w = FFMAX(w, 1);
    h = FFMAX(h, 1);
    x = (bw - w) / 2;
    y = (bh - h) / 2;
    r.left = (int)x;
    r.top = (int)y;
    r.right = (int)(x + w);
    r.bottom = (int)(y + h);
    return r;
}

/* ------------------------------------------------------------------------------------------------------- */
/* the video shader parameters: pictures are already upright and filtered by the engine                       */

static int video_cb(const Input *in, const AVFrame *frame, const PRect *dst, VideoCB *cb, char *err, size_t n)
{
    double kr, kb;

    /* as the D3D11 shader: constant-luminance BT.2020 needs a nonlinear conversion not provided here */
    if (frame->colorspace == AVCOL_SPC_BT2020_CL) {
        snprintf(err, n, "BT.2020-CL conversion not implemented");
        return AVERROR(ENOSYS);
    }
    memset(cb, 0, sizeof(*cb));
    cb->input[0] = in->storage_w; cb->input[1] = in->storage_h;
    cb->crop[0] = in->vis_x; cb->crop[1] = in->vis_y; cb->crop[2] = in->vis_w; cb->crop[3] = in->vis_h;
    switch (frame->colorspace) {
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: kr = 0.299; kb = 0.114; break;
    case AVCOL_SPC_BT2020_NCL: kr = 0.2627; kb = 0.0593; break;
    case AVCOL_SPC_BT709: kr = 0.2126; kb = 0.0722; break;
    default:
        if (frame->height > 576) { kr = 0.2126; kb = 0.0722; }
        else { kr = 0.299; kb = 0.114; }
        break;
    }
    cb->color[0] = (float)kr; cb->color[1] = (float)kb;
    cb->color[2] = frame->color_range == AVCOL_RANGE_JPEG ? 1.0f : 0.0f;
    cb->dest[0] = (float)dst->left; cb->dest[1] = (float)dst->top;
    cb->dest[2] = (float)(dst->right - dst->left); cb->dest[3] = (float)(dst->bottom - dst->top);
    return 0;
}

/* ------------------------------------------------------------------------------------------------------- */
/* the object                                                                                                */

@implementation FFPMetalPresenterObj

- (id<MTLRenderPipelineState>)makePSO:(NSString *)vs fs:(NSString *)fs fmt:(MTLPixelFormat)fmt blend:(int)blend
                                error:(char **)error
{
    MTLRenderPipelineDescriptor *d = [[MTLRenderPipelineDescriptor alloc] init];
    id<MTLFunction> v = [lib newFunctionWithName:vs];
    id<MTLFunction> f = [lib newFunctionWithName:fs];
    id<MTLRenderPipelineState> pso = nil;
    NSError *e = nil;

    if (!v || !f) {
        set_error(error, "Metal shader function missing", nil);
        return nil;
    }
    d.vertexFunction = v;
    d.fragmentFunction = f;
    d.colorAttachments[0].pixelFormat = fmt;
    if (blend) {
        /* overlay textures are premultiplied on upload (ONE / ONE_MINUS_SRC_ALPHA) */
        d.colorAttachments[0].blendingEnabled = YES;
        d.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
        d.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
        d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
        d.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
        d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        d.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    }
    pso = [dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!pso)
        set_error(error, "newRenderPipelineState", e);
    return pso;
}

- (int)setupWithLayer:(CAMetalLayer *)l device:(AVBufferRef *)vt error:(char **)error
{
    CVReturn cr;
    NSError *e = nil;
    dispatch_data_t data;
    int i;

    pthread_mutex_init(&lock, NULL);
    pthread_mutex_init(&slot_lock, NULL);
    pthread_cond_init(&slot_cond, NULL);
    pthread_mutex_init(&rb_lock, NULL);
    pthread_mutex_init(&trb_lock, NULL);
    pthread_mutex_init(&gate_lock, NULL);
    pthread_cond_init(&gate_cond, NULL);
    mode = MODE_UNSET;
    framebuffer_only = -1;
    layer = l;
    vt_device = av_buffer_ref(vt);
    if (!vt_device) {
        set_error(error, "out of memory", nil);
        return -1;
    }
    /* the layer's device when the host set one, else the system default device */
    dev = layer.device ? layer.device : MTLCreateSystemDefaultDevice();
    if (!dev) {
        set_error(error, "no Metal device", nil);
        return -1;
    }
    av_strlcpy(device_name, [[dev name] UTF8String], sizeof(device_name));
    queue = [dev newCommandQueue];
    if (!queue) {
        set_error(error, "newCommandQueue failed", nil);
        return -1;
    }
    queue.label = @"ffp_metal_presenter";
    data = dispatch_data_create(ffp_metal_shaders_metallib, ffp_metal_shaders_metallib_len, nil,
                                DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    lib = [dev newLibraryWithData:data error:&e];
    if (!lib) {
        set_error(error, "newLibraryWithData", e);
        return -1;
    }
    {
        MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        sampler = [dev newSamplerStateWithDescriptor:sd];
    }
    cr = CVMetalTextureCacheCreate(kCFAllocatorDefault, NULL, dev, NULL, &cache);
    if (cr != kCVReturnSuccess || !cache) {
        set_error(error, "CVMetalTextureCacheCreate failed", nil);
        return -1;
    }
    cr = CVMetalTextureCacheCreate(kCFAllocatorDefault, NULL, dev, NULL, &rb_cache);
    if (cr != kCVReturnSuccess || !rb_cache) {
        set_error(error, "CVMetalTextureCacheCreate (readback) failed", nil);
        return -1;
    }
    /* every pipeline up front: readback() may run concurrently with present() and must not create state lazily */
    for (i = 0; i < FMT_NB; i++) {
        video_pso[i] = [self makePSO:@"video_vs" fs:@"video_fs" fmt:mtl_pixfmt(i) blend:0 error:error];
        if (!video_pso[i])
            return -1;
        overlay_pso[i] = [self makePSO:@"overlay_vs" fs:@"overlay_fs" fmt:mtl_pixfmt(i) blend:1 error:error];
        if (!overlay_pso[i])
            return -1;
    }
    for (i = 0; i < FFP_OVERLAY_LAYER_COUNT; i++)
        ov_textures[i] = [NSMutableArray array];
    if (!layer.device) {
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        layer.device = dev;
        [CATransaction commit];
    }
    refresh_us = display_refresh_us();
    wake_queue = dispatch_queue_create("ffp_metal_presenter.wake", DISPATCH_QUEUE_SERIAL);
    wake_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, DISPATCH_TIMER_STRICT, wake_queue);
    {
        __weak FFPMetalPresenterObj *weak = self;
        dispatch_source_set_timer(wake_timer, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, 0);
        dispatch_source_set_event_handler(wake_timer, ^{
            FFPMetalPresenterObj *s = weak;
            if (!s)
                return;
            pthread_mutex_lock(&s->gate_lock);
            pthread_cond_broadcast(&s->gate_cond);
            pthread_mutex_unlock(&s->gate_lock);
        });
        dispatch_resume(wake_timer);
    }

    /* device removal (external GPUs): device_lost() reports it */
    {
        __weak FFPMetalPresenterObj *weak = self;
        id<NSObject> obs = nil;
        NSArray<id<MTLDevice>> *all = MTLCopyAllDevicesWithObserver(&obs, ^(id<MTLDevice> d, MTLDeviceNotificationName name) {
            FFPMetalPresenterObj *s = weak;
            if (!s || d != s->dev)
                return;
            if ([name isEqualToString:MTLDeviceWasRemovedNotification] ||
                [name isEqualToString:MTLDeviceRemovalRequestedNotification]) {
                snprintf(s->lost_reason, sizeof(s->lost_reason), "%s", [name UTF8String]);
                atomic_store(&s->device_lost, 1);
            }
        });
        (void)all;
        dev_observer = obs;
    }

    /* host-triggered redraws follow drawableSize / contentsScale changes (see the header) */
    view_change_queue = dispatch_queue_create("ffp_metal_presenter.view_change", DISPATCH_QUEUE_SERIAL);

    {
        const char *s = getenv("FFP_METAL_DEBUG");
        if (s && *s && strcmp(s, "0"))
            atomic_store(&debug_frames, atoi(s) > 0 ? atoi(s) : 5);
    }
    av_log(NULL, AV_LOG_VERBOSE, "metal presenter: device %s, layer %p, refresh %.2f ms, frames_held %d\n",
           device_name, (__bridge void *)layer, refresh_us / 1000.0, FRAMES_HELD);
    return 0;
}

/* ---- slots (held frames) ---------------------------------------------------------------------------------- */

static void slot_free_locked(FFPMetalPresenterObj *s, Slot *sl)
{
    if (!sl->used)
        return;
    input_release(&sl->in);
    av_frame_free(&sl->frame);
    memset(sl, 0, sizeof(*sl));
    atomic_fetch_sub(&s->held_now, 1);
    pthread_cond_broadcast(&s->slot_cond);
}

/* frees a slot nobody uses any more */
static void slot_release_if_idle_locked(FFPMetalPresenterObj *s, Slot *sl)
{
    if (sl->used && !sl->inflight && !sl->latest)
        slot_free_locked(s, sl);
}

- (Slot *)latestSlot
{
    int i;
    for (i = 0; i < SLOT_COUNT; i++)
        if (slots[i].used && slots[i].latest)
            return &slots[i];
    return NULL;
}

- (Slot *)takeSlot
{
    Slot *sl = NULL;
    int i, n;
    pthread_mutex_lock(&slot_lock);
    for (i = 0; i < SLOT_COUNT; i++)
        if (!slots[i].used) {
            sl = &slots[i];
            break;
        }
    if (sl) {
        memset(sl, 0, sizeof(*sl));
        sl->used = 1;
        n = atomic_fetch_add(&held_now, 1) + 1;
        if (n > atomic_load(&held_max))
            atomic_store(&held_max, n);
    }
    pthread_mutex_unlock(&slot_lock);
    return sl;
}

- (void)dropSlot:(Slot *)sl
{
    pthread_mutex_lock(&slot_lock);
    slot_release_if_idle_locked(self, sl);
    pthread_mutex_unlock(&slot_lock);
}

- (void)makeLatest:(Slot *)sl
{
    int i;
    pthread_mutex_lock(&slot_lock);
    for (i = 0; i < SLOT_COUNT; i++) {
        if (&slots[i] != sl && slots[i].latest) {
            slots[i].latest = 0;
            slot_release_if_idle_locked(self, &slots[i]);
        }
    }
    sl->latest = 1;
    pthread_mutex_unlock(&slot_lock);
}

- (void)releaseLatest
{
    Slot *sl;
    pthread_mutex_lock(&slot_lock);
    sl = [self latestSlot];
    if (sl) {
        sl->latest = 0;
        slot_release_if_idle_locked(self, sl);
    }
    pthread_mutex_unlock(&slot_lock);
}

/* waits until no command buffer reads a held frame (they complete within milliseconds), then frees everything */
- (void)drainSlots
{
    int64_t deadline = av_gettime_relative() + DRAIN_TIMEOUT_US;
    int i, busy;
    pthread_mutex_lock(&slot_lock);
    for (;;) {
        struct timespec ts;
        busy = 0;
        for (i = 0; i < SLOT_COUNT; i++)
            if (slots[i].used && slots[i].inflight)
                busy = 1;
        if (!busy || av_gettime_relative() >= deadline)
            break;
        ts.tv_sec = 0;
        ts.tv_nsec = 5 * 1000 * 1000;
        pthread_cond_timedwait_relative_np(&slot_cond, &slot_lock, &ts);
    }
    if (busy)
        av_log(NULL, AV_LOG_WARNING, "metal presenter: GPU still reads a held frame after %d ms; releasing it\n",
               DRAIN_TIMEOUT_US / 1000);
    for (i = 0; i < SLOT_COUNT; i++)
        slot_free_locked(self, &slots[i]);
    pthread_mutex_unlock(&slot_lock);
}

/* ---- input binding ------------------------------------------------------------------------------------------ */

/* a system NV12 / P010 frame: one copy into an IOSurface-backed CVPixelBuffer of the pool */
static CVPixelBufferRef upload_frame(const AVFrame *f, CVPixelBufferPoolRef *pool, OSType *pool_fmt, int *pool_w,
                                     int *pool_h, char *err, size_t n)
{
    OSType fmt = f->format == AV_PIX_FMT_P010 ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange
                                              : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    int bpp = f->format == AV_PIX_FMT_P010 ? 2 : 1;
    CVPixelBufferRef pb = NULL;
    int plane, y;

    if (!*pool || *pool_fmt != fmt || *pool_w != f->width || *pool_h != f->height) {
        NSDictionary *attrs = @{
            (id)kCVPixelBufferPixelFormatTypeKey: @(fmt),
            (id)kCVPixelBufferWidthKey: @(f->width),
            (id)kCVPixelBufferHeightKey: @(f->height),
            (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
            (id)kCVPixelBufferMetalCompatibilityKey: @YES,
        };
        if (*pool)
            CVPixelBufferPoolRelease(*pool);
        *pool = NULL;
        if (CVPixelBufferPoolCreate(kCFAllocatorDefault, NULL, (__bridge CFDictionaryRef)attrs, pool) != kCVReturnSuccess) {
            snprintf(err, n, "CVPixelBufferPoolCreate (upload) failed");
            return NULL;
        }
        *pool_fmt = fmt;
        *pool_w = f->width;
        *pool_h = f->height;
    }
    if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, *pool, &pb) != kCVReturnSuccess || !pb) {
        snprintf(err, n, "CVPixelBufferPoolCreatePixelBuffer (upload) failed");
        return NULL;
    }
    CVPixelBufferLockBaseAddress(pb, 0);
    for (plane = 0; plane < 2; plane++) {
        uint8_t *dst = CVPixelBufferGetBaseAddressOfPlane(pb, plane);
        size_t dst_stride = CVPixelBufferGetBytesPerRowOfPlane(pb, plane);
        int rows = plane ? (f->height + 1) / 2 : f->height;
        size_t row_bytes = plane ? (size_t)((f->width + 1) / 2) * 2 * bpp : (size_t)f->width * bpp;
        for (y = 0; y < rows; y++)
            memcpy(dst + y * dst_stride, f->data[plane] + (ptrdiff_t)y * f->linesize[plane], row_bytes);
    }
    CVPixelBufferUnlockBaseAddress(pb, 0);
    return pb;
}

static int bind_input(const AVFrame *f, CVMetalTextureCacheRef tc, CVPixelBufferPoolRef *pool, OSType *pool_fmt,
                      int *pool_w, int *pool_h, Input *in, char *err, size_t n)
{
    CVReturn cr;
    size_t w0, h0, w1, h1;

    memset(in, 0, sizeof(*in));
    if (vt_frame_ok(f)) {
        in->pb = CVPixelBufferRetain((CVPixelBufferRef)f->data[3]);
        in->direct = 1;
    } else if (f->format == AV_PIX_FMT_NV12 || f->format == AV_PIX_FMT_P010) {
        in->pb = upload_frame(f, pool, pool_fmt, pool_w, pool_h, err, n);
        if (!in->pb)
            return AVERROR(ENOMEM);
    } else {
        snprintf(err, n, "unsupported frame format %s for the metal presenter",
                 (const char *)av_x_if_null(av_get_pix_fmt_name(f->format), "?"));
        return AVERROR(EINVAL);
    }
    cv_format(CVPixelBufferGetPixelFormatType(in->pb), &in->p010);
    w0 = CVPixelBufferGetWidthOfPlane(in->pb, 0);
    h0 = CVPixelBufferGetHeightOfPlane(in->pb, 0);
    w1 = CVPixelBufferGetWidthOfPlane(in->pb, 1);
    h1 = CVPixelBufferGetHeightOfPlane(in->pb, 1);
    cr = CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, tc, in->pb, NULL,
                                                   in->p010 ? MTLPixelFormatR16Unorm : MTLPixelFormatR8Unorm,
                                                   w0, h0, 0, &in->ty);
    if (cr == kCVReturnSuccess)
        cr = CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, tc, in->pb, NULL,
                                                       in->p010 ? MTLPixelFormatRG16Unorm : MTLPixelFormatRG8Unorm,
                                                       w1, h1, 1, &in->tuv);
    if (cr != kCVReturnSuccess || !in->ty || !in->tuv) {
        snprintf(err, n, "CVMetalTextureCacheCreateTextureFromImage failed (%d)", (int)cr);
        input_release(in);
        return AVERROR_EXTERNAL;
    }
    in->storage_w = (int)w0;
    in->storage_h = (int)h0;
    in->vis_x = (int)FFMIN(f->crop_left, (size_t)f->width);
    in->vis_y = (int)FFMIN(f->crop_top, (size_t)f->height);
    in->vis_w = FFMAX(f->width - in->vis_x, 1);
    in->vis_h = FFMAX(f->height - in->vis_y, 1);
    if (in->vis_x + in->vis_w > in->storage_w || in->vis_y + in->vis_h > in->storage_h) {
        snprintf(err, n, "visible rectangle %dx%d+%d+%d outside the %dx%d source", in->vis_w, in->vis_h,
                 in->vis_x, in->vis_y, in->storage_w, in->storage_h);
        input_release(in);
        return AVERROR(EINVAL);
    }
    return 0;
}

/* ---- layer configuration ----------------------------------------------------------------------------------- */

/* pixel format, colorspace and EDR of the layer follow the content: SDR bgra8 sRGB (BT.709 primaries) or ITU-R
 * BT.2020; PQ / HLG bgr10a2 in the BT.2100 colorspaces with extended dynamic range: the drawable holds the signal,
 * the OS maps it to the display. */
- (void)configureLayerForMode:(int)m frame:(const AVFrame *)f
{
    CGColorSpaceRef cs;
    int hdr = m == MODE_PQ || m == MODE_HLG;
    CAEDRMetadata *edr = nil;

    if (m == mode)
        return;
    cs = CGColorSpaceCreateWithName(m == MODE_PQ ? kCGColorSpaceITUR_2100_PQ :
                                    m == MODE_HLG ? kCGColorSpaceITUR_2100_HLG :
                                    m == MODE_SDR2020 ? kCGColorSpaceITUR_2020 : kCGColorSpaceSRGB);
    if (m == MODE_HLG) {
        edr = [CAEDRMetadata HLGMetadata];
    } else if (m == MODE_PQ) {
        const AVFrameSideData *sd = av_frame_get_side_data(f, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
        const AVMasteringDisplayMetadata *md = sd ? (const AVMasteringDisplayMetadata *)sd->data : NULL;
        float lo = 0.005f, hi = 1000.0f;
        if (md && md->has_luminance && md->max_luminance.den && md->min_luminance.den) {
            lo = (float)av_q2d(md->min_luminance);
            hi = (float)av_q2d(md->max_luminance);
        }
        /* opticalOutputScale 100: 1.0 of the EDR linear space = 100 cd/m2 (SDR white) */
        edr = [CAEDRMetadata HDR10MetadataWithMinLuminance:lo maxLuminance:hi opticalOutputScale:100.0f];
    }
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    layer.pixelFormat = mtl_pixfmt(mode_fmt(m));
    layer.colorspace = cs;
    layer.wantsExtendedDynamicRangeContent = hdr;
    layer.EDRMetadata = edr;
    [CATransaction commit];
    CGColorSpaceRelease(cs);
    av_log(NULL, AV_LOG_VERBOSE, "metal presenter: layer %s, %s, EDR %s\n", mode_name(m),
           mtl_pixfmt_name(mode_fmt(m)), hdr ? "on" : "off");
    mode = m;
}

- (void)setFramebufferOnly:(int)on
{
    if (framebuffer_only == on)
        return;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    layer.framebufferOnly = on;
    [CATransaction commit];
    framebuffer_only = on;
}

/* ---- overlays ----------------------------------------------------------------------------------------------- */

- (void)freeOverlay:(int)l
{
    [ov_textures[l] removeAllObjects];
    av_freep(&ov_rects[l]);
    ov_count[l] = 0;
    ov_valid[l] = 0;
}

/* premultiplies and uploads a layer's rects once per (generation, canvas size); a repeat keeps the textures */
- (int)updateOverlay:(int)l layer:(const FFPOverlayLayer *)ov error:(char *)err size:(size_t)n
{
    NSMutableArray<id<MTLTexture>> *textures;
    OverlayRectGeom *rects = NULL;
    uint64_t total_bytes = 0;
    uint8_t *pixels = NULL;
    int i;

    if (!ov) {
        /* no overlays this picture: nothing to draw, keep the cache for later generations */
        ov_visible[l] = 0;
        return 0;
    }
    if (ov->rect_count < 0 || ov->rect_count > OVERLAY_MAX_RECTS ||
        (ov->rect_count && (!ov->rects || ov->canvas_width <= 0 || ov->canvas_height <= 0 ||
                            ov->canvas_width > MAX_DIM || ov->canvas_height > MAX_DIM))) {
        snprintf(err, n, "invalid bitmap overlay layer %d", l);
        return AVERROR(EINVAL);
    }
    if (ov_valid[l] && ov_generation[l] == ov->generation && ov_canvas_w[l] == ov->canvas_width &&
        ov_canvas_h[l] == ov->canvas_height && ov_count[l] == ov->rect_count) {
        ov_visible[l] = !!ov->visible;
        atomic_fetch_add(&ov_cache_hits, 1);
        return 0;
    }
    textures = [NSMutableArray arrayWithCapacity:ov->rect_count];
    if (ov->rect_count) {
        rects = av_calloc(ov->rect_count, sizeof(*rects));
        if (!rects) {
            snprintf(err, n, "bitmap overlay allocation failed");
            return AVERROR(ENOMEM);
        }
    }
    for (i = 0; i < ov->rect_count; i++) {
        const FFPOverlayRect *src = &ov->rects[i];
        MTLTextureDescriptor *td;
        id<MTLTexture> tex;
        int x, y;

        if (!src->rgba || src->width <= 0 || src->height <= 0 || src->stride <= 0 ||
            (int64_t)src->stride < (int64_t)src->width * 4 ||
            src->width > MAX_DIM || src->height > MAX_DIM ||
            (uint64_t)src->width * (uint64_t)src->height * 4u > OVERLAY_MAX_BYTES - total_bytes) {
            snprintf(err, n, "invalid RGBA bitmap overlay rect");
            goto fail;
        }
        total_bytes += (uint64_t)src->width * (uint64_t)src->height * 4u;
        pixels = av_malloc((size_t)src->width * src->height * 4u);
        if (!pixels) {
            snprintf(err, n, "bitmap overlay pixel allocation failed");
            goto fail;
        }
        /* straight alpha in, premultiplied out (correct linear filtering, ONE / ONE_MINUS_SRC_ALPHA) */
        for (y = 0; y < src->height; y++) {
            const uint8_t *s = src->rgba + (size_t)y * src->stride;
            uint8_t *d = pixels + (size_t)y * src->width * 4u;
            for (x = 0; x < src->width; x++) {
                unsigned a = s[x * 4 + 3];
                d[x * 4 + 0] = (uint8_t)((s[x * 4 + 0] * a + 127u) / 255u);
                d[x * 4 + 1] = (uint8_t)((s[x * 4 + 1] * a + 127u) / 255u);
                d[x * 4 + 2] = (uint8_t)((s[x * 4 + 2] * a + 127u) / 255u);
                d[x * 4 + 3] = (uint8_t)a;
            }
        }
        td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                width:src->width height:src->height mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        tex = [dev newTextureWithDescriptor:td];
        if (!tex) {
            snprintf(err, n, "newTexture (bitmap overlay) failed");
            goto fail;
        }
        [tex replaceRegion:MTLRegionMake2D(0, 0, src->width, src->height) mipmapLevel:0
                 withBytes:pixels bytesPerRow:(NSUInteger)src->width * 4u];
        av_freep(&pixels);
        [textures addObject:tex];
        rects[i].x = src->x;
        rects[i].y = src->y;
        rects[i].width = src->width;
        rects[i].height = src->height;
    }
    [self freeOverlay:l];
    ov_textures[l] = textures;
    ov_rects[l] = rects;
    ov_count[l] = ov->rect_count;
    ov_canvas_w[l] = ov->canvas_width;
    ov_canvas_h[l] = ov->canvas_height;
    ov_generation[l] = ov->generation;
    ov_visible[l] = !!ov->visible;
    ov_valid[l] = 1;
    atomic_fetch_add(&ov_uploads, 1);
    atomic_fetch_add(&ov_rects_uploaded, ov->rect_count);
    return 0;
fail:
    av_free(pixels);
    av_free(rects);
    atomic_fetch_add(&ov_draw_failures, 1);
    return AVERROR(EINVAL);
}

/* the overlay quads, in the same render encoder as the video, in layer order */
- (void)drawOverlays:(id<MTLRenderCommandEncoder>)enc fmt:(int)fmt videoRect:(const PRect *)vr width:(int)tw height:(int)th
{
    int l, i, active = 0;

    overlay_frame_rects = 0;
    for (l = 0; l < FFP_OVERLAY_LAYER_COUNT; l++)
        if (ov_valid[l] && ov_visible[l] && ov_count[l] && ov_canvas_w[l] > 0 && ov_canvas_h[l] > 0)
            active = 1;
    if (!active)
        return;
    [enc setRenderPipelineState:overlay_pso[fmt]];
    [enc setFragmentSamplerState:sampler atIndex:0];
    for (l = 0; l < FFP_OVERLAY_LAYER_COUNT; l++) {
        float vw = (float)(vr->right - vr->left);
        float vh = (float)(vr->bottom - vr->top);
        if (!ov_valid[l] || !ov_visible[l] || !ov_count[l] || ov_canvas_w[l] <= 0 || ov_canvas_h[l] <= 0)
            continue;
        for (i = 0; i < ov_count[l]; i++) {
            const OverlayRectGeom *r = &ov_rects[l][i];
            OverlayCB cb;
            cb.dest[0] = (float)vr->left + (float)r->x * vw / ov_canvas_w[l];
            cb.dest[1] = (float)vr->top + (float)r->y * vh / ov_canvas_h[l];
            cb.dest[2] = (float)r->width * vw / ov_canvas_w[l];
            cb.dest[3] = (float)r->height * vh / ov_canvas_h[l];
            cb.target[0] = (float)tw;
            cb.target[1] = (float)th;
            cb.target[2] = mode == MODE_PQ ? 1.0f : mode == MODE_HLG ? 2.0f : 0.0f;
            cb.target[3] = 0;
            [enc setVertexBytes:&cb length:sizeof(cb) atIndex:0];
            [enc setFragmentBytes:&cb length:sizeof(cb) atIndex:0];
            [enc setFragmentTexture:ov_textures[l][i] atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
            atomic_fetch_add(&ov_rects_drawn, 1);
            overlay_frame_rects++;
        }
    }
}

/* ---- drawable acquisition ----------------------------------------------------------------------------------- */

- (void)retireTicket:(FFPDrawableTicket *)t
{
    if (!t || atomic_exchange(&t->retired, 1))
        return;
    pthread_mutex_lock(&gate_lock);
    if (t->gen == gate_gen && pending_drawables > 0)
        pending_drawables--;
    last_retire_us = av_gettime_relative();
    pthread_cond_broadcast(&gate_cond);
    pthread_mutex_unlock(&gate_lock);
}

- (void)releaseToken
{
    pthread_mutex_lock(&gate_lock);
    if (inflight_tokens > 0)
        inflight_tokens--;
    pthread_cond_broadcast(&gate_cond);
    pthread_mutex_unlock(&gate_lock);
}

/* Waits (until deadline) for an in-flight token (the GPU is at most INFLIGHT_MAX frames behind) and for a drawable
 * that nextDrawable can hand out without blocking (at most maximumDrawableCount - 2 committed drawables are still
 * waiting for the display), then takes both. Never blocks past the deadline (a strict timer wakes the wait: plain
 * timed waits are coalesced by the kernel, 16.7 ms becomes 25 ms): nil with *why = 1 (timeout: GPU behind) /
 * 2 (timeout: display behind) / 3 (nextDrawable returned nil). */
- (id<CAMetalDrawable>)acquireDrawableUntil:(int64_t)deadline ticket:(FFPDrawableTicket **)ticket why:(int *)why
{
    id<CAMetalDrawable> drawable;
    FFPDrawableTicket *t;
    int64_t t0 = av_gettime_relative(), t1, t2, now;
    int gate_max = (int)layer.maximumDrawableCount - 2;

    *ticket = nil;
    *why = 0;
    if (gate_max < 0)
        gate_max = 0;
    dispatch_source_set_timer(wake_timer, dispatch_time(DISPATCH_TIME_NOW, FFMAX(deadline - t0, 0) * NSEC_PER_USEC),
                              DISPATCH_TIME_FOREVER, 0);
    pthread_mutex_lock(&gate_lock);
    while (inflight_tokens >= INFLIGHT_MAX || pending_drawables > gate_max) {
        struct timespec ts;
        int64_t remaining;
        now = av_gettime_relative();
        remaining = deadline - now;
        if (remaining <= 0)
            break;
        ts.tv_sec = remaining / 1000000;
        ts.tv_nsec = (remaining % 1000000) * 1000;
        pthread_cond_timedwait_relative_np(&gate_cond, &gate_lock, &ts);   /* safety net; the strict timer wakes us */
    }
    t1 = av_gettime_relative();
    if ((uint64_t)(t1 - t0) > atomic_load(&wait_sem_us_max))
        atomic_store(&wait_sem_us_max, (uint64_t)(t1 - t0));
    if (inflight_tokens >= INFLIGHT_MAX) {
        pthread_mutex_unlock(&gate_lock);
        *why = 1;
        return nil;
    }
    if (pending_drawables > gate_max) {
        /* the compositor has not shown anything for a long time (hidden window, stalled display): the drawables it
         * holds are not coming back through our handlers; start over rather than skipping for ever */
        if (t1 - last_retire_us > 1000000) {
            pending_drawables = 0;
            gate_gen++;
            last_retire_us = t1;
            atomic_fetch_add(&gate_resets, 1);
        } else {
            pthread_mutex_unlock(&gate_lock);
            if ((uint64_t)(t1 - t0) > atomic_load(&wait_gate_us_max))
                atomic_store(&wait_gate_us_max, (uint64_t)(t1 - t0));
            *why = 2;
            return nil;
        }
    }
    inflight_tokens++;
    pending_drawables++;
    t = [[FFPDrawableTicket alloc] init];
    t->gen = gate_gen;
    pthread_mutex_unlock(&gate_lock);
    drawable = [layer nextDrawable];
    t2 = av_gettime_relative();
    if ((uint64_t)(t2 - t1) > atomic_load(&next_drawable_us_max))
        atomic_store(&next_drawable_us_max, (uint64_t)(t2 - t1));
    if (!drawable) {
        [self retireTicket:t];
        [self releaseToken];
        *why = 3;
        return nil;
    }
    *ticket = t;
    return drawable;
}

/* ---- the display render ------------------------------------------------------------------------------------- */

- (void)encodeVideo:(id<MTLRenderCommandEncoder>)enc fmt:(int)fmt input:(const Input *)in cb:(const VideoCB *)cb
{
    [enc setRenderPipelineState:video_pso[fmt]];
    [enc setFragmentBytes:cb length:sizeof(*cb) atIndex:0];
    [enc setFragmentTexture:CVMetalTextureGetTexture(in->ty) atIndex:0];
    [enc setFragmentTexture:CVMetalTextureGetTexture(in->tuv) atIndex:1];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
}

- (void)debugFrame:(const Slot *)sl drawable:(id<CAMetalDrawable>)drawable encoders:(int)encoders blits:(int)blits
{
    const Input *in = &sl->in;
    IOSurfaceRef surf = CVPixelBufferGetIOSurface(in->pb);
    id<MTLTexture> y = CVMetalTextureGetTexture(in->ty), uv = CVMetalTextureGetTexture(in->tuv);
    IOSurfaceRef ys = y.iosurface, uvs = uv.iosurface;
    int same = surf && ys == surf && uvs == surf && y.iosurfacePlane == 0 && uv.iosurfacePlane == 1;

    if (!in->direct) {
        av_log(NULL, AV_LOG_INFO, "metal-debug: frame %d: system %s frame copied once into IOSurface %p (id %u); "
               "command buffer: %d render encoder, %d blit, 0 compute\n", debug_frame_no,
               av_get_pix_fmt_name(sl->frame->format), surf, surf ? IOSurfaceGetID(surf) : 0, encoders, blits);
        return;
    }
    av_log(NULL, AV_LOG_INFO,
           "metal-debug: frame %d: VideoToolbox CVPixelBuffer %p IOSurface %p (id %u, %s %zux%zu) -> Y MTLTexture.iosurface %p "
           "plane %lu (%s %lux%lu), UV MTLTexture.iosurface %p plane %lu (%s) => %s; command buffer: "
           "%d render encoder (clear + video + %d overlay quad%s), %d blit, 0 compute; drawable %lux%lu %s, layer %s\n",
           debug_frame_no, in->pb, surf, surf ? IOSurfaceGetID(surf) : 0,
           av_fourcc2str(CVPixelBufferGetPixelFormatType(in->pb)), CVPixelBufferGetWidth(in->pb),
           CVPixelBufferGetHeight(in->pb), ys, (unsigned long)y.iosurfacePlane,
           in->p010 ? "R16Unorm" : "R8Unorm", (unsigned long)y.width, (unsigned long)y.height,
           uvs, (unsigned long)uv.iosurfacePlane, in->p010 ? "RG16Unorm" : "RG8Unorm",
           same ? "same IOSurface, zero-copy" : "DIFFERENT SURFACE (copy)", encoders, overlay_frame_rects,
           overlay_frame_rects == 1 ? "" : "s", blits,
           (unsigned long)drawable.texture.width, (unsigned long)drawable.texture.height,
           drawable.texture.pixelFormat == MTLPixelFormatBGR10A2Unorm ? "bgr10a2Unorm" : "bgra8Unorm",
           mode_name(mode));
}

- (void)verifyZeroCopy:(const Slot *)sl
{
    const Input *in = &sl->in;
    IOSurfaceRef surf = CVPixelBufferGetIOSurface(in->pb);
    id<MTLTexture> y = CVMetalTextureGetTexture(in->ty), uv = CVMetalTextureGetTexture(in->tuv);
    if (surf && y.iosurface == surf && uv.iosurface == surf && y.iosurfacePlane == 0 && uv.iosurfacePlane == 1)
        atomic_fetch_add(&zero_copy_verified, 1);
    else
        atomic_fetch_add(&zero_copy_mismatch, 1);
}

- (void)notePresented:(const Slot *)sl videoRect:(const PRect *)vr
{
    FFPPresentedInfo info;
    memset(&info, 0, sizeof(info));
    info.serial = sl->serial;
    info.pts = sl->pts;
    info.shown_us = av_gettime_relative();     /* handed to the compositor now (the real scan-out time is later) */
    info.copies = sl->copies;
    info.video_width = vr->right - vr->left;
    info.video_height = vr->bottom - vr->top;
    atomic_fetch_add(&presented_count, 1);
    if (sink.presented)
        sink.presented(sink.engine, &info);
}

/* the completion of a display command buffer: releases the slot's reference and an in-flight token */
- (void)finishCommandBuffer:(id<MTLCommandBuffer>)c slot:(Slot *)sl readback:(id<MTLBuffer>)rb w:(int)rb_w h:(int)rb_h
                     tenBit:(int)rb_10bit
{
    if (c.GPUEndTime > c.GPUStartTime) {
        atomic_fetch_add(&gpu_us_total, (uint64_t)((c.GPUEndTime - c.GPUStartTime) * 1e6));
        atomic_fetch_add(&frames_completed, 1);
    }
    if (c.status == MTLCommandBufferStatusError && c.error) {
        if (c.error.code == MTLCommandBufferErrorDeviceRemoved) {
            snprintf(lost_reason, sizeof(lost_reason), "MTLCommandBufferErrorDeviceRemoved");
            atomic_store(&device_lost, 1);
        }
        av_log(NULL, AV_LOG_ERROR, "metal presenter: command buffer error: %s\n",
               [[c.error localizedDescription] UTF8String]);
    }
    if (rb) {
        size_t bytes = (size_t)rb_w * 4 * rb_h;
        pthread_mutex_lock(&trb_lock);
        if (c.status == MTLCommandBufferStatusCompleted) {
            if (trb_capacity < bytes) {
                uint8_t *nd = av_realloc(trb_data, bytes);
                if (nd) {
                    trb_data = nd;
                    trb_capacity = bytes;
                }
            }
            if (trb_capacity >= bytes) {
                if (rb_10bit) {
                    /* bgr10a2Unorm (HDR signal): BGRA8 = the top 8 of the 10 bits */
                    const uint32_t *src = [rb contents];
                    size_t k;
                    for (k = 0; k < (size_t)rb_w * rb_h; k++) {
                        uint32_t v = src[k];
                        trb_data[k * 4 + 0] = (v >> 2) & 0xff;
                        trb_data[k * 4 + 1] = (v >> 12) & 0xff;
                        trb_data[k * 4 + 2] = (v >> 22) & 0xff;
                        trb_data[k * 4 + 3] = 255;
                    }
                } else {
                    memcpy(trb_data, [rb contents], bytes);
                }
                trb_w = rb_w;
                trb_h = rb_h;
                trb_stride = rb_w * 4;
                trb_valid = 1;
                trb_seq++;
            }
        }
        pthread_mutex_unlock(&trb_lock);
    }
    pthread_mutex_lock(&slot_lock);
    sl->inflight--;
    slot_release_if_idle_locked(self, sl);
    pthread_cond_broadcast(&slot_cond);
    pthread_mutex_unlock(&slot_lock);
    [self releaseToken];
    atomic_fetch_sub(&inflight_cbs, 1);
}

/* Renders a held slot into the layer's next drawable. 0 = presented (sink told), 1 = not presented (skipped /
 * dropped; no sink call), < 0 = error (err filled). */
- (int)renderSlot:(Slot *)sl redraw:(int)redraw error:(char *)err size:(size_t)n
{
    CGSize ds = layer.drawableSize;
    double scale = layer.contentsScale;
    id<CAMetalDrawable> drawable;
    id<MTLCommandBuffer> cmd;
    id<MTLRenderCommandEncoder> enc;
    MTLRenderPassDescriptor *rp;
    id<MTLBuffer> rb_buf = nil;
    VideoCB cb;
    PRect dst;
    int ret, fmt, m, tw, th, encoders = 0, blits = 0, rb_w = 0, rb_h = 0, rb_10bit = 0;
    int64_t t0, waited;
    FFPDrawableTicket *ticket = nil;
    int why = 0;

    if (ds.width < 1 || ds.height < 1) {
        /* hidden or zero-sized host view: nothing to present (like a hidden HWND) */
        atomic_fetch_add(&dropped_hidden, 1);
        return 1;
    }
    m = frame_mode(sl->frame);
    fmt = mode_fmt(m);
    [self configureLayerForMode:m frame:sl->frame];
    [self setFramebufferOnly:!atomic_load(&trb_enabled)];

    /* never block longer than about one refresh: skip the frame when the GPU is INFLIGHT_MAX frames behind or
     * when every drawable is still queued for the display */
    t0 = av_gettime_relative();
    drawable = [self acquireDrawableUntil:t0 + refresh_us ticket:&ticket why:&why];
    waited = av_gettime_relative() - t0;
    atomic_fetch_add(&present_wait_us_total, (uint64_t)waited);
    if ((uint64_t)waited > atomic_load(&present_wait_us_max))
        atomic_store(&present_wait_us_max, (uint64_t)waited);
    if (!drawable) {
        if (why == 3) {
            atomic_fetch_add(&dropped_no_drawable, 1);
        } else {
            atomic_fetch_add(&skipped_timeout, 1);
            atomic_fetch_add(why == 1 ? &skipped_gpu : &skipped_display, 1);
        }
        return 1;
    }
    tw = (int)drawable.texture.width;
    th = (int)drawable.texture.height;
    dst = letterbox(tw, th, sl->in.vis_w, sl->in.vis_h, sl->frame->sample_aspect_ratio);
    ret = video_cb(&sl->in, sl->frame, &dst, &cb, err, n);
    if (ret < 0) {
        [self retireTicket:ticket];
        [self releaseToken];
        return ret;
    }
    video_rect[0] = dst.left;
    video_rect[1] = dst.top;
    video_rect[2] = dst.right - dst.left;
    video_rect[3] = dst.bottom - dst.top;
    last_dw = tw;
    last_dh = th;
    last_scale = scale;

    cmd = [queue commandBuffer];
    cmd.label = @"ffp_metal_presenter display";
    rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = drawable.texture;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    enc = [cmd renderCommandEncoderWithDescriptor:rp];
    encoders++;
    [self encodeVideo:enc fmt:fmt input:&sl->in cb:&cb];
    [self drawOverlays:enc fmt:fmt videoRect:&dst width:tw height:th];
    [enc endEncoding];

    if (atomic_load(&trb_enabled)) {
        /* TEST ONLY: the composed drawable is copied after the render pass */
        NSUInteger stride = (NSUInteger)tw * 4;
        rb_buf = [dev newBufferWithLength:stride * th options:MTLResourceStorageModeShared];
        if (rb_buf) {
            id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
            blits++;
            [blit copyFromTexture:drawable.texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(tw, th, 1) toBuffer:rb_buf destinationOffset:0
           destinationBytesPerRow:stride destinationBytesPerImage:stride * th];
            [blit endEncoding];
            rb_w = tw;
            rb_h = th;
            rb_10bit = drawable.texture.pixelFormat == MTLPixelFormatBGR10A2Unorm;
        }
    }

    if (sl->in.direct)
        [self verifyZeroCopy:sl];
    if (atomic_load(&debug_frames) > 0) {
        atomic_fetch_sub(&debug_frames, 1);
        [self debugFrame:sl drawable:drawable encoders:encoders blits:blits];
        debug_frame_no++;
    }

    /* the slot (its AVFrame reference and plane textures) stays alive until the GPU is done with it */
    pthread_mutex_lock(&slot_lock);
    sl->inflight++;
    pthread_mutex_unlock(&slot_lock);
    atomic_fetch_add(&inflight_cbs, 1);
    {
        int delay = atomic_load(&sim_gpu_latency_us);
        id<MTLBuffer> hold_rb = rb_buf;
        [cmd addCompletedHandler:^(id<MTLCommandBuffer> c) {
            if (c.status != MTLCommandBufferStatusCompleted)
                [self retireTicket:ticket];      /* never reaches the display: do not wait for it */
            if (delay > 0) {
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)delay * NSEC_PER_USEC),
                               dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
                    [self finishCommandBuffer:c slot:sl readback:hold_rb w:rb_w h:rb_h tenBit:rb_10bit];
                });
            } else {
                [self finishCommandBuffer:c slot:sl readback:hold_rb w:rb_w h:rb_h tenBit:rb_10bit];
            }
        }];
        {
            int64_t commit_us = av_gettime_relative();
            [drawable addPresentedHandler:^(id<MTLDrawable> d) {
                [self retireTicket:ticket];
                if (d.presentedTime > 0) {
                    int64_t now = av_gettime_relative();
                    double ago = CACurrentMediaTime() - d.presentedTime;
                    int64_t shown = now - (int64_t)(ago * 1e6);
                    if (shown > commit_us) {
                        atomic_fetch_add(&self->present_latency_us_total, (uint64_t)(shown - commit_us));
                        atomic_fetch_add(&self->present_latency_count, 1);
                    }
                }
            }];
        }
    }
    [cmd presentDrawable:drawable];
    [cmd commit];
    CVMetalTextureCacheFlush(cache, 0);

    atomic_fetch_add(&render_encoders, encoders);
    atomic_fetch_add(&blit_encoders, blits);
    atomic_fetch_add(&frames_presented, 1);
    if (redraw)
        atomic_fetch_add(&redraws, 1);
    [self notePresented:sl videoRect:&dst];
    return 0;
}

/* black into the next drawable (clear) */
- (void)renderBlack
{
    CGSize ds = layer.drawableSize;
    id<CAMetalDrawable> drawable;
    id<MTLCommandBuffer> cmd;
    MTLRenderPassDescriptor *rp;
    FFPDrawableTicket *ticket = nil;
    int why;

    if (ds.width < 1 || ds.height < 1)
        return;
    drawable = [self acquireDrawableUntil:av_gettime_relative() + refresh_us ticket:&ticket why:&why];
    if (!drawable)
        return;
    cmd = [queue commandBuffer];
    rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = drawable.texture;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    [[cmd renderCommandEncoderWithDescriptor:rp] endEncoding];
    [cmd addCompletedHandler:^(id<MTLCommandBuffer> c) {
        if (c.status != MTLCommandBufferStatusCompleted)
            [self retireTicket:ticket];
        [self releaseToken];
    }];
    [drawable addPresentedHandler:^(id<MTLDrawable> d) {
        [self retireTicket:ticket];
    }];
    [cmd presentDrawable:drawable];
    [cmd commit];
    last_dw = (int)drawable.texture.width;
    last_dh = (int)drawable.texture.height;
    last_scale = layer.contentsScale;
}

/* The drawable gate would let a render through now (or would reset a stalled display), without waiting. */
- (BOOL)gateWouldOpen
{
    int gate_max = FFMAX((int)layer.maximumDrawableCount - 2, 0);
    BOOL open;
    pthread_mutex_lock(&gate_lock);
    open = inflight_tokens < INFLIGHT_MAX &&
           (pending_drawables <= gate_max || av_gettime_relative() - last_retire_us > 1000000);
    pthread_mutex_unlock(&gate_lock);
    return open;
}

/* A picture that present() skipped on a timeout (GPU or display behind) is drawn once the gate opens: checked one
 * refresh later and then every refresh, until it is on screen, a newer picture is shown or the media closes. Without
 * it a paused seek whose landing frame is skipped leaves the previous picture up. At most one check is queued; the
 * check never waits for the gate, so present() is not held up by it. */
- (void)redrawSkippedLater
{
    __weak FFPMetalPresenterObj *weak = self;
    if (atomic_exchange(&skip_redraw_pending, 1))
        return;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, refresh_us * NSEC_PER_USEC), view_change_queue, ^{
        FFPMetalPresenterObj *s = weak;
        if (!s)
            return;
        atomic_store(&s->skip_redraw_pending, 0);
        [s redrawSkipped];
    });
}

- (void)redrawSkipped
{
    @autoreleasepool {
        char err[256] = "";
        int ret = 1, again;
        uint64_t timeouts;
        Slot *sl;

        pthread_mutex_lock(&lock);
        if (!is_open || !latest_unshown || !(sl = [self latestSlot])) {
            pthread_mutex_unlock(&lock);
            return;
        }
        if ([self gateWouldOpen]) {
            timeouts = atomic_load(&skipped_timeout);
            ret = [self renderSlot:sl redraw:1 error:err size:sizeof(err)];
            if (ret == 0)
                atomic_fetch_add(&skipped_redraws, 1);
            else if (ret < 0 && sink.failed)
                sink.failed(sink.engine, err, atomic_load(&device_lost), atomic_load(&device_lost) ? lost_reason : NULL);
            /* only a timeout is waited out; a hidden view or a missing drawable ends the attempt */
            latest_unshown = ret == 1 && atomic_load(&skipped_timeout) != timeouts;
        }
        again = latest_unshown;
        pthread_mutex_unlock(&lock);
        if (again)
            [self redrawSkippedLater];
    }
}

/* A view-change redraw that the drawable gate skipped (GPU or display behind, no drawable) runs again one refresh
 * later, until the held picture matches the view, the media closes or nothing is held. At most one retry is queued. */
- (void)retryViewChangeLater
{
    __weak FFPMetalPresenterObj *weak = self;
    if (atomic_exchange(&view_change_retry_pending, 1))
        return;
    atomic_fetch_add(&view_change_retries, 1);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, refresh_us * NSEC_PER_USEC), view_change_queue, ^{
        FFPMetalPresenterObj *s = weak;
        if (!s)
            return;
        atomic_store(&s->view_change_retry_pending, 0);
        [s viewChanged];
    });
}

/* a held picture follows the host's drawableSize / contentsScale after a view change */
- (void)viewChanged
{
    @autoreleasepool {
        CGSize ds;
        double scale;
        Slot *sl;
        char err[256] = "";
        int ret;

        pthread_mutex_lock(&lock);
        ds = layer.drawableSize;
        scale = layer.contentsScale;
        if (!is_open || ((int)ds.width == last_dw && (int)ds.height == last_dh && scale == last_scale) ||
            ds.width < 1 || ds.height < 1) {
            pthread_mutex_unlock(&lock);
            return;
        }
        sl = [self latestSlot];
        if (!sl) {
            pthread_mutex_unlock(&lock);
            return;
        }
        ret = [self renderSlot:sl redraw:1 error:err size:sizeof(err)];
        if (ret == 0) {
            atomic_fetch_add(&view_change_redraws, 1);
            latest_unshown = 0;
        }
        else if (ret < 0 && sink.failed)
            sink.failed(sink.engine, err, atomic_load(&device_lost), atomic_load(&device_lost) ? lost_reason : NULL);
        pthread_mutex_unlock(&lock);
        if (ret == 1)
            [self retryViewChangeLater];
    }
}

/* ---- the FFPPresenter entry points --------------------------------------------------------------------------- */

- (int)open:(const FFPStreamInfo *)info sink:(const FFPPresenterSink *)sk caps:(FFPPresenterCaps *)caps
{
    pthread_mutex_lock(&lock);
    if (is_open) {
        av_log(NULL, AV_LOG_WARNING, "metal presenter: open without close; closing the previous media\n");
        [self closeLocked];
    }
    if (atomic_load(&device_lost)) {
        /* a removed GPU does not come back: open fails (the host recreates the presenter on another device) */
        pthread_mutex_unlock(&lock);
        av_log(NULL, AV_LOG_ERROR, "metal presenter: device lost (%s)\n", lost_reason);
        return AVERROR(ENODEV);
    }
    memset(&sink, 0, sizeof(sink));
    if (sk)
        sink = *sk;
    memset(slots, 0, sizeof(slots));
    atomic_store(&held_now, 0);
    last_dw = last_dh = 0;
    last_scale = 0;
    memset(video_rect, 0, sizeof(video_rect));
    refresh_us = display_refresh_us();
    is_open = 1;
    opens++;

    formats[0].format = AV_PIX_FMT_VIDEOTOOLBOX; formats[0].sw_format = AV_PIX_FMT_NV12;
    formats[1].format = AV_PIX_FMT_VIDEOTOOLBOX; formats[1].sw_format = AV_PIX_FMT_P010;
    formats[2].format = AV_PIX_FMT_NV12;         formats[2].sw_format = AV_PIX_FMT_NONE;
    devices[0] = vt_device;
    memset(caps, 0, sizeof(*caps));
    caps->formats = formats;
    caps->nb_formats = 3;
    caps->devices = devices;
    caps->nb_devices = 1;
    caps->frames_held = FRAMES_HELD;
    av_log(NULL, AV_LOG_VERBOSE, "metal presenter: open %s %dx%d sar %d:%d %.3f fps rotation %d\n",
           info && info->codec ? info->codec : "?", info ? info->width : 0, info ? info->height : 0,
           info ? info->sar_num : 0, info ? info->sar_den : 1, info ? info->frame_rate : 0.0,
           info ? info->rotation : 0);
    pthread_mutex_unlock(&lock);
    return 0;
}

static int same_picture(const Slot *sl, const AVFrame *f)
{
    int i;
    if (!sl || !sl->frame || sl->frame->format != f->format || sl->frame->width != f->width ||
        sl->frame->height != f->height)
        return 0;
    for (i = 0; i < 4; i++)
        if (sl->frame->data[i] != f->data[i])
            return 0;
    return sl->frame->buf[0] && f->buf[0] && sl->frame->buf[0]->data == f->buf[0]->data;
}

- (int)present:(const FFPPicture *)pic
{
    char err[256] = "";
    Slot *sl = NULL;
    int ret, l, skipped;
    uint64_t timeouts;

    pthread_mutex_lock(&lock);
    if (!is_open || !pic || !pic->frame) {
        pthread_mutex_unlock(&lock);
        return AVERROR(EINVAL);
    }
    /* overlay uploads first (independent of the frame): once per (generation, canvas size) */
    for (l = 0; l < FFP_OVERLAY_LAYER_COUNT; l++) {
        ret = [self updateOverlay:l layer:pic->overlays ? &pic->overlays[l] : NULL error:err size:sizeof(err)];
        if (ret < 0) {
            pthread_mutex_unlock(&lock);
            av_log(NULL, AV_LOG_ERROR, "metal presenter: %s\n", err);
            return ret;
        }
    }
    /* a redraw of the picture we hold: no new reference, no rebind, no re-upload */
    if (pic->redraw) {
        pthread_mutex_lock(&slot_lock);
        sl = [self latestSlot];
        if (!same_picture(sl, pic->frame))
            sl = NULL;
        pthread_mutex_unlock(&slot_lock);
    }
    if (!sl) {
        /* the picture being replaced goes first (when no command buffer still reads it), so the new reference never
         * makes it frames_held + 1 even for a moment */
        [self releaseLatest];
        sl = [self takeSlot];
        if (!sl) {
            /* cannot happen: in-flight (<= INFLIGHT_MAX) + latest + the spare */
            pthread_mutex_unlock(&lock);
            av_log(NULL, AV_LOG_ERROR, "metal presenter: no free frame slot\n");
            return AVERROR_BUG;
        }
        sl->frame = av_frame_alloc();
        if (!sl->frame || (ret = av_frame_ref(sl->frame, pic->frame)) < 0) {
            [self dropSlot:sl];
            pthread_mutex_unlock(&lock);
            return ret ? ret : AVERROR(ENOMEM);
        }
        ret = bind_input(sl->frame, cache, &upload_pool, &upload_fmt, &upload_w, &upload_h, &sl->in, err, sizeof(err));
        if (ret < 0) {
            [self dropSlot:sl];
            pthread_mutex_unlock(&lock);
            av_log(NULL, AV_LOG_ERROR, "metal presenter: %s\n", err);
            return ret;
        }
        sl->copies = sl->in.direct ? 0 : 1;
        atomic_fetch_add(sl->in.direct ? &frames_direct : &frames_uploaded, 1);
        atomic_fetch_add(&copies_total, (uint64_t)sl->copies);
    }
    sl->serial = pic->serial;
    sl->pts = pic->pts;
    /* the newest picture is the one a later redraw shows, presented or not */
    [self makeLatest:sl];

    timeouts = atomic_load(&skipped_timeout);
    ret = [self renderSlot:sl redraw:pic->redraw error:err size:sizeof(err)];
    latest_unshown = ret == 1 && atomic_load(&skipped_timeout) != timeouts;
    skipped = latest_unshown;
    pthread_mutex_unlock(&lock);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "metal presenter: present failed: %s\n", err);
        return ret;
    }
    if (skipped)
        [self redrawSkippedLater];
    return 0;
}

- (void)clear
{
    pthread_mutex_lock(&lock);
    latest_unshown = 0;
    [self releaseLatest];
    [self drainSlots];
    if (dev)
        [self renderBlack];
    pthread_mutex_unlock(&lock);
}

- (void)closeLocked
{
    int l;
    latest_unshown = 0;
    [self releaseLatest];
    [self drainSlots];
    for (l = 0; l < FFP_OVERLAY_LAYER_COUNT; l++)
        [self freeOverlay:l];
    is_open = 0;
    memset(&sink, 0, sizeof(sink));
}

- (void)close
{
    pthread_mutex_lock(&lock);
    [self closeLocked];
    pthread_mutex_unlock(&lock);
}

- (int)accepts:(const AVFrame *)f
{
    AVHWFramesContext *fc;
    if (!vt_frame_ok(f) || !f->hw_frames_ctx)
        return 0;
    fc = (AVHWFramesContext *)f->hw_frames_ctx->data;
    return fc && fc->device_ref && fc->device_ref->data == vt_device->data;
}

/* snapshot "as shown": the upright, filtered frame into the engine's BGRA buffer of width x height, without
 * overlays. Its own texture cache / upload pool and lock: it may run concurrently with present(). */
- (int)readback:(const AVFrame *)f bgra:(uint8_t *)bgra stride:(int)stride
          width:(int)width height:(int)height
{
    Input in;
    VideoCB cb;
    PRect dst;
    char err[256] = "";
    int ret;

    if (!f || !bgra || width <= 0 || height <= 0 || stride < width * 4 || width > MAX_DIM || height > MAX_DIM)
        return AVERROR(EINVAL);
    pthread_mutex_lock(&rb_lock);
    @autoreleasepool {
        MTLTextureDescriptor *td;
        id<MTLTexture> tex = nil;
        id<MTLCommandBuffer> cmd;
        id<MTLRenderCommandEncoder> enc;
        MTLRenderPassDescriptor *rp;
        int y, x;

        ret = bind_input(f, rb_cache, &rb_pool, &rb_pool_fmt, &rb_pool_w, &rb_pool_h, &in, err, sizeof(err));
        if (ret < 0)
            goto done;
        dst.left = dst.top = 0;
        dst.right = width;
        dst.bottom = height;
        ret = video_cb(&in, f, &dst, &cb, err, sizeof(err));
        if (ret < 0)
            goto done;
        td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                width:width height:height mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModeShared;
        tex = [dev newTextureWithDescriptor:td];
        if (!tex) {
            snprintf(err, sizeof(err), "snapshot texture creation failed");
            ret = AVERROR(ENOMEM);
            goto done;
        }
        cmd = [queue commandBuffer];
        cmd.label = @"ffp_metal_presenter readback";
        rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = tex;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        enc = [cmd renderCommandEncoderWithDescriptor:rp];
        [self encodeVideo:enc fmt:FMT_BGRA8 input:&in cb:&cb];
        [enc endEncoding];
        [cmd commit];
        [cmd waitUntilCompleted];
        if (cmd.status != MTLCommandBufferStatusCompleted) {
            snprintf(err, sizeof(err), "snapshot render failed");
            ret = AVERROR_EXTERNAL;
            goto done;
        }
        [tex getBytes:bgra bytesPerRow:(NSUInteger)stride fromRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0];
        for (y = 0; y < height; y++)
            for (x = 0; x < width; x++)
                bgra[(size_t)y * stride + x * 4 + 3] = 255;
        CVMetalTextureCacheFlush(rb_cache, 0);
        ret = 0;
done:
        input_release(&in);
    }
    pthread_mutex_unlock(&rb_lock);
    if (ret < 0)
        av_log(NULL, AV_LOG_ERROR, "metal presenter: readback: %s\n", err);
    return ret;
}

- (int)statsJSON:(char *)buf size:(size_t)size
{
    uint64_t done = atomic_load(&frames_completed), lat_n = atomic_load(&present_latency_count);
    uint64_t presented = atomic_load(&frames_presented);
    int m = mode, dw = last_dw, dh = last_dh, vr[4], fbo = framebuffer_only, open_now = is_open;
    double scale = last_scale;
    Slot *sl;
    double held_pts = NAN;
    int held_serial = 0;

    int pending;

    memcpy(vr, video_rect, sizeof(vr));
    pthread_mutex_lock(&gate_lock);
    pending = pending_drawables;
    pthread_mutex_unlock(&gate_lock);
    pthread_mutex_lock(&slot_lock);
    sl = [self latestSlot];
    if (sl) {
        held_pts = sl->pts;
        held_serial = sl->serial;
    }
    pthread_mutex_unlock(&slot_lock);
    /* JSON has no NaN: nothing held (after clear / close) or a picture without pts is null */
    char held_pts_text[32] = "null";
    if (isfinite(held_pts)) snprintf(held_pts_text, sizeof(held_pts_text), "%.6f", held_pts);
    return snprintf(buf, size,
        "{\"name\":\"metal\",\"route\":\"metal:shader\",\"device\":\"%s\",\"open\":%s,\"opens\":%d,"
        "\"layer\":{\"pixel_format\":\"%s\",\"colorspace\":\"%s\",\"edr\":%s,\"drawable\":[%d,%d],"
        "\"contents_scale\":%.2f,\"framebuffer_only\":%s,\"max_drawables\":%d},"
        "\"video_rect\":[%d,%d,%d,%d],\"refresh_us\":%" PRId64 ","
        "\"frames_presented\":%" PRIu64 ",\"frames_direct\":%" PRIu64 ",\"frames_uploaded\":%" PRIu64 ","
        "\"copies\":%" PRIu64 ",\"skipped_timeout\":%" PRIu64 ",\"skipped_gpu\":%" PRIu64 ","
        "\"skipped_display\":%" PRIu64 ",\"gate_resets\":%" PRIu64 ",\"pending_drawables\":%d,"
        "\"dropped_no_drawable\":%" PRIu64 ","
        "\"dropped_hidden\":%" PRIu64 ",\"redraws\":%" PRIu64 ",\"view_change_redraws\":%" PRIu64 ","
        "\"view_change_retries\":%" PRIu64 ",\"skipped_redraws\":%" PRIu64 ","
        "\"presented_calls\":%" PRIu64 ",\"render_encoders\":%" PRIu64 ",\"blit_encoders\":%" PRIu64 ","
        "\"zero_copy_verified\":%" PRIu64 ",\"zero_copy_mismatch\":%" PRIu64 ","
        "\"frames_held\":%d,\"inflight_max\":%d,\"held_now\":%d,\"held_max\":%d,\"held_pts\":%s,\"held_serial\":%d,"
        "\"inflight_cbs\":%d,\"gpu_ms_avg\":%.3f,\"present_wait_ms_max\":%.3f,\"present_wait_ms_avg\":%.3f,"
        "\"wait_tokens_ms_max\":%.3f,\"wait_gate_ms_max\":%.3f,\"next_drawable_ms_max\":%.3f,"
        "\"present_latency_ms_avg\":%.3f,"
        "\"overlay\":{\"uploads\":%" PRIu64 ",\"rects_uploaded\":%" PRIu64 ",\"rects_drawn\":%" PRIu64 ","
        "\"cache_hits\":%" PRIu64 ",\"failures\":%" PRIu64 "},"
        "\"test_readback\":%s,\"device_lost\":%s,\"removed_reason\":\"%s\"}",
        device_name, open_now ? "true" : "false", opens,
        mtl_pixfmt_name(mode_fmt(m)), mode_name(m), m == MODE_PQ || m == MODE_HLG ? "true" : "false", dw, dh,
        scale, fbo < 0 ? "null" : fbo ? "true" : "false", (int)layer.maximumDrawableCount,
        vr[0], vr[1], vr[2], vr[3], refresh_us,
        presented, (uint64_t)atomic_load(&frames_direct), (uint64_t)atomic_load(&frames_uploaded),
        (uint64_t)atomic_load(&copies_total), (uint64_t)atomic_load(&skipped_timeout),
        (uint64_t)atomic_load(&skipped_gpu), (uint64_t)atomic_load(&skipped_display),
        (uint64_t)atomic_load(&gate_resets), pending,
        (uint64_t)atomic_load(&dropped_no_drawable), (uint64_t)atomic_load(&dropped_hidden),
        (uint64_t)atomic_load(&redraws), (uint64_t)atomic_load(&view_change_redraws),
        (uint64_t)atomic_load(&view_change_retries), (uint64_t)atomic_load(&skipped_redraws),
        (uint64_t)atomic_load(&presented_count), (uint64_t)atomic_load(&render_encoders),
        (uint64_t)atomic_load(&blit_encoders),
        (uint64_t)atomic_load(&zero_copy_verified), (uint64_t)atomic_load(&zero_copy_mismatch),
        FRAMES_HELD, INFLIGHT_MAX, atomic_load(&held_now), atomic_load(&held_max), held_pts_text, held_serial,
        atomic_load(&inflight_cbs), done ? atomic_load(&gpu_us_total) / 1000.0 / done : 0.0,
        atomic_load(&present_wait_us_max) / 1000.0,
        presented ? atomic_load(&present_wait_us_total) / 1000.0 / presented : 0.0,
        atomic_load(&wait_sem_us_max) / 1000.0, atomic_load(&wait_gate_us_max) / 1000.0,
        atomic_load(&next_drawable_us_max) / 1000.0,
        lat_n ? atomic_load(&present_latency_us_total) / 1000.0 / lat_n : 0.0,
        (uint64_t)atomic_load(&ov_uploads), (uint64_t)atomic_load(&ov_rects_uploaded),
        (uint64_t)atomic_load(&ov_rects_drawn), (uint64_t)atomic_load(&ov_cache_hits),
        (uint64_t)atomic_load(&ov_draw_failures),
        atomic_load(&trb_enabled) ? "true" : "false", atomic_load(&device_lost) ? "true" : "false", lost_reason);
}

- (void)teardown
{
    int i;
    if (view_change_queue)
        dispatch_sync(view_change_queue, ^{});     /* queued redraws are done; a later retry finds nothing open */
    if (wake_timer) {
        dispatch_source_cancel(wake_timer);
        dispatch_sync(wake_queue, ^{});
        wake_timer = nil;
    }
    pthread_mutex_lock(&lock);
    if (is_open)
        [self closeLocked];
    /* wait for the GPU work that references our textures and for the completion handlers that use self */
    if (queue) {
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        [cb commit];
        [cb waitUntilCompleted];
    }
    {
        int64_t deadline = av_gettime_relative() + DRAIN_TIMEOUT_US;
        while (atomic_load(&inflight_cbs) > 0 && av_gettime_relative() < deadline)
            av_usleep(1000);
    }
    if (dev_observer) {
        MTLRemoveDeviceObserver(dev_observer);
        dev_observer = nil;
    }
    for (i = 0; i < FFP_OVERLAY_LAYER_COUNT; i++)
        [self freeOverlay:i];
    if (upload_pool)
        CVPixelBufferPoolRelease(upload_pool);
    upload_pool = NULL;
    if (rb_pool)
        CVPixelBufferPoolRelease(rb_pool);
    rb_pool = NULL;
    if (cache) {
        CVMetalTextureCacheFlush(cache, 0);
        CFRelease(cache);
        cache = NULL;
    }
    if (rb_cache) {
        CVMetalTextureCacheFlush(rb_cache, 0);
        CFRelease(rb_cache);
        rb_cache = NULL;
    }
    av_freep(&trb_data);
    av_buffer_unref(&vt_device);
    pthread_mutex_unlock(&lock);
}

- (void)dealloc
{
    pthread_mutex_destroy(&lock);
    pthread_mutex_destroy(&slot_lock);
    pthread_cond_destroy(&slot_cond);
    pthread_mutex_destroy(&rb_lock);
    pthread_mutex_destroy(&trb_lock);
    pthread_mutex_destroy(&gate_lock);
    pthread_cond_destroy(&gate_cond);
}

@end

/* ------------------------------------------------------------------------------------------------------- */
/* the C table                                                                                               */

static inline FFPMetalPresenterObj *obj_of(void *opaque)
{
    return (__bridge FFPMetalPresenterObj *)opaque;
}

static int FFP_CALL mp_open(void *opaque, const FFPStreamInfo *info, const FFPPresenterSink *sink, FFPPresenterCaps *caps)
{
    @autoreleasepool {
        return [obj_of(opaque) open:info sink:sink caps:caps];
    }
}

static int FFP_CALL mp_present(void *opaque, const FFPPicture *pic)
{
    static _Thread_local int qos_raised;
    if (!qos_raised) {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        qos_raised = 1;
        av_log(NULL, AV_LOG_VERBOSE, "metal presenter: display-thread QoS raised to user-interactive\n");
    }
    @autoreleasepool {
        return [obj_of(opaque) present:pic];
    }
}

static void FFP_CALL mp_clear(void *opaque)
{
    @autoreleasepool {
        [obj_of(opaque) clear];
    }
}

static void FFP_CALL mp_close(void *opaque)
{
    @autoreleasepool {
        [obj_of(opaque) close];
    }
}

static int FFP_CALL mp_accepts(void *opaque, const struct AVFrame *frame)
{
    return [obj_of(opaque) accepts:frame];
}

static int FFP_CALL mp_readback(void *opaque, const struct AVFrame *frame, uint8_t *bgra,
                                int stride, int width, int height)
{
    return [obj_of(opaque) readback:frame bgra:bgra stride:stride width:width height:height];
}

static int FFP_CALL mp_stats_json(void *opaque, char *buf, size_t size)
{
    @autoreleasepool {
        return [obj_of(opaque) statsJSON:buf size:size];
    }
}

static int FFP_CALL mp_device_lost(void *opaque, char *reason, size_t size)
{
    FFPMetalPresenterObj *o = obj_of(opaque);
    if (!atomic_load(&o->device_lost))
        return 0;
    if (reason && size)
        snprintf(reason, size, "%s", o->lost_reason);
    return 1;
}

FFPPresenter *ffp_metal_presenter_create(void *layer, struct AVBufferRef *vt_device, char **error)
{
    FFPMetalPresenterObj *o;
    FFPPresenter *t;

    if (error)
        *error = NULL;
    @autoreleasepool {
        if (!layer || ![(__bridge id)layer isKindOfClass:[CAMetalLayer class]]) {
            set_error(error, "the metal presenter needs a CAMetalLayer", nil);
            return NULL;
        }
        if (!vt_device || !vt_device->data ||
            ((AVHWDeviceContext *)vt_device->data)->type != AV_HWDEVICE_TYPE_VIDEOTOOLBOX) {
            set_error(error, "the metal presenter needs an AV_HWDEVICE_TYPE_VIDEOTOOLBOX device", nil);
            return NULL;
        }
        o = [[FFPMetalPresenterObj alloc] init];
        if ([o setupWithLayer:(__bridge CAMetalLayer *)layer device:vt_device error:error] < 0) {
            [o teardown];
            return NULL;
        }
        t = &o->table;
        memset(t, 0, sizeof(*t));
        t->name = "metal";
        t->opaque = (__bridge_retained void *)o;   /* the table keeps the object alive */
        t->open = mp_open;
        t->present = mp_present;
        t->clear = mp_clear;
        t->close = mp_close;
        t->accepts = mp_accepts;
        t->setup_frames = NULL;                    /* libavcodec's own VT pool; nothing to bind on the Mac */
        t->readback = mp_readback;
        t->stats_json = mp_stats_json;
        t->device_lost = mp_device_lost;
        return t;
    }
}

void ffp_metal_presenter_destroy(FFPPresenter *p)
{
    if (!p)
        return;
    @autoreleasepool {
        FFPMetalPresenterObj *o = (__bridge_transfer FFPMetalPresenterObj *)p->opaque;
        p->opaque = NULL;
        [o teardown];
        o = nil;
    }
}

void ffp_metal_presenter_view_changed(FFPPresenter *presenter)
{
    if (!presenter || !presenter->opaque)
        return;
    FFPMetalPresenterObj *o = obj_of(presenter->opaque);
    __weak FFPMetalPresenterObj *weak = o;
    dispatch_async(o->view_change_queue, ^{
        FFPMetalPresenterObj *s = weak;
        if (s)
            [s viewChanged];
    });
}

void ffp_metal_presenter_set_debug(FFPPresenter *p, int n)
{
    if (p && p->opaque)
        atomic_store(&obj_of(p->opaque)->debug_frames, n);
}

int ffp_metal_presenter_test_readback_enable(FFPPresenter *p, int enable)
{
    FFPMetalPresenterObj *o;
    if (!p || !p->opaque)
        return AVERROR(EINVAL);
    o = obj_of(p->opaque);
    pthread_mutex_lock(&o->trb_lock);
    atomic_store(&o->trb_enabled, !!enable);
    if (!enable)
        o->trb_valid = 0;
    pthread_mutex_unlock(&o->trb_lock);
    return 0;
}

int ffp_metal_presenter_test_readback_last(FFPPresenter *p, uint8_t **bgra, int *width, int *height, int *stride)
{
    FFPMetalPresenterObj *o;
    uint8_t *copy;
    if (!p || !p->opaque || !bgra || !width || !height || !stride)
        return AVERROR(EINVAL);
    o = obj_of(p->opaque);
    *bgra = NULL;
    pthread_mutex_lock(&o->trb_lock);
    if (!o->trb_valid || !o->trb_data) {
        pthread_mutex_unlock(&o->trb_lock);
        return AVERROR(EAGAIN);
    }
    copy = av_memdup(o->trb_data, (size_t)o->trb_stride * o->trb_h);
    if (!copy) {
        pthread_mutex_unlock(&o->trb_lock);
        return AVERROR(ENOMEM);
    }
    *bgra = copy;
    *width = o->trb_w;
    *height = o->trb_h;
    *stride = o->trb_stride;
    pthread_mutex_unlock(&o->trb_lock);
    return 0;
}

void ffp_metal_presenter_test_set_gpu_latency(FFPPresenter *p, int us)
{
    if (p && p->opaque)
        atomic_store(&obj_of(p->opaque)->sim_gpu_latency_us, us < 0 ? 0 : us);
}
