#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=00-setup-env.sh
source "$SCRIPT_DIR/00-setup-env.sh"

src="$SOURCE_DIR/jellyfin-ffmpeg"
build="$BUILD_DIR/ffmpeg"
[[ -d "$src" ]] || { printf 'Run 01-fetch.sh first.\n' >&2; exit 1; }
rm -rf "$build"
mkdir -p "$build"

ffmpeg_arch_flags=(--arch="$FFMPEG_ARCH" --cpu="$FFMPEG_CPU")
if [[ "$FFMPEG_ENABLE_NEON" == 1 ]]; then
    ffmpeg_arch_flags+=(--enable-neon)
fi
if [[ "$FFMPEG_ENABLE_THUMB" == 1 ]]; then
    ffmpeg_arch_flags+=(--enable-thumb)
fi

(
    cd "$build"
    "$src/configure" \
        --prefix="$INSTALL_PREFIX" \
        --target-os=android "${ffmpeg_arch_flags[@]}" \
        --enable-cross-compile --cross-prefix="$TOOLCHAIN_BIN/${TARGET_TRIPLE}-" \
        --cc="$CC" --cxx="$CXX" --ar="$AR" --ranlib="$RANLIB" --strip="$STRIP" --nm="$NM" \
        --sysroot="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/sysroot" \
        --extra-cflags="-I$INSTALL_PREFIX/include -DCL_TARGET_OPENCL_VERSION=120" \
        --extra-ldflags="-L$INSTALL_PREFIX/lib -ldl -llog -Wl,-z,max-page-size=16384" \
        --pkg-config=pkg-config \
        --pkg-config-flags="--static" \
        --enable-pic --enable-shared --disable-static \
        --disable-programs --disable-doc --disable-debug --disable-network --disable-autodetect --disable-everything \
        --enable-avutil --enable-avcodec --enable-avformat --enable-avfilter --enable-swresample --enable-swscale \
        --enable-jni --enable-mediacodec --enable-opencl --enable-libaribcaption \
        --enable-demuxer=mpegts \
        --enable-decoder=mpeg2video --enable-decoder=mpeg2_mediacodec \
        --enable-decoder=h264 --enable-decoder=h264_mediacodec \
        --enable-decoder=aac --enable-decoder=aac_latm --enable-decoder=libaribcaption \
        --enable-parser=mpegvideo --enable-parser=h264 --enable-parser=aac --enable-parser=aac_latm \
        --enable-bsf=h264_mp4toannexb --enable-bsf=extract_extradata \
        --enable-filter=buffer --enable-filter=buffersink --enable-filter=format --enable-filter=scale \
        --enable-filter=hwupload --enable-filter=hwdownload \
        --enable-filter=ivtc_opencl --enable-filter=bwdif_opencl --enable-filter=bwdif \
        --enable-filter=yadif --enable-filter=fieldmatch --enable-filter=decimate
    make -j"$(nproc)"
    make install
)

# --disable-programs intentionally omits ffmpeg/ffprobe.  Check the generated
# configuration instead and stop before JNI installation on any missing feature.
components="$build/config_components.h"
[[ -f "$components" ]] || { printf 'Missing %s after FFmpeg configure\n' "$components" >&2; exit 1; }
for component in \
    CONFIG_MPEGTS_DEMUXER \
    CONFIG_H264_MEDIACODEC_DECODER \
    CONFIG_MPEG2_MEDIACODEC_DECODER \
    CONFIG_LIBARIBCAPTION_DECODER \
    CONFIG_IVTC_OPENCL_FILTER \
    CONFIG_BWDIF_OPENCL_FILTER \
    CONFIG_FIELDMATCH_FILTER \
    CONFIG_DECIMATE_FILTER \
    CONFIG_BWDIF_FILTER; do
    if ! grep -Eq "^#define ${component} 1$" "$components"; then
        printf 'Required FFmpeg component is disabled or missing: %s\n' "$component" >&2
        exit 1
    fi
done
printf 'Required FFmpeg components are enabled.\n'
