#!/usr/bin/env bash
# Build libffplay.dll / ffplay.exe / ffmpeg.exe + libav* for Windows (win64) by
# mirroring builder-ffmpeg's CI `build-windows` job, with our patches layered on top.
# Runs in WSL or natively on Linux (CI); needs Docker usable without sudo.
#
#   build-ffplay.sh [--force] [--work-dir DIR] [--check-only] [--print-digest]
#                   [--keep-dev-image]
#
#   --force           rebuild even when dist/win64/.stamp matches
#   --work-dir DIR    clones + build tree location; at most once. Default: the one canonical
#                     work dir (FFP_WORK_DIR_DEFAULT in lib.sh, shared with author.sh; created
#                     if missing). CI passes its own. Must be an absolute path (leading ~ ok).
#                     Unknown options and positional arguments fail with this text.
#   --check-only      prerequisite check + patch apply check on a scratch tree; no build
#   --print-digest    print the input digest (builder.lock, build-ffplay.ps1, scripts/*.sh, patches,
#                     source-patches), exit
#   --keep-dev-image  keep the finished build container as docker image ffplay-dev:win64
#                     (deps + built ffmpeg tree) for `author.sh build` incremental rebuilds
#
# Output: ffmpeg/dist/win64/{libffplay.dll,ffplay.exe,ffmpeg.exe,ffprobe.exe,av*/sw* dlls,
#         libffplay.dll.a,ffplay.h,ffcapsdump.exe,licenses/ffmpeg/{COPYING*,LICENSE.md},.stamp}.
#         Log: ffmpeg/.work/build.log (the only thing this script keeps in ffmpeg/.work)
set -euo pipefail

# shellcheck source=lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

FORCE=0; CHECK_ONLY=0; PRINT_DIGEST=0; KEEP_DEV=0; WORK_ARG=""; HAVE_WORK=0
while [ $# -gt 0 ]; do
    case "$1" in
        --force) FORCE=1 ;;
        --check-only) CHECK_ONLY=1 ;;
        --print-digest) PRINT_DIGEST=1 ;;
        --keep-dev-image) KEEP_DEV=1 ;;
        --work-dir|--work-dir=*)
            [ "${HAVE_WORK}" = 0 ] || usage_error "$0" "--work-dir given more than once"
            HAVE_WORK=1
            if [ "$1" = --work-dir ]; then
                [ $# -ge 2 ] || usage_error "$0" "--work-dir needs a value"
                WORK_ARG="$2"; shift
            else
                WORK_ARG="${1#*=}"
            fi
            [ -n "${WORK_ARG}" ] || usage_error "$0" "--work-dir needs a non-empty value" ;;
        -h|--help) awk 'NR > 1 { if ($0 !~ /^#/) exit; sub(/^# ?/, ""); print }' "$0"; exit 0 ;;
        -*) usage_error "$0" "unknown option: $1" ;;
        *)  usage_error "$0" "unexpected argument: $1" ;;
    esac
    shift
done

if [ "${PRINT_DIGEST}" = 1 ]; then compute_digest; exit 0; fi

WORK="$(resolve_work_dir "${WORK_ARG}")"
mkdir -p "${WORK}" "${FFP_ROOT}/.work"
WORK="$(cd "${WORK}" && pwd)"
LOG="${FFP_ROOT}/.work/build.log"
exec > >(tee -a "${LOG}") 2>&1
log "=== build-ffplay.sh $* (work dir: ${WORK}) ==="

DIST="${FFP_ROOT}/dist/win64"
DIGEST="$(compute_digest)"
load_lock

if [ "${FORCE}" = 0 ] && [ "${CHECK_ONLY}" = 0 ] && [ -f "${DIST}/.stamp" ] \
   && [ "$(cat "${DIST}/.stamp")" = "${DIGEST}" ] && [ -f "${DIST}/libffplay.dll" ] \
   && [ -f "${DIST}/ffcapsdump.exe" ] && [ -f "${DIST}/licenses/ffmpeg/LICENSE.md" ]; then
    log "dist/win64 is up to date (digest ${DIGEST}); use --force to rebuild."
    exit 0
fi

prereq_fail=0
for c in git make unzip sha256sum; do
    command -v "$c" >/dev/null 2>&1 || { warn "required command '$c' not found (apt-get install $c)"; prereq_fail=1; }
