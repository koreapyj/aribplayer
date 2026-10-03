# libffplay C API (contract)

`libffplay.dll` exposes `fftools/ffplay.c` as a library. The core API is in
`ffmpeg/source-patches/1009-libffplay-core-api.patch`; the ordered source series adds presenters,
D3D11/CUDA rendering, captions, Acrylic and playback control. See the [patch layout](../ffmpeg/README.md#layout).
`ffmpeg/build-ffplay.ps1` installs it into `ffmpeg/dist/win64/` with `ffplay.exe` (a thin
`main()` over this API), the matching FFmpeg DLLs, and `ffplay.h`.

The authoritative declarations are the installed [ffplay.h](../ffmpeg/dist/win64/ffplay.h). libffplay is internal to
this app, not a public API: the app is built against the header of the same build and always ships with those DLLs,
so the interface carries no version and the app checks none. It imports `libffplay.dll` normally; a missing DLL or
export stops the process at launch with the loader's error. Unavailable hardware or failed runtime probes are
capability results.

The C++ binding is [`wvp::FfPlayer`](../src/Core/include/wvp/FfPlayer.h), implemented in
[`FfPlayer.cpp`](../src/Core/src/FfPlayer.cpp), over
[`IMediaPlayer`](../src/Core/include/wvp/IMediaPlayer.h). `CreateAsync` creates the
player on a worker and completes on the UI thread; `CreateSync` pumps messages while waiting.
The wrapper queues library events for UI-thread dispatch
through `HandleNotify`; its controls and destruction belong on the UI thread.

License: jellyfin's configuration uses `--enable-gpl`, so libffplay is GPL; any host
loading it must be GPL-compatible.

## Conventions

* Calling convention: `__cdecl`. All exports carry `FFP_API`
  (`__declspec(dllexport)` when building, `__declspec(dllimport)` otherwise).
* Strings are UTF-8, NUL-terminated. Strings passed in are copied; the caller keeps ownership.
* Return codes: `int` functions return `0` (or a non-negative value) on success and a
  negative `AVERROR` code on failure. `FFP_EINVAL` = `AVERROR(EINVAL)` (-22),
  `FFP_EAGAIN` = `AVERROR(EAGAIN)` (-11).
* Allocated JSON and pixels returned by the library are released with `ffp_free`. `ffp_version`
  returns a static string. Event JSON and log strings are valid only during their callback: copy
  them before queuing work elsewhere, and return quickly.
* Playback controls use the library's event/atomic synchronization. Creation and destruction wait
  for the player loop; `ffp_stop` joins media read/decode threads, and `ffp_open` stops existing
  media before opening its replacement. Snapshots perform synchronous conversion/readback.
  Capability probes and first-use presenter validation may block on driver work. Run probes off
  the UI thread and keep the host message queue pumping during creation; the app's binding (wvp::FfPlayer) handles
  creation and UI event dispatch as described above.
* Must NOT be called from inside the event callback: `ffp_destroy`, `ffp_stop`, `ffp_open`
  (they would deadlock). Post them to the host's UI thread instead.
* Multiple instances may coexist. SDL is initialised once per process (ref-counted: first
  `ffp_create` starts it, last `ffp_destroy` shuts it down). One process-wide "player loop"
  thread owns SDL, every window/renderer and runs the video refresh of all instances (SDL's
  event queue is process-global); each instance additionally has its own read and
  decode threads. The event callback therefore runs on the loop thread, on a read/decode thread,
  or (`STATE stopped`) on the thread that called `ffp_stop`; callbacks of one instance are serialised.
* On Windows, `SDL_AUDIODRIVER` and `SDL_VIDEODRIVER` are read with `GetEnvironmentVariableW`
  when SDL is initialised (the first `ffp_create`) and passed to SDL as hints, so a host that
  sets them with `SetEnvironmentVariable` (e.g. `SDL_AUDIODRIVER=dummy` for silent playback)
  is honoured even if the CRT's `getenv()` does not see them.

## Hosted windows (`hwnd != NULL`)

The host window is only a render target. libffplay never shows, hides, raises, focuses,
resizes, repositions, retitles or makes it fullscreen, never grabs the keyboard/mouse
(`SDL_HINT_GRAB_KEYBOARD=0`, mouse auto-capture off), never calls `SDL_ShowCursor`, tells SDL
not to run its own Windows message loop (`SDL_HINT_WINDOWS_ENABLE_MESSAGELOOP=0`) and stops
text input; the render size follows the HWND client size. A hidden (or zero-sized) host window
is fine: SDL skips presenting while the window is not shown, playback, events, `ffp_get_*` and
`ffp_snapshot` keep working. Only `hwnd == NULL` (ffplay.exe) creates, shows, resizes and
retitles a window of its own.

## Core API contracts

Use the installed header for function signatures, constants and `FFPBackdropRegion` layout rather
than duplicating those declarations here. The following contracts supplement it.

* `ffp_create(hwnd, cb, user)` returns an opaque player or `NULL` on failure; `hwnd == NULL`
  creates an owned window. Destroy the player before its host HWND. `ffp_destroy(NULL)` is allowed.
* `ffp_open` stops the current media and starts the new open asynchronously. A return of `0`
  means the request started, not that media is ready: wait for PREPARED or ERROR.
  `options_kv` is `NULL` or newline-separated `key=value` pairs, split at the first `=`.
  Keys are ffplay options without `-` (`vf`, `af`, `ss`, `t`, `sn`, `an`, `vn`, `hwaccel`,
  `loop`, `framedrop`, `infbuf`, `fast`, etc.) or AVFormat/AVCodec/protocol options
  (`user_agent`, `headers`, `timeout`, `reconnect`, etc.). Values can contain `=`, `:` and `,`.
  `video_bg` (FFmpeg 8.1 ffplay's `-video_bg`) is `tiles` (default), `none` or a color: the backdrop the
  SDL presenter draws behind video with alpha; an invalid color fails the open.
* D3D11 adapter / debug / test options of `ffp_open` (all absent = off; `ffp_probe_caps_opts` takes the first three):
  * `d3d11_adapter_luid=HHHHHHHH:LLLLLLLL` (hex LUID `HighPart:LowPart`, the format of `adapter.luid` in the caps JSON, e.g.
    `00000000:0001d2c4`): every D3D11 and CUDA device libffplay creates for this open uses that GPU - the d3d11 presenter's
    device (shared with the d3d11va decoder), a `d3d11va` hwaccel / `filter_hw_device` created without the presenter, and
    the CUDA devices (`hwaccel=cuda`, `filter_hw_device=cuda`, the presenter's CUDA import). A CUDA device is only used
    when `cuDeviceGetLuid` matches (the presenter's import uses `cuD3D11GetDevice` of its adapter): when none matches,
    creating the cuda device fails like an absent CUDA does (`hwaccel_error` names the LUID, the host blacklists CUDA for
    that adapter). **An unknown LUID makes `ffp_open` return `FFP_EINVAL` synchronously; the reason ("... this machine has
    no GPU with that LUID") is an `AV_LOG_ERROR` line (`ffp_set_log_callback`); there is no fallback to another GPU.** A
    LUID that exists but cannot create a D3D11 video device (the WARP adapter) is a normal presenter failure (event 9 and
    the SDL fallback). Not applied: `d3d12va`, `dxva2`, `qsv`, `vulkan` devices (also not for `hwaccel=auto`).
  * `d3d11_debug=1`: the presenter's device (and a `d3d11va` device made without the presenter) is created with
    `D3D11_CREATE_DEVICE_DEBUG`; when the SDK layers are not installed creation is retried without the flag (a warning
    line says so, `presenter.debug_layer` stays `false`). Debug layer messages go to `av_log`, prefixed `d3d11-debug:`
    (`CORRUPTION`/`ERROR` -> error, `WARNING` -> warning, `INFO` -> info, `MESSAGE` -> verbose), each message ID once,
    drained after the device init, after presented frames (at most every 250 ms) and at close (repeat counts are logged
    then). `presenter.debug_messages` counts every message seen. Cost: the layer slows the device down; tests only.
  * `d3d11_force_caps=key=0[,key=0..]` (tests): masks probed capabilities so a GPU without them can be simulated on any GPU.
    Keys `nv12_srv`, `p010_srv` (decoder pool without `SHADER_RESOURCE`; `decoder_srv=0` = both), `p010` (the video
    processor takes no P010), `rotate`, `mirror`, `procamp`, `shader`, `cuda_import`. The mask applies to the open's
    presenter (limits and routes) and to `ffp_probe_caps_opts`; each masked key is a `force_caps:<key>=0` fallback.
  * `d3d11_route=vp|shader` (tests): forces the presenter route per frame. A frame that cannot take it (the shader is
    unavailable, the VP cannot do the settings or the format) takes the normal route and records
    `route_forced_unavailable:<route>`; otherwise `route_forced:<route>`.
  * `presenter_debug_srv_fail=N` (tests only): the first N decoder pools that request `SHADER_RESOURCE` fail as if the
    driver rejected the flag (see "Decoder pool without SRV").
* `ffp_stop` closes media and clears the window to black. Volume (`0..100`) and mute persist
  across opens. Position is in seconds (`NAN` when unknown); duration `<= 0` means unknown/live.
* `ffp_set_video_filter` / `ffp_set_audio_filter` store filter strings across opens;
  `NULL` or `""` clears a filter. An open's `vf` / `af` replaces the stored string. The decode
  thread rebuilds the graph at the next frame; on failure FILTER_ERROR is raised and the previous
  string is restored.
* `filter_hw_device=type[:device]` (e.g. `d3d11va`, `cuda`) creates a hardware
  device for that playback and attaches it to the video filters. No device is created unless the
  option is given. Device creation is lazy, when the graph is first built;
  failure follows the filter-error/restore path above.
* `ffp_filter_command` queues `avfilter_graph_send_command` before the next frame; prefix a target
  with `a:` for the audio graph. `0` means queued, failures arrive as FILTER_ERROR, and an optional
  response buffer receives `queued`. Commands do not update the stored filter string: the host must
  keep it in sync.
* `ffp_snapshot` returns the last displayed, post-filter video frame as allocated BGRA (A=255),
  before window scaling. The filtered size is stretched by sample aspect ratio: width is
  `round(w*sar)` for `sar>1`, height is `round(h/sar)` for `sar<1`, rounded to even dimensions.
  Presenter transforms also apply, as described below. Free pixels with `ffp_free`; no displayed
  frame returns `FFP_EAGAIN`. `ffp_snapshot_ex` optionally includes visible bitmap subtitle layers.

### Seeking and frame stepping

`ffp_seek(p, seconds, flags)` is asynchronous; use `FFP_SEEK_ABSOLUTE` or `FFP_SEEK_RELATIVE`. Absolute seconds
are relative to the media start; timed media uses timestamp seeks rather than bitrate-based byte
estimates. Relative requests during an in-flight seek accumulate from the requested timeline;
absolute requests replace the pending target. For video media the landing is keyframe-only and exact: the read thread
finds the video **key packet closest to the target time** (the last key packet at or before it and the first one after it,
the nearer wins, the earlier on a tie) with the key packet scan described under `ffp_seek_keyframe`, seeks the demuxer to
just before it and drops video packets up to it, so the first presented frame is that key packet (nothing is decoded and
thrown away to reach the exact time). A relative seek also obeys the direction rule: a backward one lands before the
displayed frame and a forward one after it, so when the nearest key packet is on the wrong side the nearest one on the
right side is taken (with none, or when the scan is unavailable, the previous planning of the seek is used). The same
applies to the seek bar (absolute) and to the `ss` open option (resume position), which is resolved with the playing
demuxer before the first packet is queued. Audio-only media, cover art, live and non-seekable sources keep the plain
timestamp seek. Read POSITION / `ffp_get_position` for the displayed result.

`ffp_step_frame(p, direction)` accepts `+1` for the next or `-1` for the preceding **output**
video frame, after the complete filter graph. Both directions pause and remain paused. Call after PREPARED.
One step is in flight at a time and requests are never buffered: `0` means accepted, `FFP_EBUSY` (-16) means a
step is still in flight and the request was dropped, invalid arguments return `FFP_EINVAL`. A step ends with an
immediate POSITION event followed by STEP_DONE `{"direction":1,"moved":1}` (`moved` is `0` at the start/EOF when no
adjacent frame exists, or when there is no video); `ffp_get_position` then reports the displayed output-frame
position. A new step is accepted from the moment STEP_DONE is raised; a held key is therefore handled by the host
starting the next step from STEP_DONE while the key is still down, with nothing left to cancel on key release.

`ffp_seek_keyframe(p, direction)` seeks to the previous (`-1`) or next (`+1`) **keyframe** of the video stream,
relative to the currently displayed frame, while playing or paused. Previous is the last key packet whose PTS is strictly
before the displayed frame's source PTS (on a keyframe, the one before it, so repeated calls walk back through the GOPs);
next is the first key packet strictly after it. The landing is exact: the key packets of the video stream are read with a
private demuxer from a few GOPs before the frame (previous widens backwards until one is seen) up to the first key packet
past it, then the playing demuxer is seeked to just before that packet and its leading video packets are dropped, so the
decoder starts on the key packet and decoded frames in front of it (open-GOP leading B pictures) are discarded. A keyframe-only
policy: nothing is decoded and thrown away to reach an exact time. Audio follows the video through the normal post-seek trim
(and the ARIB audio model). A step back (`-1`) **pauses** like `ffp_step_frame` (from playing too, silent) and stays paused on the landing, so
repeated taps walk back one keyframe each however far apart they are; a step forward (`+1`) keeps the play state (playing
continues from the key packet, paused presents it and stays paused). At the first / last
keyframe, with nothing displayed yet, no video, a live or non-seekable source, or while another seek or step is in flight,
nothing moves and `moved` is `0` (the playing demuxer is not touched). The request shares the one-at-a-time slot of
`ffp_step_frame`: `0` accepted, `FFP_EBUSY` while a step or keyframe seek is in flight (dropped, never queued), `FFP_EINVAL`
for invalid arguments; it is accepted again from the moment KEYSEEK_DONE is raised, so a held key is handled by the host
starting the next one from KEYSEEK_DONE while the key is still down. With IVTC / deinterlacing the landing is still the
source keyframe; the frame shown is the first one the filter graph outputs for the new playback serial (fieldmatch /
decimate need a few input frames, so it can be a frame or two after the keyframe), `position` reports that output frame
and `key` the source PTS it carries, which is also what the next request is relative to while it is displayed.

`ffp_skip(p, seconds)` is the timed relative seek with the keyframe step's completion model, for held keys: it lands on the
video key packet closest to the displayed frame + `seconds` (negative = back), with the direction rule above (a backward
skip lands before the displayed frame, a forward one after it; with no qualifying key packet, e.g. a skip back at the first
key or forward past the last, nothing moves). It takes the same one-at-a-time slot as `ffp_step_frame` /
`ffp_seek_keyframe` (`FFP_EBUSY`, never queued), completes with POSITION and KEYSEEK_DONE, and keeps the play state. Without
a seekable video stream (audio only, cover art, live) it queues a plain relative `ffp_seek` and reports `moved` `0`.

## Event payloads (JSON, UTF-8)

| Code | Name | Payload |
|---|---|---|
| 1 | PREPARED | `{"duration":12.5,"seekable":true,"has_video":true,"has_audio":true,"width":1920,"height":1080,"sar_num":1,"sar_den":1,"title":"..."}` (duration `0` for live) |
| 2 | POSITION | `{"position":3.25,"duration":12.5}` — about 4 Hz while media is open (also while paused) |
| 3 | STATE | `{"state":"opening"}` — one of `idle`, `opening`, `playing`, `paused`, `stopped`, `ended`, `error` |
| 4 | BUFFERING | `{"buffering":true}` / `{"buffering":false}` |
| 5 | EOF | `{}` — end of media reached (state becomes `ended`; no looping unless the `loop` option is set) |
| 6 | ERROR | `{"code":-5,"message":"Input/output error","url":"..."}` (state becomes `error`). When the video decoder failed because its D3D11 device was removed the json also has `"device_lost":true,"removed_reason":"0x887a0005"` |
| 7 | INPUT | see below (`{"type":"close"}` is additionally raised with `hwnd == NULL` when the user closes the window or presses q/Esc; ffplay.exe uses it) |
| 8 | FILTER_ERROR | `{"type":"video","filter":"<string that failed>","code":-22,"message":"Invalid argument"}` (`type` is `video` or `audio`) |
| 9 | PRESENTER_ERROR | `{"presenter":"d3d11","error":"<reason>","fallback":"sdl"}` (the presenter failed; playback continues with the SDL presenter, see "presenters"). A lost GPU device adds `"device_lost":true,"removed_reason":"0x887a0005"` |
| 10 | STEP_DONE | `{"direction":1,"moved":1}` (the `ffp_step_frame` request finished, after its POSITION event; `moved` is `0` when no frame was presented) |
| 11 | KEYSEEK_DONE | `{"direction":-1,"moved":1,"position":12.480,"key":12.480}` (the `ffp_seek_keyframe` / `ffp_skip` request finished, after its POSITION event; `position` is the presented output frame, `key` the source PTS of the decoder frame it came from, both media-relative seconds; both `-1` and `moved` `0` when nothing moved) |

Order on a successful open: `STATE opening` -> `PREPARED` -> `STATE playing` -> `POSITION`...
`ffp_stop` -> `STATE stopped`. `ffp_pause`/`ffp_play` -> `STATE paused`/`playing`.

### INPUT

Mouse/keyboard events received by the video window. When hosted (`hwnd != NULL`) ffplay does
not act on them (no ffplay key bindings); the host decides. Coordinates are client pixels of
the video HWND.

```json
{"type":"move","x":10,"y":20,"buttons":0,"mods":0}
{"type":"down","x":10,"y":20,"button":1,"mods":0}
{"type":"up","x":10,"y":20,"button":1,"mods":0}
{"type":"dblclick","x":10,"y":20,"button":1,"mods":0}
{"type":"wheel","x":10,"y":20,"delta":1,"mods":0}
{"type":"keydown","vk":32,"mods":2}
{"type":"keyup","vk":32,"mods":2}
```

`button`: 1 left, 2 middle, 3 right. `buttons`: bit mask of held buttons (1 left, 2 middle, 4 right).
`delta`: +1 per notch up, -1 per notch down. `vk`: Windows virtual-key code.
`mods` bit mask: 1 = Shift, 2 = Ctrl, 4 = Alt.

With `hwnd == NULL` (own window, ffplay.exe) the classic ffplay key/mouse bindings stay
active and INPUT events are still raised.

## ffp_get_media_info JSON

```json
{
  "url": "...", "title": "...", "format": "mov,mp4,m4a,3gp,3g2,mj2",
  "duration": 12.5, "seekable": true, "bit_rate": 123456,
  "render": {"width": 1920, "height": 1080},
  "streams": [
    {"index":0,"type":"video","codec":"h264","width":1920,"height":1080,"sar_num":1,"sar_den":1,
     "fps":25.0,"pix_fmt":"yuv420p","color_transfer":"bt709","fourcc":"avc1","selected":true,"language":"und"},
    {"index":1,"type":"audio","codec":"aac","fourcc":"mp4a","sample_rate":48000,"channels":2,"selected":true,"language":"eng"},
    {"index":2,"type":"subtitle","codec":"subrip","selected":false,"language":"eng","title":"..."}
  ]
}
```

`fourcc` (optional, every stream type): 4 printable ASCII chars, the container's `codec_tag` when it has that form, else the RIFF tag
of the codec id (`avformat_get_riff_video_tags` / `avformat_get_riff_audio_tags`; video and audio only); absent when there is none.
Case is as stored (`avc1`, `mpg2`), compare case-insensitively. Video streams also carry `color_transfer`: `av_color_transfer_name` of the
stream's `color_trc` (`"bt709"`, `"smpte2084"` = PQ, `"arib-std-b67"` = HLG, `"unknown"` when unspecified).

For an ARIB transport stream the object has one more key, `"arib_audio":{"audio1":1,"audio2":-1}`: the AVStream
indexes of the two audio roles at the time of the call (`-1` = no stream holds the role now; audio 2 may appear later).
It is absent for every other input. See "ARIB audio model".

`render` is the renderer output size (SDL_GetRendererOutputSize) as last seen by the player loop, i.e. the
client size of the hosted window the picture is drawn into; `0x0` until the first loop tick. It follows host
resizes (maximize, full screen, `SetWindowPos`) within a few ms, also while paused, and is mainly a diagnostic
(the headless resize test `ResizeTests` uses it). Hosts keep sizing their child HWND themselves; libffplay
re-reads the client rect every tick, and SDL's window procedure for the hosted HWND runs on the player thread
for size messages (never on the host UI thread).

**Host contract for destroying a hosted player.** `ffp_destroy` blocks the calling (UI) thread until the player
thread has exited, and the player thread may, shortly before, feed a synthetic `WM_WINDOWPOSCHANGED` to SDL's window
procedure for your HWND. libffplay swallows that replay below SDL (it never reaches the host's window procedure),
so no cross-thread `SendMessage` to the UI thread can occur and a host that is blocked inside `ffp_destroy` will not
deadlock. Hosts must still not hold locks the player thread may need and must not call `ffp_destroy` from the event
callback. The real size messages are delivered to the host's window procedure on the UI thread as usual. Destroy
the player before destroying the HWND (libffplay restores the host's original window procedure on destroy).

## Diagnostics and logging

Debug info, FPS statistics and the log callback are part of the required core API
(`1009-libffplay-core-api.patch`). Runtime values may be unknown until media or hardware is ready.

### `ffp_get_debug_info`

Returns a JSON snapshot (free with `ffp_free`) or `NULL` when nothing is open. Thread-safe, cheap (about 4 Hz is
fine) and never blocks for long: decoder/filter facts are copies taken by the owning thread when the object is
opened/configured, live values (clocks, queues, drop counters) are read under short mutexes, and the media is
unpublished before it is torn down. **Every value is what is actually in use at runtime**, not what was
configured: the codec context that was really opened, the device that was really created, the filter graphs that
were really configured, the renderer object that was really created. Requested and actual values are reported side
by side so a host can highlight a mismatch. All keys are always present; unknown numbers are `null`, unknown
strings `""`.

```json
{
  "position": 12.34, "duration": 60.0, "paused": false, "speed": 1.0,
  "renderer": "sdl:direct3d11",
  "video": {
    "stream_index": 0, "codec": "h264", "decoder": "h264", "hwaccel": "d3d11va",
    "hwaccel_requested": "d3d11va", "hwaccel_device": "d3d11va", "hwaccel_error": "",
    "frame_path": "hw_download",
    "frame_path_counts": {"hw_direct": 0, "hw_download": 740, "software": 0},
    "pix_fmt": "d3d11", "pix_fmt_decoded": "d3d11", "pix_fmt_after_transfer": "nv12",
    "hw_pix_fmt": "d3d11", "sw_pix_fmt": "nv12",
    "width": 1920, "height": 1080, "sar": "1:1", "frame_rate": 29.97,
    "out_width": 1920, "out_height": 1080, "out_pix_fmt": "yuv420p",
    "time_base": "1/90000", "start_time": 0.0
  },
  "audio": {
    "stream_index": 1, "codec": "aac", "decoder": "aac", "sample_rate": 48000, "channels": 2,
    "channel_layout": "stereo", "sample_fmt": "fltp", "time_base": "1/48000", "start_time": 0.0,
    "out_device": "Speakers (Realtek) [wasapi]", "out_sample_rate": 48000, "out_channels": 2,
    "hw_buf_size": 8192, "diff_avg": null, "latency_ms": 42.7
  },
  "subtitle": {"stream_index": 2, "codec": "subrip", "time_base": "1/1000", "start_time": 0.0},
  "filters": {
    "vf_requested": "yadif", "af_requested": "atempo=1.5",
    "vf_graph": "buffer=video_size=1920x1080:...,yadif,scale,format,buffersink",
    "af_graph": "abuffer=...,atempo=1.5,aresample,abuffersink",
    "vf_filters": ["buffer", "yadif", "scale", "format", "buffersink"],
    "af_filters": ["abuffer", "atempo", "aresample", "abuffersink"],
    "vf_nodes": [{"name": "yadif", "instance": "Parsed_yadif_0", "args": "mode=1", "hw_device": ""}, "..."],
    "af_nodes": ["..."]
  },
  "sync": {
    "master": "audio", "audio_clock": 12.30, "video_clock": 12.31, "external_clock": 12.31, "master_clock": 12.30,
    "av_diff_ms": -8.5, "frame_drops_early": 0, "frame_drops_late": 0, "frames_duplicated": 0,
    "frame_timer_drift_ms": 0.5
  },
  "fps": {
    "stream_avg": 29.97, "stream_r": 29.97,
    "filter_out": 23.976, "filter_out_source": "sink_frame_rate", "filter_out_time_base": "1/24000",
    "decoded": 29.9, "filtered": 23.9, "displayed": 23.9,
    "frames_decoded": 300, "frames_filtered": 240, "frames_displayed": 236,
    "drops_early": 0, "drops_late": 0
  },
  "queues": {
    "audio":    {"packets": 12, "bytes": 34567, "duration": 0.48},
    "video":    {"packets": 25, "bytes": 912345, "duration": 1.00},
    "subtitle": {"packets": 0, "bytes": 0, "duration": 0.0}
  }
}
```

`video`/`audio`/`subtitle` are `null` when that stream is not open. Field notes:

* `decoder` is `avctx->codec->name` of the opened decoder (`h264_cuvid` vs native `h264`), `codec` the codec id name.
* `hwaccel_requested` is the `hwaccel` open option (`""` if none). `hwaccel_device` / `hwaccel` is the type of the
  device that is actually used for decoding, or `"none"` when it could not be created, the decoder has no hwaccel
  for it, or the first decoded frames turned out to be software frames; `hwaccel_error` then says why. With the
  SDL presenter, the default path downloads hardware frames (`hw_download`); `hwframes=keep` can
  retain them through filters. The D3D11 presenter consumes supported D3D11/CUDA frames directly,
  and the Vulkan renderer (own window only) can also consume hardware frames directly (`hw_direct`).
* `hwaccel` open option values: a device type name (`d3d11va`, `d3d12va`, `cuda`, `dxva2`, ...) or `auto`. `auto` tries
  `d3d11va`, `d3d12va`, `cuda`, `dxva2` in that order and uses the first type whose device can be created and for which a
  decoder of the stream's codec exists; `hwaccel_device` is the type that was picked, and when none works `hwaccel_error`
  says `auto: no usable hardware decoder ...` and decoding is software. When the default decoder of a codec is software only
  (libdav1d for AV1) the native decoder with a hwaccel (`av1`) is used instead, but only for `d3d11va` and only after the
  GPU's D3D11 video decoder profile (+NV12/P010 output) was verified, because that decoder cannot decode in software.
  An explicit `-codec`/`vcodec` choice is never replaced.
* `frame_path` is the path of the last decoded frame (`hw_direct`, `hw_download`, `software`, `""` before the first
  frame); `frame_path_counts` counts frames per path since the stream was opened, so a mid-stream fallback is visible.
  `pix_fmt` is the decoder context's negotiated format, `pix_fmt_decoded` the format of the last decoded frame
  (`d3d11` for hardware surfaces), `pix_fmt_after_transfer` the system-memory format after a download (`""` if none;
  taken from the frames context's software format), `hw_pix_fmt`/`sw_pix_fmt` the hardware surface format and its
  software format (`""` for software decoding).
* `renderer`: `"vulkan"`, `"sdl:<SDL renderer name>"`, `"d3d11:videoprocessor"`, `"d3d11:shader"`
  or `"d3d11:shader-cuda"` for the renderer/route actually used (`""` if none).
* `vf_graph`/`af_graph`: `name=args` of every filter of the configured graph, in dataflow order (a filter after all
  filters feeding it), including filters the graph inserted itself (`scale`, `format`, `aresample`, ...). Args are
  the non-default options (`av_opt_serialize`); the sinks have no args. `*_nodes` add the instance name and the type of
  the hardware device attached to the filter (`""` if none). They are empty until the first frame has configured the
  graph. `vf_requested`/`af_requested` are the strings last set on the instance, so a mismatch (rejected filter,
  not yet applied) is visible. `speed` is the product of the `atempo` filters in the audio graph (1.0 if none).
* `sync.av_diff_ms` is the `A-V:` value of the status line (audio clock minus video clock, or master minus the only
  stream), in ms. `frame_timer_drift_ms` is how far the video frame timer lags behind real time (positive = late).
  `frames_duplicated` is always 0 (ffplay does not count duplicates). `audio.diff_avg` is the running average
  A-V correction ffplay keeps only when audio is not the master, otherwise `null`. `audio.latency_ms` is
  `(2 * hw_buf_size + bytes_left_in_buffer) / bytes_per_sec`, the estimate ffplay subtracts from the audio clock.
* `fps` reports output frame rates, all observed at runtime:
  `stream_avg` / `stream_r` are the stream's `avg_frame_rate` / `r_frame_rate` (`null` if unknown or no video);
  `filter_out` is the configured video graph's buffersink frame rate (`av_buffersink_get_frame_rate`, e.g. 4/5 of the
  input after `decimate`, 10 after `fps=10`), `filter_out_source` says where it came from (`sink_frame_rate`, or
  `time_base` = 1/sink time base when the sink has no rate and that is <= 1000, else `null`/`""`),
  `filter_out_time_base` is the sink time base `"a/b"`. `decoded`, `filtered` and `displayed` are measured frames per
  second over a ~1.5 s sliding window of timestamps taken on the owning threads (decoder output; frames leaving the
  filter graph; a new frame presented by `video_refresh` + `video_display`). They are `null` with fewer than two frames
  in the window or when the newest frame is older than max(0.5 s, 2 frame intervals) (paused / stalled / not started),
  so they lag a rate change by up to 1.5 s, and `decoded` can spike above the real rate while a graph is being rebuilt
  (the decoder catches up). `frames_decoded` / `frames_filtered` / `frames_displayed` are cumulative since the video
  stream was opened; `drops_early` / `drops_late` repeat `sync.frame_drops_*`. The whole object is present (values
  `null`/0) when no video stream is open.
* `pause_timing` (`1014-libffplay-playback-control.patch`) describes the last `ffp_pause` / `ffp_play` / `ffp_toggle_pause` that changed the state, in
  `av_gettime_relative()` microseconds (0 = not yet; `now_us` is the clock when the JSON was written, to align it with the
  host's clock): `want` (1 pause, 0 resume, -1 none yet), `api_us` (the API call), `apply_us` (the loop thread applied it),
  `state_us` (its STATE event was handed to the callback), `last_frame_us` / `frames_after` / `first_frame_us` (newly
  presented frames: the last one at any time, how many after the apply, the first one after it), `audio_last_end_us` /
  `audio_first_us` / `audio_after_ms` (decoded audio handed to the audio device: the end of the last chunk, the start of
  the first chunk after a resume, how much after the apply) and `audio_device_ms` (the device's in-flight estimate, two
  device buffers). Audible stop / start ~ `audio_last_end_us` / `audio_first_us` + `audio_device_ms`. A pause takes effect in
  the next audio callback with a 5 ms fade-out (the rest of the decoded frame is kept and faded in on resume); the device
  is opened with its own period (`SDL_AUDIO_ALLOW_SAMPLES_CHANGE`, ~10 ms callbacks).
* `queues.*.duration` is in seconds, `bytes` includes ffplay's per-packet list overhead (same as the `aq=`/`vq=` KB).
  Sparse caption / superimpose queues do not count towards the read-ahead "enough packets" threshold
  (`1012-libffplay-captions.patch`), so they do not force buffering up to the packet-byte cap.
* D3D11 presenter memory (`1011-libffplay-d3d11-cuda-shader.patch`): the CUDA context is created only
  for an open that uses CUDA. Reusable zero-copy pool buffers released by a rebuilt graph are parked
  (still registered, at most 12 per presenter) for the next pool, avoiding repeated D3D11/CUDA
  registration churn in the driver. They are destroyed with the presenter; retired mappings are
  freed rather than recycled.

### `ffp_set_log_callback`

`ffp_set_log_callback(cb, user)` installs a process-wide `av_log` sink. Each call of `cb` receives one UTF-8 string
formatted like the default logger (`av_log_format_line2`, with the `[context @ pointer]` prefix; it keeps its
trailing `\n` / `\r`) and the `AV_LOG_*` level of the message. Messages less severe than the level set with
`ffp_set_log_level` are not delivered (the default level is warning, so a host that wants info lines or the `stats`
line must set `ffp_set_log_level(32)`). `cb` may be called from any thread (decoder, loop, SDL audio...) and must
return quickly and not log. `cb == NULL` restores the default stderr logger; a call already running may finish
afterwards, so keep `user` valid a little longer.

### `stats` option

`ffp_open(..., "stats=1")` enables ffplay's periodic status line (`  12.31 A-V: -0.008 fd=   0 aq=   34KB vq=  890KB sq=    0B`,
every 30 ms at most). It is emitted with `av_log(AV_LOG_INFO)`, so it reaches the log callback
when the log level is info (32) or more verbose.

## Capabilities: build table + runtime probe, and `hwframes`

The planner's `wvp::CapabilitySet` is `wvp::BuildCapabilities()` merged with the runtime probe `ffp_probe_caps`
(`wvp::MergeBuildCapabilities`). What the FFmpeg/libffplay build fixes comes from the build table; only what depends on the
machine (GPU, drivers) is probed. An unavailable probe result is distinct from a successful probe reporting that an
individual device or operation is unsupported.

### Build table (`BuildCapabilities`)

`ffcapsdump.exe` (`1015-libffplay-static-caps.patch`) is built with FFmpeg and installed in `ffmpeg/dist/win64`; it is not
shipped. At app build, `src/Core/CMakeLists.txt` runs it to generate `ffmpeg-caps.inc` (X-macro calls) in the build tree,
which `src/Core/src/BuildCapabilities.cpp` compiles into PlayerCore:

```c
FFCAPS_VERSION("8.1.3-Jellyfin")
FFCAPS_FEATURE("hwframes_keep")
FFCAPS_DECODER("h264", "cuda", "h264")
FFCAPS_FILTER("ivtc_cuda", "list", ("cuda"), 1, ("tff", "cthresh", "dupthresh", "scthresh", "verbose"), 1)
```

`ffcapsdump` reads only static FFmpeg tables and creates no device, so the output is the same on every machine. It is the
only place that uses the libavfilter-internal `FFFilter` (`libavfilter/filters.h`); libffplay itself does not.

* `FFCAPS_FEATURE`: libffplay features of the build: `hwframes_keep`, `presenter_params`, `upload_hw_sink`.
  `upload_hw_sink` (`1010-libffplay-presenters.patch`) permits a `vf` that uploads system frames (`hwupload` on
  `filter_hw_device`) to end in a hardware format; the D3D11 presenter consumes supported frames directly (a `cuda` /
  `d3d11va` filter device without a device index uses the presenter's device), unsupported hardware formats are
  downloaded after the sink.
* `FFCAPS_DECODER(codec, hwaccel, decoder)`: for the codecs h264, hevc, mpeg2video, vp9, av1, vc1, mpeg4 and each hwaccel
  type (d3d11va, cuda, d3d12va, dxva2) of the build: the decoder that carries the hwaccel (the default decoder when it has
  a `AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX` config for the type, else another decoder of that codec, e.g. native `av1`
  instead of libdav1d), `""` when none.
* `FFCAPS_FILTER(name, formats, pix_fmts, options known, options, hw_device)`: every filter that has a video input (or
  dynamic inputs and a video output; sources and sinks are skipped). `formats`: `"list"` (fixed pixel format list),
  `"any"` (passes every format through) or `"dynamic"` (negotiated by a query function, `pix_fmts` empty). `hw_device`:
  `AVFILTER_FLAG_HWDEVICE` (needs a filter hw device, `filter_hw_device` option). The options (names of the filter's
  private AVOptions, constants excluded) are known only for hardware filters (hw pixel format, `AVFILTER_FLAG_HWDEVICE`,
  or a `_cuda/_opencl/_qsv/_vaapi/_vulkan/_npp/_d3d11/_amf/_vpp` name suffix) and for fieldmatch, decimate, bwdif, yadif,
  transpose, hflip, vflip, eq, setpts, null, scale, format, hwupload, hwdownload, hwmap, hwupload_cuda, tonemap, fps,
  pullup; for other filters they are unknown.

### `ffp_probe_caps` / `ffp_probe_caps_opts`

* `char *ffp_probe_caps(void)`: runs the probe once per process (blocking, typically 0.5-2 s: creates hardware devices,
  checks hardware decode support, maps 64x64 test frames) and returns the cached result as a UTF-8 JSON copy (free with
  `ffp_free`); later calls return the cached copy immediately. Needs no `FFPlayer`, creates no window, runs on the
  calling thread (call it from a worker thread). `av_log` output is suppressed while it runs (probe failures are results,
  not log lines). NULL only on out-of-memory. The app runs it in the `--probe-gpu` child process.
* `char *ffp_probe_caps_opts(const char *options_kv)`: the same with options (`d3d11_adapter_luid`, `d3d11_force_caps`,
  `d3d11_debug`, same syntax as `ffp_open`; other keys are ignored). `ffp_probe_caps()` is `ffp_probe_caps_opts(NULL)`. The
  probe still runs once per process and **the options of the first call win**; a bad option value returns
  `{"schema":1,"source":"probe","error":"..."}` without counting as the probe. With `d3d11_adapter_luid` the d3d11va
  and cuda devices are created on that GPU (the other device types keep their default GPU) and `adapter` is that GPU;
  an unknown LUID gives an empty `adapter`, `d3d11va` / the d3d11 presenter `ok:false` and a `cuda` error that names the LUID
  (as does a GPU without a CUDA device with that LUID). With `d3d11_debug=1` `av_log` is not silenced, so
  `d3d11-debug:` lines of the probe's devices come through.

The JSON is parsed by `wvp::ParseCapabilities` (`src/Core/include/wvp/CapabilitySet.h`); every member is optional there.
Written by the probe:

```json
{
  "schema": 1, "source": "probe", "ffmpeg": "8.1.3-Jellyfin",
  "adapter": {"name": "NVIDIA GeForce RTX 5070", "vendor_id": 4318, "luid": "00000000:000188ca", "driver": "32.0.15.9186"},
  "features": ["presenter_d3d11"],
  "devices": [{"type": "cuda", "ok": true, "name": "NVIDIA GeForce RTX 5070", "error": ""},
              {"type": "qsv", "ok": false, "name": "", "error": "Unknown error occurred"}, "..."],
  "decoders": [{"codec": "h264", "hwaccel": "cuda", "decoder": "h264", "ok": true, "bit_depths": [8, 10],
                "max_width": 8192, "max_height": 8192, "error": ""}, "..."],
  "interop": [{"from": "d3d11", "to": "qsv", "method": "map", "ok": false,
               "error": "d3d11 to qsv mapping not usable: No such device"}, "..."],
  "presenters": [{"name":"sdl","ok":true,"error":"","inputs":["system"]},
                 {"name":"d3d11","ok":true,"error":"","inputs":["d3d11","system","cuda"],
                  "vp":{"rotate":true,"mirror":false,"procamp":true,"p010":true,"brightness":[-34.0,34.0],"..":".."},
                  "decoder_srv":{"nv12":true,"p010":true},
                  "shader":true,"import":{"cuda":true,"cuda_zero_copy":true}}]
}
```

* `adapter`: the DXGI adapter of the d3d11va device (adapter 0 when no d3d11va device); `name` is the DXGI
  description (UTF-8), `luid` is `"%08lx:%08lx"` (HighPart:LowPart), `driver` the user mode driver version
  `a.b.c.d` from `IDXGIAdapter::CheckInterfaceSupport(IDXGIDevice)`; fields are empty/0 when DXGI fails.
* `features`: `presenter_d3d11` when the D3D11 presenter's runtime probe succeeds. The planner must still consult the
  presenter/import probe results before selecting a route.
* `devices`: one entry per device type known to this FFmpeg build, in the order cuda, d3d11va, d3d12va, dxva2, qsv,
  vulkan, from a real `av_hwdevice_ctx_create` (`ok`/`error`). `name`: adapter description (cuda, d3d11va,
  d3d12va, dxva2); empty for qsv/vulkan.
* `decoders`: as in the table, for each hwaccel type whose device is ok, plus the hardware result: `ok` = such a decoder
  exists and the hardware supports it. d3d11va: `ok` needs the 8-bit decoder profile (NV12) via
  `ID3D11VideoDevice::CheckVideoDecoderFormat`; `bit_depths` lists 8 (NV12) and/or 10 (P010; hevc Main10, vp9 profile 2,
  av1) that the GPU supports. cuda: `cuvidGetDecoderCaps` (ffnvcodec) for 4:2:0 8/10 bit gives `bit_depths`,
  `max_width`, `max_height` (largest over the supported depths). d3d12va/dxva2: `ok` = decoder exists (not verified
  against the GPU), `bit_depths` empty. When no decoder exists `ok` is false and `error` says why; the probe never
  throws, any failure is `ok:false` + `error`. The merge keeps a probed decoder only when the table names the same
  decoder for its `(codec, hwaccel)`.
* `interop`: a real 64x64 test per pair (device derived from the source device, frames context derived,
  `av_hwframe_map`): `d3d11 -> qsv`; `ok:false` + error when a device is
  missing or the mapping fails.
* `presenters`: SDL accepts system frames; the D3D11 entry reports `ok`/`error`, accepted inputs,
  CUDA import support, video-processor limits (`vp`, with `p010`: the video processor takes P010 input) and shader
  availability. `decoder_srv` `{"nv12":bool,"p010":bool}`: a decoder pool of that format takes
  `DECODER|SHADER_RESOURCE`, probed separately per format with a texture array that has the misc flags of a real pool
  (`SHARED`) and the plane formats the shader reads. With `d3d11_force_caps` the masked capabilities read `false` and the
  entry gets `"forced":["p010=0",..]`. See the presenter section
  below. Capability examples are illustrative; do not assume a particular adapter's results.

For NVDEC the probe reads the first fields of `AVCUDADeviceContext`, which ties it to this FFmpeg build.

### `hwframes` open option

`ffp_open(..., "hwframes=keep")` retains decoded hardware frames. SDL defaults to `download`;
D3D11 defaults to `keep` with D3D11VA/CUDA, unless explicitly overridden. Unknown values behave
like `download`. With a `hwaccel` and `keep`, a hardware decoded frame is **not** downloaded in
the decode thread: it enters the video filter graph as a hardware frame (the `buffer` source gets
the frame's `hw_frames_ctx`), so `hwaccel=cuda` with `vf=ivtc_cuda,hwdownload,format=nv12` runs the
filters on the GPU. The buffersink accepts hardware formats (the decoder's own, cuda, d3d11,
opencl, dxva2_vld, qsv) besides SDL texture formats. A hardware frame leaving the sink is downloaded
with `av_hwframe_transfer_data` only when the presenter cannot consume it directly. Extra decoder
surfaces cover graph/queue retention (`extra_hw_frames = 4`, or `8` with the D3D11 presenter).

Fallback to `download` for the rest of the stream (same stream, no reopen):

* `vf` is empty (or becomes empty) and the presenter cannot consume the hardware frame directly:
  frames are downloaded at decode time. Supported D3D11/CUDA frames stay on the direct D3D11 path.
* configuring the graph with hardware input fails (e.g. `vf=bwdif_cuda` on d3d11 frames, a software-only filter on a
  hardware frame): the vf string is **kept**, a warning is logged and `FFP_EVENT_FILTER_ERROR` is raised with
  `{"type":"video","filter":"<vf>","code":-40,"message":"...","hwframes_fallback":true,"error":"..."}`; the frame is
  downloaded and the same string is configured again with software frames (if that also fails, the usual FILTER_ERROR +
  restore of the last good string follows).

`keep` only matters when the decoder outputs a hardware format: `hwaccel=cuda` (nvdec) gives `AV_PIX_FMT_CUDA`,
`hwaccel=d3d11va` gives `AV_PIX_FMT_D3D11`.

Debug info (`ffp_get_debug_info`, keys in `video`):

| key | value |
|---|---|
| `hwframes_requested` | `"keep"` / `"download"`: the open option |
| `hwframes` | `"keep"` when the last decoded frame went into the graph as a hardware frame (`frame_path` = `hw_direct`), else `"download"` |
| `frame_path_counts.sink_download` | frames downloaded after the sink (a keep-mode graph that still ends in a hardware format) |

`frame_path`: `hw_direct` = a hardware frame retained for the graph/presenter, `hw_download` =
downloaded at decode time, `software` = software-decoded frame. A frame that triggers the hardware
filter fallback is re-counted from `hw_direct` to `hw_download`. Use the sink-download and presenter
counters too: `hw_direct` alone does not prove that the entire pipeline has no host copy.

## Presenters

`1010-libffplay-presenters.patch` provides presenter selection, parameters, event 9 and the
D3D11 VideoProcessor path; `1011-libffplay-d3d11-cuda-shader.patch` supplies CUDA import and the
D3D11VA/system shader route. The required API includes `ffp_set_presenter_params`.

A presenter shows the pictures of the video queue. Open option `presenter=sdl|d3d11` defaults to
`sdl`; the standalone Vulkan renderer is separate. Frames the presenter cannot consume are
converted or downloaded.

**`presenter=d3d11`** (needs a hosted window: `ffp_create` with an HWND):

* **Window**: a *child window* of the host HWND fills the host's client area and follows its size (`WS_CHILD`,
  `WS_EX_NOPARENTNOTIFY`; shown after the first presented picture, destroyed when the media is closed). The flip-model DXGI swap
  chain (`FLIP_DISCARD`, 3 buffers, vsync `Present(1,0)`) lives on that child, so nothing fights SDL's own swap chain on the host
  window: the SDL renderer stays untouched underneath (it is the fallback, and it draws the audio visualisation of audio-only
  media). Mouse messages that reach the child are forwarded to the host window (same client coordinates).
  Fallback destroys the child without rebuilding the host window's procedure chain; the app's
  DirectComposition UI remains above the video HWND. No top-level window is created or shown.
* **Shared device**: the presenter owns an `ID3D11Device` (video support, BGRA, multithread protected). With `hwaccel=d3d11va`
  (or `auto` choosing `d3d11va`) the decoder's `AVHWDeviceContext` *wraps that device* (`av_hwdevice_ctx_alloc` +
  `AVD3D11VADeviceContext.device` + `av_hwdevice_ctx_init`; ffmpeg's recursive lock callbacks protect the context), so decoded
  D3D11 NV12 / P010 frames are shown **without any copy**. With `presenter=d3d11` and a D3D11VA device `hwframes` defaults to
  `keep` (`hwframes=download` still forces a download at decode time). A frame the presenter shows as is is not downloaded, also
  with an **empty `vf`** and also after the filter graph (the buffersink pix_fmts include `d3d11`): this extends the rule of
  `hwframes=keep` - a hardware frame is only downloaded when the presenter cannot show it. CUDA NV12/P010 frames on the presenter adapter are imported with zero host copies via a shader (pool allocation -> raw buffer SRV, or a GPU-only two-plane fallback copy); other hardware domains (d3d12, ...) are downloaded after the sink and uploaded. The decoder gets
  `extra_hw_frames = 8` (frame queue + snapshot).
* **Device description**: the presenter's `AVHWDeviceContext` is built around its own `ID3D11Device`, so unlike a device made by
  `d3d11va_device_create` it used to start with a zeroed `device_desc` / `is_uma`; libffplay now fills both from the DXGI adapter
  (`ffp_wrap_hwdevice`), which switches on the vendor workarounds of `libavcodec/dxva2.c` (`ff_dxva2_common_frame_params`):
  **Intel (VendorId 0x8086)**: the decoder surface alignment becomes 32 (instead of 16, or 128 for HEVC / AV1; 32 is also what
  the MSDK implementation uses, which avoids resizing when mapping to QSV) and `AVD3D11VAFramesContext.require_sync = 1` (only
  read by the OpenCL mapping); **AMD (0x1002)**: `require_sync = 1` when the decoder wants an array of textures (also only the
  OpenCL mapping). **NVIDIA (0x10de) and everything else: no branch, no change** - the array-of-textures choice itself comes
  from the decoder configuration (`ConfigDecoderSpecific`), not from the vendor. The vendor, device id and name are logged
  at verbose level (`d3d11 device: vendor 10de device 2f04 (..)`).
* **Decoder pool without SRV** (`decoder_bind_flags` of `1007`): the presenter asks for `DECODER|SHADER_RESOURCE`
  (`AVD3D11VADeviceContext.decoder_bind_flags` for NV12 pools, `decoder_bind_flags_p010` for P010 / P012 pools) only for a
  format whose probe (`decoder_srv`) accepted it. If the driver still rejects the combination at the real frame size,
  `hwcontext_d3d11va` creates the pool again without the extra bits (warning "retrying without 0x8",
  `AVD3D11VADeviceContext.decoder_bind_fallbacks` counts it): the hwaccel keeps working, the shader route makes one device-local
  slice copy (`frames_d3d11_copied`) and the presenter lists `decoder_pool_no_srv` in its fallbacks. Test hook
  `presenter_debug_srv_fail=N` fails the first N such pool creations.
* **View cache**: the VideoProcessor input views are cached per (texture, slice) - 64 entries (a decoder pool has 25-33
  surfaces; with an array of textures, as AMD drivers choose, every surface is its own texture) with least-recently-used
  eviction, so a pool does not recreate its views constantly.
* **P010 without VP support**: when the video processor takes no P010 input (probe, or `d3d11_force_caps=p010=0`) D3D11 P010
  frames use the pixel shader route (`p010_vp_unsupported:shader`); with no shader route they are downloaded and converted
  to BGRA like a system frame (`p010_vp_unsupported:download`; system 10-bit frames take that conversion too). Never a presenter
  error or an SDL fallback.
* **Other D3D11 frame formats**: a D3D11 frame that is not NV12 / P010 (an Intel HEVC 4:2:2 / 4:4:4 decode gives YUY2 /
  Y210 / VUYX / Y410 ..) is downloaded (before the presenter in the player, or by the presenter itself when it is handed one)
  and takes the system-frame path (swscale to NV12 / P010 / BGRA, upload): fallback `format_download:<pix_fmt>`. 4:2:0 paths
  are untouched.
* **System frames** are uploaded once: NV12 / P010 / BGRA directly, other 4:2:0 YUV through swscale to NV12 (8 bit) or P010
  (9-16 bit), everything else through swscale to BGRA, into an `AVHWFramesContext` D3D11 surface pool (staging -> default).
* **Display**: `ID3D11VideoProcessor::VideoProcessorBlt` from the frame's texture-array slice (input view) to the back buffer.
  Colour space from the frame (`VideoProcessorSetStreamColorSpace1` with `DXGI_COLOR_SPACE_YCBCR_{STUDIO,FULL}_G22_LEFT_P{601,709,2020}`,
  PQ -> `G2084`; **untagged frames are tagged after decode** as BT.709 for height > 576 and BT.601 for height <= 576; SDL, VP, CUDA shader and snapshots use the tag),
  output `RGB_FULL_G22_NONE_P709`. Source rectangle = the frame's crop (`crop_left/top`, `width/height`), destination rectangle =
  the aspect-correct letterbox with the SAR (same rounding as the SDL path; the SAR is inverted for 90/270 degrees like the
  `transpose` filter does), black background. HDR is not tone mapped; deinterlacing is not applied
  (frames are presented as progressive). Bitmap subtitle layers are composited upright after either
  video route; text/ASS rectangles are not drawn by D3D11 and require a burn-in filter or another output path.
* **Rotation / mirror / procamp** (`ffp_set_presenter_params`): `VideoProcessorSetStreamRotation`, `VideoProcessorSetStreamMirror`
  (an `ID3D11VideoContext1` method, not `2`) and the stream filters brightness / contrast / saturation. Rotation and mirroring
  are **verified by a self test at the first use** (a 64x32 R/G/B/W picture rendered with each setting and read back; it also
  decides whether the driver mirrors before or after it rotates), and a driver that does not really do it is reported as not
  supporting it. On the development machine (NVIDIA GeForce RTX 5070, driver 32.0.15.9186) rotation works and mirroring does
  **not** (feature bit unset; no effect with the bit ignored; reversed destination rectangles do not mirror either), so
  the VP cannot flip there; these settings select the D3D11 shader instead of requiring graph-side `hflip` / `vflip`.
* **eq mapping** (ffmpeg `eq` semantics -> driver levels; ranges from `GetVideoProcessorFilterRange`, the brightness filter is
  measured with a flat NV12 gray picture at the first use):
  * saturation: level = `s * Default` (NVIDIA: range 13..187, default 100 -> ffmpeg 0.13..1.87);
  * contrast: the video processor scales about black, eq about mid grey (luma code 128 = RGB 130.4): level = `c * Default`
    (NVIDIA 40..160 -> 0.4..1.6) plus an offset `130.4 * (1 - c)` RGB codes that is added through the brightness filter;
  * brightness: eq adds `b * 255` to the luma code = `b * 297` RGB codes (`255*255/219`); the driver's brightness filter moves
    RGB by `bri_k` codes per level (measured: 1.0 on NVIDIA, range -101..101) and acts after the contrast scaling (measured; a
    driver that acts before it gets the level divided by the contrast): level = `(b * 297 + 130.4 * (1 - c)) / bri_k`.
  The honoured brightness range in the caps entry is for contrast 1 (NVIDIA: -0.34..0.34, as `[-34.0, 34.0]` in the caps JSON);
  a contrast other than 1 uses part of it. **gamma is not supported by the VP** (`!= 1` selects the shader for D3D11VA/system/CUDA frames).
* **Snapshot** (`ffp_snapshot`): the retained frame uses the selected VP/shader route into an offscreen BGRA texture of the
  documented size - native visible size, **rotation applied** (90 / 270 swap width and height before the SAR stretch, the SAR
  inverted like `transpose` does), stretched by the SAR (width `round(w*sar)` if sar > 1, height `round(h/sar)` if sar < 1,
  both rounded to even) - with the rotation / flip / eq of the presenter parameters applied, read back through a staging
  texture (A = 255). Hardware frames work without a download. Fallback to the swscale snapshot when the presenter is gone.

### `ffp_set_presenter_params(json)` / `presenter_params=<json>`

```json
{"rotation":90,"flip_h":false,"flip_v":false,"eq":{"brightness":0.1,"contrast":1.2,"saturation":1,"gamma":1}}
```

`rotation` is clockwise (0/90/180/270; -90 and 360 are normalised), the flips are applied **after** the rotation (the filter
chain `transpose` -> `hflip` / `vflip`), `eq` has ffmpeg `eq` semantics (brightness -1..1, contrast, saturation 0..3, gamma);
`"eq":null` switches eq off. The object replaces all parameters (absent members are identity); unknown members are ignored.
Applied from the next displayed frame (the current picture is redrawn when paused; `ffp_snapshot` uses them at once) and kept
across `ffp_open` / `ffp_stop` until changed. `presenter_params=` as an open option is applied together with that open and
checked against the presenter requested by the same open. Returns `0`, or `FFP_EINVAL` **without changing anything** for bad JSON,
a rotation that is not a multiple of 90, eq values outside the limits above, or a value the presenter in effect cannot apply.
For D3D11VA, uploaded system and CUDA shader frames, flips, gamma, and eq outside VP limits are accepted. The VP is used per frame when the entire setting fits its capabilities; otherwise a pixel shader applies the same settings regardless of hardware/software input. Validation is independent of whether CUDA was requested. With the SDL presenter only identity parameters are accepted: the host
keeps rotation / flip / eq in the filter graph. "In effect" is the presenter of the last `ffp_open` (SDL before the first one, and
after a fallback to SDL). The first check against the d3d11 limits probes the video processor (tens of ms).

### Failure handling: event 9

`FFP_EVENT_PRESENTER_ERROR`: `{"presenter":"d3d11","error":"<reason>","fallback":"sdl"}`. When the failure is a lost GPU
device (`DXGI_ERROR_DEVICE_REMOVED` / `DEVICE_RESET` / `DEVICE_HUNG` / `DRIVER_INTERNAL_ERROR`, or
`ID3D11Device::GetDeviceRemovedReason() != S_OK` at the failure) the payload also has `"device_lost":true,"removed_reason":"0x887a0005"`
(the HRESULT): the host should open again on a new device instead of blacklisting d3d11. A video decoder that fails because the
shared device was removed ends the open with `FFP_EVENT_ERROR` carrying the same two members (the decode loop checks
`GetDeviceRemovedReason()` of the decoder's d3d11va device; the playback state becomes `error`). `ffp_stop` / `ffp_open` release
the removed device (presenter, video processor, views and decoder references all go with the media; no device, VP or view state
is cached across opens - only plain probe results are, keyed by adapter LUID) and the next open creates a new device on the
`d3d11_adapter_luid` it is given (the DXGI factory is created anew for every lookup). Raised when the presenter cannot be set
up (device / video processor / swap chain / no hosted window; before PREPARED) or fails later (device removed or reset,
`VideoProcessorBlt`, `Present`, upload errors). The player switches to the SDL presenter for the rest of that open, hardware frames
are downloaded from then on and playback goes on. The host should put rotation / flip / eq back into the filter graph (SDL does
not apply the presenter parameters). Test hook: open option `presenter_debug_fail=display|init|device_lost` (also `1` = `display`) makes the
presenter fail at its first display / at creation / report a device loss (`device_lost:true`,
`removed_reason` `0x887a0005`) at its first display; **tests only**.

### Debug info and caps

* `ffp_get_debug_info`: `"renderer"` is `"d3d11:videoprocessor"` for VP, `"d3d11:shader"` for D3D11VA/system shader, or `"d3d11:shader-cuda"` for CUDA frames (`vulkan` / `sdl:<name>` otherwise);
  `"presenter":{"name":"d3d11"|"sdl" (in use),"requested":"..","params":{..},"error":"","frames_direct":N,"frames_uploaded":M}`
  (`frames_direct` counts direct D3D11 inputs and zero-copy CUDA imports; a D3D11 shader input can
  still require a device-local slice copy. `frames_uploaded` includes both system-memory uploads
  and GPU-only CUDA fallback copies; the SDL presenter counts neither). Neither aggregate alone
  measures host/device copy costs. CUDA adds `frames_cuda_zero_copy`, `frames_cuda_copied`
  (one GPU-only two-plane transfer per frame), and
  `frames_cuda_readdressed` (registered buffers retired after remap), `frames_dropped_readdress` (retained frames skipped because their plane pointers are stale),
  `route` (`vp` or `shader`), `frames_shader` (shader displays), and `frames_d3d11_copied` (device-local slice copies for non-SRV surfaces).
  With the d3d11 presenter in use `presenter` also has `"adapter":{"name":"..","vendor_id":N,"luid":"HHHHHHHH:LLLLLLLL"}` (the
  GPU of the presenter's device), `"debug_layer":bool` (D3D11 debug layer active), `"debug_messages":N` (messages seen, repeats
  included; the layer's default filter does not store INFO messages) and `"fallbacks":[..]`: distinct short names in order of
  first occurrence - `decoder_pool_no_srv`, `p010_vp_unsupported:shader`, `p010_vp_unsupported:download`,
  `format_download:<pix_fmt>`, `foreign_device_download`, `route_forced:<vp|shader>`, `route_forced_unavailable:<vp|shader>`,
  `force_caps:<key>=0` (one per masked key). `video.hwframes_requested` defaults to `keep` for
  CUDA and D3D11VA when the presenter is in use.
* The build table lists `presenter_params`; `ffp_probe_caps`: `features` gets `presenter_d3d11`
  when the video processor is usable; `presenters[]`
  gets `{"name":"d3d11","ok":..,"error":"","inputs":["d3d11","system","cuda"],"import":{"cuda":true,"cuda_zero_copy":true},
  "vp":{"rotate":..,"mirror":..,"procamp":..,"brightness":[min,max],"contrast":[min,max],"saturation":[min,max],
  "deinterlace":["blend","bob","adaptive","motion"],"telecine":..},"shader":true|false}`. `brightness` is in ffmpeg `eq` units * 100 (for
  contrast 1), `contrast` / `saturation` in ffmpeg `eq` units, `deinterlace` comes from the rate conversion caps `ProcessorCaps`,
  `telecine` is `ITelecineCaps != 0`. A filter the driver lacks is reported with the neutral range (`[0,0]`, `[1,1]`).

### CUDA import and D3D11 shader internals

The FFmpeg library extensions live in `1007-libavcodec-d3d11va-decoder-bind-flags.patch` and
`1008-libavutil-cuda-pool-hook.patch`; presenter integration is in
`1011-libffplay-d3d11-cuda-shader.patch`.

`libavutil/hwcontext_cuda.h` appends a libffplay extension to `AVCUDADeviceContext`:
`AVBufferRef *(*pool_alloc)(void *opaque, AVHWFramesContext *fc, size_t size)`, `pool_alloc_opaque`,
and an optional `pool_recycle(opaque, data)` predicate. When no user pool exists, `cuda_frames_init`
uses the allocator for internally created CUDA frame pools. NULL retains upstream `cuMemAlloc`;
the returned AVBufferRef must own a texture-aligned linear `CUdeviceptr` of at least `size` bytes,
including FFmpeg's pitch/alignment padding. A zero recycle result frees a retired allocation instead
of returning it to the pool. These extensions change the CUDA hwcontext ABI: ship the matching
`avutil-60.dll` and header.

The presenter chooses the CUDA device with `cuD3D11GetDevice` on its DXGI adapter. A reference-counted registry maps
CUdeviceptr ranges to D3D11 buffers (DEFAULT, raw SRV/UAV, registered and mapped with CUDA). Decoder/filter pool outputs
use those buffers when available. Display synchronizes CUDA, unmaps the buffer, reads Y/UV through an HLSL
`ByteAddressBuffer` shader, then remaps before the frame can be reused by CUDA; the registry outlives the presenter while
frame pools retain buffers. Foreign NVDEC or filter buffers instead use R8/R8G8 (P010: R16/R16G16) textures registered
with CUDA and two `cuMemcpy2D` device-to-array transfers per frame. The test-only `presenter_debug_cuda_copy=1` leaves
the hook unset to force this fallback. TEST-ONLY `presenter_debug_cuda_readdress=N` simulates relocation after
its real remap/pointer lookup on every Nth direct display (0 disables it). A changed mapped pointer retires
that pool entry; its final release unregisters/frees instead of recycling, so subsequent allocations are fresh.
Already-rendered frames remain displayed; any held frame referencing the old pointer is skipped, not copied
from an invalid address. Pointer relocation itself does not fail the presenter or emit event 9 (independent GPU/device failures still can). Remap remains immediately
after the graphics flush because AVBufferPool has no producer-side checkout hook: CUDA map orders after
preceding D3D reads, while a held frame reference prevents CUDA reuse until release. Shader compilation is at runtime via dynamically loaded `D3DCompile` from
`d3dcompiler_47.dll` (or `_43`). Offscreen shader rendering + staging readback supplies snapshots. It applies
color matrix/range, postrotation flips, luma contrast/brightness/gamma and chroma saturation in ffmpeg eq style, and
bilinear SAR/aspect scaling. Gamma and chroma-edge/P010 conversion are approximations to CPU `vf_eq`/swscale rather than
byte-exact LUT/upsampling. HDR PQ/HLG and BT.2020 constant-luminance are not tone mapped and reject CUDA shader import.
D3D11VA and uploaded system NV12/P010/BGRA frames use the same shader when a parameter cannot be honoured by the VP: unsupported rotation/mirror, gamma != 1, or procamp outside VP levels. Other frames stay on the cheaper VP. Decoder array surfaces request `DECODER|SHADER_RESOURCE` at pool creation if a per-format `CheckFormatSupport` and allocation probe accepts both NV12/P010; otherwise the normal decoder pool is retained and one device-local slice copy supplies the shader. A foreign non-SRV-capable surface takes the same one-copy fallback. `AVD3D11VADeviceContext` appends `decoder_bind_flags` for this request; ship the matching `libavutil` header and DLL. The shader probe renders a tiny NV12 plane-view texture offscreen and verifies BGRA output; only then is `presenters[d3d11].shader:true`. Snapshots select the same VP/shader route as display. `presenter.route` reports `vp` or `shader`; `frames_shader` counts shader displays, `frames_d3d11_copied` counts fallback device-local copies. D3D11/system shader renderer is `d3d11:shader` (`d3d11:shader-cuda` remains CUDA).

`ffp_probe_caps.presenters[].import.cuda_zero_copy` is true only when a live probe registers/maps a D3D11 buffer,
writes via CUDA, reads it through the pixel shader to an offscreen BGRA texture, and verifies its pixels. The `cuda`
import key is true on this successful path. `presenter_debug_fail=display` still falls back to SDL,
which downloads CUDA frames for display.

## Acrylic backdrop regions

`ffp_set_backdrop_regions` (`1013-libffplay-acrylic-backdrop.patch`) copies an array of
`FFPBackdropRegion` entries for the active D3D11 presenter. Regions are rounded rectangles in
client pixels, composited above video and bitmap captions in back-to-front `z` order. The installed
header defines their layout: colors are packed `0xAARRGGBB` (alpha ignored), tint/luminosity
opacities are `0..1`, and noise is `0` or `1`.

The copied list appears on the next presented frame; paused video is redrawn. `count=0` clears
regions. Invalid regions, more than `FFP_BACKDROP_REGION_MAX` (64) entries, or a non-D3D11 active
presenter return `FFP_EINVAL`. Snapshots never include Acrylic.

## ffplay.exe

`ffplay.exe` keeps the ffplay command line (`ffplay [options] input`). Its `main()` parses
options with the usual cmdutils tables, builds an `options_kv` string, calls
`ffp_create(NULL, ...)` and `ffp_open`, and runs until the window is closed, `q`/Esc is
pressed, or EOF is reached with `-autoexit`. `ffplay -version` prints the normal
banner/configuration.

## ARIB captions and bitmap subtitle layers

`1012-libffplay-captions.patch` integrates independent caption and superimpose layers. The
required API includes `ffp_set_subtitle_visible`, `ffp_refresh_caption_style` and `ffp_snapshot_ex`.
The installed header defines `FFP_SUBTITLE_CAPTION`, `FFP_SUBTITLE_SUPERIMPOSE` and
`FFP_SNAPSHOT_SUBTITLES` (bit 0, the only supported snapshot flag).

`ffp_set_subtitle_visible` accepts `FFP_SUBTITLE_CAPTION` or `FFP_SUBTITLE_SUPERIMPOSE`; `on` is zero or non-zero. Visibility only affects presentation: decoding continues, so an active indefinite caption reappears when made visible again. Unknown layers return `FFP_EINVAL`. `ffp_cycle_stream(FFP_STREAM_SUBTITLE)` continues to cycle the regular caption stream and never changes the superimpose layer.

`ffp_refresh_caption_style` reopens only the active ARIB caption decoder with the updated background and outline flags. It leaves the video, audio and superimpose decoders running; the caption layer continues at the current playback position. Hosts can call it after a Windows caption-settings change.

`ffp_snapshot_ex` has the same allocation, pixel format, dimensions, sample-aspect-ratio behavior and `ffp_free` ownership contract as `ffp_snapshot`. With `flags == 0`, it is equivalent to `ffp_snapshot`. `FFP_SNAPSHOT_SUBTITLES` composites the layers that are currently visible; caption visibility and the `superimpose` open option are applied independently. `ffp_snapshot` always returns video only.

### Open options and stream classification

The `arib_font` open option selects the ARIB font file. The host normally resolves the bundled file beside the executable and passes its plain path as `arib_font`; libffplay converts it to the decoder's `font=file:<path>` option. An empty or unavailable override falls back to libaribcaption's platform defaults. `arib_ignore_background` and `arib_force_outline` are boolean options applied to the caption decoder only. `superimpose=0|1` controls whether a superimpose decoder is opened (default `1`). The ARIB decoders use bitmap output, the layer's `caption_type` (`caption` or `superimpose`), and a canvas matching the presenter's video rectangle in pixels.

Subtitle stream entries in `ffp_get_media_info` may include `arib_type` (`"caption"` or `"superimpose"`) and `component_tag` (integer). Caption selection excludes streams classified as superimpose. A stream without `arib_type` remains an ordinary selectable subtitle stream.

### Cue timing and presentation

ARIB decoder output is a sequence of timed RGBA bitmap events. Cue start is the subtitle PTS plus `start_display_time`; an end time of `UINT32_MAX` means that the cue remains active until replaced or cleared. An event with no rectangles clears that layer immediately. Cue state is driven by the playback clock, not the coarse POSITION event cadence, and is flushed on seek and stream changes.

Both subtitle layers are drawn over the video rectangle. SDL uses the existing bitmap subtitle conversion path; D3D11 uploads cue rectangles as cached RGBA textures and blends them over the presented frame in both presenter routes. Caption toggle visibility does not hide superimpose. A presenter resize updates decoder canvas sizes after the size is stable; the old cue remains scaled until its exact-size replacement arrives. Re-rendered cues preserve their original timing and cannot override a newer cue.

The ARIB decoder keeps the decoded caption objects, and the clear events, of the last 10 minutes of media time around its decode position in a history capped at 5 MiB (`history_window`, `history_max_bytes`; oldest first out). A seek flush keeps it, and the history of a closed decoder is parked under the file's identity (`history_key`: path, size, mtime and stream, set by libffplay for local files), so a reopen of the same file after a device loss or a settings change continues from it while another file starts empty. The first video frame shown after a seek or open asks the decoder for the caption on screen at that frame's pts: the cue appears with the landing frame, paused too, unless it ended or a clear event followed it. This works for the part of the file already decoded in this session; a forward seek into an unplayed part, or a fresh open, has no history and waits for the next cue. A cue read from the queue always wins over one from the history. `ffp_get_debug_info` reports `subtitle_layers.<layer>.history` (`events`, `bytes`, `appended_bytes`, `landed`).

`ffp_get_debug_info` includes per-layer stream/decoder options, resolved font, active cue timing and rectangle count, cue/clear/upload counters, current canvas size and re-render count. The two layer records are independent; fields unavailable in a non-ARIB subtitle layer remain empty or null.

A `FFP_SNAPSHOT_SUBTITLES` snapshot is composed at the snapshot's native, SAR-stretched size. Active layers are re-rendered at that canvas size, then the live canvas is restored. If re-rendering fails, the current cue bitmaps are scaled for the snapshot and the debug log records the fallback. Snapshot calls still return `FFP_EAGAIN` if no video frame has been displayed yet.

The test-only `ffp_test_d3d11_readback_enable` and `ffp_test_d3d11_readback_last` exports can read the composed D3D11 back buffer and its video rectangle in client pixels. Readback is disabled by default and incurs a GPU-to-CPU copy on each presented frame while enabled; it is not a production snapshot API. The returned BGRA allocation belongs to the caller and must be released with `ffp_free`. On a missing presenter or a frame not yet captured, the readback API returns `FFP_EAGAIN`.

## ARIB audio model

For an ARIB transport stream (`AVProgram.metadata["arib"]=="1"` of the video's program, set by the mpegts demuxer; see
`1006a-mpegts-arib-detection.patch`) `1016-libffplay-arib-audio.patch` replaces "one selected audio stream" by two roles
that are demuxed, decoded and mixed together. Every other input keeps the single audio stream and `ffp_cycle_stream`.

* **Roles** (tsreadex `CServiceFilter::AddPmt`, re-run after every `av_read_frame` that shows a changed `nb_streams`,
  `pmt_version` or stream list of the video's program): audio 1 = ADTS AAC with component tag `0x10` / `0x83` / `0x85`
  (the last match wins), else the first untagged AAC; audio 2 = AAC with tag `0x11`. Only the video's program counts.
  The open uses audio 1's stream (audio 2's when audio 1 does not exist) for the device; FFmpeg's own pick is not used.
* **`ffp_set_audio_preference(p, pref)`**: `FFP_ARIB_AUDIO_1` (0) always plays audio 1; `FFP_ARIB_AUDIO_2` (1) plays
  audio 2 wherever it has samples and audio 1 otherwise, gaplessly and without any event. The value belongs to the
  instance (it applies to the current and to later opens; `FfPlayer::Open` resets it to audio 1 because every file
  starts there) and reaches the audio within the pipeline latency (~0.2 s). `FFP_EINVAL` for any other value.
  `ffp_cycle_stream(FFP_STREAM_AUDIO)` does nothing for such a media.
* **Mix**: each role has its own packet queue (audio 1's is the usual audio queue, so the audio serial, the seek flush and
  the queue statistics are unchanged), decoder thread and converter, and writes float PCM in one format (the device rate and
  layout) into a ring keyed by the source PTS (at most 1.5 s ahead of the output cursor). A change of the stream of a role
  (a new PID without `merge_pmt_versions`) or of its channel layout (2ch <-> 5.1) re-opens only that role's decoder /
  converter and dips over 5 ms. One selector thread replaces `audio_thread`: a cursor advances through the ring timeline and
  per run of samples the pure function `ffp_arib_select` (`fftools/ffplay_arib_audio.h`, table-tested by
  `ffmpeg/tests/arib_select_tests.c`) picks audio 1, audio 2 or silence. Every change of source is a 5 ms crossfade (a
  fade out / fade in over the last / first 5 ms when the leaving source has no more samples); the result enters the one
  shared audio filter graph (rebuilt only for a new audio serial or filter generation, so `atempo`, equalizer and
  normaliser state survive a switch), then `sampq` and the unchanged callback. The device is opened once.
* **Coverage and hold**: with audio 2 preferred, audio 2 plays at every sample where its ring has data; after it was left
  it is entered again only after `FFP_ARIB_HOLD_S` = 1 s of the output timeline, so a patchy stream does not flap. The
  cursor waits (bounded by the next rule) for a source that has neither a sample nor a verdict, so the 0.1 s by which
  audio 2 trails audio 1 never makes it look absent.
* **Starvation** (demux fronts, `ffp_arib_starved`): a role is starved when a PMT removed it, it finished, it lags the
  other audio role by more than `T_starve` = 0.5 s (measured interleave: p99 85 ms, max 106 ms), or it lags the video by more than 1.5 s
  (audio trails video by up to ~0.95 s on CS). Its absence is certain up to the other front minus 0.15 s (video front
  minus 1.2 s). The read thread treats starved roles as satisfied in its "enough packets" test and finished at EOF.
* **Silence**: where no role supplies samples (both missing, audio 1 missing with audio 1 preferred) the selector
  synthesises silence with advancing pts, so the audio clock never freezes and the video never runs on a stalled clock.
  A pts discontinuity (a chunk more than 8 s ahead of, or 1 s behind, the cursor) restarts the cursor at the new data.
* **Seek, pause, stepping**: a seek flushes all queues to the one shared serial, the rings empty, the cursor restarts at
  audio 1's first sample (later of the two when audio 2 starts within 0.25 s of it and is preferred) and the post-seek
  audio trim works on the selector's frames as before. Pause, the pause fade, `start_paused`, `step_silence` and frame
  stepping only touch the callback and `sampq`.
* **Debug info**: `"arib_audio":{"preference":"audio1"|"audio2","playing":"audio1"|"audio2"|"silence"|null,"roles":[{"role":"audio1","present":true,"stream_index":1,"pid":274,"component_tag":16,"starved":false,"buffered_ms":812.4},{"role":"audio2",...}],"switches":3,"silence_ms":0.0,"rebinds":3}`
  (`null` for other media). `playing` is the source of the last rendered samples; `switches` counts source changes at
  the cursor; `silence_ms` the synthesised silence; `rebinds` the role recomputations (the first PMT replay at the start of
  a file counts).
