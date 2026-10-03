#!/usr/bin/env bash
# Cross-compiles ffmpeg/tools/arib_inspect.c against the dev container's /opt/ffmpeg and copies
# arib_inspect.exe next to the dist DLLs' consumer (artifacts/scratch/aribdetect/bin).
# Run in WSL after `author.sh build`. The exe needs ffmpeg/dist/win64 on PATH to run.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out="${root}/artifacts/scratch/aribdetect/bin"
mkdir -p "${out}"
docker start ffplay-dev-work >/dev/null 2>&1 || true
docker cp "${root}/ffmpeg/tools/arib_inspect.c" ffplay-dev-work:/tmp/arib_inspect.c
docker exec ffplay-dev-work bash -c 'x86_64-w64-mingw32-gcc -O2 -Wall -D__USE_MINGW_ANSI_STDIO=1 -I/opt/ffmpeg/include -o /tmp/arib_inspect.exe /tmp/arib_inspect.c -L/opt/ffmpeg/lib -lavformat -lavcodec -lavutil -lshell32'
docker cp ffplay-dev-work:/tmp/arib_inspect.exe "${out}/arib_inspect.exe"
echo "built ${out}/arib_inspect.exe"
