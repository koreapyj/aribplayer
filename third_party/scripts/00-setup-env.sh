#!/usr/bin/env bash
# Source this file from every build step; it does not build anything itself.
# shellcheck shell=bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=versions.env
source "$SCRIPT_DIR/versions.env"

export ANDROID_NDK_HOME="${ANDROID_NDK_HOME:-$BUILD_DIR/ndk}"
if [[ -d "$ANDROID_NDK_HOME" ]]; then
    TOOLCHAIN_BIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin"
    [[ -x "$TOOLCHAIN_BIN/${TARGET_TRIPLE}${ANDROID_API}-clang" ]] || {
        printf 'NDK r27c toolchain is missing at %s\n' "$TOOLCHAIN_BIN" >&2
        return 1 2>/dev/null || exit 1
    }
    export PATH="$TOOLCHAIN_BIN:$PATH"
    export CC="$TOOLCHAIN_BIN/${TARGET_TRIPLE}${ANDROID_API}-clang"
    export CXX="$TOOLCHAIN_BIN/${TARGET_TRIPLE}${ANDROID_API}-clang++"
    export AR="$TOOLCHAIN_BIN/llvm-ar"
    export RANLIB="$TOOLCHAIN_BIN/llvm-ranlib"
    export STRIP="$TOOLCHAIN_BIN/llvm-strip"
    export NM="$TOOLCHAIN_BIN/llvm-nm"
fi

# Dependency .pc files use the absolute install prefix already, so do not
# rewrite their paths with PKG_CONFIG_SYSROOT_DIR.
export PKG_CONFIG_LIBDIR="$INSTALL_PREFIX/lib/pkgconfig"
unset PKG_CONFIG_SYSROOT_DIR
