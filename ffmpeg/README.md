# ffmpeg/ - ffplay / libffplay build

Builds `libffplay.dll`, `ffplay.exe`, `ffmpeg.exe`, `ffprobe.exe`, `ffcapsdump.exe` and the `av*/sw*` DLLs for
win64 from [builder-ffmpeg](https://github.com/koreapyj/builder-ffmpeg) (pinned in `builder.lock`) on top of
jellyfin-ffmpeg `v8.1.3-1` (FFmpeg 8.1), with our patches layered on top. The result lands in `dist/win64/`.
The build is GPL (jellyfin enables `--enable-gpl`); fdk-aac is dropped so it can be redistributed.

## Layout

```
builder.lock                 builder-ffmpeg URL + pinned commit, jellyfin FF_REPO/FF_REF
patches/                     build/dependency/packaging patches (git apply, after builder's 0001-*), ours: 1001-1002
source-patches/              ffmpeg-source patches (appended to debian/patches/series, applied last
                             via quilt), ours: 1002-1017 (+1006a), grouped by concern:
                             FFmpeg library changes (separate from libffplay integration)
                               1002-libaribcaption-clear-and-type.patch = clear events yield an empty subtitle, caption_type option
                               1003-libaribcaption-runtime-rerender.patch = runtime canvas_size, caption history (10 min / 5 MiB) and re-render
                                                                           (AV_SUBTITLE_FLAG_RERENDER)
                               1004-libaribcaption-directwrite-file-font.patch = file: fonts with the DirectWrite font provider
                               1005-libaribcaption-outline-aa.patch = outline anti-aliasing
                               1006-mpegts-arib-superimpose.patch = ARIB superimpose component tags
                               1006a-mpegts-arib-detection.patch = ARIB TS detection (two-hit rule), component_tag metadata for every ES, merge_pmt_versions for ARIB
                                                                      (sorts between 1006 and 1007 in the C-locale series order; there is no free number there)
                               1007-libavcodec-d3d11va-decoder-bind-flags.patch = AVD3D11VADeviceContext.decoder_bind_flags[_p010] (per format), pool retry without
                                                                      the extra bits (decoder_bind_fallbacks, test knob)
                               1008-libavutil-cuda-pool-hook.patch = optional CUDA frame-pool allocator hook + buffer recycle predicate
                             libffplay (fftools/ffplay*.c, ffplay.h)
                               1009-libffplay-core-api.patch = ffplay.c as a library: API, hosted-window resize, debug info + fps stats,
                                                              hwaccel fix, ffp_probe_caps / hwframes open option
                               1010-libffplay-presenters.patch = presenter vtable (SDL), D3D11 VideoProcessor presenter,
                                                                presenter input, hw sink for uploaded system frames, adapter (LUID) /
                                                                debug layer / force_caps open options, ffp_probe_caps_opts, device loss
                               1011-libffplay-d3d11-cuda-shader.patch = CUDA import (zero-copy / per-plane copy, remap retirement),
                                                                       D3D11VA/system shader route, per-format decoder SRV probe, P010 /
                                                                       other-format fallbacks, d3d11_route
                               1012-libffplay-captions.patch = ARIB caption / superimpose streams, overlay and re-render
                               1013-libffplay-acrylic-backdrop.patch = D3D11 Acrylic backdrop regions
                               1014-libffplay-playback-control.patch = frame step, time seek, post-seek sync, pause latency
                               1015-libffplay-static-caps.patch = ffcapsdump (what the build fixes: filters, the decoder of
                                                                 each hwaccel, build features; compiled into the app), the
                                                                 runtime probe keeps only machine-dependent results, no API version
                               1016-libffplay-arib-audio.patch = ARIB audio model: audio 1 / audio 2 decoded in parallel (tsreadex roles, rebind on PMT change),
                                                              selector with preference + coverage + 1 s hold + 5 ms crossfade, starvation, silence fill,
                                                              ffp_set_audio_preference (ffplay_arib_audio.h = the pure decisions, ffmpeg/tests/ = table tests)
                               1017-libffplay-keyframe-seek.patch = ffp_seek_keyframe: previous / next keyframe relative to the displayed frame (key packets read with a
                                                              private demuxer, landing on the exact adjacent key packet), FFP_EVENT_KEYSEEK_DONE;
                                                              ffp_skip + every timed seek (ffp_seek, seek bar, ss) land on the key packet closest to the target
scripts/build-ffplay.sh      the build (Linux/WSL), mirrors builder's CI `build-windows` job
scripts/author.sh            quilt authoring flow + incremental build
scripts/lib.sh               shared helpers
build-ffplay.ps1             Windows wrapper (calls WSL, digest-cached)
.work/  (ignored)            build.log only (the work dir lives in WSL, see Work dir below)
dist/win64/  (ignored)       output + .stamp
```

## Patch ordering and namespaces

* `patches/*.patch` - build/dependency/packaging patches against the jellyfin checkout.
  `git apply`-ed in sorted order *after* builder's own `patches/`.
  * `1001-win64-enable-ffplay-sdl2.patch` builds SDL2 2.32.8 statically (cmake, into `${FF_DEPS_PREFIX}`),
    flips `--enable-ffplay --enable-sdl2`, replaces `--enable-libfdk-aac` with `--disable-libfdk-aac` (the
    fdk-aac build is removed), and adds a second zip `jellyfin-ffmpeg_*-ffplay_win64.zip` with all exe/dll
    plus `libffplay.dll.a` and `ffplay.h` (taken from `${FF_PREFIX}/lib` and `${FF_PREFIX}/include/libffplay`).
  * `1002-libaribcaption-directwrite-file-font.patch` patches the external libaribcaption dependency
    before it is built, adding DirectWrite `file:` font support. It also pins libaribcaption v1.1.1 with
    `-DARIBCC_USE_FREETYPE=ON` (fontconfig stays off) instead of builder's DirectWrite-only v1.1.2: the FreeType
    renderer of v1.1.1 is what the Android app uses, and v1.1.2 moves glyphs (new ideographic baseline). It complements source patch
    `1004-libaribcaption-directwrite-file-font.patch`, which selects the font provider and renderer
    in FFmpeg's libaribcaption wrapper.
* `source-patches/*.patch` - ffmpeg source patches. Appended to `debian/patches/series` after jellyfin's
  (and builder's `0002`, `0003`, `0005`-`0007`), so they apply last. Authored with quilt (see below). The library changes
  and libffplay integration are grouped separately, but the series is ordered and may have
  dependencies between patches; individual patches are not guaranteed to apply or build alone.
* Build patches (`1001-1002`) and source patches (`1002-1017`) use separate numbering namespaces.
  The `1000+` range keeps our patches separate from builder's patches.

For win64, `1009-libffplay-core-api.patch` provides the build/install integration for
`$(BINDIR)/libffplay.dll`, `$(BINDIR)/ffplay.exe`, `$(LIBDIR)/libffplay.dll.a` and
`$(INCDIR)/libffplay/ffplay.h` on `make install`.

## Build

Needs WSL (Ubuntu-26.04) with Docker usable **without sudo**, plus `git make unzip` (`quilt` for authoring).
Nothing is installed automatically; missing prerequisites are reported with install guidance.

```powershell
pwsh ffmpeg\build-ffplay.ps1                # skip if dist\win64\.stamp matches the input digest
pwsh ffmpeg\build-ffplay.ps1 -Force         # rebuild
pwsh ffmpeg\build-ffplay.ps1 -CheckOnly     # prerequisites + patch apply check only
# or directly in WSL:  ffmpeg/scripts/build-ffplay.sh [--force] [--check-only] [--work-dir DIR] [--keep-dev-image]
```

The jellyfin build recompiles ~30 dependencies on every run (1h+).

### Work dir

There is exactly one work dir (clones `builder-ffmpeg/` and `jellyfin-ffmpeg.pristine/`, the build tree and the quilt
tree `quilt-tree/`), defined once as `FFP_WORK_DIR_DEFAULT` in `scripts/lib.sh`:
`~/.cache/windows-video-player/ffmpeg81` inside WSL (far faster than `/mnt/c`). `author.sh` and `build-ffplay.sh`
(and so the `.ps1` wrapper and CI) all use it. The only way to use another one is an explicit
`--work-dir DIR` (`-WorkDir` in the wrapper); CI passes `"$RUNNER_TEMP/ffmpeg-work"`. There is no other default or fallback.

Both scripts reject unknown options, typos and surplus arguments with their usage text (exit 2) before touching
anything. `author.sh` takes `--work-dir` only BEFORE the command (`author.sh --work-dir DIR update`; after the
command it is an error). `author.sh prepare` creates the work dir; every other `author.sh` command fails if the dir
or its quilt tree is missing.
Stamp = sha256 of `builder.lock` + `build-ffplay.ps1` + `scripts/*.sh` + `patches/*.patch` + `source-patches/*.patch` (computed by
`build-ffplay.sh --print-digest`; the same digest is the CI cache key, so editing any script, the wrapper or a
patch invalidates it). `dist/win64/licenses/ffmpeg/` receives FFmpeg's `COPYING.GPLv2`, `COPYING.GPLv3`,
`COPYING.LGPLv2.1`, `COPYING.LGPLv3` and `LICENSE.md` from the staged source after extraction. The script also
runs natively on Linux (CI): `bash ffmpeg/scripts/build-ffplay.sh --work-dir "$RUNNER_TEMP/ffmpeg-work"`.

`dist/win64/ffcapsdump.exe` is a build-time tool, not shipped: the app build runs it to compile what this FFmpeg build
fixes (filters, the decoder of each hwaccel, libffplay features) into the app (`src/Core/CMakeLists.txt`,
`BuildCapabilities`).

## Author source patches

```bash
scripts/author.sh prepare [--no-git]       # fresh tree, all patches staged and pushed (quilt); --no-git reuses the cached clones without running git
scripts/author.sh quilt new 1015-foo.patch # or: quilt push/pop <existing patch>
scripts/author.sh quilt add fftools/ffplay.c
$EDITOR ~/.cache/windows-video-player/ffmpeg81/quilt-tree/fftools/ffplay.c   # <work dir>/quilt-tree
scripts/author.sh quilt refresh
scripts/author.sh update                   # copy our patches back to source-patches/
scripts/author.sh refresh                  # rebase our patches against current source, then update
```

### Faster loop (incremental, no dependency rebuild)

Once, after a full build: `build-ffplay.sh --keep-dev-image` commits the finished build container as docker
image `ffplay-dev:win64` (deps in `/opt/ffdeps`, built ffmpeg tree in `/ffmpeg`). Then
`scripts/author.sh build` copies the files touched by our patches into a persistent container
(`ffplay-dev-work`), runs `make && make install` there (only changed files recompile) and copies the
results to `dist/win64` (marked `.dev-build`). `author.sh dev-reset` discards the container's state,
`author.sh dev-shell` opens a shell in it. After the loop, run the normal full build to validate.
