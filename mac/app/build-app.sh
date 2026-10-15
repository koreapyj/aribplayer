#!/bin/bash
# One command from a clean checkout to "ARIB Player.app" and a .dmg:
#   mac/app/build-app.sh [--version X.Y.Z] [--sign IDENTITY] [--out DIR] [--no-dmg]
#
#   1. libffplay + FFmpeg dylibs: ffmpeg/darwin/build-ffplay.sh when ffmpeg/dist/darwin-arm64 lacks them (long: FFmpeg
#      from source; needs Xcode's Metal toolchain, git, quilt, cmake, ninja, nasm, pkg-config — see that script);
#   2. the Metal presenter (make -C mac/presenter);
#   3. the app (CMake + Ninja, Release) into .tmp/build-mac-app-dist; its POST_BUILD step (bundle-frameworks.sh) copies
#      the dylibs into Contents/Frameworks, fixes the rpaths and signs ad hoc;
#   4. OUT/ARIB Player.app (default OUT = .tmp/dist-mac), checked: LSMinimumSystemVersion is 13.3, every Mach-O inside
#      has a minos at or below it and links only @rpath / @executable_path / @loader_path / system libraries,
#      `codesign --verify --deep --strict` passes;
#   5. OUT/ARIB-Player-<version>.dmg (hdiutil, UDZO: the app + an /Applications link + the licences).
#
# --version   CFBundleShortVersionString / CFBundleVersion (default: $ARIBPLAYER_VERSION or 0.0.0, as the Windows build)
# --sign ID   a Developer ID Application identity instead of the ad-hoc signature (hardened runtime, timestamp).
#             Notarisation is then: xcrun notarytool submit OUT/ARIB-Player-*.dmg --keychain-profile <profile> --wait
#             && xcrun stapler staple OUT/ARIB-Player-*.dmg   (needs an Apple Developer account).
# Run from anywhere; paths are resolved from this script.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
VERSION="${ARIBPLAYER_VERSION:-0.0.0}"
SIGN=""
OUT="$ROOT/.tmp/dist-mac"
DMG=1
while [ $# -gt 0 ]; do
    case "$1" in
        --version) VERSION=$2; shift 2 ;;
        --sign) SIGN=$2; shift 2 ;;
        --out) OUT=$2; shift 2 ;;
        --no-dmg) DMG=0; shift ;;
        -h|--help) sed -n '2,19p' "$0"; exit 0 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
DIST="$ROOT/ffmpeg/dist/darwin-arm64"
BUILD="$ROOT/.tmp/build-mac-app-dist"
APPNAME="ARIB Player.app"
T0=$(date +%s)
step() { echo "== [$(( $(date +%s) - T0 ))s] $*"; }

# 1. the engine
if [ ! -f "$DIST/libffplay.dylib" ] || [ ! -f "$DIST/ffplay.h" ] || [ ! -f "$DIST/include/libavcodec/avcodec.h" ] || [ ! -f "$DIST/ffmpeg-caps.inc" ]; then
    step "libffplay: $DIST is incomplete, running ffmpeg/darwin/build-ffplay.sh (FFmpeg from source, this takes a while)"
    "$ROOT/ffmpeg/darwin/build-ffplay.sh"
else
    step "libffplay: $DIST (present; ffmpeg/darwin/build-ffplay.sh --force rebuilds it)"
fi

# 2. the Metal presenter (incremental)
step "mac/presenter"
make -s -C "$ROOT/mac/presenter" FFINC="$DIST/include" FFLIB="$DIST" >/dev/null

# 3. the app
step "app: cmake (Release, version $VERSION)"
cmake -S "$HERE" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DARIBPLAYER_VERSION="$VERSION" \
      -DAP_FFLIB="$DIST" -DAP_FFINC="$DIST/include" >"$BUILD.configure.log" 2>&1 || { cat "$BUILD.configure.log"; exit 1; }
