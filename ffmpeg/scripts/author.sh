#!/usr/bin/env bash
# Quilt authoring flow for OUR source patches (source-patches/, numbered from 1002), equivalent to
# builder-ffmpeg's `make prepare QUILT=1` / `make update` / `make refresh`, plus a fast
# incremental build of only ffmpeg/libffplay inside a persistent docker container.
# Runs in WSL.
#
#   author.sh [--work-dir DIR] <command> [command args]
#
#   --work-dir DIR is accepted ONLY before the command (anything after it is an error). Without
#   it the one canonical work dir from lib.sh (FFP_WORK_DIR_DEFAULT) is used. Everything lives in
#   it: builder-ffmpeg/, jellyfin-ffmpeg.pristine/, quilt-tree/. Only `prepare` creates it; every
#   other command fails if the dir (or its quilt tree) is missing. Unknown options/commands and
#   surplus arguments fail with this text before anything is touched.
#
#   prepare [--no-git]
#                stage a fresh tree (jellyfin + builder patches/ + all source-patches in the
#                series), `quilt push -a`, and print the authoring steps
#                --no-git: never run git (cached builder-ffmpeg / jellyfin-ffmpeg.pristine checkouts are
#                copied without .git and the build patches are applied with patch -p1 -F0)
#   quilt ARGS...  run quilt inside the tree with builder's .quiltrc (e.g. `quilt new 1011-x.patch`)
#   update       copy our quilt-refreshed patches (series entries beyond jellyfin's series and
#                builder's source-patches) back to ffmpeg/source-patches/
#   refresh      pop all, push up to builder's last source patch, then `quilt refresh` each of
#                OUR patches in turn; then `update`
#   build        incremental: sync files touched by our patches into the persistent dev
#                container (image ffplay-dev:win64; files our patches delete are removed there),
#                `make && make install`, package libffplay.dll/... like patches/1001 into
#                ffmpeg/dist/win64
#   dev-shell    open a shell in the persistent dev container
#   dev-reset    remove the persistent dev container (next `build` starts from the image)
#   clean        remove the quilt tree
#
# One-time prerequisite for `build`:   build-ffplay.sh --keep-dev-image   (full build; ~1h+)
# Typical loop:  prepare -> quilt new/add/edit/refresh -> build -> (test) -> update
#
# The quilt tree is <work dir>/quilt-tree.
set -euo pipefail

# shellcheck source=lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

WORK_ARG=""
if [ "${1:-}" = "--work-dir" ]; then
    [ $# -ge 2 ] && [ -n "$2" ] || usage_error "$0" "--work-dir needs a value"
    WORK_ARG="$2"; shift 2
fi
CMD="${1:-}"; [ $# -gt 0 ] && shift
# Validate the whole command line first; nothing below runs (or is created) on a usage error.
no_args() { [ $# -eq 0 ] || usage_error "$0" "'${CMD}' takes no arguments (got: $*); --work-dir goes BEFORE the command"; }
case "${CMD}" in
    prepare)
        for a in "$@"; do
            [ "$a" = "--no-git" ] || usage_error "$0" "prepare: unknown argument '$a' (only --no-git; --work-dir goes BEFORE the command)"
        done
        [ $# -le 1 ] || usage_error "$0" "prepare: --no-git given more than once" ;;
    quilt)
        [ $# -gt 0 ] || usage_error "$0" "quilt needs quilt arguments (e.g. series, top, new NAME)" ;;
    update|refresh|build|dev-shell|dev-reset|clean) no_args "$@" ;;
    -h|--help) no_args "$@"; awk 'NR > 1 { if ($0 !~ /^#/) exit; sub(/^# ?/, ""); print }' "$0"; exit 0 ;;
    "") usage_error "$0" "no command given" ;;
    -*) usage_error "$0" "unknown option: ${CMD} (the only option is --work-dir DIR, before the command)" ;;
    *)  usage_error "$0" "unknown command: ${CMD}" ;;
esac

