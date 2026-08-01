#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=00-setup-env.sh
source "$SCRIPT_DIR/00-setup-env.sh"

jni_dir="$THIRD_PARTY_DIR/../player-native/src/main/jniLibs/$ANDROID_ABI"
mkdir -p "$jni_dir"

for library in libavcodec.so libavformat.so libavfilter.so libavutil.so libswresample.so libswscale.so libaribcaption.so; do
    source_path="$INSTALL_PREFIX/lib/$library"
    [[ -f "$source_path" ]] || { printf 'Missing built library: %s\n' "$source_path" >&2; exit 1; }
    install -m 0644 "$source_path" "$jni_dir/$library"
    "$STRIP" --strip-unneeded "$jni_dir/$library"
done

# libc++_shared.so is intentionally NOT installed here: AGP packages it
# automatically from the NDK because CMake builds with ANDROID_STL=c++_shared,
# and a jniLibs copy would collide with it at merge time.
rm -f "$jni_dir/libc++_shared.so"

# CMake consumes ABI-specific sysroots first. Preserve the original arm64 path
# as a copy for existing local workflows that refer to it directly.
rm -rf "$SYSROOT_OUTPUT_DIR"
mkdir -p "$(dirname "$SYSROOT_OUTPUT_DIR")"
cp -a "$INSTALL_PREFIX" "$SYSROOT_OUTPUT_DIR"
if [[ "$ANDROID_ABI" == arm64-v8a ]]; then
    legacy_sysroot="$THIRD_PARTY_DIR/prebuilt/sysroot"
    rm -rf "$legacy_sysroot"
    cp -a "$INSTALL_PREFIX" "$legacy_sysroot"
fi

printf 'Installed JNI libraries in %s and sysroot in %s:\n' "$jni_dir" "$SYSROOT_OUTPUT_DIR"
du -h "$jni_dir"/*.so
