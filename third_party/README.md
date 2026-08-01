# Native dependency build

These scripts author and build the Android arm64-v8a native dependency stack for
ARIB playback. Run them from Ubuntu under WSL, not from Windows.

## Prerequisites

```bash
sudo apt update
sudo apt install build-essential make cmake pkg-config quilt git curl unzip python3
```

The scripts download Android NDK r27c and source releases into `build/`, use API
26 and `arm64-v8a`, and install intermediate artifacts in `build/sysroot`.

## One-command build

From the repository root in WSL:

```bash
bash third_party/scripts/build-all.sh
```

Or run numbered scripts individually in order. `01-fetch.sh` clones fresh,
shallow source trees under `third_party/build/src`, applies Jellyfin's quilt
series, then applies the local IVTC and Android explicit-font patches. Every
script uses `set -euo pipefail`; the FFmpeg step fails if its required demuxer,
MediaCodec decoders, caption decoder, and OpenCL/deinterlacing filters are not
configured.

`06-install-jnilibs.sh` copies and strips the resulting shared libraries and
NDK `libc++_shared.so` into `player-native/src/main/jniLibs/arm64-v8a/`.
