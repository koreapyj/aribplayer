#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=00-setup-env.sh
source "$SCRIPT_DIR/00-setup-env.sh"

src="$SOURCE_DIR/libaribcaption"
build="$BUILD_DIR/libaribcaption"
[[ -d "$src" ]] || { printf 'Run 01-fetch.sh first.\n' >&2; exit 1; }
rm -rf "$build"
cmake -S "$src" -B "$build" \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ANDROID_ABI" -DANDROID_PLATFORM="android-$ANDROID_API" \
    -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
    -DCMAKE_PREFIX_PATH="$INSTALL_PREFIX" \
    -DCMAKE_FIND_ROOT_PATH="$INSTALL_PREFIX" \
    -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH \
    -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=BOTH \
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH \
    -DFREETYPE_LIBRARY="$INSTALL_PREFIX/lib/libfreetype.a" \
    -DFREETYPE_INCLUDE_DIRS="$INSTALL_PREFIX/include/freetype2" \
    -DARIBCC_USE_FREETYPE=ON -DARIBCC_USE_FONTCONFIG=OFF \
    -DARIBCC_SHARED_LIBRARY=ON -DBUILD_SHARED_LIBS=ON
cmake --build "$build" --parallel
cmake --install "$build"

# v1.1.1's CMake package is sufficient for CMake consumers but FFmpeg uses
# require_pkg_config("libaribcaption >= 1.1.1", "aribcaption/aribcaption.h",
# aribcc_context_alloc), so provide the matching pkg-config metadata if needed.
pc="$INSTALL_PREFIX/lib/pkgconfig/libaribcaption.pc"
if [[ ! -f "$pc" ]]; then
    mkdir -p "$(dirname "$pc")"
    cat > "$pc" <<EOF
prefix=$INSTALL_PREFIX
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: libaribcaption
Description: ARIB STD-B24 caption decoder and renderer
Version: $LIBARIBCAPTION_VERSION
Requires.private: freetype2
Libs: -L\${libdir} -laribcaption
Cflags: -I\${includedir}
EOF
fi
PKG_CONFIG_LIBDIR="$INSTALL_PREFIX/lib/pkgconfig" pkg-config --exists 'libaribcaption >= 1.1.1'