done
for c in quilt zip; do
    command -v "$c" >/dev/null 2>&1 || warn "optional command '$c' not found (quilt: needed by author.sh; zip: runs inside the container)"
done
docker_diagnose || prereq_fail=1

ensure_builder
ensure_pristine
sync_ours_into_builder

if [ "${CHECK_ONLY}" = 1 ]; then
    CHK="${WORK}/check-tree"
    log "Patch apply check on scratch tree ${CHK}"
    stage_tree "${CHK}" 1
    if ls "${FFP_ROOT}"/source-patches/*.patch >/dev/null 2>&1 && command -v quilt >/dev/null 2>&1; then
        log "quilt push -a dry run (jellyfin + builder + our source patches)"
        ( cd "${CHK}" && QUILTRC="${WORK}/builder-ffmpeg/.quiltrc" QUILT_PATCHES=debian/patches quilt push -a >/dev/null ) \
            && log "quilt push -a OK" || die "quilt push -a failed on the staged tree"
    fi
    rm -rf "${CHK}"
    log "Patch check OK (digest ${DIGEST})."
    if [ "${prereq_fail}" != 0 ]; then
        die "prerequisites missing (see above); a full build cannot run yet."
    fi
    log "Prerequisites OK."
    exit 0
fi

[ "${prereq_fail}" = 0 ] || die "prerequisites missing (see above)."

TREE="${WORK}/jellyfin-ffmpeg"
OUT="${WORK}/out"
stage_tree "${TREE}" 0
rm -rf "${OUT}"; mkdir -p "${OUT}"

SHIM=""
if [ "${KEEP_DEV}" = 1 ]; then
    # Wrap `docker run --rm ...` made by build-windows-win64 so the finished container
    # (deps in /opt/ffdeps, built ffmpeg in /ffmpeg) survives and can be committed.
    SHIM="$(mktemp -d)"
    REAL_DOCKER="$(command -v docker)"
    docker rm -f "${DEV_CONTAINER}" >/dev/null 2>&1 || true
    cat > "${SHIM}/docker" <<EOF
#!/usr/bin/env bash
if [ "\${1:-}" = run ]; then
    shift; args=()
    for a in "\$@"; do [ "\$a" = --rm ] || args+=("\$a"); done
    exec "${REAL_DOCKER}" run --name "${DEV_CONTAINER}" "\${args[@]}"
fi
exec "${REAL_DOCKER}" "\$@"
EOF
    chmod +x "${SHIM}/docker"
fi

log "Running ./build-windows-win64 (this rebuilds ~30 dependencies; expect 1h+)"
set +e
( cd "${TREE}" && PATH="${SHIM:+${SHIM}:}${PATH}" ./build-windows-win64 "${OUT}" )
rc=$?
set -e
[ -n "${SHIM}" ] && rm -rf "${SHIM}"
[ "${rc}" = 0 ] || die "build-windows-win64 failed (exit ${rc}); see ${LOG}"

if [ "${KEEP_DEV}" = 1 ]; then
    log "Committing build container as ${DEV_IMAGE}"
    docker commit --change "LABEL ffplay.digest=${DIGEST}" "${DEV_CONTAINER}" "${DEV_IMAGE}" >/dev/null
    docker rm "${DEV_CONTAINER}" >/dev/null
fi

ZIP="$(ls "${OUT}"/jellyfin-ffmpeg_*-ffplay_win64.zip 2>/dev/null | head -n1 || true)"
[ -n "${ZIP}" ] || die "expected ${OUT}/jellyfin-ffmpeg_*-ffplay_win64.zip not produced (is patch 1001 applied?)"
rm -rf "${DIST}"; mkdir -p "${DIST}"
unzip -q -o "${ZIP}" -d "${DIST}"
LIC="${DIST}/licenses/ffmpeg"; mkdir -p "${LIC}"
for f in "${TREE}"/COPYING* "${TREE}/LICENSE.md"; do
    [ -f "$f" ] || die "FFmpeg license file missing in staged source: $f"
    cp "$f" "${LIC}/"
done
for f in ffplay.exe ffmpeg.exe ffprobe.exe ffcapsdump.exe libffplay.dll libffplay.dll.a ffplay.h; do
    [ -f "${DIST}/${f}" ] || die "${f} missing from ${ZIP}"
done
printf '%s\n' "${DIGEST}" > "${DIST}/.stamp"
log "Done. Output in ${DIST} (digest ${DIGEST})"
ls -l "${DIST}"
