/*
 * The D3D11 presenter core (C): devices on one DXGI adapter, a flip-model swap chain on a child window of the host view,
 * the VideoProcessor and pixel-shader routes, overlays and the Acrylic backdrop. D3D11Presenter.cpp wraps
 * it as an FFPPresenter; probe.c uses its probes.
 *
 * This file is part of FFmpeg-derived code and is distributed under the GNU Lesser General Public License 2.1 or later.
 */
#ifndef WVP_PRESENTER_CORE_H
#define WVP_PRESENTER_CORE_H

#include <stddef.h>
#include <stdint.h>

#include "ffplay.h"

#ifdef __cplusplus
extern "C" {
#endif

struct AVFrame;
struct AVBufferRef;
struct AVBPrint;

/* force caps: probed capabilities that are masked off (to exercise the fallbacks on a GPU that has them) */
enum {
    PR_FORCE_NV12_SRV    = 1 << 0,     /* decoder pool of NV12 can have SHADER_RESOURCE */
    PR_FORCE_P010_SRV    = 1 << 1,     /* ... of P010 */
    PR_FORCE_P010        = 1 << 2,     /* the video processor takes P010 input */
    PR_FORCE_SHADER      = 1 << 3,
};

enum { PR_ROUTE_VP = 1, PR_ROUTE_SHADER = 2 };

/* test-only failure injection, each fires once per presenter */
enum { PR_FAIL_NONE = 0, PR_FAIL_DISPLAY = 1, PR_FAIL_INIT = 2, PR_FAIL_DEVICE_LOST = 3 };

typedef struct PrConfig {
    int adapter_set;                /* the DXGI adapter with this LUID is used for every D3D11 device */
    uint32_t luid_high, luid_low;
    int debug;                      /* D3D11 debug layer when installed */
    unsigned force;                 /* PR_FORCE_* */
    int route;                      /* PR_ROUTE_*, 0 = automatic */
    int srv_fail;                   /* TEST ONLY: this many decoder pools with SRV fail */
} PrConfig;

/* "HHHHHHHH:LLLLLLLL" (hex LUID HighPart:LowPart); 0, or -1 */
int pr_parse_luid(const char *s, PrConfig *cfg);
/* "key=0,key=0": ORs the PR_FORCE_* bits into *mask; 0, or -1 with err */
int pr_parse_force(const char *s, unsigned *mask, char *err, size_t err_size);
/* the names of the bits of mask, "p010=0" ..., written as entries of a JSON string array (without the brackets) */
void pr_force_names_json(struct AVBPrint *bp, unsigned mask);
/* the n-th name of the bits of mask ("p010=0"), NULL past the last */
const char *pr_force_name(unsigned mask, int n);

/* What the presenter can do on the adapter of cfg (probed with real devices; the video processor limits are cached per
 * adapter for the process). */
typedef struct PrPresenterCaps {
    int ok;
    char error[192];
    int p010;                       /* the video processor takes P010 input */
    int nv12_srv, p010_srv;         /* decoder pools of that format can be shader resources */
    int shader;                     /* the pixel-shader route works */
    int deint_blend, deint_bob, deint_adaptive, deint_motion;
    int telecine;
    unsigned forced;                /* PR_FORCE_* bits of cfg that were applied */
} PrPresenterCaps;
void pr_probe_presenter(const PrConfig *cfg, PrPresenterCaps *out);

/* A D3D11VA AVHWDeviceContext on the adapter of cfg (with the debug layer of cfg). */
int pr_create_d3d11_device(const PrConfig *cfg, struct AVBufferRef **out, char *err, size_t err_size);
/* The bit depths (8, 10) the adapter's D3D11 video decoder takes for the codec (FFmpeg codec name); the number written. */
int pr_d3d11_decoder_depths(struct AVBufferRef *d3d11, const char *codec, int depths[2]);
/* "HHHHHHHH:LLLLLLLL", the adapter's description (UTF-8), vendor id and UMD version of a D3D11VA device */
void pr_d3d11_adapter_info(struct AVBufferRef *d3d11, char *luid, size_t luid_size, char *name, size_t name_size,
                           unsigned *vendor, char *driver, size_t driver_size);

typedef struct PrBackdropRegion {
    int x, y, width, height;        /* client pixels of the host view */
    float corner_radius;
    uint32_t tint_color;            /* 0xAARRGGBB, alpha ignored */
    float tint_opacity;
    uint32_t luminosity_color;
    float luminosity_opacity;
    int noise;
    int z;
} PrBackdropRegion;
#define PR_BACKDROP_REGION_MAX 64

typedef struct PrD3D11 PrD3D11;

/* Called on the presenter's window thread after it redrew the held picture on its own (the host view was resized, the
 * backdrop changed): ret 0 with the displayed video size, or a negative value with err (lost: the device is gone, reason
 * its HRESULT). */
typedef void (*PrRedrawFn)(void *opaque, int ret, int video_width, int video_height, const char *err, int lost,
                           unsigned reason);

/* host_hwnd: the view the presenter fills with a child window (created at the first present). */
PrD3D11 *pr_d3d11_create(void *host_hwnd, const PrConfig *cfg, int fail_mode, PrRedrawFn redraw, void *redraw_opaque,
                         char *err, size_t err_size);
/* Not concurrent with any other call of p. */
void pr_d3d11_destroy(PrD3D11 *p);
/* The presenter's D3D11VA device (borrowed). */
struct AVBufferRef *pr_d3d11_device(PrD3D11 *p);
/* Can present() take P010 (video processor P010 input or the shader route)? */
int pr_d3d11_p010_ok(PrD3D11 *p);
/* Does present() take this hardware frame without a download (a D3D11 frame of p's device)? Any thread. */
int pr_d3d11_accepts(PrD3D11 *p, const struct AVFrame *f);
/* Shows f with the overlay layers (NULL: the cached ones) and keeps a reference to f for redraws. 0 shown (copies, video
 * size filled), < 0 failed with err (*lost / *reason: the device is gone). */
int pr_d3d11_present(PrD3D11 *p, const struct AVFrame *f, const FFPOverlayLayer *layers, int *copies, int *video_width,
                     int *video_height, char *err, size_t err_size, int *lost, unsigned *reason);
/* Black; releases the held frame. */
void pr_d3d11_clear(PrD3D11 *p);
void pr_d3d11_release_held(PrD3D11 *p);
/* Renders f as shown (no overlays) into the BGRA buffer of width x height. 0 or < 0 with err. Any thread. */
int pr_d3d11_readback(PrD3D11 *p, const struct AVFrame *f, uint8_t *bgra, int stride, int width, int height,
                      char *err, size_t err_size);
/* Is the device lost? 1 with its HRESULT. Any thread. */
int pr_d3d11_device_lost(PrD3D11 *p, unsigned *reason);
/* Appends the presenter statistics as one JSON object. Any thread. */
void pr_d3d11_stats_json(PrD3D11 *p, struct AVBPrint *bp);
/* Replaces the rounded backdrop regions (count 0: no blur) and redraws the held picture. */
int pr_d3d11_set_backdrop_regions(PrD3D11 *p, const PrBackdropRegion *regions, int count, char *err, size_t err_size);
/* The host view was resized: the child window follows and the held picture is redrawn (window thread). */
void pr_d3d11_host_resized(PrD3D11 *p);
/* Test only: a copy of every composed back buffer before Present. */
void pr_d3d11_readback_test_enable(PrD3D11 *p, int enable);
/* The last composed back buffer (BGRA, av_malloc) and the video rectangle in it. */
int pr_d3d11_readback_last(PrD3D11 *p, uint8_t **bgra, int *width, int *height, int *stride, int *video_x,
                           int *video_y, int *video_width, int *video_height, char *err, size_t err_size);

#ifdef __cplusplus
}
#endif
#endif /* WVP_PRESENTER_CORE_H */