WORK="$(resolve_work_dir "${WORK_ARG}")"
if [ "${CMD}" = prepare ]; then
    mkdir -p "${WORK}"
else
    [ -d "${WORK}" ] || die "work dir ${WORK} does not exist (wrong --work-dir? create it with: author.sh [--work-dir DIR] prepare)"
fi
WORK="$(cd "${WORK}" && pwd)"
TREE="${WORK}/quilt-tree"
WORK_CONTAINER="ffplay-dev-work"
load_lock

qrun() ( cd "${TREE}" && QUILTRC="${WORK}/builder-ffmpeg/.quiltrc" QUILT_PATCHES=debian/patches quilt "$@" )

need_tree() { [ -f "${TREE}/debian/patches/series.orig" ] || die "no quilt tree; run: author.sh prepare"; }

# True if $1 is a source patch shipped by builder-ffmpeg itself. sync_ours_into_builder copies OUR
# source-patches into the builder checkout too, so presence there is not enough: exclude the
# files recorded in its .ffplay-ours manifest.
is_builder_patch() {
    local b="${WORK}/builder-ffmpeg"
    [ -e "${b}/source-patches/$1" ] || return 1
    ! grep -qxF "source-patches/$1" "${b}/.ffplay-ours" 2>/dev/null
}

our_patches() {
    local p
    while IFS= read -r p; do
        [ -n "$p" ] || continue
        grep -qxF "$p" "${TREE}/debian/patches/series.orig" && continue
        is_builder_patch "$p" && continue
        echo "$p"
    done < "${TREE}/debian/patches/series"
}

cmd_prepare() {
    command -v quilt >/dev/null || die "quilt not found (sudo apt-get install quilt)"
    ensure_builder; ensure_pristine; sync_ours_into_builder
    stage_tree "${TREE}" 1
    log "quilt push -a"
    qrun push -a >/dev/null
    cat <<EOF

Tree ready: ${TREE}
  author.sh quilt new 1011-name.patch ; author.sh quilt add <file> ; edit <file> in the tree ;
  author.sh quilt refresh                 (existing patches: author.sh quilt push/pop <name>)
  author.sh build                         (incremental build -> ffmpeg/dist/win64)
  author.sh update                        (copy patches back to ffmpeg/source-patches/)
Raw form:  cd ${TREE} && export QUILTRC=${WORK}/builder-ffmpeg/.quiltrc QUILT_PATCHES=debian/patches
EOF
}

cmd_update() {
    need_tree
    mkdir -p "${FFP_ROOT}/source-patches"
    local p n=0
    for p in $(our_patches); do
        log "Updating source-patches/${p}"
        cp "${TREE}/debian/patches/${p}" "${FFP_ROOT}/source-patches/${p}"
        n=$((n+1))
    done
    log "${n} patch(es) copied back to ${FFP_ROOT}/source-patches/ -- review them."
}

cmd_refresh() {
    need_tree
    local last_builder="" p
    while IFS= read -r p; do
        is_builder_patch "$p" && last_builder="$p"
    done < "${TREE}/debian/patches/series"
    [ -n "${last_builder}" ] || last_builder="$(tail -n1 "${TREE}/debian/patches/series.orig")"
    qrun pop -a >/dev/null 2>&1 || true
    qrun push "${last_builder}" >/dev/null
    for p in $(our_patches); do
        qrun push "$p" >/dev/null || die "failed to push ${p}"
        qrun refresh
    done
    cmd_update
}

ensure_work_container() {
    docker image inspect "${DEV_IMAGE}" >/dev/null 2>&1 \
        || die "image ${DEV_IMAGE} not found. Run once: build-ffplay.sh --keep-dev-image (full build)"
    local state
    # (newer docker CLIs print the "No such container" error on stdout, so test existence first)
    if docker container inspect "${WORK_CONTAINER}" >/dev/null 2>&1; then
        state="$(docker container inspect -f '{{.State.Running}}' "${WORK_CONTAINER}" 2>/dev/null)"
    else
        state=none
    fi
    case "${state}" in
        true) ;;
        false) docker start "${WORK_CONTAINER}" >/dev/null ;;
        none)  log "Creating persistent container ${WORK_CONTAINER} from ${DEV_IMAGE}"
               docker run -d --name "${WORK_CONTAINER}" --entrypoint sleep "${DEV_IMAGE}" infinity >/dev/null ;;
    esac
}

