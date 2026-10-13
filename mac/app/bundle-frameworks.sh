#!/bin/bash
# Makes "ARIB Player.app" self-contained (CMake POST_BUILD of the app target, also usable by hand):
#   bundle-frameworks.sh <App.app> <ffmpeg dist dir> <libffpmetalpresenter.dylib>
# 1. Contents/Frameworks gets every @rpath dylib the executable needs, transitively (libffplay, libav*/libsw*, the Metal
#    presenter), found in the dist dir / next to the presenter; symlinks are resolved (the file is copied under the name
#    the load command uses, e.g. libavcodec.62.dylib).
# 2. rpaths: absolute LC_RPATH entries (build-tree paths) are removed from every Mach-O in the bundle; the executable
#    keeps @executable_path/../Frameworks, the dylibs get @loader_path. Afterwards `otool -L` shows only @rpath /
#    @executable_path / @loader_path / system paths.
# 3. Ad-hoc signature (codesign -s -): dylibs first, then the bundle (build-app.sh --sign applies a Developer ID).
set -euo pipefail
APP=$1; DIST=$2; PRESENTER=$3
EXE_DIR="$APP/Contents/MacOS"
FW="$APP/Contents/Frameworks"
EXE="$EXE_DIR/$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$APP/Contents/Info.plist")"
mkdir -p "$FW"
SEARCH="$DIST $(dirname "$PRESENTER")"

deps() { otool -L "$1" | tail -n +2 | awk '{print $1}' | grep '^@rpath/' | sed 's|^@rpath/||'; }

find_lib() {
    for d in $SEARCH; do
        if [ -e "$d/$1" ]; then echo "$d/$1"; return 0; fi
    done
    return 1
}

queue=("$EXE")
done_list=" "
while [ ${#queue[@]} -gt 0 ]; do
    cur=${queue[0]}
    queue=("${queue[@]:1}")
    while IFS= read -r name; do
        [ -n "$name" ] || continue
        case "$done_list" in *" $name "*) continue ;; esac
        done_list="$done_list$name "
        src=$(find_lib "$name") || { echo "bundle-frameworks: $name (needed by $cur) not found in $SEARCH" >&2; exit 1; }
        dst="$FW/$name"
        if [ ! -e "$dst" ] || ! cmp -s "$src" "$dst"; then
            rm -f "$dst"
            cp -L "$src" "$dst"
            chmod u+w "$dst"
        fi
        queue+=("$dst")
    done < <(deps "$cur")
done

strip_abs_rpaths() {
    otool -l "$1" | awk '/cmd LC_RPATH/{getline; getline; print $2}' | while read -r rp; do
        case "$rp" in @*) ;; *) install_name_tool -delete_rpath "$rp" "$1" 2>/dev/null ;; esac
    done
}
has_rpath() { otool -l "$1" | awk '/cmd LC_RPATH/{getline; getline; print $2}' | grep -qx "$2"; }

for f in "$FW"/*.dylib; do
    strip_abs_rpaths "$f"
    has_rpath "$f" @loader_path || install_name_tool -add_rpath @loader_path "$f" 2>/dev/null
done
strip_abs_rpaths "$EXE"
has_rpath "$EXE" @executable_path/../Frameworks || install_name_tool -add_rpath @executable_path/../Frameworks "$EXE"

for f in "$FW"/*.dylib; do codesign --force --sign - --timestamp=none "$f" 2>/dev/null; done
codesign --force --sign - --timestamp=none --deep "$APP"
echo "bundle-frameworks: $(ls "$FW" | wc -l | tr -d ' ') dylibs in Contents/Frameworks, ad-hoc signed"
