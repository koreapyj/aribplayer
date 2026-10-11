# libffplay C API (contract)

`libffplay.dll` exposes `fftools/ffplay.c` as a playback engine: demux, decode, filters, the A/V clock, SDL audio and
ARIB captions. It never owns a window and never renders: every picture goes to a presenter that the host implements
(`FFPPresenter`, see "Presenters"). See the [patch layout](../ffmpeg/README.md#layout). `ffmpeg/build-ffplay.ps1`
installs it into `ffmpeg/dist/win64/` with the matching FFmpeg DLLs, their public headers and `ffplay.h`.

The authoritative declarations are the installed [ffplay.h](../ffmpeg/dist/win64/ffplay.h). libffplay is internal to
this app, not a public API: the app is built against the header of the same build and always ships with those DLLs,
so the interface carries no version and the app checks none. It imports `libffplay.dll` normally; a missing DLL or
export stops the process at launch with the loader's error. Unavailable hardware or failed runtime probes are
capability results.

The C++ binding is [`wvp::FfPlayer`](../src/Core/include/wvp/FfPlayer.h), implemented in
[`FfPlayer.cpp`](../src/Core/src/FfPlayer.cpp), over
[`IMediaPlayer`](../src/Core/include/wvp/IMediaPlayer.h). `FfPlayer::Create(presenter, post)` creates the player
synchronously on a presenter from [`VideoPlatform`](../src/Core/include/wvp/VideoPlatform.h); `SetPresenter` swaps it.
The wrapper queues library events and hands `post` one closure per batch that raises them on the UI thread; its
controls and destruction belong on the UI thread.

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
  Capability probes may block on driver work: run them off the UI thread.
* Must NOT be called from inside the event callback: `ffp_destroy`, `ffp_stop`, `ffp_open`, `ffp_set_output`
  (they would deadlock). Post them to the host's UI thread instead.
* Multiple instances may coexist. SDL audio is initialised once per process (ref-counted: first `ffp_create` starts it,
  last `ffp_destroy` shuts it down); the `audio_driver=` creation option of the first instance picks SDL's audio driver.
  One process-wide display thread times the pictures of all instances and calls their presenters' `present`, `clear`
  and `close`; each instance additionally has its own read and decode threads. The event callback therefore runs on
  the display thread, on a read/decode thread, or (`STATE stopped`) on the thread that called `ffp_stop`; callbacks of
  one instance are serialised.

## Core API contracts

Use the installed header for function signatures, constants and structure layouts rather than duplicating those
declarations here. The following contracts supplement it.

* `ffp_create(output, options_kv, cb, user)` returns an opaque player or `NULL` on failure; `output` (the presenter and
  further devices) is copied, `NULL` = no output (pictures are timed and dropped). `ffp_set_output` replaces it (stops the
  media; the old presenter is never called again when it returns). `ffp_destroy(NULL)` is allowed.
* `ffp_open` stops the current media and starts the new open asynchronously. A return of `0`
  means the request started, not that media is ready: wait for PREPARED or ERROR.
  `options_kv` is `NULL` or newline-separated `key=value` pairs, split at the first `=`.
  Keys are ffplay options without `-` (`vf`, `af`, `ss`, `t`, `sn`, `an`, `vn`, `hwaccel`,
  `loop`, `framedrop`, `infbuf`, `fast`, etc.) or AVFormat/AVCodec/protocol options
  (`user_agent`, `headers`, `timeout`, `reconnect`, etc.). Values can contain `=`, `:` and `,`.
* `ffp_stop` closes media and clears the presenter to black. Volume (`0..100`) and mute persist
  across opens. Position is in seconds (`NAN` when unknown); duration `<= 0` means unknown/live.
* `ffp_set_video_filter` / `ffp_set_audio_filter` store filter strings across opens;
  `NULL` or `""` clears a filter. An open's `vf` / `af` replaces the stored string. The decode
  thread rebuilds the graph at the next frame; on failure FILTER_ERROR is raised and the previous
  string is restored.
* `filter_hw_device=type[:device]` (e.g. `d3d11va`, `qsv`) creates a hardware
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
  The presenter's `readback` renders it as shown when the presenter has one. Free pixels with `ffp_free`; no displayed
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
| 6 | ERROR | `{"code":-5,"message":"Input/output error","url":"..."}` (state becomes `error`). When the video decoder failed because its device was removed (FFPPresenter.device_lost) the json also has `"device_lost":true,"removed_reason":"0x887a0005"` |
| 7 | (unused) | was INPUT: the host owns its view and its input |
| 8 | FILTER_ERROR | `{"type":"video","filter":"<string that failed>","code":-22,"message":"Invalid argument"}` (`type` is `video` or `audio`) |
| 9 | PRESENTER_ERROR | `{"presenter":"<FFPPresenter.name>","error":"<reason>","device_lost":false,"removed_reason":""}` (the presenter called `sink.failed()` or returned an error: no more pictures for this open, audio continues; the host reopens, the presenter recovers its device in its next `open`) |
| 10 | STEP_DONE | `{"direction":1,"moved":1}` (the `ffp_step_frame` request finished, after its POSITION event; `moved` is `0` when no frame was presented) |
| 11 | KEYSEEK_DONE | `{"direction":-1,"moved":1,"position":12.480,"key":12.480}` (the `ffp_seek_keyframe` / `ffp_skip` request finished, after its POSITION event; `position` is the presented output frame, `key` the source PTS of the decoder frame it came from, both media-relative seconds; both `-1` and `moved` `0` when nothing moved) |

Order on a successful open: `STATE opening` -> `PREPARED` -> `STATE playing` -> `POSITION`...
`ffp_stop` -> `STATE stopped`. `ffp_pause`/`ffp_play` -> `STATE paused`/`playing`.

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

## Diagnostics and logging

Debug info, FPS statistics and the log callback are part of the required core API
(`1009-libffplay-engine.patch`). Runtime values may be unknown until media or hardware is ready.

### `ffp_get_debug_info`

Returns a JSON snapshot (free with `ffp_free`) or `NULL` when nothing is open. Thread-safe, cheap (about 4 Hz is
fine) and never blocks for long: decoder/filter facts are copies taken by the owning thread when the object is
opened/configured, live values (clocks, queues, drop counters) are read under short mutexes, and the media is
unpublished before it is torn down. **Every value is what is actually in use at runtime**, not what was
configured: the codec context that was really opened, the device that was really created, the filter graphs that
were really configured, the presenter that was really used. Requested and actual values are reported side
by side so a host can highlight a mismatch. All keys are always present; unknown numbers are `null`, unknown
strings `""`.

```json
{
  "position": 12.34, "duration": 60.0, "paused": false, "speed": 1.0,
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
  for it, or the first decoded frames turned out to be software frames; `hwaccel_error` then says why. Hardware frames
  the presenter takes (its caps) stay on the GPU (`hw_direct`); others are downloaded (`hw_download`).
* `hwaccel` open option values: a device type name (`d3d11va`, `d3d12va`, `videotoolbox`, ...); there is no `auto` (the host
  resolves it from its capabilities). The decoder is the `vcodec` option when given, else a decoder of the codec that has
  this hwaccel.
* `frame_path` is the path of the last decoded frame (`hw_direct`, `hw_download`, `software`, `""` before the first
  frame); `frame_path_counts` counts frames per path since the stream was opened, so a mid-stream fallback is visible.
  `pix_fmt` is the decoder context's negotiated format, `pix_fmt_decoded` the format of the last decoded frame
  (`d3d11` for hardware surfaces), `pix_fmt_after_transfer` the system-memory format after a download (`""` if none;
  taken from the frames context's software format), `hw_pix_fmt`/`sw_pix_fmt` the hardware surface format and its
  software format (`""` for software decoding).
* `presenter`: `{"name":..,"error":"","frames_direct":N,"frames_uploaded":M,"stats":{..}}`: the presenter's name, its
  failure for this open (`""` while it works), the pictures it got as hardware frames as is (`frames_direct`) or as
  system frames (`frames_uploaded`), and its own statistics (`FFPPresenter.stats_json`; `{}` without). The Windows D3D11
  presenter reports there `renderer` (`d3d11:videoprocessor`, `d3d11:shader`), `route`, the
  copy counters, `adapter`, `debug_layer`, `debug_messages`, `fallbacks`, `overlay`, `acrylic` and
  `frame_stats` (DXGI frame statistics and GPU timing).
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
* `pause_timing` (`1009-libffplay-engine.patch`) describes the last `ffp_pause` / `ffp_play` / `ffp_toggle_pause` that changed the state, in
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
  (`1009-libffplay-engine.patch`), so they do not force buffering up to the packet-byte cap.

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

`ffcapsdump.exe` (`1010-libffplay-ffcapsdump.patch`) is built with FFmpeg and installed in `ffmpeg/dist/win64`; it is not
shipped. At app build, `src/Core/CMakeLists.txt` runs it to generate `ffmpeg-caps.inc` (X-macro calls) in the build tree,
which `src/Core/src/BuildCapabilities.cpp` compiles into PlayerCore:

```c
FFCAPS_VERSION("8.1.3-Jellyfin")
FFCAPS_FEATURE("hwframes_keep")
FFCAPS_DECODER("h264", "d3d11va", "h264")
FFCAPS_FILTER("ivtc_d3d11", "list", ("d3d11"), 1, ("tff", "cthresh", "dupthresh", "scthresh", "verbose", "force_output_copy"), 1)
```

`ffcapsdump` reads only static FFmpeg tables and creates no device, so the output is the same on every machine. It is the
only place that uses the libavfilter-internal `FFFilter` (`libavfilter/filters.h`); libffplay itself does not.

* `FFCAPS_FEATURE`: libffplay features of the build: `hwframes_keep`, `upload_hw_sink`.
  `upload_hw_sink` (`1009-libffplay-engine.patch`) permits a `vf` that uploads system frames (`hwupload` on
  `filter_hw_device`) to end in a hardware format; the D3D11 presenter consumes supported frames directly (a
  `d3d11va` filter device without a device index uses the presenter's device), unsupported hardware formats are
  downloaded after the sink.
* `FFCAPS_DECODER(codec, hwaccel, decoder)`: for the codecs h264, hevc, mpeg2video, vp9, av1, vc1, mpeg4 and each hwaccel
  type (d3d11va, d3d12va, dxva2) of the build: the decoder that carries the hwaccel (the default decoder when it has
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

### `ffp_probe_caps`

* `char *ffp_probe_caps(AVBufferRef *const *devices, int nb_devices)`: probes with the host's devices (`AVHWDeviceContext`
  references; `nb_devices` 0 = one default device of every hardware type of the build). Blocking (device frame pools and
  device-to-device mapping are tested with 64x64 frames), any thread, no `FFPlayer` needed, not cached: every call
  probes. `av_log` output is suppressed while it runs. Returns UTF-8 JSON (free with `ffp_free`; NULL only on
  out-of-memory) with `devices` (one entry per given device: usable for frame pools), `decoders` (the decoders of the
  build that have a hwaccel of a given device type; `ok` means present, not verified on the GPU), `interop` (map / copy
  between the given devices) and `features`. There is no `adapter` and no `presenters`: both come from the host.

The host's video platform (`wvp::VideoPlatform::Probe`; Windows: `src/Presenter`, run in the `--probe-gpu` child) adds
the adapter, the verified decoders (`CheckVideoDecoderFormat` profiles and bit depths) and its
presenter's capabilities, and merges the engine's answer with `wvp::MergePlatformCapabilities`; Core then adds the build
table (`MergeBuildCapabilities`). The JSON is parsed by `wvp::ParseCapabilities` (`src/Core/include/wvp/CapabilitySet.h`);
every member is optional there.

### `hwframes` open option

`ffp_open(..., "hwframes=keep")` retains decoded hardware frames. The default is `keep` when the presenter takes the
hwaccel's frames (its caps), else `download`; the app always passes it explicitly. Unknown values behave
like `download`. With a `hwaccel` and `keep`, a hardware decoded frame is **not** downloaded in
the decode thread: it enters the video filter graph as a hardware frame (the `buffer` source gets
the frame's `hw_frames_ctx`), so `hwaccel=d3d11va` with `vf=ivtc_d3d11,hwdownload,format=nv12` runs the
filters on the GPU. The buffersink accepts hardware formats (the decoder's own, cuda, d3d11,
opencl, dxva2_vld, qsv, videotoolbox) besides system formats. A hardware frame leaving the sink is downloaded
with `av_hwframe_transfer_data` only when the presenter cannot consume it directly. Extra decoder
surfaces cover graph/queue retention and the frames the presenter holds (`FFPPresenterCaps.frames_held`).

Fallback to `download` for the rest of the stream (same stream, no reopen):

* `vf` is empty (or becomes empty) and the presenter cannot consume the hardware frame directly:
  frames are downloaded at decode time. Supported D3D11 frames stay on the direct D3D11 path.
* configuring the graph with hardware input fails (e.g. `vf=bwdif_cuda` on d3d11 frames, a software-only filter on a
  hardware frame): the vf string is **kept**, a warning is logged and `FFP_EVENT_FILTER_ERROR` is raised with
  `{"type":"video","filter":"<vf>","code":-40,"message":"...","hwframes_fallback":true,"error":"..."}`; the frame is
  downloaded and the same string is configured again with software frames (if that also fails, the usual FILTER_ERROR +
  restore of the last good string follows).

`keep` only matters when the decoder outputs a hardware format: `hwaccel=d3d11va` gives `AV_PIX_FMT_D3D11`.

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

The host implements `FFPPresenter` (`ffplay.h`): `open` (a video stream is about to be decoded; it fills the caps:
the `(format, sw_format)` pairs it takes as is, its own hardware devices, how many frames it holds after `present`),
`present` (the picture is due: the `AVFrame` borrowed for the call, pts, due time, serial, the caption and superimpose
overlay layers as straight-alpha RGBA), `clear` (black, all frames released; the whole output of audio-only media) and
`close`, plus the optional `accepts`, `setup_frames` (the decoder's hardware frame pool before it is initialised),
`readback` (the snapshot as shown), `stats_json` and `device_lost`. It reports through a sink: `presented()` with the
displayed video size (captions are re-rendered at it; also after a redraw on a resize while paused) and `failed()`
(device lost, Present errors: no pictures for the rest of the open, PRESENTER_ERROR, the host reopens).

Engine policy: the decoder and the filters use the presenter's devices (so decoding writes straight into presentable
memory), the decoder pool grows by the frames the presenter holds, and frames the presenter does not take are downloaded
or converted. `hwframes` defaults to `keep` when the presenter takes the hwaccel's frames. With `only_given_devices` a
device type that neither the presenter nor `FFPOutput.devices` provides is absent (software decoding), so nothing runs on
another GPU.

`open` comes from the media's read thread; `present`, `clear` and `close` from the shared display thread (never
concurrently for one presenter); the optional calls may run concurrently with them. A presenter function never calls
`ffp_*` on its own player, sink calls never block and never come after `close` returns, and after `ffp_destroy` or
`ffp_set_output` returns the old presenter is never called again.

The Windows D3D11 presenter (VideoProcessor and shader routes, device-loss recovery, overlays, the Acrylic
backdrop, readback and statistics) is the presenter library in `src/Presenter`
(`src/Presenter/include/wvp/win32/WindowsVideoPlatform.h`).

## ARIB captions and bitmap subtitle layers

`1009-libffplay-engine.patch` integrates independent caption and superimpose layers. The
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

Both subtitle layers are handed to the presenter with every picture (`FFPPicture.overlays`, straight-alpha RGBA in the canvas of the displayed video area; a new `generation` means new pixels); the D3D11 presenter uploads cue rectangles as cached RGBA textures and blends them over the presented frame in both routes. Caption toggle visibility does not hide superimpose. A presenter resize updates decoder canvas sizes after the size is stable; the old cue remains scaled until its exact-size replacement arrives. Re-rendered cues preserve their original timing and cannot override a newer cue.

The ARIB decoder keeps the decoded caption objects, and the clear events, of the last 10 minutes of media time around its decode position in a history capped at 5 MiB (`history_window`, `history_max_bytes`; oldest first out). A seek flush keeps it, and the history of a closed decoder is parked under the file's identity (`history_key`: path, size, mtime and stream, set by libffplay for local files), so a reopen of the same file after a device loss or a settings change continues from it while another file starts empty. The first video frame shown after a seek or open asks the decoder for the caption on screen at that frame's pts: the cue appears with the landing frame, paused too, unless it ended or a clear event followed it. This works for the part of the file already decoded in this session; a forward seek into an unplayed part, or a fresh open, has no history and waits for the next cue. A cue read from the queue always wins over one from the history. `ffp_get_debug_info` reports `subtitle_layers.<layer>.history` (`events`, `bytes`, `appended_bytes`, `landed`).

`ffp_get_debug_info` includes per-layer stream/decoder options, resolved font, active cue timing and rectangle count, cue/clear/upload counters, current canvas size and re-render count. The two layer records are independent; fields unavailable in a non-ARIB subtitle layer remain empty or null.

A `FFP_SNAPSHOT_SUBTITLES` snapshot is composed at the snapshot's native, SAR-stretched size. Active layers are re-rendered at that canvas size, then the live canvas is restored. If re-rendering fails, the current cue bitmaps are scaled for the snapshot and the debug log records the fallback. Snapshot calls still return `FFP_EAGAIN` if no video frame has been displayed yet.

The test-only `ffp_test_d3d11_readback_enable` and `ffp_test_d3d11_readback_last` exports can read the composed D3D11 back buffer and its video rectangle in client pixels. Readback is disabled by default and incurs a GPU-to-CPU copy on each presented frame while enabled; it is not a production snapshot API. The returned BGRA allocation belongs to the caller and must be released with `ffp_free`. On a missing presenter or a frame not yet captured, the readback API returns `FFP_EAGAIN`.

## ARIB audio model

For an ARIB transport stream (`AVProgram.metadata["arib"]=="1"` of the video's program, set by the mpegts demuxer; see
`1006a-mpegts-arib-detection.patch`) `1009-libffplay-engine.patch` (with the `1008-libffplay-arib-audio-model.patch` decision header) replaces "one selected audio stream" by two roles
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