cmd_build() {
    need_tree
    docker_diagnose || die "docker unavailable"
    ensure_work_container
    local files=() p f
    for p in $(our_patches); do
        while IFS= read -r f; do
            [ -n "$f" ] && files+=("$f")
        done < <(qrun files "$p" 2>/dev/null || warn "patch ${p} is not applied in the tree (quilt push it)")
    done
    [ "${#files[@]}" -gt 0 ] || die "no files touched by our patches (is any pushed? author.sh quilt series -v)"
    local present=() gone=()
    while IFS= read -r f; do
        if [ -e "${TREE}/${f}" ]; then present+=("$f"); else gone+=("$f"); fi
    done < <(printf '%s\n' "${files[@]}" | sort -u)
    log "Syncing ${#present[@]} file(s) into ${WORK_CONTAINER}:/ffmpeg, removing ${#gone[@]} deleted by our patches"
    ( cd "${TREE}" && printf '%s\n' "${present[@]}" | tar -cf - -T - ) \
        | docker exec -i "${WORK_CONTAINER}" tar -C /ffmpeg -xf -
    [ "${#gone[@]}" -eq 0 ] || docker exec -w /ffmpeg "${WORK_CONTAINER}" rm -f "${gone[@]}"
    log "Incremental make (in container)"
    docker exec "${WORK_CONTAINER}" bash -c 'cd /ffmpeg && make -j"$(nproc)" && make install'

    # the package layout of patches/1001 (docker-build-win64.sh, "ffplay package")
    local dist="${FFP_ROOT}/dist/win64"
    docker exec "${WORK_CONTAINER}" bash -c 'set -e
        pkg=/tmp/ffplay-pkg; rm -rf "${pkg}"; mkdir -p "${pkg}/include" "${pkg}/lib"
        cp /opt/ffmpeg/bin/*.exe /opt/ffmpeg/bin/*.dll /opt/ffmpeg/lib/libffplay.dll.a /opt/ffmpeg/include/libffplay/ffplay.h "${pkg}/"
        cp -r /opt/ffmpeg/include/libavutil "${pkg}/include/"
        rm -f "${pkg}/include/libavutil/hwcontext_cuda.h"
        def="$(ls /opt/ffmpeg/lib/avutil-*.def)"
        "$(ls /usr/bin/llvm-dlltool-* | sort -V | tail -n1)" -m i386:x86-64 -d "${def}" -D "$(basename "${def}" .def).dll" \
            -l "${pkg}/lib/avutil.lib"' || die "packaging in ${WORK_CONTAINER} failed"
    mkdir -p "${dist}"
    rm -rf "${dist}/include" "${dist}/lib"
    docker exec -w /tmp/ffplay-pkg "${WORK_CONTAINER}" tar -cf - . | tar -C "${dist}" -xf -
    compute_digest > "${dist}/.stamp"
    : > "${dist}/.dev-build"
    [ -f "${dist}/libffplay.dll" ] || warn "libffplay.dll was not produced"
    log "Incremental build copied to ${dist} (marked .dev-build)"
}

case "${CMD}" in
    prepare)   [ "${1:-}" = "--no-git" ] && export FFP_NO_GIT=1; cmd_prepare ;;
    quilt)     need_tree; qrun "$@" ;;
    update)    cmd_update ;;
    refresh)   cmd_refresh ;;
    build)     cmd_build ;;
    dev-shell) ensure_work_container; exec docker exec -it "${WORK_CONTAINER}" bash ;;
    dev-reset) docker rm -f "${WORK_CONTAINER}" ;;
    clean)     rm -rf "${TREE}" ;;
esac
