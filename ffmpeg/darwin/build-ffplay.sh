#!/usr/bin/env bash
# Build libffplay.dylib / ffmpeg / ffprobe / ffcapsdump + the shared libav*/libsw* dylibs
# natively on macOS (Apple Silicon), from the same sources as the win64 build: jellyfin-ffmpeg
# (FF_REF in ../builder.lock) + builder-ffmpeg's source-patches (BUILDER_COMMIT) + our
# ../source-patches + the darwin-only ./source-patches (1101 dylib link,
# 1102 ivtc_videotoolbox). libffplay is the pure engine of ../source-patches/1009 (no window, no renderer:
# the host implements FFPPresenter; the macOS one is mac/presenter/), so there is no ffplay program.
# A lean native configure build (no docker, no jellyfin dependency set): static SDL2 (audio only),
# libaribcaption (CoreText), and libass (CoreText, with FreeType / FriBidi / HarfBuzz) are built from
# pinned sources into the work dir. libass is required by ../source-patches/1012.
#
#   darwin/build-ffplay.sh [--force] [--work-dir DIR] [--jobs N] [--check-only] [--print-digest]
#                          [--src-tree DIR]
#
#   --force           rebuild even when dist/darwin-arm64/.stamp matches
#   --work-dir DIR    clones, deps, staged tree, build tree; at most once. Absolute path (leading ~ ok).
#                     Default: ~/.cache/windows-video-player/ffmpeg81-darwin-arm64
#   --jobs N          make -j (default: hw.ncpu)
#   --check-only      stage the tree and run the quilt series (patch apply check); no build
#   --print-digest    print the input digest (builder.lock, lib.sh, this script, source-patches,
#                     darwin/source-patches), exit
#   --src-tree DIR    authoring: build an existing, already quilt-pushed tree (e.g. a quilt tree with
#                     extra patches under development) instead of staging a fresh one. The result is
#                     not stamped.
#
# Needs: Xcode with the Metal Toolchain component (DEVELOPER_DIR defaults to
# /Applications/Xcode.app/Contents/Developer; the command line tools alone have no `metal`),
# git, curl, quilt, cmake, ninja, nasm, pkg-config, autoconf, automake, libtool
# (brew install cmake ninja nasm pkg-config quilt autoconf automake libtool).
# Nothing is installed automatically.
#
# Output: ffmpeg/dist/darwin-arm64/{libffplay.dylib,ffplay.h,ffmpeg,ffprobe,ffcapsdump,
#         ffmpeg-caps.inc,libav*/libsw*.dylib,include/libav*/,
#         licenses/{ffmpeg,libass,freetype,fribidi,harfbuzz}/*,.stamp}. Every dylib
#         has an @rpath/ install name; the executables carry rpath @executable_path, libffplay
#         @loader_path, so the directory is self-contained (system frameworks only). include/ holds
#         the installed FFmpeg headers (what a presenter / host builds against, like win64's
#         include/libavutil).
#         Log: ffmpeg/.work/build-darwin.log
set -euo pipefail

DARWIN_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../scripts/lib.sh
. "${DARWIN_DIR}/../scripts/lib.sh"

# Pinned dependencies (same versions as the win64 build).
SDL2_VER=2.32.8      # ffmpeg/patches/1001-win64-enable-ffplay-sdl2.patch
SDL2_SHA256=0ca83e9c9b31e18288c7ec811108e58bac1f1bb5ec6577ad386830eac51c787e
ARIBCC_REPO=https://github.com/xqq/libaribcaption.git
ARIBCC_TAG=v1.1.2     # builder-ffmpeg patches/0001-add-libaribcaption-support.patch
ARIBCC_COMMIT=c64c23b8905ba514b87c9789269e9f66f949ffe0
# Same releases as jellyfin's docker-build-win64.sh, with native static builds below.
FREETYPE_TAG=VER-2-14-3
FREETYPE_COMMIT=0a0221a1347e2f1e07c395263540026e9a0aa7c7
FRIBIDI_TAG=v1.0.16
FRIBIDI_COMMIT=68162babff4f39c4e2dc164a5e825af93bda9983
HARFBUZZ_TAG=14.2.1
HARFBUZZ_COMMIT=56feae4035bdd48f62ba2b8d8c16232d4d89b3a4
LIBASS_TAG=0.17.5
LIBASS_COMMIT=4a05d8127f525943ebf45fdc6497c9e665947f0d
export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-12.0}"   # jellyfin's defaults-mac.sh
ARCH=arm64
DIST="${FFP_ROOT}/dist/darwin-${ARCH}"
WORK_DEFAULT="${HOME}/.cache/windows-video-player/ffmpeg81-darwin-${ARCH}"

