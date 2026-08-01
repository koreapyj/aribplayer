#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
for step in \
    01-fetch.sh \
    02-build-freetype.sh \
    03-build-libaribcaption.sh \
    04-build-opencl-loader.sh \
    05-build-ffmpeg.sh \
    06-install-jnilibs.sh; do
    "$SCRIPT_DIR/$step"
done
