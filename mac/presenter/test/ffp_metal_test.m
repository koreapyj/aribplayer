/*
 * Test harness of libffpmetalpresenter: a Cocoa host that plays the engine's role.
 *
 * Main thread: NSApplication, an NSWindow with a layer-hosting NSView whose layer is a CAMetalLayer; it keeps
 * drawableSize / contentsScale up to date (resize, backing change) and nothing else touches the window.
 * "display thread": runs the tests; normally calls present / clear / close on the presenter.
 * "read thread": normally open() is made from a freshly spawned thread (the media's read thread).
 * Test i also cycles open() on the display thread and calls clear / close on the main thread; lifecycle calls
 * remain serialised, as the FFPPresenter contract requires.
 * Frames come from FFmpeg (hwaccel videotoolbox via get_format with the device the presenter was created with, or
 * software decoding + swscale to system NV12). FFPPicture is built as the engine would (pts, due_us at the clip's
 * frame rate, serial, overlays) and presented at its due time.
 *
 * Usage: ffp_metal_test --media DIR [--tests abcdefghi] [--frames N] [--no-framedrop]
 * --no-framedrop keeps late decoded frames on slow/virtual hosts; it does not relax presenter timing assertions.
 * CI sets ARIBPLAYER_PRESENTER_SKIP_PRESENT_LATENCY=1 on Apple Paravirtual GPUs to skip only test d's latency bounds.
 * MEDIA in the environment may supply DIR instead; an explicit --media overrides it. No default media directory.
 * Each test prints PASS / FAIL; the exit code is the number of failed tests.
 */

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreVideo/CoreVideo.h>

#include <errno.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>

#include "ffplay.h"
#include "ffp_metal_presenter.h"

/* ------------------------------------------------------------------------------------------------------- */
/* globals                                                                                                   */

static CAMetalLayer *g_layer;
static NSWindow *g_window;
static NSView *g_view;
static const char *g_media = NULL;
static int g_max_frames = 0;               /* 0: whole clip in test h */
static int g_no_framedrop;                 /* keep decode throughput separate from presenter correctness */
static AVBufferRef *g_vt_dev;
static FFPPresenter *g_pres;
static int g_fail_count;
static int g_check_fail;                   /* failures of the current test */
static int64_t g_refresh_us = 16667;

#define CLIP_MPEG2 "tc_1080i_mpeg2.ts"
#define CLIP_HEVC10 "tc_1080p_hevc10.mp4"
#define CLIP_HLG "hlg_tagged.mp4"
#define CLIP_8K "tc_8k_hevc10.mp4"

#define CHECK(cond, ...) do { if (!(cond)) { g_check_fail++; printf("    CHECK FAILED (%s:%d): ", __FILE__, __LINE__); \
                              printf(__VA_ARGS__); printf("\n"); } } while (0)
#define NOTE(...) do { printf("    "); printf(__VA_ARGS__); printf("\n"); } while (0)

static char *media_path(const char *name)
{
    static char buf[4][1024];
    static int k;
    char *b = buf[k++ & 3];
    snprintf(b, sizeof(buf[0]), "%s/%s", g_media, name);
    return b;
}

/* ------------------------------------------------------------------------------------------------------- */
/* the sink: what the engine receives                                                                        */

static pthread_mutex_t g_sink_lock = PTHREAD_MUTEX_INITIALIZER;
static FFPPresentedInfo g_presented[8192];
static int g_presented_n;
static atomic_int g_presented_total;
static int g_failed_n;
static char g_failed_reason[256];

static void FFP_CALL sink_presented(void *engine, const FFPPresentedInfo *info)
{
    pthread_mutex_lock(&g_sink_lock);
    if (g_presented_n < (int)(sizeof(g_presented) / sizeof(g_presented[0])))
        g_presented[g_presented_n++] = *info;
    pthread_mutex_unlock(&g_sink_lock);
    atomic_fetch_add(&g_presented_total, 1);
}

static void FFP_CALL sink_failed(void *engine, const char *reason, int device_lost, const char *removed_reason)
{
    pthread_mutex_lock(&g_sink_lock);
    g_failed_n++;
    snprintf(g_failed_reason, sizeof(g_failed_reason), "%s (device_lost %d, %s)", reason ? reason : "?", device_lost,
             removed_reason ? removed_reason : "-");
    pthread_mutex_unlock(&g_sink_lock);
    printf("    sink.failed: %s\n", g_failed_reason);
}

static void sink_reset(void)
{
    pthread_mutex_lock(&g_sink_lock);
    g_presented_n = 0;
    g_failed_n = 0;
    pthread_mutex_unlock(&g_sink_lock);
}

static int sink_count(void)
{
    int n;
    pthread_mutex_lock(&g_sink_lock);
    n = g_presented_n;
    pthread_mutex_unlock(&g_sink_lock);
    return n;
}

static FFPPresentedInfo sink_last(void)
{
    FFPPresentedInfo i;
    pthread_mutex_lock(&g_sink_lock);
    i = g_presented_n ? g_presented[g_presented_n - 1] : (FFPPresentedInfo){0};
    pthread_mutex_unlock(&g_sink_lock);
    return i;
}

static FFPPresentedInfo sink_at(int k)
{
    FFPPresentedInfo i;
    pthread_mutex_lock(&g_sink_lock);
    i = k < g_presented_n ? g_presented[k] : (FFPPresentedInfo){0};
    pthread_mutex_unlock(&g_sink_lock);
    return i;
}

static const FFPPresenterSink g_sink = { NULL, sink_presented, sink_failed };

/* ------------------------------------------------------------------------------------------------------- */
/* stats_json helpers (flat key lookups; the keys used here are unique in the document)                     */

static char g_stats[8192];

static const char *get_stats(void)
{
    int n = g_pres->stats_json(g_pres->opaque, g_stats, sizeof(g_stats));
    if (n < 0 || n >= (int)sizeof(g_stats))
        snprintf(g_stats, sizeof(g_stats), "{}");
    return g_stats;
}

static const char *json_find(const char *json, const char *key)
{
    char pat[128];
    const char *p;
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    p = strstr(json, pat);
    return p ? p + strlen(pat) : NULL;
}

static double json_num(const char *json, const char *key)
{
    const char *p = json_find(json, key);
    if (!p)
        return NAN;
    while (*p == '[' || *p == ' ')
        p++;
    return strtod(p, NULL);
}

static int64_t json_int(const char *json, const char *key)
{
    double d = json_num(json, key);
    return isnan(d) ? -1 : (int64_t)llround(d);
}

static int json_str(const char *json, const char *key, char *out, size_t n)
{
    const char *p = json_find(json, key);
    const char *e;
    if (!p || *p != '"')
        return -1;
    p++;
    e = strchr(p, '"');
    if (!e)
        return -1;
    snprintf(out, n, "%.*s", (int)(e - p), p);
    return 0;
}

static int json_bool(const char *json, const char *key)
{
    const char *p = json_find(json, key);
    if (!p)
        return -1;
    return !strncmp(p, "true", 4);
}

typedef struct StatSnap {
    int64_t presented, direct, uploaded, copies, skipped, no_drawable, hidden, redraws, view_change, calls;
    int64_t render, blit, zc_ok, zc_bad, held_max, ov_uploads, ov_hits, ov_drawn;
    int64_t skipped_gpu, skipped_display;
} StatSnap;

static StatSnap snap(void)
{
    const char *j = get_stats();
    StatSnap s;
    s.presented = json_int(j, "frames_presented");
    s.direct = json_int(j, "frames_direct");
    s.uploaded = json_int(j, "frames_uploaded");
    s.copies = json_int(j, "copies");
    s.skipped = json_int(j, "skipped_timeout");
    s.no_drawable = json_int(j, "dropped_no_drawable");
    s.hidden = json_int(j, "dropped_hidden");
    s.redraws = json_int(j, "redraws");
    s.view_change = json_int(j, "view_change_redraws");
    s.calls = json_int(j, "presented_calls");
    s.render = json_int(j, "render_encoders");
    s.blit = json_int(j, "blit_encoders");
    s.zc_ok = json_int(j, "zero_copy_verified");
    s.zc_bad = json_int(j, "zero_copy_mismatch");
    s.held_max = json_int(j, "held_max");
    s.ov_uploads = json_int(j, "uploads");
    s.ov_hits = json_int(j, "cache_hits");
    s.ov_drawn = json_int(j, "rects_drawn");
    s.skipped_gpu = json_int(j, "skipped_gpu");
    s.skipped_display = json_int(j, "skipped_display");
    return s;
}

/* ------------------------------------------------------------------------------------------------------- */
/* the Cocoa host                                                                                            */

@interface FFPTestView : NSView
@end

@implementation FFPTestView
- (instancetype)initWithFrame:(NSRect)frame
{
    if ((self = [super initWithFrame:frame])) {
        self.wantsLayer = YES;
        self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawDuringViewResize;
    }
    return self;
}

- (CALayer *)makeBackingLayer
{
    CAMetalLayer *l = [CAMetalLayer layer];
    l.device = MTLCreateSystemDefaultDevice();
    l.pixelFormat = MTLPixelFormatBGRA8Unorm;
    l.opaque = YES;
    l.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
    l.contentsGravity = kCAGravityResizeAspect;
    return l;
}

