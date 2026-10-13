# ffmpeg/darwin/ — macOS FFmpeg / libffplay build

Native Apple Silicon counterpart of `../scripts/build-ffplay.sh`, producing `dist/darwin-arm64`.
libffplay owns playback, not a window or renderer; the host's Metal `FFPPresenter` is in `mac/presenter/`.

## Layout

- `build-ffplay.sh`: source staging, dependency builds, FFmpeg build and dist validation.
- `source-patches/`: Darwin-only source patches, sorted with builder and shared patches.

## Source patches

- `1101-darwin-libffplay-dylib.patch`: Mach-O libffplay link, install name, rpath,
  FFP_API export list and BINDIR creation before installing ffcapsdump.
- `1102-ivtc-videotoolbox-filter.patch`: Metal backend of `ivtc_common` for NV12/P010
  VideoToolbox frames; kernels correspond to `d3d11/ivtc.hlsl`.

## Build

Requires macOS arm64, Xcode with the Metal Toolchain component, and
`brew install cmake ninja nasm pkg-config quilt autoconf automake libtool`.
Nothing is installed automatically. The log is `ffmpeg/.work/build-darwin.log`.

```sh
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
ffmpeg/darwin/build-ffplay.sh                 # skip when the dist input digest matches
ffmpeg/darwin/build-ffplay.sh --force         # rebuild
ffmpeg/darwin/build-ffplay.sh --check-only    # stage and apply the quilt series only
ffmpeg/darwin/build-ffplay.sh --src-tree DIR  # build an already applied quilt tree, without a stamp
ffmpeg/darwin/build-ffplay.sh --work-dir DIR --jobs N
ffmpeg/darwin/build-ffplay.sh --print-digest
```

The default work directory is `~/.cache/windows-video-player/ffmpeg81-darwin-arm64`.
`MACOSX_DEPLOYMENT_TARGET` defaults to 12.0 for the engine; the app targets 13.0.

1. Stage pinned Jellyfin FFmpeg from `builder.lock`, apply its quilt series, then append
   builder source patches, shared `1002`–`1012` patches (including `1006a`), and Darwin
   `1101`/`1102`, sorted by filename. Rejects fail the build; fuzz produces a warning.
2. Build pinned static SDL2 2.32.8 (audio), libaribcaption 1.1.2 (CoreText), and libass 0.17.5
   with FreeType 2.14.3, FriBidi 1.0.16 and HarfBuzz 14.2.1. Only that dependency prefix is
   visible to pkg-config; libass uses CoreText, without fontconfig or libunibreak.
3. Configure shared libav*/libsw*, GPL, libffplay/SDL2/libass, VideoToolbox, AudioToolbox,
   Metal, OpenCL and libaribcaption with `--install-name-dir=@rpath`; check `CONFIG_FFPLAY=yes`,
   then build and install. Encoders include VideoToolbox, not x264/x265.
4. Package `libffplay.dylib`, `ffplay.h`, `ffmpeg`, `ffprobe`, `ffcapsdump`, `ffmpeg-caps.inc`,
   versioned dylibs/symlinks and FFmpeg headers in `include/`. `licenses/` contains FFmpeg,
   libass, FreeType, FriBidi and HarfBuzz license texts; the input digest is stored in `.stamp`.
5. Check every Mach-O dependency is `@rpath` or a system library, and that
   `nm -gU libffplay.dylib` exports exactly the installed header's FFP_API functions.
