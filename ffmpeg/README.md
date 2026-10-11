# ffmpeg/ - ffplay / libffplay build

Builds `libffplay.dll`, `ffmpeg.exe`, `ffprobe.exe`, `ffcapsdump.exe` and the `av*/sw*` DLLs for
win64 from [builder-ffmpeg](https://github.com/koreapyj/builder-ffmpeg) (pinned in `builder.lock`) on top of
jellyfin-ffmpeg `v8.1.3-1` (FFmpeg 8.1), with our patches layered on top. The result lands in `dist/win64/`, together with what the
host's presenter library builds against: the libavutil headers (`include/libavutil/`, incl. the generated
`avconfig.h`), and an MSVC import library of avutil
(`lib/avutil.lib`).
The build is GPL (jellyfin enables `--enable-gpl`); fdk-aac is dropped so it can be redistributed.

## Layout

```
builder.lock                 builder-ffmpeg URL + pinned commit, jellyfin FF_REPO/FF_REF
patches/                     build/dependency/packaging patches (git apply, after builder's 0001-*), ours: 1001-1002
source-patches/              ffmpeg-source patches (appended to debian/patches/series, applied last
                             via quilt), ours: 1002-1010 (+1006a), grouped by concern:
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
                             libffplay (fftools/ffplay*, ffcapsdump.c, Makefile): net patches of the final tree, split by file
                             (ffplay.c is a near-complete rewrite of upstream, so its concerns cannot be split into separate hunks)
                               1008-libffplay-arib-audio-model.patch = ffplay_arib_audio.h: the pure decisions of the ARIB audio model
                                                              (tsreadex roles, selector with preference + coverage + 1 s hold + 5 ms
                                                              crossfade, starvation), table-tested by ffmpeg/tests/
                               1009-libffplay-engine.patch = ffplay.c as the libffplay engine + its C API ffplay.h + the Makefile
                                                              integration (libffplay.dll / .dll.a / ffplay.h on make install; no ffplay
                                                              program): presenter interface (FFPPresenter / FFPOutput, ffp_create,
                                                              ffp_set_output; pictures, overlays and devices go to the host's presenter,
                                                              SDL is used for audio only), events, debug info + fps stats, hwaccel /
                                                              hwframes open options, ffp_probe_caps(devices, n), ARIB caption /
                                                              superimpose layers with re-render, frame step, timed and keyframe seek
                                                              (ffp_seek_keyframe / ffp_skip land on the exact key packet), start_paused,
                                                              ARIB audio 1 / audio 2 decoded in parallel with ffp_set_audio_preference
                               1010-libffplay-ffcapsdump.patch = ffcapsdump (what the build fixes: filters, the decoder of each hwaccel,
                                                              build features; compiled into the app) + its Makefile rules
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
    plus `libffplay.dll.a` and `ffplay.h` (taken from `${FF_PREFIX}/lib` and `${FF_PREFIX}/include/libffplay`),
    `include/libavutil/` (`${FF_PREFIX}/include`, without `hwcontext_cuda.h`) and
    `lib/avutil.lib` (`llvm-dlltool` from `avutil-*.def`). `author.sh build` packages the same layout.
  * `1002-libaribcaption-directwrite-file-font.patch` patches the external libaribcaption dependency
    before it is built, adding DirectWrite `file:` font support. It also pins libaribcaption v1.1.1 with
    `-DARIBCC_USE_FREETYPE=ON` (fontconfig stays off) instead of builder's DirectWrite-only v1.1.2: the FreeType
    renderer of v1.1.1 matches the reference rendering, and v1.1.2 moves glyphs (new ideographic baseline). It complements source patch
    `1004-libaribcaption-directwrite-file-font.patch`, which selects the font provider and renderer
    in FFmpeg's libaribcaption wrapper.
* `source-patches/*.patch` - ffmpeg source patches. Appended to `debian/patches/series` after jellyfin's
  (and builder's `0002`, `0003`, `0005`-`0007`), so they apply last. Authored with quilt (see below). The library changes
  and libffplay integration are grouped separately, but the series is ordered and may have
  dependencies between patches; individual patches are not guaranteed to apply or build alone.
* Build patches (`1001-1002`) and source patches (`1002-1010`) use separate numbering namespaces.
  The `1000+` range keeps our patches separate from builder's patches.

For win64, `1009-libffplay-engine.patch` provides the build/install integration for
`$(BINDIR)/libffplay.dll`, `$(LIBDIR)/libffplay.dll.a` and `$(INCDIR)/libffplay/ffplay.h` on `make install`
(`1010-libffplay-ffcapsdump.patch` adds `$(BINDIR)/ffcapsdump.exe`). `--enable-ffplay` builds libffplay instead of an
ffplay program.

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
scripts/author.sh quilt new 1011-foo.patch # or: quilt push/pop <existing patch>
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
(`ffplay-dev-work`; files our patches delete are removed there), runs `make && make install` there (only changed
files recompile) and packages the results into `dist/win64` in the layout of patch 1001 (marked `.dev-build`, which `build-ffplay` never takes as up to date). `author.sh dev-reset` discards the container's state,
`author.sh dev-shell` opens a shell in it. After the loop, run the normal full build to validate.