- (BOOL)wantsUpdateLayer
{
    return YES;
}

/* the host keeps drawableSize = bounds in backing pixels (main thread); the presenter follows it */
- (void)updateDrawableSize
{
    CAMetalLayer *l = (CAMetalLayer *)self.layer;
    CGFloat scale = self.window ? self.window.backingScaleFactor : [NSScreen mainScreen].backingScaleFactor;
    NSSize sz = self.bounds.size;
    l.contentsScale = scale;
    l.drawableSize = CGSizeMake(MAX(1.0, floor(sz.width * scale)), MAX(1.0, floor(sz.height * scale)));
    ffp_metal_presenter_view_changed(g_pres);
}

- (void)setFrameSize:(NSSize)size
{
    [super setFrameSize:size];
    [self updateDrawableSize];
}

- (void)viewDidChangeBackingProperties
{
    [super viewDidChangeBackingProperties];
    [self updateDrawableSize];
}
@end

@interface FFPTestDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end
@implementation FFPTestDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app { return NO; }
- (void)windowWillClose:(NSNotification *)n { exit(2); }
@end

/* main-thread helpers (the host's own view management) */
static void host_set_content_size(int w, int h)
{
    dispatch_sync(dispatch_get_main_queue(), ^{
        [g_window setContentSize:NSMakeSize(w, h)];
        [(FFPTestView *)g_view updateDrawableSize];
    });
}

static void host_set_scale(double scale)
{
    dispatch_sync(dispatch_get_main_queue(), ^{
        NSSize sz = g_view.bounds.size;
        g_layer.contentsScale = scale;
        g_layer.drawableSize = CGSizeMake(floor(sz.width * scale), floor(sz.height * scale));
        ffp_metal_presenter_view_changed(g_pres);
    });
}

static void host_drawable_size(int *w, int *h)
{
    __block CGSize ds;
    dispatch_sync(dispatch_get_main_queue(), ^{ ds = g_layer.drawableSize; });
    *w = (int)ds.width;
    *h = (int)ds.height;
}

/* ------------------------------------------------------------------------------------------------------- */
/* decoding (the engine's read / decode threads, inline here)                                                */

typedef struct Clip {
    AVFormatContext *fmt;
    AVCodecContext *cc;
    int vs;
    int hw;
    AVRational frame_rate;
    struct SwsContext *sws;
    AVFrame *dec;
    AVPacket *pkt;
    int eof;
    int frames;
} Clip;

static enum AVPixelFormat get_format_vt(AVCodecContext *c, const enum AVPixelFormat *fmts)
{
    const enum AVPixelFormat *p;
    for (p = fmts; *p != AV_PIX_FMT_NONE; p++)
        if (*p == AV_PIX_FMT_VIDEOTOOLBOX)
            return *p;
    return fmts[0];
}

