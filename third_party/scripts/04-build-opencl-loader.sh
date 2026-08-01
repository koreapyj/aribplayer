#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=00-setup-env.sh
source "$SCRIPT_DIR/00-setup-env.sh"

src="$THIRD_PARTY_DIR/opencl-loader"
build="$BUILD_DIR/opencl-loader"
rm -rf "$build"
cmake -S "$src" -B "$build" \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ANDROID_ABI" -DANDROID_PLATFORM="android-$ANDROID_API" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX"
cmake --build "$build" --parallel
cmake --install "$build"
[[ -f "$INSTALL_PREFIX/lib/libOpenCL.a" && -f "$INSTALL_PREFIX/include/CL/cl.h" ]] || {
    printf 'OpenCL loader install is incomplete\n' >&2
    exit 1
}
