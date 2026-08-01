#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=00-setup-env.sh
source "$SCRIPT_DIR/00-setup-env.sh"

src="$SOURCE_DIR/freetype-${FREETYPE_VERSION}"
build="$BUILD_DIR/freetype"
[[ -d "$src" ]] || { printf 'Run 01-fetch.sh first.\n' >&2; exit 1; }
rm -rf "$build"
mkdir -p "$build"
(
    cd "$build"
    CC="$CC" CXX="$CXX" AR="$AR" RANLIB="$RANLIB" \
        "$src/configure" --host="$TARGET_TRIPLE" --prefix="$INSTALL_PREFIX" \
        --enable-static --disable-shared --with-pic \
        --with-harfbuzz=no --with-png=no --with-brotli=no --with-bzip2=no --with-zlib=no
    make -j"$(nproc)"
    make install
)
[[ -f "$INSTALL_PREFIX/lib/pkgconfig/freetype2.pc" ]] || {
    printf 'FreeType did not install freetype2.pc\n' >&2
    exit 1
}
PKG_CONFIG_LIBDIR="$INSTALL_PREFIX/lib/pkgconfig" pkg-config --exists freetype2