static int clip_open(Clip *c, const char *path, int hw)
{
    const AVCodec *codec;
    AVStream *st;
    int ret;

    memset(c, 0, sizeof(*c));
    c->hw = hw;
    if ((ret = avformat_open_input(&c->fmt, path, NULL, NULL)) < 0) {
        printf("    cannot open %s: %s\n", path, av_err2str(ret));
        return ret;
    }
    if ((ret = avformat_find_stream_info(c->fmt, NULL)) < 0)
        return ret;
    c->vs = av_find_best_stream(c->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (c->vs < 0)
        return c->vs;
    st = c->fmt->streams[c->vs];
    c->cc = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(c->cc, st->codecpar);
    c->cc->pkt_timebase = st->time_base;
    if (hw) {
        c->cc->hw_device_ctx = av_buffer_ref(g_vt_dev);
        c->cc->get_format = get_format_vt;
    }
    if ((ret = avcodec_open2(c->cc, codec, NULL)) < 0)
        return ret;
    c->frame_rate = av_guess_frame_rate(c->fmt, st, NULL);
    if (c->frame_rate.num <= 0)
        c->frame_rate = av_make_q(30000, 1001);
    c->dec = av_frame_alloc();
    c->pkt = av_packet_alloc();
    return 0;
}

static void clip_close(Clip *c)
{
    av_frame_free(&c->dec);
    av_packet_free(&c->pkt);
    sws_freeContext(c->sws);
    avcodec_free_context(&c->cc);
    avformat_close_input(&c->fmt);
}

/* the engine's tagging rule: untagged frames are BT.709 above 576 lines, BT.601 at or below */
static void tag_frame(AVFrame *f)
{
    if (f->colorspace == AVCOL_SPC_UNSPECIFIED)
        f->colorspace = f->height > 576 ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
    if (f->color_primaries == AVCOL_PRI_UNSPECIFIED)
        f->color_primaries = f->height > 576 ? AVCOL_PRI_BT709 : AVCOL_PRI_SMPTE170M;
    if (f->color_trc == AVCOL_TRC_UNSPECIFIED)
        f->color_trc = f->height > 576 ? AVCOL_TRC_BT709 : AVCOL_TRC_SMPTE170M;
    if (f->color_range == AVCOL_RANGE_UNSPECIFIED)
        f->color_range = AVCOL_RANGE_MPEG;
}

/* Test b counts references: the decoder keeps its own references (DPB) on the buffers it outputs, so a VT frame is
 * rewrapped in a harness-owned AVBufferRef around the retained CVPixelBuffer (the shape a filter graph output has);
 * then every reference beyond the harness's is the presenter's. */
static int g_rewrap;

static void pb_release(void *opaque, uint8_t *data)
{
    CVPixelBufferRelease((CVPixelBufferRef)opaque);
}

static int rewrap_vt(AVFrame *f)
{
    AVFrame *n = av_frame_alloc();
    CVPixelBufferRef pb = (CVPixelBufferRef)f->data[3];
    int ret;
    n->format = f->format;
    n->width = f->width;
    n->height = f->height;
    n->hw_frames_ctx = av_buffer_ref(f->hw_frames_ctx);
    n->buf[0] = av_buffer_create((uint8_t *)pb, 1, pb_release, (void *)CVPixelBufferRetain(pb), AV_BUFFER_FLAG_READONLY);
    n->data[3] = (uint8_t *)pb;
    ret = av_frame_copy_props(n, f);
    av_frame_unref(f);
    av_frame_move_ref(f, n);
    av_frame_free(&n);
    return ret;
}

/* the next decoded picture into out (unref'd first): VT frame (hw) or system NV12 (software + swscale) */
static int clip_read(Clip *c, AVFrame *out)
{
    int ret;
    av_frame_unref(out);
    for (;;) {
        ret = avcodec_receive_frame(c->cc, c->dec);
        if (ret == 0)
            break;
        if (ret == AVERROR_EOF)
            return ret;
        if (ret != AVERROR(EAGAIN))
            return ret;
        if (c->eof)
            return AVERROR_EOF;
        ret = av_read_frame(c->fmt, c->pkt);
        if (ret < 0) {
            c->eof = 1;
            avcodec_send_packet(c->cc, NULL);
            continue;
        }
        if (c->pkt->stream_index == c->vs)
            ret = avcodec_send_packet(c->cc, c->pkt);
        av_packet_unref(c->pkt);
        if (ret < 0 && ret != AVERROR(EAGAIN))
            return ret;
    }
    c->frames++;
    if (c->dec->format == AV_PIX_FMT_VIDEOTOOLBOX) {
        av_frame_move_ref(out, c->dec);
        if (g_rewrap && (ret = rewrap_vt(out)) < 0)
            return ret;
    } else {
        /* software: convert to the system format the presenter lists (NV12), one more frame copy as the engine's
         * swscale conversion would make */
        out->format = AV_PIX_FMT_NV12;
        out->width = c->dec->width;
        out->height = c->dec->height;
        if ((ret = av_frame_get_buffer(out, 0)) < 0)
            return ret;
        c->sws = sws_getCachedContext(c->sws, c->dec->width, c->dec->height, c->dec->format, out->width, out->height,
                                      AV_PIX_FMT_NV12, SWS_BILINEAR, NULL, NULL, NULL);
        sws_scale(c->sws, (const uint8_t *const *)c->dec->data, c->dec->linesize, 0, c->dec->height, out->data,
                  out->linesize);
        av_frame_copy_props(out, c->dec);
        av_frame_unref(c->dec);
    }
    tag_frame(out);
    return 0;
}

/* a synthetic system NV12 frame of one colour (Y, U, V), BT.709 limited */
static AVFrame *make_nv12(int w, int h, int Y, int U, int V)
{
    AVFrame *f = av_frame_alloc();
    int y;
    f->format = AV_PIX_FMT_NV12;
    f->width = w;
    f->height = h;
    if (av_frame_get_buffer(f, 0) < 0)
        return NULL;
    for (y = 0; y < h; y++)
        memset(f->data[0] + (ptrdiff_t)y * f->linesize[0], Y, w);
    for (y = 0; y < (h + 1) / 2; y++) {
        uint8_t *row = f->data[1] + (ptrdiff_t)y * f->linesize[1];
        int x;
        for (x = 0; x < (w + 1) / 2; x++) {
            row[x * 2] = U;
            row[x * 2 + 1] = V;
        }
    }
    f->colorspace = AVCOL_SPC_BT709;
    f->color_primaries = AVCOL_PRI_BT709;
    f->color_trc = AVCOL_TRC_BT709;
    f->color_range = AVCOL_RANGE_MPEG;
    f->sample_aspect_ratio = av_make_q(1, 1);
    return f;
}

/* ------------------------------------------------------------------------------------------------------- */
/* frame reference accounting (test b): the harness keeps the only engine references in a ring, so every      */
/* extra reference on a frame's buffer is the presenter's                                                     */

#define TRACK_N 16
static AVFrame *g_track[TRACK_N];
static int g_track_pos;
static int g_held_max_seen;

static void track_reset(void)
{
    int i;
    for (i = 0; i < TRACK_N; i++)
        av_frame_free(&g_track[i]);
    g_track_pos = 0;
    g_held_max_seen = 0;
}

static void track_push(const AVFrame *f)
{
    AVFrame **slot = &g_track[g_track_pos++ % TRACK_N];
    av_frame_free(slot);
    *slot = av_frame_clone(f);
}

static int track_presenter_refs(void)
{
    int i, n = 0;
    for (i = 0; i < TRACK_N; i++)
        if (g_track[i] && g_track[i]->buf[0])
            n += av_buffer_get_ref_count(g_track[i]->buf[0]) - 1;
    if (n > g_held_max_seen)
        g_held_max_seen = n;
    return n;
}

/* ------------------------------------------------------------------------------------------------------- */
/* the engine side of the presenter calls                                                                    */

typedef struct Engine {
    int serial;
    FFPOverlayLayer ov[FFP_OVERLAY_LAYER_COUNT];
    int use_ov;
    int64_t present_us_max, present_us_total;
    int presents;
    FFPPresenterCaps caps;
} Engine;

static Engine g_eng;

typedef struct OpenArgs { FFPStreamInfo info; int ret; } OpenArgs;

static void *open_thread(void *arg)
{
    OpenArgs *a = arg;
    a->ret = g_pres->open(g_pres->opaque, &a->info, &g_sink, &g_eng.caps);
    return NULL;
}

/* Initial open() comes from the read thread; a cycled stream may open on the display thread. */
static int eng_open_on_thread(const Clip *c, int read_thread)
{
    OpenArgs a;
    pthread_t t;
    memset(&a, 0, sizeof(a));
    if (c) {
        a.info.width = c->cc->width;
        a.info.height = c->cc->height;
        a.info.sar_num = c->cc->sample_aspect_ratio.num;
        a.info.sar_den = c->cc->sample_aspect_ratio.den ? c->cc->sample_aspect_ratio.den : 1;
        a.info.pix_fmt = c->cc->pix_fmt;
        a.info.frame_rate = av_q2d(c->frame_rate);
        a.info.codec = c->cc->codec->name;
    } else {
        a.info.width = 640;
        a.info.height = 360;
        a.info.sar_den = 1;
        a.info.pix_fmt = AV_PIX_FMT_NV12;
        a.info.frame_rate = 30;
        a.info.codec = "rawvideo";
    }
    memset(&g_eng.caps, 0, sizeof(g_eng.caps));
    g_eng.serial++;
    g_eng.present_us_max = g_eng.present_us_total = 0;
    g_eng.presents = 0;
    if (read_thread) {
        pthread_create(&t, NULL, open_thread, &a);
        pthread_join(t, NULL);
    } else {
        open_thread(&a);
    }
    return a.ret;
}

static int eng_open(const Clip *c)
{
    return eng_open_on_thread(c, 1);
}

static int eng_present(AVFrame *f, double pts, int64_t due_us, int redraw)
{
    FFPPicture pic;
    int64_t t0, dt;
    int ret;
    memset(&pic, 0, sizeof(pic));
    pic.frame = f;
    pic.pts = pts;
    pic.due_us = due_us;
    pic.serial = g_eng.serial;
    pic.redraw = redraw;
    pic.overlays = g_eng.use_ov ? g_eng.ov : NULL;
    t0 = av_gettime_relative();
    ret = g_pres->present(g_pres->opaque, &pic);
    dt = av_gettime_relative() - t0;
    g_eng.presents++;
    g_eng.present_us_total += dt;
    if (dt > g_eng.present_us_max)
        g_eng.present_us_max = dt;
    return ret;
}

static void eng_clear(void)
{
    g_pres->clear(g_pres->opaque);
}

static void eng_close(void)
{
    g_pres->close(g_pres->opaque);
}

typedef struct PlayResult { int decoded, presented_ok, dropped_late, errors; } PlayResult;

/* plays up to n frames of a clip at its frame rate (due_us = start + i * frame duration): the engine's display loop.
 * A frame that is more than one interval late when it is ready is dropped (framedrop), like ffplay. */
static PlayResult play_clip(Clip *c, int n, int paced, int (*hook)(int i, AVFrame *f))
{
    PlayResult r = {0};
    AVFrame *f = av_frame_alloc();
    int64_t dur = (int64_t)llround(1e6 * av_q2d(av_inv_q(c->frame_rate)));
    int64_t start = av_gettime_relative() + 20000;
    int i;

    for (i = 0; n <= 0 || i < n; i++) {
        int64_t due = start + i * dur, now;
        int ret = clip_read(c, f);
        if (ret < 0)
            break;
        r.decoded++;
        now = av_gettime_relative();
        if (paced) {
            /* Functional tests on slow decoders keep every frame without catch-up bursts. */
            if (g_no_framedrop && now > due) {
                start += now - due;
                due = now;
            }
            if (!g_no_framedrop && now > due + dur) {
                r.dropped_late++;
                continue;
            }
            if (due > now)
                av_usleep((unsigned)(due - now));
        } else {
            due = now;
        }
        if (hook && hook(i, f) < 0)
            break;
        ret = eng_present(f, i * av_q2d(av_inv_q(c->frame_rate)), due, 0);
        track_push(f);
        av_frame_unref(f);
        if (ret < 0)
            r.errors++;
        else
            r.presented_ok++;
        track_presenter_refs();
    }
    av_frame_free(&f);
    return r;
}

/* ------------------------------------------------------------------------------------------------------- */
/* colour reference (the shader's math on the CPU)                                                           */

typedef struct RGB { int r, g, b; } RGB;

static void rgb_to_yuv709(int r, int g, int b, int *Y, int *U, int *V)
{
    const double kr = 0.2126, kb = 0.0722, kg = 1 - kr - kb;
    double rr = r / 255.0, gg = g / 255.0, bb = b / 255.0;
    double y = kr * rr + kg * gg + kb * bb;
    double cb = (bb - y) / (2 * (1 - kb)), cr = (rr - y) / (2 * (1 - kr));
    *Y = (int)lrint(16 + 219 * y);
    *U = (int)lrint(128 + 224 * cb);
    *V = (int)lrint(128 + 224 * cr);
}

static RGB yuv709_to_rgb(double Y, double U, double V)
{
    const double kr = 0.2126, kb = 0.0722, kg = 1 - kr - kb;
    double y0 = (Y / 255.0 - 16.0 / 255.0) * 255.0 / 219.0;
    double cb = (U / 255.0 - 0.5) * 255.0 / 224.0, cr = (V / 255.0 - 0.5) * 255.0 / 224.0;
    double r = y0 + 2 * (1 - kr) * cr, b = y0 + 2 * (1 - kb) * cb, g = (y0 - kr * r - kb * b) / kg;
    RGB o;
    o.r = (int)lrint(fmin(1, fmax(0, r)) * 255);
    o.g = (int)lrint(fmin(1, fmax(0, g)) * 255);
    o.b = (int)lrint(fmin(1, fmax(0, b)) * 255);
    return o;
}

static int rgb_close(const uint8_t *bgra, RGB e, int tol)
{
    return abs(bgra[2] - e.r) <= tol && abs(bgra[1] - e.g) <= tol && abs(bgra[0] - e.b) <= tol;
}

/* ------------------------------------------------------------------------------------------------------- */
/* test readback helper: the composed drawable of the next present                                           */

static int fetch_drawable(uint8_t **bgra, int *w, int *h, int *stride)
{
    int i;
    for (i = 0; i < 100; i++) {
        if (ffp_metal_presenter_test_readback_last(g_pres, bgra, w, h, stride) == 0)
            return 0;
        av_usleep(5000);
    }
    return -1;
}

static void arm_readback(void)
{
    int i;
    /* the previous frame's readback must have landed (its completion handler fills the buffer) */
    for (i = 0; i < 100 && json_int(get_stats(), "inflight_cbs") > 0; i++)
        av_usleep(2000);
    ffp_metal_presenter_test_readback_enable(g_pres, 0);
    ffp_metal_presenter_test_readback_enable(g_pres, 1);
}

static int video_rect(int *x, int *y, int *w, int *h)
{
    const char *j = get_stats();
    const char *p = json_find(j, "video_rect");
    if (!p || *p != '[')
        return -1;
    return sscanf(p, "[%d,%d,%d,%d]", x, y, w, h) == 4 ? 0 : -1;
}

/* ------------------------------------------------------------------------------------------------------- */
/* tests                                                                                                     */

static int test_a(void)
{
    Clip c;
    PlayResult r;
    StatSnap s0, s1;
    int i, ok, n;

    printf("[a] zero-copy\n");
    /* 1. VideoToolbox P010 (HEVC 10-bit) */
    if (clip_open(&c, media_path(CLIP_HEVC10), 1) < 0)
        return -1;
    sink_reset();
    CHECK(eng_open(&c) == 0, "open failed");
    CHECK(g_eng.caps.nb_formats == 3 && g_eng.caps.formats[0].format == AV_PIX_FMT_VIDEOTOOLBOX &&
          g_eng.caps.formats[0].sw_format == AV_PIX_FMT_NV12 && g_eng.caps.formats[1].sw_format == AV_PIX_FMT_P010 &&
          g_eng.caps.formats[2].format == AV_PIX_FMT_NV12 && g_eng.caps.formats[2].sw_format == AV_PIX_FMT_NONE,
          "caps.formats");
    CHECK(g_eng.caps.nb_devices == 1 && g_eng.caps.devices[0] && g_eng.caps.devices[0]->data == g_vt_dev->data,
          "caps.devices is the VT device (the presenter's own reference)");
    CHECK(g_eng.caps.frames_held == 3, "frames_held %d", g_eng.caps.frames_held);
    NOTE("caps: %d formats, %d device(s), frames_held %d", g_eng.caps.nb_formats,
         g_eng.caps.nb_devices, g_eng.caps.frames_held);
    ffp_metal_presenter_set_debug(g_pres, 3);
    s0 = snap();
    {
        AVFrame *f = av_frame_alloc();
        int accepted = 0;
        for (i = 0; i < 3 && clip_read(&c, f) == 0; i++)
            accepted += g_pres->accepts(g_pres->opaque, f) == 1 && f->format == AV_PIX_FMT_VIDEOTOOLBOX;
        av_frame_free(&f);
        CHECK(accepted == 3, "accepts() takes the VT frames of our device (%d/3)", accepted);
    }
    r = play_clip(&c, 60, 1, NULL);
    s1 = snap();
    n = sink_count();
    ok = 1;
    for (i = 0; i < n; i++)
        if (sink_at(i).copies != 0)
            ok = 0;
    CHECK(r.errors == 0, "present errors %d", r.errors);
    CHECK(n > 0 && ok, "every presented() of the %d VT frames reports copies == 0", n);
    CHECK(s1.direct - s0.direct == r.presented_ok, "frames_direct %" PRId64 " == presented %d", s1.direct - s0.direct,
          r.presented_ok);
    CHECK(s1.render - s0.render == s1.presented - s0.presented, "render encoders %" PRId64 " == frames presented %" PRId64,
          s1.render - s0.render, s1.presented - s0.presented);
    CHECK(s1.blit - s0.blit == 0, "blit %" PRId64, s1.blit - s0.blit);
    CHECK(s1.zc_ok - s0.zc_ok == s1.presented - s0.presented && s1.zc_bad == 0,
          "MTLTexture.iosurface == CVPixelBufferGetIOSurface for %" PRId64 "/%" PRId64 " frames (mismatch %" PRId64 ")",
          s1.zc_ok - s0.zc_ok, s1.presented - s0.presented, s1.zc_bad);
    NOTE("P010 VT: %d presented, copies 0, render encoders %" PRId64 ", blit %" PRId64
         ", iosurface verified %" PRId64, r.presented_ok, s1.render - s0.render, s1.blit - s0.blit,
         s1.zc_ok - s0.zc_ok);
    eng_close();
    clip_close(&c);

    /* 2. VideoToolbox NV12 (MPEG-2, VT software decoder into IOSurfaces) */
    if (clip_open(&c, media_path(CLIP_MPEG2), 1) < 0)
        return -1;
    sink_reset();
    CHECK(eng_open(&c) == 0, "open failed");
    ffp_metal_presenter_set_debug(g_pres, 1);
    s0 = snap();
    r = play_clip(&c, 30, 1, NULL);
    s1 = snap();
    n = sink_count();
    ok = n > 0;
    for (i = 0; i < n; i++)
        if (sink_at(i).copies != 0)
            ok = 0;
    CHECK(ok && r.errors == 0, "NV12 VT frames: copies == 0 (%d presented)", n);
    CHECK(s1.zc_ok - s0.zc_ok == s1.presented - s0.presented && s1.blit == s0.blit, "NV12 VT zero-copy verified");
    NOTE("NV12 VT (mpeg2): %d presented, copies 0, iosurface verified %" PRId64, r.presented_ok, s1.zc_ok - s0.zc_ok);
    eng_close();
    clip_close(&c);

    /* 3. system NV12 (software decode): uploaded once, copies == 1 */
    if (clip_open(&c, media_path(CLIP_MPEG2), 0) < 0)
        return -1;
    sink_reset();
    CHECK(eng_open(&c) == 0, "open failed");
    ffp_metal_presenter_set_debug(g_pres, 1);
    s0 = snap();
    {
        AVFrame *f = av_frame_alloc();
        if (clip_read(&c, f) == 0)
            CHECK(g_pres->accepts(g_pres->opaque, f) == 0, "accepts() rejects a system frame");
        av_frame_free(&f);
    }
    r = play_clip(&c, 30, 1, NULL);
    s1 = snap();
    n = sink_count();
    ok = n > 0;
    for (i = 0; i < n; i++)
        if (sink_at(i).copies != 1)
            ok = 0;
    CHECK(ok && r.errors == 0, "system NV12 frames: copies == 1 (%d presented)", n);
    CHECK(s1.uploaded - s0.uploaded == r.presented_ok, "frames_uploaded %" PRId64, s1.uploaded - s0.uploaded);
    NOTE("system NV12: %d presented, copies 1 each (frames_uploaded %" PRId64 ")", r.presented_ok, s1.uploaded - s0.uploaded);
    eng_close();
    clip_close(&c);
    return 0;
}

static int test_b(void)
{
    Clip c;
    PlayResult r;
    StatSnap s;
    int refs;

    printf("[b] frames_held contract\n");
    if (clip_open(&c, media_path(CLIP_HEVC10), 1) < 0)
        return -1;
    sink_reset();
    track_reset();
    g_rewrap = 1;
    CHECK(eng_open(&c) == 0, "open failed");
    r = play_clip(&c, 60, 1, NULL);
    CHECK(r.errors == 0, "present errors %d", r.errors);
    CHECK(g_held_max_seen <= g_eng.caps.frames_held, "paced: presenter refs max %d <= frames_held %d", g_held_max_seen,
          g_eng.caps.frames_held);
    NOTE("paced 60 frames: max live presenter references seen %d (frames_held %d)", g_held_max_seen,
         g_eng.caps.frames_held);
    /* a burst with a slow GPU: timeout skips keep the newest picture; still within frames_held */
    g_held_max_seen = 0;
    ffp_metal_presenter_test_set_gpu_latency(g_pres, 40000);
    r = play_clip(&c, 60, 0, NULL);
    ffp_metal_presenter_test_set_gpu_latency(g_pres, 0);
    s = snap();
    CHECK(g_held_max_seen <= g_eng.caps.frames_held, "burst: presenter refs max %d <= frames_held %d", g_held_max_seen,
          g_eng.caps.frames_held);
    CHECK(s.held_max <= g_eng.caps.frames_held, "presenter's own held_max %" PRId64 " <= %d", s.held_max,
          g_eng.caps.frames_held);
    NOTE("unpaced burst with 40 ms simulated GPU latency: max refs seen %d, presenter held_max %" PRId64
         ", skipped_timeout so far %" PRId64, g_held_max_seen, s.held_max, s.skipped);
    av_usleep(100000);
    refs = track_presenter_refs();
    NOTE("before clear(): %d presenter references alive", refs);
    eng_clear();
    refs = track_presenter_refs();
    s = snap();
    CHECK(refs == 0, "after clear(): %d references alive", refs);
    CHECK(json_int(get_stats(), "held_now") == 0, "presenter held_now after clear");
    NOTE("after clear(): %d references (presenter held_now %" PRId64 ")", refs, json_int(get_stats(), "held_now"));
    /* presenting goes on after a clear */
    r = play_clip(&c, 10, 1, NULL);
    CHECK(r.errors == 0 && r.presented_ok == 10, "present after clear");
    eng_close();
    refs = track_presenter_refs();
    CHECK(refs == 0, "after close(): %d references alive", refs);
    CHECK(json_int(get_stats(), "held_now") == 0, "presenter held_now after close");
    NOTE("after close(): %d references (presenter held_now %" PRId64 ")", refs, json_int(get_stats(), "held_now"));
    track_reset();
    g_rewrap = 0;
    clip_close(&c);
    return 0;
}

static uint8_t *solid_rgba(int w, int h, int r, int g, int b, int a)
{
    uint8_t *p = av_malloc((size_t)w * h * 4);
    int i;
    for (i = 0; i < w * h; i++) {
        p[i * 4] = r; p[i * 4 + 1] = g; p[i * 4 + 2] = b; p[i * 4 + 3] = a;
    }
    return p;
}

static int test_c(void)
{
    AVFrame *black = make_nv12(1920, 1080, 16, 128, 128);
    AVFrame *white = make_nv12(1920, 1080, 235, 128, 128);
    FFPOverlayRect cap_rect, sup_rect;
    uint8_t *cap_px, *sup_px, *bgra = NULL;
    int w, h, stride, vx, vy, vw, vh, cw, ch;
    StatSnap s0, s1;
    FFPPresentedInfo pi;
    const uint8_t *p;

    printf("[c] overlays\n");
    sink_reset();
    CHECK(eng_open(NULL) == 0, "open failed");
    g_eng.use_ov = 0;
    /* the engine learns the canvas size from the first presented() */
    CHECK(eng_present(black, 0.0, av_gettime_relative(), 0) == 0, "present");
    pi = sink_last();
    cw = pi.video_width;
    ch = pi.video_height;
    CHECK(cw > 0 && ch > 0, "presented video size %dx%d", cw, ch);
    CHECK(video_rect(&vx, &vy, &vw, &vh) == 0 && vw == cw && vh == ch, "video rect %d,%d %dx%d", vx, vy, vw, vh);
    NOTE("canvas = displayed video area %dx%d at (%d,%d) of the drawable", cw, ch, vx, vy);

    /* caption: red, straight alpha 128, 200x100 at (100,100); superimpose: opaque blue 200x100 at (200,150) */
    cap_px = solid_rgba(200, 100, 255, 0, 0, 128);
    sup_px = solid_rgba(200, 100, 0, 0, 255, 255);
    cap_rect = (FFPOverlayRect){ cap_px, 100, 100, 200, 100, 200 * 4 };
    sup_rect = (FFPOverlayRect){ sup_px, 200, 150, 200, 100, 200 * 4 };
    g_eng.ov[FFP_OVERLAY_CAPTION] = (FFPOverlayLayer){ 1, 1, cw, ch, &cap_rect, 1 };
    g_eng.ov[FFP_OVERLAY_SUPERIMPOSE] = (FFPOverlayLayer){ 1, 1, cw, ch, &sup_rect, 1 };
    g_eng.use_ov = 1;

    s0 = snap();
    arm_readback();
    CHECK(eng_present(black, 0.0, av_gettime_relative(), 1) == 0, "redraw present with overlays");
    CHECK(fetch_drawable(&bgra, &w, &h, &stride) == 0, "test readback");
    if (bgra) {
        /* straight (255,0,0,128) premultiplied -> (128,0,0,128) over black: (128,0,0) */
        p = bgra + (size_t)(vy + 120) * stride + (vx + 150) * 4;
        CHECK(rgb_close(p, (RGB){128, 0, 0}, 2), "caption over black at (150,120): got R%d G%d B%d, expect 128,0,0",
              p[2], p[1], p[0]);
        /* superimpose above the caption in the overlap */
        p = bgra + (size_t)(vy + 170) * stride + (vx + 250) * 4;
        CHECK(rgb_close(p, (RGB){0, 0, 255}, 2), "superimpose over caption at (250,170): got R%d G%d B%d, expect 0,0,255",
              p[2], p[1], p[0]);
        p = bgra + (size_t)(vy + 50) * stride + (vx + 50) * 4;
        CHECK(rgb_close(p, (RGB){0, 0, 0}, 2), "video outside overlays: got R%d G%d B%d", p[2], p[1], p[0]);
        NOTE("drawable %dx%d: caption pixel (128,0,0), overlap pixel (0,0,255), background (0,0,0): ok", w, h);
        av_free(bgra);
        bgra = NULL;
    }
    s1 = snap();
    CHECK(s1.ov_uploads - s0.ov_uploads == 2, "two layer uploads for the new generations (%" PRId64 ")",
          s1.ov_uploads - s0.ov_uploads);
    CHECK(s1.ov_drawn - s0.ov_drawn == 2, "two rects drawn (%" PRId64 ")", s1.ov_drawn - s0.ov_drawn);

    /* same generations again: no upload */
    s0 = snap();
    CHECK(eng_present(black, 0.0, av_gettime_relative(), 1) == 0, "present");
    CHECK(eng_present(black, 0.0, av_gettime_relative(), 1) == 0, "present");
    s1 = snap();
    CHECK(s1.ov_uploads == s0.ov_uploads && s1.ov_hits - s0.ov_hits == 4, "generation reuse: uploads +%" PRId64
          ", cache hits +%" PRId64, s1.ov_uploads - s0.ov_uploads, s1.ov_hits - s0.ov_hits);
    NOTE("same generation twice more: uploads +%" PRId64 ", cache hits +%" PRId64, s1.ov_uploads - s0.ov_uploads,
         s1.ov_hits - s0.ov_hits);

    /* white video + the half-transparent red caption: (255,127,127) */
    g_eng.ov[FFP_OVERLAY_CAPTION].generation = 2;
    g_eng.ov[FFP_OVERLAY_SUPERIMPOSE].visible = 0;
    s0 = snap();
    arm_readback();
    CHECK(eng_present(white, 1.0, av_gettime_relative(), 0) == 0, "present white");
    CHECK(fetch_drawable(&bgra, &w, &h, &stride) == 0, "test readback");
    s1 = snap();
    if (bgra) {
        p = bgra + (size_t)(vy + 120) * stride + (vx + 150) * 4;
        CHECK(rgb_close(p, (RGB){255, 127, 127}, 2), "caption over white at (150,120): got R%d G%d B%d, expect 255,127,127",
              p[2], p[1], p[0]);
        p = bgra + (size_t)(vy + 170) * stride + (vx + 250) * 4;
        CHECK(rgb_close(p, (RGB){255, 127, 127}, 2), "hidden superimpose not drawn at (250,170): got R%d G%d B%d",
              p[2], p[1], p[0]);
        NOTE("white video + caption: (255,127,127); hidden superimpose layer not drawn: ok");
        av_free(bgra);
        bgra = NULL;
    }
    CHECK(s1.ov_uploads - s0.ov_uploads == 1, "new caption generation: one upload (%" PRId64 ")", s1.ov_uploads - s0.ov_uploads);
    ffp_metal_presenter_test_readback_enable(g_pres, 0);
    g_eng.use_ov = 0;
    eng_close();
    av_free(cap_px);
    av_free(sup_px);
    av_frame_free(&black);
    av_frame_free(&white);
    return 0;
}

static int test_d(void)
{
    Clip c;
    PlayResult r;
    StatSnap s0, s1;
    int64_t margin = 10000;
    int before, after;
    double held_pts;
    BOOL skip_latency = [NSProcessInfo.processInfo.environment[@"ARIBPLAYER_PRESENTER_SKIP_PRESENT_LATENCY"] isEqualToString:@"1"];

    printf("[d] timeout skip (present never blocks beyond ~1 refresh = %.2f ms)\n", g_refresh_us / 1000.0);
    if (skip_latency)
        NOTE("SKIP present() latency bounds: Apple Paravirtual GPU scheduling is not real-time "
             "(ARIBPLAYER_PRESENTER_SKIP_PRESENT_LATENCY=1); functional assertions remain enabled");
    if (clip_open(&c, media_path(CLIP_HEVC10), 1) < 0)
        return -1;
    sink_reset();
    CHECK(eng_open(&c) == 0, "open failed");
    /* 1. presents as fast as the clip decodes, no pacing: the drawable queue gates */
    s0 = snap();
    r = play_clip(&c, 120, 0, NULL);
    s1 = snap();
    CHECK(r.errors == 0, "present errors %d", r.errors);
    if (!skip_latency)
        CHECK(g_eng.present_us_max <= g_refresh_us + margin, "unpaced: max present() %.2f ms <= %.2f ms",
              g_eng.present_us_max / 1000.0, (g_refresh_us + margin) / 1000.0);
    NOTE("unpaced x%d: present() max %.2f ms, avg %.2f ms; presented %" PRId64 ", skipped_timeout %" PRId64
         " (gpu %" PRId64 ", display %" PRId64 "), no_drawable %" PRId64, r.presented_ok, g_eng.present_us_max / 1000.0,
         g_eng.present_us_total / 1000.0 / FFMAX(1, g_eng.presents), s1.presented - s0.presented, s1.skipped - s0.skipped,
         s1.skipped_gpu - s0.skipped_gpu, s1.skipped_display - s0.skipped_display, s1.no_drawable - s0.no_drawable);

    /* 2. a GPU that falls behind (simulated 50 ms per command buffer): skips, no presented() for them */
    g_eng.present_us_max = g_eng.present_us_total = 0;
    g_eng.presents = 0;
    ffp_metal_presenter_test_set_gpu_latency(g_pres, 50000);
    before = sink_count();
    s0 = snap();
    r = play_clip(&c, 60, 0, NULL);
    s1 = snap();
    after = sink_count();
    ffp_metal_presenter_test_set_gpu_latency(g_pres, 0);
    CHECK(r.errors == 0, "present errors %d", r.errors);
    CHECK(s1.skipped - s0.skipped > 0, "timeout skips happened (%" PRId64 ")", s1.skipped - s0.skipped);
    if (!skip_latency)
        CHECK(g_eng.present_us_max <= g_refresh_us + margin, "slow GPU: max present() %.2f ms <= %.2f ms",
              g_eng.present_us_max / 1000.0, (g_refresh_us + margin) / 1000.0);
    CHECK(after - before == s1.presented - s0.presented, "presented() calls %d == frames presented %" PRId64,
          after - before, s1.presented - s0.presented);
    CHECK((s1.presented - s0.presented) + (s1.skipped - s0.skipped) + (s1.no_drawable - s0.no_drawable) == r.presented_ok,
          "presented + skipped + no_drawable == present() calls (%" PRId64 " + %" PRId64 " + %" PRId64 " vs %d)",
          s1.presented - s0.presented, s1.skipped - s0.skipped, s1.no_drawable - s0.no_drawable, r.presented_ok);
    held_pts = json_num(get_stats(), "held_pts");
    CHECK(fabs(held_pts - (r.decoded - 1) * av_q2d(av_inv_q(c.frame_rate))) < 1e-6,
          "the newest picture (pts %.4f) is the one kept for redraws (held_pts %.4f)",
          (r.decoded - 1) * av_q2d(av_inv_q(c.frame_rate)), held_pts);
    NOTE("slow GPU x%d: present() max %.2f ms; presented %" PRId64 " (presented() calls %d), skipped_timeout %" PRId64
         "; held picture pts %.4f = last present's pts", r.presented_ok, g_eng.present_us_max / 1000.0,
         s1.presented - s0.presented, after - before, s1.skipped - s0.skipped, held_pts);
    av_usleep(200000);

    /* 3. hidden window (ordered out): drawables are not shown; present() must still return within the budget */
    g_eng.present_us_max = g_eng.present_us_total = 0;
    g_eng.presents = 0;
    dispatch_sync(dispatch_get_main_queue(), ^{ [g_window orderOut:nil]; });
    av_usleep(300000);
    s0 = snap();
    r = play_clip(&c, 60, 0, NULL);
    s1 = snap();
    dispatch_sync(dispatch_get_main_queue(), ^{ [g_window makeKeyAndOrderFront:nil]; });
    CHECK(r.errors == 0, "hidden: present errors %d", r.errors);
    if (!skip_latency)
        CHECK(g_eng.present_us_max <= g_refresh_us + margin, "hidden window: max present() %.2f ms <= %.2f ms",
              g_eng.present_us_max / 1000.0, (g_refresh_us + margin) / 1000.0);
    NOTE("hidden window x%d: present() max %.2f ms, avg %.2f ms; presented %" PRId64 ", skipped_timeout %" PRId64
         " (gpu %" PRId64 ", display %" PRId64 "), gate resets %" PRId64 ", no_drawable %" PRId64,
         r.presented_ok, g_eng.present_us_max / 1000.0, g_eng.present_us_total / 1000.0 / FFMAX(1, g_eng.presents),
         s1.presented - s0.presented, s1.skipped - s0.skipped, json_int(get_stats(), "skipped_gpu") - s0.skipped_gpu,
         json_int(get_stats(), "skipped_display") - s0.skipped_display, json_int(get_stats(), "gate_resets"),
         s1.no_drawable - s0.no_drawable);
    av_usleep(300000);
    r = play_clip(&c, 10, 1, NULL);
    CHECK(r.presented_ok == 10, "presenting after the window is back");
    NOTE("stage maxima over the test: token/gate wait %.2f ms, gate-only timeout wait %.2f ms, nextDrawable %.2f ms",
         json_num(get_stats(), "wait_tokens_ms_max"), json_num(get_stats(), "wait_gate_ms_max"),
         json_num(get_stats(), "next_drawable_ms_max"));
    eng_close();
    clip_close(&c);
    return 0;
}

static int g_resize_at = -1;
static int resize_hook(int i, AVFrame *f)
{
    if (i == g_resize_at)
        host_set_content_size(800, 450);
    return 0;
}

static int test_e(void)
{
    Clip c;
    PlayResult r;
    int w, h, i, n, k, found;
    double scale0, scale1;
    FFPPresentedInfo pi;

    printf("[e] Retina / resize: presented() reports the backing-pixel video size\n");
    if (clip_open(&c, media_path(CLIP_HEVC10), 1) < 0)
        return -1;
    sink_reset();
    CHECK(eng_open(&c) == 0, "open failed");
    host_drawable_size(&w, &h);
    r = play_clip(&c, 5, 1, NULL);
    pi = sink_last();
    CHECK(pi.video_width == w && pi.video_height == h, "initial: presented %dx%d == drawable %dx%d", pi.video_width,
          pi.video_height, w, h);
    NOTE("initial drawable %dx%d (contentsScale %.1f): presented %dx%d", w, h, g_layer.contentsScale, pi.video_width,
         pi.video_height);
    /* mid-play resize on the main thread */
    g_resize_at = 3;
    n = sink_count();
    r = play_clip(&c, 20, 1, resize_hook);
    host_drawable_size(&w, &h);
    found = 0;
    for (k = n; k < sink_count(); k++) {
        pi = sink_at(k);
        if (pi.video_width == w && pi.video_height == h)
            found++;
    }
    CHECK(found > 0, "mid-play resize to 800x450 points: presented() reports %dx%d", w, h);
    NOTE("mid-play resize -> drawable %dx%d: %d of the following presented() report it", w, h, found);
    /* paused: no present() calls; the presenter redraws the held picture and reports the new size by itself */
    n = sink_count();
    host_set_content_size(640, 360);
    host_drawable_size(&w, &h);
    for (i = 0; i < 100 && sink_count() == n; i++)
        av_usleep(10000);
    pi = sink_last();
    CHECK(sink_count() > n && pi.video_width == w && pi.video_height == h,
          "paused resize to 640x360 points: presented() %dx%d after %d ms (expected %dx%d)", pi.video_width,
          pi.video_height, i * 10, w, h);
    NOTE("paused resize -> drawable %dx%d: presented() %dx%d without a present() call (%d ms)", w, h, pi.video_width,
         pi.video_height, i * 10);
    /* backing scale change while paused (simulated: contentsScale 1 <-> 2, drawableSize follows; the window may start on a
     * 1x or a Retina screen, so the scale goes to the other one) */
    scale0 = g_layer.contentsScale;
    scale1 = scale0 >= 1.5 ? 1.0 : 2.0;
    n = sink_count();
    host_set_scale(scale1);
    host_drawable_size(&w, &h);
    for (i = 0; i < 100 && sink_count() == n; i++)
        av_usleep(10000);
    pi = sink_last();
    CHECK(sink_count() > n && pi.video_width == w && pi.video_height == h,
          "paused contentsScale %.0f -> %.0f: presented() %dx%d (expected %dx%d)", scale0, scale1, pi.video_width,
          pi.video_height, w, h);
    NOTE("paused contentsScale %.0f -> %.0f -> drawable %dx%d: presented() %dx%d (%d ms)", scale0, scale1, w, h,
         pi.video_width, pi.video_height, i * 10);
    /* playing again at that scale */
    n = sink_count();
    r = play_clip(&c, 5, 1, NULL);
    pi = sink_last();
    CHECK(pi.video_width == w && pi.video_height == h, "playing at scale %.0f: presented %dx%d", scale1, pi.video_width,
          pi.video_height);
    host_set_scale(scale0);
    host_set_content_size(960, 540);
    av_usleep(150000);
    eng_close();
    clip_close(&c);
    (void)r;
    return 0;
}

static int test_f(void)
{
    Clip c;
    PlayResult r;
    char pf[64], cs[64];
    const char *j;
    __block MTLPixelFormat lpf;
    __block BOOL ledr, lmeta;
    __block NSString *lcs;
    int i, n, ok;

    printf("[f] HDR: HLG frames -> EDR layer, SDR frames reset it\n");
    if (clip_open(&c, media_path(CLIP_HLG), 1) < 0)
        return -1;
    sink_reset();
    CHECK(eng_open(&c) == 0, "open failed");
    r = play_clip(&c, 30, 1, NULL);
    CHECK(r.errors == 0, "present errors %d", r.errors);
    j = get_stats();
    json_str(j, "pixel_format", pf, sizeof(pf));
    json_str(j, "colorspace", cs, sizeof(cs));
    CHECK(!strcmp(pf, "bgr10a2Unorm") && !strcmp(cs, "bt2100-hlg") && json_bool(j, "edr") == 1,
          "stats layer: %s %s edr %d", pf, cs, json_bool(j, "edr"));
    dispatch_sync(dispatch_get_main_queue(), ^{
        lpf = g_layer.pixelFormat;
        ledr = g_layer.wantsExtendedDynamicRangeContent;
        lmeta = g_layer.EDRMetadata != nil;
        lcs = g_layer.colorspace ? (__bridge_transfer NSString *)CGColorSpaceCopyName(g_layer.colorspace) : @"(nil)";
    });
    CHECK(lpf == MTLPixelFormatBGR10A2Unorm && ledr && lmeta &&
          [lcs isEqualToString:(__bridge NSString *)kCGColorSpaceITUR_2100_HLG],
          "layer: pixelFormat %lu, EDR %d, EDRMetadata %d, colorspace %s", (unsigned long)lpf, ledr, lmeta, [lcs UTF8String]);
    n = sink_count();
    ok = n > 0;
    for (i = 0; i < n; i++)
        if (sink_at(i).copies != 0)
            ok = 0;
    CHECK(ok, "HLG P010 VT frames presented zero-copy");
    NOTE("HLG: layer %s / %s / EDR %s / EDRMetadata %s (presented %d, copies 0)", [lcs UTF8String],
         lpf == MTLPixelFormatBGR10A2Unorm ? "bgr10a2Unorm" : "other", ledr ? "on" : "off", lmeta ? "set" : "nil", n);
    eng_close();
    clip_close(&c);

    if (clip_open(&c, media_path(CLIP_HEVC10), 1) < 0)
        return -1;
    sink_reset();
    CHECK(eng_open(&c) == 0, "open failed");
    r = play_clip(&c, 10, 1, NULL);
    j = get_stats();
    json_str(j, "pixel_format", pf, sizeof(pf));
    json_str(j, "colorspace", cs, sizeof(cs));
    CHECK(!strcmp(pf, "bgra8Unorm") && !strcmp(cs, "srgb") && json_bool(j, "edr") == 0, "stats layer after SDR: %s %s edr %d",
          pf, cs, json_bool(j, "edr"));
    dispatch_sync(dispatch_get_main_queue(), ^{
        lpf = g_layer.pixelFormat;
        ledr = g_layer.wantsExtendedDynamicRangeContent;
        lmeta = g_layer.EDRMetadata != nil;
        lcs = g_layer.colorspace ? (__bridge_transfer NSString *)CGColorSpaceCopyName(g_layer.colorspace) : @"(nil)";
    });
    CHECK(lpf == MTLPixelFormatBGRA8Unorm && !ledr && !lmeta && [lcs isEqualToString:(__bridge NSString *)kCGColorSpaceSRGB],
          "layer after SDR: pixelFormat %lu, EDR %d, EDRMetadata %d, colorspace %s", (unsigned long)lpf, ledr, lmeta,
          [lcs UTF8String]);
    NOTE("SDR: layer %s / %s / EDR %s / EDRMetadata %s", [lcs UTF8String],
         lpf == MTLPixelFormatBGRA8Unorm ? "bgra8Unorm" : "other", ledr ? "on" : "off", lmeta ? "set" : "nil");
    eng_close();
    clip_close(&c);
    return 0;
}

/* a 640x360 NV12 frame with four coloured quadrants (TL, TR, BL, BR) */
static const RGB g_quad[4] = { {160, 60, 60}, {60, 160, 60}, {60, 60, 160}, {120, 120, 120} };
static int g_quad_yuv[4][3];

static AVFrame *make_quadrants(void)
{
    AVFrame *f = make_nv12(640, 360, 16, 128, 128);
    int q, y, x;
    for (q = 0; q < 4; q++)
        rgb_to_yuv709(g_quad[q].r, g_quad[q].g, g_quad[q].b, &g_quad_yuv[q][0], &g_quad_yuv[q][1], &g_quad_yuv[q][2]);
    for (y = 0; y < 360; y++)
        for (x = 0; x < 640; x++) {
            int q = (y >= 180) * 2 + (x >= 320);
            f->data[0][y * f->linesize[0] + x] = g_quad_yuv[q][0];
            if (!(y & 1) && !(x & 1)) {
                f->data[1][(y / 2) * f->linesize[1] + x] = g_quad_yuv[q][1];
                f->data[1][(y / 2) * f->linesize[1] + x + 1] = g_quad_yuv[q][2];
            }
        }
    return f;
}

static int test_g(void)
{
    AVFrame *f = make_quadrants();
    struct { const char *name; int width, height; } cases[] = {
        { "native", 640, 360 }, { "scaled", 320, 180 }, { "portrait target", 360, 640 },
    };
    size_t k;
    int worst = 0;

    printf("[g] upright, unmodified pictures via readback() against a CPU reference\n");
    for (k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        int w = cases[k].width, h = cases[k].height;
        int o, ret, bad = 0, maxd = 0;
        char detail[256] = "";
        uint8_t *bgra = av_malloc((size_t)w * h * 4);
        ret = g_pres->readback(g_pres->opaque, f, bgra, w * 4, w, h);
        CHECK(ret == 0, "readback %s: %s", cases[k].name, av_err2str(ret));
        for (o = 0; o < 4 && ret == 0; o++) {
            int cx = (o & 1) ? w * 3 / 4 : w / 4, cy = (o >> 1) ? h * 3 / 4 : h / 4;
            const uint8_t *p = bgra + (size_t)cy * w * 4 + cx * 4;
            /* TL / TR / BL / BR stay in place; only YUV -> RGB and output scaling are applied. */
            RGB e = yuv709_to_rgb(g_quad_yuv[o][0], g_quad_yuv[o][1], g_quad_yuv[o][2]);
            int d = FFMAX3(abs(p[2] - e.r), abs(p[1] - e.g), abs(p[0] - e.b));
            maxd = FFMAX(maxd, d);
            CHECK(p[3] == 255, "%s: quadrant %d alpha %d", cases[k].name, o, p[3]);
            if (d > 4) {
                bad++;
                snprintf(detail, sizeof(detail), "quadrant %d: got (%d,%d,%d) expected (%d,%d,%d)", o, p[2], p[1], p[0],
                         e.r, e.g, e.b);
            }
        }
        worst = FFMAX(worst, maxd);
        CHECK(bad == 0, "%s: %d quadrant(s) off: %s", cases[k].name, bad, detail);
        NOTE("%-22s %dx%d: max channel error %d %s", cases[k].name, w, h, maxd, bad ? "FAIL" : "ok");
        av_free(bgra);
    }
    NOTE("worst channel error over all cases: %d (tolerance 4)", worst);
    av_frame_free(&f);
    return 0;
}

/* CPU sampler of the harness process (ps -o %cpu, 1 s apart) */
static atomic_int g_cpu_run;
static double g_cpu_sum;
static int g_cpu_n;
static void *cpu_thread(void *arg)
{
    char cmd[128], line[64];
    snprintf(cmd, sizeof(cmd), "ps -o %%cpu= -p %d", (int)getpid());
    while (atomic_load(&g_cpu_run)) {
        FILE *p;
        av_usleep(1000000);
        p = popen(cmd, "r");
        if (p) {
            if (fgets(line, sizeof(line), p)) {
                g_cpu_sum += atof(line);
                g_cpu_n++;
            }
            pclose(p);
        }
    }
    return NULL;
}

static int test_h(void)
{
    struct { const char *file; int loops; } clips[] = {
        { CLIP_MPEG2, 1 }, { CLIP_HEVC10, 1 }, { CLIP_HLG, 1 }, { CLIP_8K, 3 },
    };
    size_t k;
    double load[3] = {0, 0, 0};

    printf("[h] all clips end to end (paced at the clip frame rate)\n");
    getloadavg(load, 3);
    NOTE("machine load average before: %.2f %.2f %.2f (other processes may be busy; numbers are noisy)", load[0],
         load[1], load[2]);
    for (k = 0; k < sizeof(clips) / sizeof(clips[0]); k++) {
        Clip c;
        PlayResult r, tot = {0};
        StatSnap s0, s1;
        pthread_t t;
        int loop;
        double cpu;
        int64_t t0;

        g_cpu_sum = 0;
        g_cpu_n = 0;
        atomic_store(&g_cpu_run, 1);
        pthread_create(&t, NULL, cpu_thread, NULL);
        s0 = snap();
        sink_reset();
        t0 = av_gettime_relative();
        for (loop = 0; loop < clips[k].loops; loop++) {
            if (clip_open(&c, media_path(clips[k].file), 1) < 0)
                return -1;
            CHECK(eng_open(&c) == 0, "open failed");
            r = play_clip(&c, g_max_frames, 1, NULL);
            tot.decoded += r.decoded;
            tot.presented_ok += r.presented_ok;
            tot.dropped_late += r.dropped_late;
            tot.errors += r.errors;
            eng_close();
            clip_close(&c);
        }
        atomic_store(&g_cpu_run, 0);
        pthread_join(t, NULL);
        s1 = snap();
        cpu = g_cpu_n ? g_cpu_sum / g_cpu_n : 0;
        CHECK(tot.errors == 0 && g_failed_n == 0, "%s: errors %d, failed() %d", clips[k].file, tot.errors, g_failed_n);
        CHECK(s1.presented > s0.presented, "%s: at least one frame actually presented", clips[k].file);
        CHECK(s1.zc_bad == s0.zc_bad, "%s: zero-copy mismatches", clips[k].file);
        CHECK(s1.direct - s0.direct == s1.presented - s0.presented, "%s: all frames direct", clips[k].file);
        NOTE("%-22s x%d: %.1f s, decoded %d, presented %" PRId64 " (zero-copy %" PRId64 "), dropped late (harness) %d, "
             "skipped_timeout %" PRId64 ", no_drawable %" PRId64 ", gpu avg %.2f ms, present() avg %.2f ms, harness CPU %.1f%% (%d samples)",
             clips[k].file, clips[k].loops, (av_gettime_relative() - t0) / 1e6, tot.decoded, s1.presented - s0.presented,
             s1.zc_ok - s0.zc_ok, tot.dropped_late, s1.skipped - s0.skipped, s1.no_drawable - s0.no_drawable,
             json_num(get_stats(), "gpu_ms_avg"), g_eng.presents ? g_eng.present_us_total / 1000.0 / g_eng.presents : 0.0,
             cpu, g_cpu_n);
    }
    getloadavg(load, 3);
    NOTE("machine load average after: %.2f %.2f %.2f", load[0], load[1], load[2]);
    return 0;
}

/* The display-thread driver waits for each main-thread call before touching the presenter again.
 * AppKit's main thread keeps running its event loop; it never joins the driver. */
static void lifecycle_on_main(const char *stage, int clear, int pending_view_change)
{
    __block int64_t dt;
    int refs;
    int64_t held;
    dispatch_block_t call = ^{
        CGSize saved_size = g_layer.drawableSize;
        int64_t t0;
        CHECK([NSThread isMainThread], "%s: lifecycle call is on the main thread", stage);
        if (pending_view_change) {
            /* Make the queued view change require a redraw, rather than being a no-op. */
            g_layer.drawableSize = CGSizeMake(saved_size.width + 2, saved_size.height);
            ffp_metal_presenter_view_changed(g_pres);
        }
        t0 = av_gettime_relative();
        if (clear)
            eng_clear();
        else
            eng_close();
        dt = av_gettime_relative() - t0;
        if (pending_view_change)
            g_layer.drawableSize = saved_size;
    };
    if ([NSThread isMainThread])
        call();
    else
        dispatch_sync(dispatch_get_main_queue(), call);
    refs = track_presenter_refs();
    held = json_int(get_stats(), "held_now");
    CHECK(dt <= 500000, "%s: %s() took %.2f ms (limit 500 ms)", stage, clear ? "clear" : "close", dt / 1000.0);
    CHECK(refs == 0 && held == 0, "%s: after %s(), refs %d, held_now %" PRId64, stage, clear ? "clear" : "close",
          refs, held);
    NOTE("%s: main-thread %s() %.2f ms, refs %d, held_now %" PRId64, stage, clear ? "clear" : "close", dt / 1000.0,
         refs, held);
}

static int test_i(void)
{
    Clip c;
    PlayResult r;
    int stage;
    const char *stages[] = { "read-thread open", "display-thread reopen", "pending view change" };

    printf("[i] lifecycle off the display thread\n");
    CHECK(![NSThread isMainThread], "test driver is the display thread, not the main thread");
    if (clip_open(&c, media_path(CLIP_HEVC10), 1) < 0)
        return -1;
    track_reset();
    g_rewrap = 1;
    for (stage = 0; stage < 3; stage++) {
        CHECK(eng_open_on_thread(&c, stage == 0) == 0, "%s: open failed", stages[stage]);
        r = play_clip(&c, 30, 1, NULL);
        CHECK(r.errors == 0 && r.presented_ok == 30, "%s: present errors %d, presented %d/30", stages[stage], r.errors,
              r.presented_ok);
        CHECK(track_presenter_refs() > 0, "%s: presenter holds a frame before lifecycle calls", stages[stage]);
        if (stage == 1)
            lifecycle_on_main(stages[stage], 1, 0);
        lifecycle_on_main(stages[stage], 0, stage == 2);
        track_reset();
    }
    g_rewrap = 0;
    clip_close(&c);
    return 0;
}

/* ------------------------------------------------------------------------------------------------------- */
/* runner                                                                                                    */

typedef int (*TestFn)(void);

static void run_test(char id, TestFn fn, const char *title)
{
    int ret;
    g_check_fail = 0;
    sink_reset();
    g_eng.use_ov = 0;
    ret = fn();
    if (ret < 0)
        g_check_fail++;
    if (g_failed_n) {
        g_check_fail++;
        printf("    sink.failed was called: %s\n", g_failed_reason);
    }
    printf("TEST %c (%s): %s\n\n", id, title, g_check_fail ? "FAIL" : "PASS");
    if (g_check_fail)
        g_fail_count++;
    fflush(stdout);
}

static void *test_thread(void *arg)
{
    const char *tests = arg;
    char *error = NULL;
    int ret;

    /* the engine's display thread: interactive QoS (timer slack of a default-QoS thread is several ms on a busy
     * machine, which shows in the present() timeout) */
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    @autoreleasepool {
        av_log_set_level(AV_LOG_WARNING);
        if (getenv("FFP_METAL_DEBUG") || strchr(tests, 'a'))
            av_log_set_level(AV_LOG_INFO);
        ret = av_hwdevice_ctx_create(&g_vt_dev, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, NULL, NULL, 0);
        if (ret < 0) {
            printf("no VideoToolbox device: %s\n", av_err2str(ret));
            exit(1);
        }
        g_pres = ffp_metal_presenter_create((__bridge void *)g_layer, g_vt_dev, &error);
        if (!g_pres) {
            printf("ffp_metal_presenter_create failed: %s\n", error ? error : "?");
            exit(1);
        }
        g_refresh_us = json_int(get_stats(), "refresh_us");
        printf("presenter \"%s\" created: %s\n\n", g_pres->name, get_stats());
        /* the mandatory calls present; the optional ones we rely on */
        if (!g_pres->open || !g_pres->present || !g_pres->clear || !g_pres->close || !g_pres->accepts ||
            !g_pres->readback || !g_pres->stats_json || !g_pres->device_lost) {
            printf("presenter table incomplete\n");
            exit(1);
        }
        {
            char reason[64] = "";
            printf("device_lost(): %d %s\n\n", g_pres->device_lost(g_pres->opaque, reason, sizeof(reason)), reason);
        }
        /* clear() with nothing open is legal (ffp_stop without video) */
        g_pres->clear(g_pres->opaque);

        if (strchr(tests, 'a')) run_test('a', test_a, "zero-copy");
        if (strchr(tests, 'b')) run_test('b', test_b, "frames_held contract");
        if (strchr(tests, 'c')) run_test('c', test_c, "overlays");
        if (strchr(tests, 'd')) run_test('d', test_d, "timeout skip");
        if (strchr(tests, 'e')) run_test('e', test_e, "Retina / resize");
        if (strchr(tests, 'f')) run_test('f', test_f, "HDR");
        if (strchr(tests, 'g')) run_test('g', test_g, "upright / unmodified");
        if (strchr(tests, 'h')) run_test('h', test_h, "all clips end to end");
        if (strchr(tests, 'i')) run_test('i', test_i, "lifecycle off the display thread");

        printf("final stats: %s\n\n", get_stats());
        ffp_metal_presenter_destroy(g_pres);
        g_pres = NULL;
        av_buffer_unref(&g_vt_dev);
        printf("%s: %d test(s) failed\n", g_fail_count ? "FAIL" : "PASS", g_fail_count);
    }
    exit(g_fail_count);
    return NULL;
}

/* Validate inputs before opening a window or starting GPU work. Only the selected tests need their clips. */
static int validate_media(const char *tests)
{
    const struct { const char *name, *tests; } clips[] = {
        { CLIP_MPEG2, "ah" }, { CLIP_HEVC10, "abdefhi" }, { CLIP_HLG, "fh" }, { CLIP_8K, "h" },
    };
    struct stat st;
    size_t i;

    if (!g_media || !*g_media) {
        fprintf(stderr, "media directory is required: pass --media DIR or set MEDIA\n");
        return -1;
    }
    if (stat(g_media, &st) < 0) {
        fprintf(stderr, "cannot access media directory '%s': %s\n", g_media, strerror(errno));
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "media path is not a directory: '%s'\n", g_media);
        return -1;
    }
    for (i = 0; i < sizeof(clips) / sizeof(clips[0]); i++) {
        const char *path;
        if (!strpbrk(tests, clips[i].tests))
            continue;
        path = media_path(clips[i].name);
        if (stat(path, &st) < 0 || access(path, R_OK) < 0) {
            fprintf(stderr, "cannot read required clip '%s': %s\n", path, strerror(errno));
            return -1;
        }
        if (!S_ISREG(st.st_mode) || st.st_size == 0) {
            fprintf(stderr, "required clip is not a nonempty regular file: '%s'\n", path);
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *tests = "abcdefghi";
    int i;

    g_media = getenv("MEDIA");
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--media") && i + 1 < argc)
            g_media = argv[++i];
        else if (!strcmp(argv[i], "--tests") && i + 1 < argc)
            tests = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            g_max_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-framedrop"))
            g_no_framedrop = 1;
        else {
            fprintf(stderr, "usage: %s --media DIR [--tests abcdefghi] [--frames N] [--no-framedrop]\n"
                            "       MEDIA may supply DIR instead of --media\n", argv[0]);
            return 1;
        }
    }
    if (validate_media(tests) < 0)
        return 1;
    if (g_no_framedrop)
        printf("Harness framedrop disabled: late decoded frames reach the presenter; this is not a real-time decode test.\n");

    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        FFPTestDelegate *del = [[FFPTestDelegate alloc] init];
        NSRect r = NSMakeRect(0, 0, 960, 540);
        pthread_t t;

        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        app.delegate = del;
        g_window = [[NSWindow alloc] initWithContentRect:r
                                               styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                         NSWindowStyleMaskResizable
                                                 backing:NSBackingStoreBuffered defer:NO];
        g_window.releasedWhenClosed = NO;
        g_window.delegate = del;
        g_window.title = @"ffp_metal_test";
        g_window.backgroundColor = [NSColor blackColor];
        g_view = [[FFPTestView alloc] initWithFrame:r];
        g_window.contentView = g_view;
        [(FFPTestView *)g_view updateDrawableSize];
        [g_window center];
        [g_window makeKeyAndOrderFront:nil];
        [app activateIgnoringOtherApps:YES];
        g_layer = (CAMetalLayer *)g_view.layer;

        if (pthread_create(&t, NULL, test_thread, (void *)tests)) {
            fprintf(stderr, "cannot create the test thread\n");
            return 1;
        }
        [app run];
    }
    return 0;
}
