# Hardware test checklist (Intel / AMD / hybrid laptops)

For a tester with an Intel or AMD GPU (or an iGPU + dGPU laptop). Each check names the Tab-overlay lines to read: press **Tab**
during playback. Start the app with `--console` (or `--log-file=<path>`) so the `adapter:`, `caps:` and `pipeline:` log lines are
kept. Report the overlay text (a screenshot is fine) and the log for every failed check.

Test files: any H.264, HEVC 8-bit, HEVC 10-bit, MPEG-2 1080i broadcast (`.ts`, telecined and true interlaced), AV1 and VP9
clip, one HEVC 4:2:2 10-bit clip (camera footage), and an ARIB broadcast recording with captions.

## 1. Adapter selection

| Check | Expected (Tab overlay / log) |
|---|---|
| Single GPU | `adapter: video GPU <your GPU> (luid ...), display <your GPU> - automatic: display adapter, 0 host, 0 device` |
| Startup log | `adapter: UI device: <GPU> (vendor 0x8086 / 0x1002, luid ...)` and one `caps: video GPU candidate <GPU>` per GPU |
| Hybrid laptop, window on the internal panel | video GPU = the iGPU for H.264 / HEVC / MPEG-2; the dGPU appears as `runner-up <dGPU> [host 0, device 1]` |
| Hybrid laptop, codec the iGPU cannot decode (e.g. AV1 on an older iGPU) | video GPU = the dGPU, line is yellow: `not the display adapter (+1 cross-adapter copy)` |
| Hybrid laptop with an external monitor on the dGPU | drag the window to the external monitor and release: within a second the log has `adapter: drag end: video GPU <dGPU>` (only if that is cheaper) and playback continues at the same spot; dragging back reverts. No reopen while dragging. |
| Settings > Playback > Video GPU = a specific GPU | overlay says `manual: <GPU>`; dragging never changes it |
| Manual GPU disabled in Device Manager (or eGPU unplugged) | one toast `Video GPU <name> not found: using Automatic`; the setting still shows `<name> (not present)` |
| No CUDA on Intel/AMD | the startup `caps: devices ... failed:` line lists `cuda`, nothing else complains |

## 2. Decode per codec and bit depth

For each test file read the `decoder` / `hwaccel` lines and the plan summary (`pipeline` section).

| Content | Expected plan |
|---|---|
| H.264 / HEVC 8-bit / MPEG-2 / VP9 / AV1 (when the GPU decodes it) | `d3d11va(keep) > present:d3d11 [host 0, device 0]` |
| HEVC 10-bit | same, and the presenter decodes P010 (`route vp` or `route shader`) |
| HEVC 4:2:2 / 4:4:4 | note `chroma 4:2:2: hw frames are downloaded (no hwframes=keep)`, plan `d3d11va > present:d3d11 [host 2, device 0]`, picture correct (no green/purple tint) |
| A codec the GPU cannot decode | `sw > present:d3d11 [host 1 (+N sw-decode) ...]`, plays smoothly |

## 3. Zero-copy counters

`renderer ... frame_path ... (hw_direct N | hw_download 0 | ...)`: with `d3d11va(keep)` plans `hw_download` must stay 0 and
`hw_direct` must grow. The `copies` row says `planned host 0, device 0 | measured hw_direct ..., hw_download 0, software 0`
with no `MISMATCH` line.

## 4. VideoProcessor features

| Action | Expected |
|---|---|
| Filters > Rotate 90 / flips / brightness + contrast | plan shows `absorbed:transpose` / `absorbed:eq`, vf empty, `presenter rotate 90, eq ...`; picture correct |
| Gamma != 1 | presenter route `shader` (VP cannot do gamma) |
| Presenter line | `presenter d3d11 direct N uploaded 0`, `route vp` for plain playback; `adapter <GPU> (vendor 0x...., luid ...)`; no `fallbacks` line |

## 5. D3D11 filters, IVTC and deinterlace

| Setting | Expected |
|---|---|
| Deinterlace On, priority Efficiency, 1080i | `deinterlace bwdif_d3d11 (d3d11va); priority efficiency: GPU deinterlacer -> bwdif_d3d11`; no combing |
| Deinterlace On, priority Quality | same filter (GPU is always bwdif) |
| Deinterlace On with Settings > Hardware decoding = Off | `deinterlace yadif (CPU); priority efficiency: no GPU deinterlacer -> yadif` (Quality: `bwdif`) |
| Output rate = One frame per field | ~59.94 fps in the fps line |
| IVTC on telecined MPEG-2 | `IVTC: ivtc_d3d11 (d3d11va), residual-combing fallback unavailable`, ~23.976 fps, no duplicate frames on pans |
| No driver deinterlacer | the filter graph never contains `deinterlace_d3d11`, `deinterlace_qsv`, `vpp_qsv`, `vpp_amf` |
| Seek and frame-step (`.` / `,`) with IVTC on | works; note the backward-step time if it feels slow (known upstream shader-compile cost) |

If a D3D11 filter fails on your driver the overlay shows `pipeline ... fallback: filter:ivtc_d3d11` and a CPU or other plan;
report the log lines around it.

## 6. Captions and Acrylic

ARIB recording: captions appear, positioned and coloured as on NVIDIA; the bar's Acrylic background blurs the video
(`acrylic: N regions, x.x ms`, no `solid fallback`). Toggle full screen and the mini player: both still correct.

## 7. Forced fallbacks

Start the app once per row (`aribplayer.exe --console <switch> <file>`); the picture must stay correct and the presenter
`fallbacks` line must list the expected entry.

| Switch | Expected `fallbacks` entry |
|---|---|
| `--d3d11-force-caps=decoder_srv=0` + `--d3d11-route=shader` | `force_caps:nv12_srv=0`, `force_caps:p010_srv=0`, `decoder_pool_no_srv` |
| `--d3d11-force-caps=p010=0` (HEVC 10-bit) | `force_caps:p010=0`, `p010_vp_unsupported:shader` |
| `--d3d11-force-caps=p010=0,shader=0` (HEVC 10-bit) | `p010_vp_unsupported:download` |
| `--d3d11-force-caps=rotate=0` + Rotate 90 | rotation moves into the graph (`transpose_d3d11` or `transpose`), picture correct |
| `--d3d11-route=shader` | `route shader`, `route_forced:shader` |
| `--d3d11-force-caps=p010=0 --d3d11-route=vp` (HEVC 10-bit) | `route_forced_unavailable:vp` |
| `--d3d11-debug` (needs Settings > System > Optional features > Graphics Tools) | `d3d11 debug layer on, 0 messages`; any `d3d11-debug:` log line is a bug report |
| `--no-cuda` (NVIDIA only) | plans as above with `d3d11va`, never `cuda` |

## 8. Device loss

While playing, update or restart the graphics driver (Device Manager > disable/enable the display adapter, or install a driver).
Expected: a short flicker, a toast `Graphics device reset`, playback resumes at the same position (paused stays paused), the
log has `gpu: device loss: ...` and `gpu: UI rebuilt: ok ... ms after detection`. Repeat three times quickly: after the third,
one notice about the graphics fallback, and the app keeps working (software playback).
