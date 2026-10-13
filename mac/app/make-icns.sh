#!/bin/sh
# assets/icon/aribplayer.svg -> AppIcon.icns with sips (renders SVG) + iconutil, both part of macOS.
#   make-icns.sh <aribplayer.svg> <out.icns>
# The artwork is drawn at 80 % of each tile, centred (the macOS icon grid leaves a margin around the shape).
set -eu
SVG=$1; OUT=$2
TMP=$(mktemp -d)
SET="$TMP/AppIcon.iconset"
mkdir -p "$SET"
for spec in 16:icon_16x16 32:icon_16x16@2x 32:icon_32x32 64:icon_32x32@2x 128:icon_128x128 256:icon_128x128@2x \
            256:icon_256x256 512:icon_256x256@2x 512:icon_512x512 1024:icon_512x512@2x; do
    size=${spec%%:*}; name=${spec#*:}
    inner=$(( size * 4 / 5 ))
    sips -s format png -z "$inner" "$inner" "$SVG" --out "$TMP/art.png" >/dev/null
    sips --padToHeightWidth "$size" "$size" "$TMP/art.png" --out "$SET/$name.png" >/dev/null
done
iconutil -c icns -o "$OUT" "$SET"
rm -rf "$TMP"
