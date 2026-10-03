#!/usr/bin/env bash
# Builds and runs arib_select_tests.c against the header in the quilt work tree (or the path given). Run in WSL.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
hdr="${1:-$HOME/.cache/windows-video-player/ffmpeg81/quilt-tree/fftools/ffplay_arib_audio.h}"
out="$(mktemp -d)"
gcc -std=c11 -Wall -Wextra -O1 -I"$(dirname "$hdr")" -o "$out/arib_select_tests" "$here/arib_select_tests.c" -lm
"$out/arib_select_tests"
