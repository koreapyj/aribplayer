# mac/ — macOS port

The portable `src/Core`, `src/Ui` widgets, and `src/App` shell, pages and features are compiled as-is.
The Mac host implements `VideoPlatform`, `FFPPresenter`, `AppContext`, the renderer and the theme.

## Requirements

- macOS 13 or later on Apple Silicon; Xcode with the Metal Toolchain component.
- `brew install cmake ninja nasm pkg-config quilt autoconf automake libtool coreutils`; Python 3 for fixtures.
- Fixture generation requires VideoToolbox H.264 and HEVC Main10 encoding, including 8K.
- The FFmpeg dist at `ffmpeg/dist/darwin-arm64` (dylibs, `ffplay.h`, `include/`, `ffmpeg-caps.inc`).
  `build-app.sh` builds it if missing; see [the engine build](../ffmpeg/darwin/README.md).

## Build

Run these commands from the repository root, with Xcode selected for every build:

```sh
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
mac/app/build-app.sh
mac/app/build-app.sh --version 1.2.3 --sign "Developer ID Application: NAME (TEAM)"
# Development bundle (build the dist and presenter first):
cmake -S mac/app -B .tmp/build-mac-app -G Ninja && cmake --build .tmp/build-mac-app
```

The release outputs are `.tmp/dist-mac/ARIB Player.app` and `ARIB-Player-<version>.dmg`.
The default signature is ad hoc; `--sign` applies Developer ID with hardened runtime and timestamp.
Notarize with `xcrun notarytool submit PATH.dmg --keychain-profile PROFILE --wait`,
then `xcrun stapler staple PATH.dmg`. The bundle ID is `kr.dcmys.aribplayer` (`ARIBPLAYER_BUNDLE_ID`).

## Test

Generate fixtures in an empty directory using the dist's FFmpeg and VideoToolbox encoders:

```sh
MEDIA="$(mktemp -d /tmp/aribplayer-media.XXXXXX)"
mac/app/make-testmedia.sh "$MEDIA"
mac/app/selftest.sh ".tmp/dist-mac/ARIB Player.app" "$MEDIA"
# An installed bundle uses the same tests:
mac/app/selftest.sh "/Applications/ARIB Player.app" "$MEDIA"
make -C mac/presenter && make -C mac/presenter test MEDIA="$MEDIA"
cmake -S mac/platform -B .tmp/build-mac-platform -G Ninja
cmake --build .tmp/build-mac-platform && .tmp/build-mac-platform/wvp_platform_mac_test
```

The generator produces hard-telecined 1080i MPEG-2, 1080p HEVC Main10, HLG-tagged HEVC,
1080p H.264/AAC TS, and 8K HEVC Main10 for the presenter's size test. `subs/` contains
`clip.ts`, Korean/English SRT, Japanese SAMI, ASS and `extra.srt` subtitle fixtures.
HLG is tagged synthetic test imagery, not a photometrically mastered HDR reference.
The app test generates an ARIB caption TS and checks UI/playback, two-launch persistence and `open -a`.
For real ARIB broadcasts, run the app with `--selftest --media DIR --subs-dir DIR/subs --caption-clip PATH.ts`.
`SUBS_DIR` overrides the wrapper's subtitle fixture directory.

The portable `PlayerCore.LogicTests` target uses the same clock compatibility header:

```sh
cmake -S . -B .tmp/build-portable -G Ninja -DWVP_PORTABLE_CORE=ON \
  "-DCMAKE_CXX_FLAGS=-include $PWD/mac/core/include/wvpmac/libcxx_clock_cast.h"
cmake --build .tmp/build-portable
.tmp/build-portable/bin/RelWithDebInfo/PlayerCore.LogicTests/PlayerCore.LogicTests NAME_FILTER
```

Use name filters: the two tests using `LocalHttpServer` hang on macOS during server shutdown.
CI uses macOS 26 (Core's floating-point `std::from_chars`) and presenter `--no-framedrop`; explicit skips cover those two Core cases, presenter h on Paravirtual GPUs (wall-clock timeout drops), only presenter d's `present()` latency bounds on those GPUs (`ARIBPLAYER_PRESENTER_SKIP_PRESENT_LATENCY=1`; functional assertions still run), only the self-test's real-time A-V bounds on those GPUs (`ARIBPLAYER_SELFTEST_SKIP_AV_REALTIME=1`: median |A-V| < 40 ms and max < 60 ms over 2 s; A-V reporting and a 100 ms median bound still run), macOS-owned menu icons (`ARIBPLAYER_SELFTEST_SKIP_SYSTEM_MENU_ICONS=1`), and hardware/GUI-dependent tests when runtime probes report unavailable prerequisites; local assertions remain enabled.

## Layout

| Directory | Target / role |
|---|---|
| `core/` | `wvp_core_logic`, `wvp_core_ffplay`, `wvp_core_buildcaps`: Core with the POSIX platform layer |
| `presenter/` | `libffpmetalpresenter`: Metal presentation and its standalone harness |
| `platform/` | `wvp_platform_mac`: adapters, VideoToolbox capabilities and presenter creation |
| `ui/` | `pui_mac`: CoreGraphics/CoreText renderer, SF Symbols, Mac theme and input mapping |
| `app/` | AppKit host, build/packaging scripts, media generator and app tests |

The bundle's `MacOS/` holds the executable; `Frameworks/` holds libffplay, libav*/libsw* and
libffpmetalpresenter; `Resources/` holds the icon, caption font, notices and `licenses/`.

## Constraints

- Apple's libc++ lacks `std::chrono::clock_cast`; `core/` force-includes `wvpmac/libcxx_clock_cast.h`
  into `wvp_core_logic`. The header is empty on a standard library that provides it.
- Metal adapters use registryID halves as a synthetic PCI location so `GroupPhysicalGpus` retains them.
- `BuildCapabilitiesMac` adds the VideoToolbox decoder rows that `ffcapsdump` does not emit;
  otherwise `MergeBuildCapabilities` removes those decoders.
- libaribcaption's CoreText provider uses family names. `CaptionFont.mm` registers the bundled TTF
  per process and passes its family.
- SDL's audio initialization installs SIGINT/SIGTERM handlers; `main.mm` reclaims both with dispatch sources.
- `ui/src/TextBoxMac.mm` mirrors `src/Ui/src/TextBox.cpp` except its clipboard helpers; keep them in sync.
- `CGRenderer` maps pui's Fluent text sizes 14/12/20/28 to Mac sizes 13/11/17/22.
