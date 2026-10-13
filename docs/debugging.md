# Debugging the player

Everything below reports what libffplay and the presenter actually do at runtime (the opened decoder, the hardware device
that really got created, the filter graphs really configured, the presenter route really drawing), next to what the app
asked for. A difference between the two is highlighted.

The app is built against `ffmpeg/dist/win64` and ships those DLLs; it imports `libffplay.dll` normally and checks no
version (a missing DLL or export stops it at launch with the loader's error). The startup log records the loaded
FFmpeg version. Filters, the decoder of each hwaccel and the build features are compiled into the app from that dist
(`BuildCapabilities`); GPU devices, drivers, decode support, the presenter's capabilities and interop paths are checked at
runtime by the probe child of each adapter.

## Command line

```
aribplayer.exe [options] [file | url | playlist ...]
```

| Flag | Effect |
| --- | --- |
| `--console` | Log lines go to a console. Attaches to the parent console, opens one when there is none. Streams already redirected to a file or pipe are used as they are. Log lines go to stderr, like ffplay. |
| `--log-level=<level>` | libffplay / FFmpeg level: `quiet`, `error`, `warning`, `info`, `verbose`, `debug`, `trace` (or the AV_LOG number). Default `warning`; `info` while any log output (`--console`, `--log-file`, Debug logging) or `--stats` is active. |
| `--stats` | Adds `stats=1` to every open: ffplay's periodic status line (`12.31 A-V: -0.008 fd=0 aq=34KB vq=890KB sq=0B`) goes through the log callback at info level. In the log it is throttled to 2 lines per second. |
| `--log-file=<path>` | Appends timestamped lines to `<path>` (folders are created). |
| `--help`, `-h`, `/?` | Prints the usage text to the console and exits. |
| `--logictest [filter]`, `--headless-check <media> [--result=<json>]` | Test modes, no visible window. |
| `--probe-gpu <file> --adapter-luid=<id>` | Internal: the video platform starts the app with this to probe one adapter in a short-lived child process (the drivers keep tens of MB of per-process state after probing) and reads the JSON answer from `<file>`. When the child gives no answer (it could not start or crashed) the adapter is probed once more inside the app (`GPU probe helper gave no answer ...` warning); a child that runs over 30 s is terminated and its adapter is not used (`GPU probe helper hung ...`). |

Log line format: `2026-10-04 12:34:56.789 [INFO ] src: message`. `av:` lines are libffplay / FFmpeg `av_log` output (delivered
through `FfPlayer::SetLogCallback`, so they are not written to the raw stderr), everything else is an app event:

| src | events |
| --- | --- |
| `app` | start (libffplay version / API), close |
| `open` | one record pair per open: `url: ...`, `options: key=value ...` (the exact list handed to `ffp_open`) |
| `player` | `prepared: ...`, `state: Playing|Paused|...`, `ended`, `ERROR ...` |
| `queue` | current item changes |
| `seek` | seeks from the keyboard (`key relative -5 s` for Left / Right and `-30`, `+60`, `-300` ... for the modifier arrows, each landing on the key packet closest to the target; Ctrl+Shift+Left / Ctrl+Shift+Right log `key previous keyframe` / `key next keyframe`) and the seek bar (`bar to 12.5 s`) |
| `filter` | every live filter change (vf / af rebuild strings, the effective options) and filter errors |
| `mode` | `now-playing`, `full-screen`, `mini-player` |
| `presenter` | the presenter of each adapter switch, presenter failures (`PRESENTER_ERROR`: the item is reopened) |
| `debug` | overlay on / off |

Settings > Advanced > **Debug logging** writes the same stream (info level) to
`%LOCALAPPDATA%\aribplayer\logs\aribplayer-YYYYMMDD.log` (setting `debugLogging` in `settings.json`). It can be combined
with `--log-file`.

## The Tab debug overlay

`Tab` toggles a monospace panel (Cascadia Mono, fallback Consolas) at the top left, below the title strip, in Now playing,
full screen and the mini player. It refreshes about four times a second while shown and does nothing while hidden. It lives
in its own composition layer below the title, the bar and all popups; it never takes input.

Tab keeps its normal meaning (focus traversal) in these cases: a popup / flyout is open, a visible widget has
keyboard focus, or Shift is held (Shift+Tab walks into the player bar). Clicking the video clears focus, so Tab toggles
again.

How to read it (`requested -> actual`; a row in the warning colour with `MISMATCH` means the two differ):

| Row | Meaning |
| --- | --- |
| `output` | The drawing path the presenter reports (`d3d11:videoprocessor` or `d3d11:shader`; the presenter's name when it reports none). `frame_path` describes the last decoded frame: `hw_direct` (retained hardware frame), `hw_download` (downloaded to system memory), `software`. Read sink-download and presenter counters too: `hw_direct` alone does not prove the whole pipeline has no host copy. More than one non-zero decode-path counter shows `PATH CHANGED` (mid-stream fallback). |
| `video` | codec, the decoder implementation really opened, decoded size -> output size, SAR, frame rate. |
| `hwaccel` | `requested d3d11va -> actual <device type of the decoder's hw_device_ctx>`. `actual none` plus `error:` means the request could not be honoured (e.g. a codec without d3d11va support) and decoding is software. |
| `pix_fmt` | pixel format as decoded (`d3d11` for hardware frames), after the hardware transfer (`nv12`), and the format handed to the presenter. |
| `audio`, `audio out` | codec / decoder, source rate, channels, layout, sample format; output device, rate, channels, `hw_buf`, average A-V `diff_avg`, output `latency`. |
| `arib audio`, `audio 1`, `audio 2` | ARIB transport streams only: the preference (`audio1` / `audio2`), the source playing now (`audio1`, `audio2`, `silence`), source changes at the cursor, synthesised silence in ms, role recomputations; then per role the stream, PID, component tag, decoded audio buffered ahead of the cursor, and `STARVED` when its data stopped (or a PMT removed it). |
| `vf req` / `vf graph` | the video filter string the app set vs the graph really configured, in order, including auto-inserted `scale` / `format` (listed as `auto-inserted:`). A requested filter missing from the graph, or out of order, is a mismatch. |
| `af req` / `af graph` | the same for audio (`aformat`, `aresample` appear as auto-inserted). |
| `vf nodes` / `af nodes` | the configured nodes with the hardware device each one runs on, e.g. `hwupload[d3d11va] > ivtc_d3d11[d3d11va] > hwdownload`. |
| `deinterlace` | the single Deinterlace option as really in the graph: `IVTC: ivtc_d3d11 (d3d11), residual-combing fallback unavailable`, `IVTC: fieldmatch+bwdif(interlaced)+decimate (CPU)`, `bwdif (CPU)`, `yadif (CPU)` or `none`. Warning when the mode is enabled but not in the graph. |
| `path` | only for a `hwupload` ... GPU filter ... `hwdownload` round trip in the graph: the data path of a frame (see below). |
| `speed` | the app's requested speed vs the product of all `atempo` values in the audio graph. |
| `sync` | master clock (`audio` / `video` / `external`), the clocks, and `A-V` in milliseconds (the same number as ffplay's `A-V:`); dropped frames (early / late), duplicated frames, frame timer drift. |
| `queues` | packets, bytes and duration queued for audio / video / subtitles. |
| `streams` | stream index, start time and time base per stream. |
| `position` | position / duration, paused, speed. |
| `open`, `options` | the url and the exact `key=value` options of the last open (with the live filter strings applied). |

Unknown values in a runtime snapshot show `?` and are never treated as a mismatch.

### Frame paths

* Software decode: `CPU decode -> filters (CPU) -> presenter upload`.
* `hwaccel=d3d11va` with `hwframes=download`: `GPU decode (d3d11) -> CPU download (nv12) -> filters -> presenter upload`
  (`frame_path hw_download`).
* D3D11VA with the D3D11 presenter: `GPU decode (d3d11) -> D3D11 VP/shader`; supported frames remain on the GPU.
  Unsupported formats still fall back to download/upload.
* A `hwupload` ... GPU filter ... `hwdownload` round trip in the graph:
  `GPU decode -> CPU download -> <domain> upload + <filter> -> CPU download -> <presenter>`.
  This path has four GPU/CPU transfers; compare it with the CPU recipe in the overlay (`deinterlace` row, `queues`, drops).

Run with `--console --log-level=verbose --stats --log-file=C:\temp\wvp.log` to see the `av:` lines (decoder selection,
hwaccel negotiation, filter graph configuration) and the status line next to the app events in one timeline.

## Headless checks

```
aribplayer.exe --console --log-level=verbose --stats --log-file=%TEMP%\aribplayer-check.log --headless-check testmedia\testsrc2-360p-short.mp4
```

`--headless-check` plays on SDL's dummy audio driver and verifies the overlay (Tab on / off, layer z-order below title / bar /
popup, text from the real player, refresh), and with `--log-file` it reads its own log at the end and requires the open / options lines, `prepared`,
`state`, `seek`, `filter`, `av:` lines and, with `--stats`, the `A-V:` status line (missing ones are listed in the
`log_file_missing` note). No window is ever visible.

The suite runs on short clips (`testmedia\testsrc2-360p-short.mp4`, 3 s) and long ones (`testmedia\arib\tbs-captions.ts`, 90 s):
steps that need running media restart an item that ended (`playback_restarted_*` notes), the drop step drops another media file
from the input's folder or its parent, and the queue-end step makes one keyframe seek to about 8 s before the end of a long clip
(`queue_end_seek_target_s`) and then plays to the natural end.

## GPU adapter

Video runs on the adapter the video GPU selection picks (Settings > Playback > Video GPU, Automatic by default:
`wvp::SelectVideoGpu`, see `src/Core/include/wvp/VideoGpu.h`). The Windows video platform (`src/Presenter`) lists the
hardware adapters (DXGI, grouped into physical GPUs by PCI location), probes each one in a child process and creates the
D3D11 presenter on the chosen adapter; its D3D11VA device is the device libffplay decodes and filters on (nothing runs
on another GPU). Windows video is D3D11 only: there is no CUDA decoding, filtering or import.

The probe starts first thing at startup, while the window is created, and every open waits for its plan: an item opened
before the probe answered (command line, restored queue) is opened when the plan arrives (`caps: startup probe done N ms
after start`, then `open: first media prepared N ms after start`). If no adapter can be probed, neither by its helper nor
in the app, nothing is opened and an error notice says that video cannot be played (`caps: no adapter could be probed`);
the same notice names the reason when the planner finds no pipeline for the current settings (e.g. a custom filter the
player cannot run). The presenter starts on the adapter of the window's monitor; the choice moves it. The log has
`adapter: <trigger>: video GPU <name> (luid ...), display adapter ...: <reason>` and `presenter: presenter on <luid>` lines.

Test / diagnostic switches (default off; the presenter options also reach the probe child, except the route):

- `--test-ui-device-loss=<ms>`: simulates one UI GPU device loss that many ms after playback starts (the Gfx draw/commit fails
  with DXGI_ERROR_DEVICE_REMOVED; the normal rebind runs). `--headless-check` always runs the simulated-loss steps
  (`ui_loss_*`); the `gpu:` log lines carry detection -> UI-visible timings.
- `--d3d11-debug`: the D3D11 debug layer on the presenter's devices (needs the Graphics Tools optional feature);
  `ID3D11InfoQueue` messages appear in the log prefixed `d3d11-debug:`.
- `--d3d11-force-caps=p010=0,decoder_srv=0,...`: masks presenter capabilities (`nv12_srv`, `p010_srv`, `decoder_srv`, `p010`,
  `shader`) to exercise the fallbacks; the overlay's presenter `fallbacks` line lists what was taken.
- `--d3d11-route=vp|shader`: forces the presenter route.

## GPU pipeline planner

Deinterlace is ONE option: Off / On / IVTC, with the preference "Deinterlace priority" Efficiency (default) / Quality (settings
keys `deinterlace` = off|on|ivtc, `deinterlacePriority` = efficiency|quality; output rate and "only interlaced" apply to On only).
On is one planner operation with bwdif and yadif as candidates:

| Where the deinterlacer runs | Efficiency | Quality |
|---|---|---|
| GPU (a `bwdif_*` variant in the chosen domain: `bwdif_d3d11`, ...) | bwdif | bwdif |
| GPU domain without any bwdif variant in the build | that domain's yadif | that domain's yadif |
| CPU | `yadif` | `bwdif` |

The cost model picks GPU vs CPU; the step reason (Tab overlay `deinterlace` row and the plan steps) says why, e.g.
`priority efficiency: GPU deinterlacer -> bwdif_d3d11`, `priority efficiency: no GPU deinterlacer -> yadif`,
`priority quality: GPU deinterlacer not on the cheapest path -> bwdif`. Driver deinterlacers and video post-processors
(`deinterlace_d3d11/_qsv/_vaapi/_amf`, `vpp_qsv/_amf`, ...) are never planned (`PipelineTable::IsDenied`), and the D3D11
presenter's VideoProcessor always runs progressive with auto processing off. Content with 4:2:2 / 4:4:4 chroma is never
planned with `hwframes=keep` (the presenter and the GPU filters take 4:2:0 hardware frames only): the plan note says
`chroma 4:2:2: hw frames are downloaded (no hwframes=keep)`. IVTC plans the
cheapest variant: `ivtc_d3d11` alone (GPU ivtc clears the interlaced flag, so no deinterlacer follows and the plan
note says `residual-combing fallback: unavailable (GPU ivtc)`; a caps filter with `"flags_combed": true` gets
`<bwdif in the same domain>=mode=send_frame:parity=auto:deint=interlaced` appended), or on the CPU
`fieldmatch,bwdif=mode=send_frame:parity=auto:deint=interlaced,decimate`.
With software decoding a GPU variant is uploaded inside the graph (`sw > hwupload > ivtc_d3d11 > present:d3d11`).
The build's CUDA filters (`ivtc_cuda`, `bwdif_cuda`, `scale_npp`, ...) are classified in the frame domain `cuda`, which has
no device type: no device, decoder, upload, interop or presenter input reaches it, so they are never planned (a custom chain
naming one has no pipeline).
Hardware decoders: "Auto" is resolved by the planner from the capabilities (it costs every usable decoder and software
decoding), libffplay always gets an explicit `hwaccel` and `hwframes`, plus `vcodec` when the build's decoder for that codec
and hwaccel is not the codec's default decoder.
The current library supports hardware-frame output from that graph (`upload_hw_sink`, source patch
`1009-libffplay-engine.patch`); the D3D11 presenter consumes D3D11 frames from it directly.
The planner still accounts for available filters, devices, interop and copy costs when choosing GPU or CPU processing.
The primary real-playback interlace fixture is Japanese/Korean broadcast 1920x1080i at 30000/1001 pictures/s (60000/1001
fields/s): `testmedia/hw/h264-1920x1080i-2997.ts`. `bwdif=mode=send_frame` should produce about 29.97 fps and
`mode=send_field` about 59.94 fps. These fixtures are made by interleaving 59.94p fields; libx264 emits MBAFF
(`mb_adaptive_frame_field_flag=1`, `field_pic_flag=0`), not genuine PAFF (libx264 cannot encode PAFF).

The app probes every adapter at startup (the platform's checks of decode support and the presenter, merged with libffplay's
portable `ffp_probe_caps` on the same devices and the build tables) and plans the whole video pipeline from that: decoder,
the `vf` string with its hardware transitions, and which frames reach the presenter. The presenter is always the
platform's (D3D11 on Windows); there is no presenter preference. The probe results, not the library, determine whether a
GPU runtime or device is usable. Log lines (all with `--console` or `--log-file`):

- `caps: ...` one line per section of the probe: source / adapter / features, devices (`ok` and `failed`), decoders usable,
  filters (and which of them are hardware filters), interop checks (`d3d11>qsv map FAILED` = that mapping is not usable and
  will not be planned), presenters. Device or interop failures in an otherwise valid probe exclude those paths.
- `pipeline: planner active: <summary>` once capabilities arrived. A summary reads `d3d11va(keep) > ivtc_d3d11 > present:d3d11
  [host 0, device 0]`: decoder (`(keep)` = hardware frames stay on the GPU), the filter steps, the presenter, and the planned
  copies per frame (host = GPU<->system memory, device = GPU-side copies). Compare with the `frame_path` counters of the overlay.
- `pipeline: <summary>` at every open, plus the rejected alternatives at `--log-level=verbose` (`rejected: ... (1 more host copy)`).
- `pipeline: content: hevc 1280x720 8-bit` after PREPARED: the plan is made with the real codec / size / bit depth. A first open of a
  file is planned with unknown content (assumed H.264 1080p); if the plan changes, the item is reopened once at its position
  (`replanned for the content ... -> reopen`). The content is cached per URL, so the next open is planned right the first time.
- `pipeline: fallback: <key> (<reason>)` a plan element did not work in practice (planned `hwaccel` is not what decodes after 40
  frames, `hwframes=keep` fell back to download, a hardware filter raised a filter error). The key is blacklisted for the
  session (`hwaccel:d3d11va`, `hwframes_keep`, `filter:ivtc_d3d11`), the pipeline is replanned and the item reopened. At most one
  fallback per open. A presenter failure (`PRESENTER_ERROR` event) is no fallback: the engine stops presenting for that open,
  the app reopens the item at its position and the presenter recreates its device in its next open (a lost device also
  re-selects the video GPU). A second failure within 2 s leaves the item playing without video.

The Tab overlay shows a `pipeline` section while a plan is active: the plan, each step with its copy kind, planned vs measured
copies, and the rejected alternatives; a mismatch (e.g. planned d3d11va, decoding in software) is flagged there too.
The planner takes a GPU decoder of the capabilities whenever one fits the stream and decodes in
software only when none does (e.g. HEVC 12-bit) or when that saves copies. The overlay also shows
`presenter d3d11 direct N uploaded M` and warns about presenter mismatches/errors or an uploaded count
against a no-host-copy plan. These are aggregate counters, not copy guarantees: `direct` can include
D3D11 shader inputs requiring a device-local slice copy. Consult `frames_d3d11_copied` in the debug JSON before diagnosing a
host-copy mismatch.

## ARIB captions and superimpose

The ARIB section of the Tab overlay reports the caption and superimpose layers separately: selected stream and `arib_type`, decoder/options, resolved font, current cue rectangle count and start/end times, cues shown/cleared/uploaded, and the active canvas size and resize re-render count. An empty cue with zero rectangles is a clear event; an end time of `UINT32_MAX` means the caption remains active until replaced or cleared. The caption visibility toggle does not pause decoding, and it never hides superimpose. If stream metadata has `arib_type: superimpose`, it must not appear as a selectable caption track.

For an ARIB stream, the open log should show bitmap subtitle output, the `caption` or `superimpose` decoder type, and the canvas dimensions matching the presenter's video rectangle. The caption style options (`arib_ignore_background` and `arib_force_outline`) apply only to captions. A resolved bundled font normally points to `<exe directory>\fonts\WLCMARU2004ARIBU.TTF`; a missing override falls back to libaribcaption's platform defaults.

### Headless fixture check

The broadcast fixture is local, generated from a read-only source on `Y:` and intentionally excluded by `.gitignore` (`testmedia/`). A 90-second sample can be extracted with stream copy; `-ignore_unknown` omits unsupported service-data PIDs while retaining the ARIB subtitle stream:

```powershell
New-Item -ItemType Directory -Force testmedia\arib | Out-Null
ffmpeg -ss 240 -i 'Y:\2026-10-01\ＴＢＳ１ - 🈟氷の城壁　第１５話　曇雨天🈑.ts' -ignore_unknown -map 0 -c copy -t 90 testmedia\arib\tbs-captions.ts
ffprobe -v warning -select_streams s -show_streams -show_packets testmedia\arib\tbs-captions.ts
```

Confirm a subtitle stream with codec `arib_caption` and packets in ffprobe before opening it. Then run `aribplayer.exe --headless-check testmedia\arib\tbs-captions.ts`. A successful check should report caption bitmap events rendered and should not require a visible window. The fixture may contain no superimpose stream; in that case its automatic classification still needs a separate stream-metadata test. When investigating timing, compare cue start/end against the video clock, not the four-per-second POSITION notifications. After resize, the previous cue may be scaled briefly while a re-render is pending; it should not disappear or replace a newer real cue.
