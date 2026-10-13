/*
 * libffpmetalpresenter: a standalone macOS Metal implementation of libffplay's FFPPresenter (ffplay.h).
 * Built only against the public FFmpeg headers; nothing here calls ffp_*.
 *
 * Ownership and threads
 *  - The host owns the NSWindow / NSView / CAMetalLayer. It creates them on the main thread and keeps
 *    layer.drawableSize and layer.contentsScale up to date there (AppKit rules: view resize, backing change). The
 *    presenter never creates, sizes or shows anything. It retains the layer until ffp_metal_presenter_destroy.
 *  - The presenter sets the layer properties that follow the content, in an explicit CATransaction on the calling
 *    thread (present): pixelFormat (bgra8Unorm for SDR, bgr10a2Unorm for PQ / HLG), colorspace (sRGB, ITU-R BT.2020,
 *    ITU-R BT.2100 PQ / HLG), wantsExtendedDynamicRangeContent and EDRMetadata (on for PQ / HLG), framebufferOnly
 *    (YES; NO only while the test readback below is enabled) and, when the host left it nil, device. The host must
 *    not fight these: it should leave them alone after creation.
 *  - open / present / clear / close follow the FFPPresenter contract (never concurrent, any thread). accepts,
 *    readback, stats_json and device_lost are safe at any time, concurrently with them.
 *  - The host calls ffp_metal_presenter_view_changed() after updating drawableSize / contentsScale on the main
 *    thread: a private queue redraws the held picture at the new size and calls sink->presented() with the new size,
 *    so the engine re-renders its captions and sends a redraw present.
 *
 * Display path (one render pass, zero-copy for VideoToolbox frames): CVPixelBuffer (IOSurface, data[3]) ->
 * CVMetalTextureCacheCreateTextureFromImage per plane -> ONE MTLRenderCommandEncoder into the layer's drawable
 * (clear, NV12 / P010 -> R'G'B', the overlay quads) -> presentDrawable -> commit. Pictures arrive upright and already
 * filtered by the engine (autorotate / rotate / flip / eq). No intermediate texture, no blit or compute encoder, no
 * CPU access to the pixels; FFPPresentedInfo.copies = 0.
 * System NV12 frames are copied once into an IOSurface-backed CVPixelBuffer of a pool (copies = 1).
 *
 * Frame references: each in-flight command buffer holds one av_frame_ref of the picture it reads (released in its
 * completed handler); the newest picture is kept for redraws. At most FFPPresenterCaps.frames_held (3 = 2 command
 * buffers in flight + the newest picture, which is a different frame after a timeout skip) are alive at once;
 * clear() / close() release them all before they return (they wait for the GPU, a few ms).
 *
 * present() never blocks for more than about one refresh interval of the main display. Before nextDrawable it
 * waits, with that one budget, until (a) fewer than 2 display command buffers are still executing and (b) at most
 * layer.maximumDrawableCount - 2 committed drawables are still waiting for the display (counted with
 * addPresentedHandler; nextDrawable itself blocks up to a second otherwise and that cannot be configured). On
 * timeout the frame is skipped (no presented() call), its reference is kept as the newest picture, and the next
 * present shows the next frame. The wait is woken by a DISPATCH_TIMER_STRICT timer: the kernel coalesces plain
 * timed waits by about 50 % (a 16.7 ms wait returns after 25 ms).
 */

#ifndef FFP_METAL_PRESENTER_H
#define FFP_METAL_PRESENTER_H

#include <stddef.h>
#include <stdint.h>

#include "ffplay.h"

#ifdef __cplusplus
extern "C" {
#endif

struct AVBufferRef;

/* layer: a CAMetalLayer * (as void *; checked with isKindOfClass). vt_device: an AVBufferRef to an
 * AV_HWDEVICE_TYPE_VIDEOTOOLBOX AVHWDeviceContext, the device open() hands back in FFPPresenterCaps.devices (the
 * presenter takes its own reference). *error (may be NULL): on failure a malloc'd message (free with free()).
 * Any thread. The returned table (name "metal") is valid until ffp_metal_presenter_destroy. */
FFPPresenter *ffp_metal_presenter_create(void *layer, struct AVBufferRef *vt_device, char **error);
/* Releases everything; waits for in-flight GPU work. Not from inside a presenter callback. close() must have been
 * called for an open media (the engine guarantees it). NULL ok. */
void ffp_metal_presenter_destroy(FFPPresenter *p);

/* The host calls this on the main thread after updating the layer's drawableSize / contentsScale (from
 * viewDidChangeBackingProperties, setFrameSize: / layout, or a display change). If a picture is held, the presenter
 * redraws it at the new size and calls presented() with the new drawable size so the engine re-renders captions.
 * Safe any time, including when nothing is open; NULL ok. Never blocks the main thread on GPU work: the redraw is
 * dispatched to the presenter's private queue. */
void ffp_metal_presenter_view_changed(FFPPresenter *presenter);

/* --- debug / test hooks (not part of the engine contract) --- */

/* Log (av_log INFO) the zero-copy evidence of the next n presented frames: the CVPixelBuffer's IOSurface against
 * MTLTexture.iosurface / iosurfacePlane of both plane textures and the encoders of the command buffer. The
 * environment variable FFP_METAL_DEBUG=<n> does the same at creation. */
void ffp_metal_presenter_set_debug(FFPPresenter *p, int n);
/* Test only: while enabled every composed drawable is copied after the render pass (a blit encoder, counted in
 * stats "blit_encoders"; the layer is set framebufferOnly = NO) and the last one can be fetched as BGRA (bgr10a2
 * drawables as the top 8 bits). *bgra is av_malloc'd: free with av_free. 0, or a negative AVERROR. */
int ffp_metal_presenter_test_readback_enable(FFPPresenter *p, int enable);
int ffp_metal_presenter_test_readback_last(FFPPresenter *p, uint8_t **bgra, int *width, int *height, int *stride);
/* Test only: delays the completion of every display command buffer by us microseconds (simulates a GPU that falls
 * behind, to exercise the present() timeout skip). 0 = off. */
void ffp_metal_presenter_test_set_gpu_latency(FFPPresenter *p, int us);

#ifdef __cplusplus
}
#endif
#endif /* FFP_METAL_PRESENTER_H */