cmake --build "$BUILD" >"$BUILD.build.log" 2>&1 || { tail -40 "$BUILD.build.log"; exit 1; }

# 4. the distributable copy
step "bundle: $OUT/$APPNAME"
rm -rf "$OUT/$APPNAME"
ditto "$BUILD/$APPNAME" "$OUT/$APPNAME"
APP="$OUT/$APPNAME"
if [ -n "$SIGN" ]; then
    for f in "$APP/Contents/Frameworks/"*.dylib; do codesign --force --options runtime --timestamp --sign "$SIGN" "$f"; done
    codesign --force --options runtime --timestamp --sign "$SIGN" "$APP"
fi
bad=0
MIN_MACOS="$(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' "$APP/Contents/Info.plist")"
[ "$MIN_MACOS" = 13.3 ] || { echo "LSMinimumSystemVersion is '$MIN_MACOS', expected 13.3 (mac/cmake/MacDeploymentTarget.cmake)" >&2; exit 1; }
version_le() {  # major.minor[.patch] $1 <= $2
    awk -v a="$1" -v b="$2" 'BEGIN { split(a, x, "."); split(b, y, "."); for (i = 1; i <= 3; i++) { if (x[i] + 0 != y[i] + 0) exit !(x[i] + 0 < y[i] + 0) } }'
}
while IFS= read -r -d '' f; do
    file -b "$f" | grep -q 'Mach-O' || continue
    minos="$(xcrun vtool -show-build "$f" | awk '$1 == "minos" { print $2; exit }')"
    if [ -z "$minos" ] || ! version_le "$minos" "$MIN_MACOS"; then
        echo "  minos '${minos:-none}' above LSMinimumSystemVersion $MIN_MACOS: $f"; bad=1
    fi
    while IFS= read -r dep; do
        case "$dep" in @rpath/*|@executable_path/*|@loader_path/*|/usr/lib/*|/System/Library/*) ;; *) echo "  NOT self-contained: $f -> $dep"; bad=1 ;; esac
    done < <(otool -L "$f" | tail -n +2 | awk '{print $1}')
    while IFS= read -r rp; do
        case "$rp" in @*) ;; *) echo "  absolute LC_RPATH in $f: $rp"; bad=1 ;; esac
    done < <(otool -l "$f" | awk '/cmd LC_RPATH/{getline; getline; print $2}')
done < <(find "$APP" -type f -print0)
[ $bad -eq 0 ] || { echo "the bundle is not self-contained or needs a newer macOS than $MIN_MACOS" >&2; exit 1; }
codesign --verify --deep --strict "$APP"
echo "   self-contained: $(ls "$APP/Contents/Frameworks" | wc -l | tr -d ' ') dylibs in Contents/Frameworks; macOS $MIN_MACOS or later; signature: $(codesign -dv "$APP" 2>&1 | awk -F= '/^Signature/{print $2}')"
# Let Finder / "Open With" know the document types of this copy.
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$APP" 2>/dev/null || true

# 5. the disk image
if [ $DMG -eq 1 ]; then
    step "dmg"
    STAGE="$BUILD/dmg-stage"
    rm -rf "$STAGE"
    mkdir -p "$STAGE/Licenses"
    ditto "$APP" "$STAGE/$APPNAME"
    ln -s /Applications "$STAGE/Applications"
    cp "$ROOT/THIRD-PARTY-NOTICES.txt" "$STAGE/Licenses/"
    ditto "$DIST/licenses" "$STAGE/Licenses"
    IMG="$OUT/ARIB-Player-$VERSION.dmg"
    rm -f "$IMG"
    hdiutil create -quiet -volname "ARIB Player" -srcfolder "$STAGE" -fs HFS+ -format UDZO -ov "$IMG"
    [ -n "$SIGN" ] && codesign --force --timestamp --sign "$SIGN" "$IMG"
    echo "   $IMG ($(du -h "$IMG" | cut -f1 | tr -d ' '))"
fi
step "done: $APP"