FORCE=0; CHECK_ONLY=0; PRINT_DIGEST=0; WORK_ARG=""; HAVE_WORK=0; SRC_TREE=""
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 8)"
optval() { [ $# -ge 2 ] && [ -n "$2" ] || usage_error "$0" "$1 needs a value"; }
while [ $# -gt 0 ]; do
    case "$1" in
        --force) FORCE=1 ;;
        --check-only) CHECK_ONLY=1 ;;
        --print-digest) PRINT_DIGEST=1 ;;
        --work-dir)
            [ "${HAVE_WORK}" = 0 ] || usage_error "$0" "--work-dir given more than once"
            optval "$@"; HAVE_WORK=1; WORK_ARG="$2"; shift ;;
        --jobs) optval "$@"; JOBS="$2"; shift ;;
        --src-tree) optval "$@"; SRC_TREE="$(cd "$2" && pwd)"; shift ;;
        -h|--help) awk 'NR > 1 { if ($0 !~ /^#/) exit; sub(/^# ?/, ""); print }' "$0"; exit 0 ;;
        -*) usage_error "$0" "unknown option: $1" ;;
        *)  usage_error "$0" "unexpected argument: $1" ;;
    esac
    shift
done

# sha256 over builder.lock, scripts/lib.sh (sourced), this script, source-patches/*.patch,
# darwin/source-patches/*.patch (same framing as lib.sh's compute_digest).
darwin_digest() (
    cd "${FFP_ROOT}"
    export LC_ALL=C
    files=(builder.lock scripts/lib.sh darwin/build-ffplay.sh)
    for f in source-patches/*.patch darwin/source-patches/*.patch; do [ -e "$f" ] && files+=("$f"); done
    sorted=$(printf '%s\n' "${files[@]}" | sort)
    {
        while IFS= read -r f; do printf '%s\n' "$f"; cat "$f"; printf '\n'; done <<< "${sorted}"
    } | shasum -a 256 | cut -d' ' -f1
)

if [ "${PRINT_DIGEST}" = 1 ]; then darwin_digest; exit 0; fi

[ "$(uname -s)" = Darwin ] || die "this script runs on macOS only"
[ "$(uname -m)" = "${ARCH}" ] || die "only native ${ARCH} builds are supported (host is $(uname -m))"

if [ -n "${WORK_ARG}" ]; then WORK="$(resolve_work_dir "${WORK_ARG}")"; else WORK="${WORK_DEFAULT}"; fi
mkdir -p "${WORK}" "${FFP_ROOT}/.work"
WORK="$(cd "${WORK}" && pwd)"
LOG="${FFP_ROOT}/.work/build-darwin.log"
exec > >(tee -a "${LOG}") 2>&1
log "=== darwin/build-ffplay.sh $* (work dir: ${WORK}) ==="

DIGEST="$(darwin_digest)"
load_lock

if [ "${FORCE}" = 0 ] && [ "${CHECK_ONLY}" = 0 ] && [ -z "${SRC_TREE}" ] && [ -f "${DIST}/.stamp" ] \
   && [ "$(cat "${DIST}/.stamp")" = "${DIGEST}" ] && [ -f "${DIST}/libffplay.dylib" ] \
   && [ -f "${DIST}/ffcapsdump" ]; then
    log "dist/darwin-${ARCH} is up to date (digest ${DIGEST}); use --force to rebuild."
    exit 0
fi

# ---- prerequisites ---------------------------------------------------------------------------
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
prereq_fail=0
for c in git curl quilt cmake ninja nasm pkg-config make shasum install_name_tool otool autoreconf automake; do
    command -v "$c" >/dev/null 2>&1 || { warn "required command '$c' not found (brew install $c)"; prereq_fail=1; }
done
command -v glibtoolize >/dev/null 2>&1 || command -v libtoolize >/dev/null 2>&1 \
    || { warn "libtoolize not found (brew install libtool)"; prereq_fail=1; }
xcrun -sdk macosx -f metal >/dev/null 2>&1 \
    || { warn "no Metal compiler (xcrun metal) with DEVELOPER_DIR=${DEVELOPER_DIR}: install Xcode and its Metal Toolchain component (xcodebuild -downloadComponent MetalToolchain)"; prereq_fail=1; }
[ "${prereq_fail}" = 0 ] || die "prerequisites missing (see above)."

export QUILT_PATCHES=debian/patches
QUILTRC_FILE="${WORK}/builder-ffmpeg/.quiltrc"

# ---- sources ---------------------------------------------------------------------------------
# stage_darwin_tree DEST: pristine jellyfin checkout + series += sorted(builder source-patches,
# our source-patches, darwin source-patches), quilt push -a. builder's patches/ (docker build
# scripts and debian packaging only) are not applied: nothing of them is used here.
stage_darwin_tree() {
    local dest="$1" p out
    rm -rf "${dest}"; mkdir -p "${dest}"
    tar --exclude=./.git -C "${WORK}/jellyfin-ffmpeg.pristine" -cf - . | tar -C "${dest}" -xf -
    {
        for p in "${WORK}"/builder-ffmpeg/source-patches/*.patch \
                 "${FFP_ROOT}"/source-patches/*.patch "${FFP_ROOT}"/darwin/source-patches/*.patch; do
            [ -e "$p" ] && printf '%s\t%s\n' "$(basename "$p")" "$p"
        done
    } | LC_ALL=C sort -t "$(printf '\t')" -k1,1 | while IFS="$(printf '\t')" read -r name p; do
        [ -e "${dest}/debian/patches/${name}" ] && die "patch name collision: ${name}"
        cp "$p" "${dest}/debian/patches/${name}"
        echo "${name}" >> "${dest}/debian/patches/series"
    done
    log "quilt push -a ($(wc -l < "${dest}/debian/patches/series" | tr -d ' ') patches)"
    out="$(cd "${dest}" && QUILTRC="${QUILTRC_FILE}" quilt push -a 2>&1)" || { printf '%s\n' "${out}"; die "quilt push -a failed"; }
    if printf '%s\n' "${out}" | grep -q 'fuzz'; then
        printf '%s\n' "${out}" | grep -B30 'fuzz' | grep -E '^Applying|fuzz' >&2
        warn "some patches applied with fuzz (see above)"
    fi
}

# ---- dependencies ----------------------------------------------------------------------------
DEPS="${WORK}/deps"
build_sdl2() {
    local stamp="${DEPS}/.sdl2-${SDL2_VER}" tgz="${WORK}/dl/SDL2-${SDL2_VER}.tar.gz" src="${WORK}/dl/SDL2-${SDL2_VER}"
    [ -f "${stamp}" ] && return 0
    log "Building SDL2 ${SDL2_VER} (static)"
    mkdir -p "${WORK}/dl"
    [ -f "${tgz}" ] || curl -sSLf -o "${tgz}" \
        "https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VER}/SDL2-${SDL2_VER}.tar.gz"
    [ "$(shasum -a 256 "${tgz}" | cut -d' ' -f1)" = "${SDL2_SHA256}" ] || die "SDL2 tarball checksum mismatch"
    rm -rf "${src}"; tar -C "${WORK}/dl" -xzf "${tgz}"
    cmake -S "${src}" -B "${src}/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${DEPS}" -DCMAKE_OSX_ARCHITECTURES="${ARCH}" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET}" \
        -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_STATIC_PIC=ON -DSDL_TEST=OFF -DSDL_TESTS=OFF \
        -DSDL_INSTALL_TESTS=OFF >/dev/null
    cmake --build "${src}/build" -j "${JOBS}" >/dev/null
    cmake --install "${src}/build" >/dev/null
    touch "${stamp}"
}
build_aribcaption() {
    local stamp="${DEPS}/.aribcaption-${ARIBCC_COMMIT}" src="${WORK}/dl/libaribcaption"
    [ -f "${stamp}" ] && return 0
    log "Building libaribcaption ${ARIBCC_TAG} (static, CoreText)"
    mkdir -p "${WORK}/dl"; rm -rf "${src}"
    git clone -q -c advice.detachedHead=false --depth 1 -b "${ARIBCC_TAG}" "${ARIBCC_REPO}" "${src}"
    [ "$(git -C "${src}" rev-parse HEAD)" = "${ARIBCC_COMMIT}" ] || die "libaribcaption ${ARIBCC_TAG} is not ${ARIBCC_COMMIT}"
    cmake -S "${src}" -B "${src}/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${DEPS}" -DCMAKE_OSX_ARCHITECTURES="${ARCH}" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET}" \
        -DBUILD_SHARED_LIBS=OFF -DARIBCC_SHARED_LIBRARY=OFF -DARIBCC_USE_CORETEXT=ON \
        -DARIBCC_USE_FREETYPE=OFF -DARIBCC_USE_FONTCONFIG=OFF >/dev/null
    cmake --build "${src}/build" -j "${JOBS}" >/dev/null
    cmake --install "${src}/build" >/dev/null
    touch "${stamp}"
}

# 1012 makes libass a hard ffplay dependency. Keep its whole dependency chain static and local;
# CoreText supplies fonts, so neither fontconfig nor Homebrew runtime libraries are needed.
clone_dep() {
    local name="$1" repo="$2" tag="$3" commit="$4" src="${WORK}/dl/$1"
    mkdir -p "${WORK}/dl"; rm -rf "${src}"
    git clone -q -c advice.detachedHead=false --depth 1 -b "${tag}" "${repo}" "${src}"
    [ "$(git -C "${src}" rev-parse HEAD)" = "${commit}" ] || die "${name} ${tag} is not ${commit}"
}
build_libass() (
    # One stamp covers the chain: updating any pin rebuilds all four libraries.
    stamp="${DEPS}/.libass-$(printf '%s\n' "${FREETYPE_COMMIT}" "${FRIBIDI_COMMIT}" \
        "${HARFBUZZ_COMMIT}" "${LIBASS_COMMIT}" "${MACOSX_DEPLOYMENT_TARGET}" | shasum -a 256 | cut -d' ' -f1)"
    [ -f "${stamp}" ] && exit 0
    log "Building libass ${LIBASS_TAG} (static, CoreText) + FreeType / FriBidi / HarfBuzz"
    export PKG_CONFIG_LIBDIR="${DEPS}/lib/pkgconfig"
    unset PKG_CONFIG_PATH
    export CC=clang CXX=clang++
    export CFLAGS="-O2 -arch ${ARCH} -mmacosx-version-min=${MACOSX_DEPLOYMENT_TARGET}"
    export CXXFLAGS="${CFLAGS}" LDFLAGS="-arch ${ARCH} -mmacosx-version-min=${MACOSX_DEPLOYMENT_TARGET}"
    cmake_flags=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="${DEPS}"
        -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_OSX_ARCHITECTURES="${ARCH}"
        -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET}" -DBUILD_SHARED_LIBS=OFF
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON)

    clone_dep freetype https://github.com/freetype/freetype.git "${FREETYPE_TAG}" "${FREETYPE_COMMIT}"
    src="${WORK}/dl/freetype"
    cmake -S "${src}" -B "${src}/build" "${cmake_flags[@]}" \
        -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON \
        -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON
    cmake --build "${src}/build" -j "${JOBS}"
    cmake --install "${src}/build"

    clone_dep fribidi https://github.com/fribidi/fribidi.git "${FRIBIDI_TAG}" "${FRIBIDI_COMMIT}"
    src="${WORK}/dl/fribidi"
    ( cd "${src}" && NOCONFIGURE=1 ./autogen.sh )
    mkdir -p "${src}/build"
    ( cd "${src}/build" && ../configure --prefix="${DEPS}" --disable-shared --enable-static --with-pic )
    # Only the library and generated Unicode tables; the autotools docs need c2man.
    make -C "${src}/build" -j"${JOBS}" SUBDIRS="gen.tab lib"
    make -C "${src}/build" SUBDIRS="gen.tab lib" install

    clone_dep harfbuzz https://github.com/harfbuzz/harfbuzz.git "${HARFBUZZ_TAG}" "${HARFBUZZ_COMMIT}"
    src="${WORK}/dl/harfbuzz"
    cmake -S "${src}" -B "${src}/build" "${cmake_flags[@]}" \
        -DHB_HAVE_FREETYPE=ON -DFREETYPE_INCLUDE_DIRS="${DEPS}/include/freetype2" \
        -DFREETYPE_LIBRARY="${DEPS}/lib/libfreetype.a" \
        -DHB_BUILD_UTILS=OFF -DHB_BUILD_SUBSET=OFF -DHB_BUILD_RASTER=OFF \
        -DHB_BUILD_VECTOR=OFF -DHB_BUILD_GPU=OFF
    cmake --build "${src}/build" -j "${JOBS}"
    cmake --install "${src}/build"

    clone_dep libass https://github.com/libass/libass.git "${LIBASS_TAG}" "${LIBASS_COMMIT}"
    src="${WORK}/dl/libass"
    ( cd "${src}" && ./autogen.sh )
    mkdir -p "${src}/build"
    ( cd "${src}/build" && ../configure --prefix="${DEPS}" --disable-shared --enable-static \
        --with-pic --enable-coretext --disable-fontconfig --disable-libunibreak )
    make -C "${src}/build" -j"${JOBS}"
    make -C "${src}/build" install
    touch "${stamp}"
)

# ---- main ------------------------------------------------------------------------------------
if [ -n "${SRC_TREE}" ]; then
    TREE="${SRC_TREE}"
    log "Using existing source tree ${TREE} (not staged, result not stamped)"
else
    ensure_builder
    ensure_pristine
    TREE="${WORK}/jellyfin-ffmpeg"
    stage_darwin_tree "${TREE}"
fi
if [ "${CHECK_ONLY}" = 1 ]; then
    log "Patch check OK (digest ${DIGEST})."
    exit 0
fi

build_sdl2
build_aribcaption
build_libass

BUILD="${WORK}/build"
PREFIX="${WORK}/install"
rm -rf "${BUILD}" "${PREFIX}"; mkdir -p "${BUILD}"
# Only our deps prefix is visible to pkg-config (no Homebrew libraries leak in).
export PKG_CONFIG_LIBDIR="${DEPS}/lib/pkgconfig"
unset PKG_CONFIG_PATH
CONFIGURE_FLAGS=(
    --prefix="${PREFIX}"
    --install-name-dir=@rpath
    --extra-version=Jellyfin
    --disable-unstable
    --enable-gpl
    --enable-shared --disable-static
    --enable-pthreads
    --enable-ffplay --enable-sdl2 --enable-libass
    --enable-videotoolbox --enable-audiotoolbox --enable-metal --enable-opencl
    --enable-neon --enable-runtime-cpudetect
    --enable-libaribcaption
    --disable-libxcb --disable-xlib --disable-libjack --disable-indev=jack
    --disable-doc --disable-debug
    --cc=clang
    --arch="${ARCH}"
    --extra-cflags="-I${DEPS}/include -mmacosx-version-min=${MACOSX_DEPLOYMENT_TARGET}"
    --extra-ldflags="-L${DEPS}/lib -mmacosx-version-min=${MACOSX_DEPLOYMENT_TARGET}"
    --extra-ldexeflags="-Wl,-rpath,@executable_path"
    --pkg-config-flags=--static
)
log "configure ${CONFIGURE_FLAGS[*]}"
( cd "${BUILD}" && "${TREE}/configure" "${CONFIGURE_FLAGS[@]}" > configure.out 2>&1 ) \
    || { tail -n 30 "${BUILD}/ffbuild/config.log" 2>/dev/null; die "configure failed (${BUILD}/ffbuild/config.log)"; }
# configure can silently disable an explicitly requested program when a dependency is disabled.
grep -qx 'CONFIG_FFPLAY=yes' "${BUILD}/ffbuild/config.mak" \
    || die "configure disabled ffplay; check its dependencies (${BUILD}/configure.out)"
log "make -j${JOBS}"
START=$(date +%s)
( cd "${BUILD}" && make -j"${JOBS}" >make.out 2>&1 ) \
    || { grep -E 'error|Error' "${BUILD}/make.out" | head -n 40; die "make failed (${BUILD}/make.out)"; }
( cd "${BUILD}" && make install >install.out 2>&1 ) || die "make install failed (${BUILD}/install.out)"
log "build took $(( $(date +%s) - START )) s"

# ---- dist ------------------------------------------------------------------------------------
rm -rf "${DIST}"; mkdir -p "${DIST}/licenses/ffmpeg"
for f in ffmpeg ffprobe ffcapsdump; do
    [ -f "${PREFIX}/bin/${f}" ] || die "${f} not installed"
    cp "${PREFIX}/bin/${f}" "${DIST}/"
done
[ -f "${PREFIX}/lib/libffplay.dylib" ] || die "libffplay.dylib not installed"
[ -f "${PREFIX}/include/libffplay/ffplay.h" ] || die "ffplay.h not installed"
cp "${PREFIX}/include/libffplay/ffplay.h" "${DIST}/"
# the FFmpeg headers a presenter / host builds against (libavutil/hwcontext*.h, avconfig.h, ...)
mkdir -p "${DIST}/include"
for d in libavcodec libavdevice libavfilter libavformat libavutil libswresample libswscale; do
    [ -d "${PREFIX}/include/${d}" ] || die "${d} headers not installed"
    cp -R "${PREFIX}/include/${d}" "${DIST}/include/"
done
# libffplay exports exactly the FFP_API functions of ffplay.h (1101's exported-symbols list)
want="$(sed -n 's/^FFP_API.*FFP_CALL *\(ffp_[A-Za-z0-9_]*\) *(.*/_\1/p' "${DIST}/ffplay.h" | LC_ALL=C sort)"
have="$(nm -gU "${PREFIX}/lib/libffplay.dylib" | awk '$2 == "T" { print $3 }' | LC_ALL=C sort)"
[ "${want}" = "${have}" ] || { diff <(printf '%s\n' "${want}") <(printf '%s\n' "${have}") >&2 || true
    die "libffplay.dylib exports differ from the FFP_API set of ffplay.h (see above: < expected, > exported)"; }
log "libffplay.dylib exports exactly the $(printf '%s\n' "${want}" | wc -l | tr -d ' ') FFP_API functions of ffplay.h"
# lib*.dylib: real files are the versioned names; keep the major-version name (= install name)
# and an unversioned link name for linking.
cp -a "${PREFIX}"/lib/*.dylib "${DIST}/"
for f in "${TREE}"/COPYING* "${TREE}/LICENSE.md"; do
    [ -f "$f" ] || die "FFmpeg license file missing: $f"
    cp "$f" "${DIST}/licenses/ffmpeg/"
done
# The libass dependency chain is linked statically; ship its source-tree licenses too.
for lib in libass freetype fribidi harfbuzz; do
    license_files=(COPYING)
    if [ "${lib}" = freetype ]; then license_files=(LICENSE.TXT docs/FTL.TXT docs/GPLv2.TXT); fi
    mkdir -p "${DIST}/licenses/${lib}"
    for f in "${license_files[@]}"; do
        src="${WORK}/dl/${lib}/${f}"
        [ -f "${src}" ] || die "${lib} license file missing: ${src}"
        cp "${src}" "${DIST}/licenses/${lib}/"
    done
done
for f in "${DIST}"/*.dylib "${DIST}"/ffmpeg "${DIST}"/ffprobe "${DIST}"/ffcapsdump; do
    [ -L "$f" ] && continue
    if otool -L "$f" | tail -n +2 | awk '{print $1}' | grep -vE '^(@rpath/|/usr/lib/|/System/Library/)'; then
        die "$(basename "$f") links something outside @rpath and the system (see above)"
    fi
done
log "ffcapsdump -> ffmpeg-caps.inc"
( cd "${DIST}" && ./ffcapsdump ffmpeg-caps.inc ) || die "ffcapsdump failed"
if [ -z "${SRC_TREE}" ]; then printf '%s\n' "${DIGEST}" > "${DIST}/.stamp"; fi
log "Done. Output in ${DIST} (digest ${DIGEST})"
ls -l "${DIST}"
