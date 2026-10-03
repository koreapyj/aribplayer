#!/usr/bin/env bash
# Shared helpers for build-ffplay.sh and author.sh (sourced, not executed).

FFP_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FFP_ROOT="$(cd "${FFP_SCRIPT_DIR}/.." && pwd)"
DEV_IMAGE="ffplay-dev:win64"                          # committed dev image (see build-ffplay.sh --keep-dev-image)
DEV_CONTAINER="ffplay-dev-last"

# THE work dir (clones, quilt tree, build tree), for author.sh and build-ffplay.sh alike. This is
# the only default anywhere; it can be replaced only by an explicit --work-dir (CI does that).
# A WSL-native path: /mnt/c is far too slow for the docker build context and quilt.
FFP_WORK_DIR_DEFAULT="${HOME}/.cache/windows-video-player/ffmpeg81"

log()  { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
warn() { printf '[%s] WARNING: %s\n' "$(date +%H:%M:%S)" "$*" >&2; }
die()  { printf '[%s] ERROR: %s\n' "$(date +%H:%M:%S)" "$*" >&2; exit 1; }

# usage_error SCRIPT MESSAGE...: message + the script's leading comment block (its usage), exit 2.
usage_error() {
    local script="$1"; shift
    printf 'ERROR: %s\n\n' "$*" >&2
    awk 'NR > 1 { if ($0 !~ /^#/) exit; sub(/^# ?/, ""); print }' "${script}" >&2
    exit 2
}

# resolve_work_dir [EXPLICIT]: absolute work dir path to stdout (no filesystem access).
resolve_work_dir() {
    local w="${1:-${FFP_WORK_DIR_DEFAULT}}"
    case "${w}" in "~"|"~/"*) w="${HOME}${w#\~}" ;; esac   # the PowerShell wrapper single-quotes arguments
    case "${w}" in /*) ;; *) die "work dir must be an absolute path: ${w}" ;; esac
    printf '%s\n' "${w%/}"
}

load_lock() {
    [ -f "${FFP_ROOT}/builder.lock" ] || die "missing ${FFP_ROOT}/builder.lock"
    # shellcheck disable=SC1091
    . "${FFP_ROOT}/builder.lock"
    : "${BUILDER_REPO:?}" "${BUILDER_COMMIT:?}" "${FF_REPO:?}" "${FF_REF:?}"
}

# sha256 over the ordered inputs: builder.lock, build-ffplay.ps1 (local wrapper), scripts/*.sh,
# patches/*.patch, source-patches/*.patch (paths relative to ffmpeg/, LC_ALL=C sorted; each file
# contributes "<path>", newline, raw bytes, newline). Single source of truth: --print-digest,
# dist/win64/.stamp and the CI cache key all come from here.
compute_digest() (
    cd "${FFP_ROOT}"
    export LC_ALL=C
    files=(builder.lock)
    for f in build-ffplay.ps1 scripts/*.sh patches/*.patch source-patches/*.patch; do [ -e "$f" ] && files+=("$f"); done
    sorted=$(printf '%s\n' "${files[@]}" | sort)
    {
        while IFS= read -r f; do
            printf '%s\n' "$f"; cat "$f"; printf '\n'
        done <<< "${sorted}"
    } | sha256sum | cut -d' ' -f1
)

ensure_builder() {
    local dir="${WORK}/builder-ffmpeg"
    if [ "${FFP_NO_GIT:-0}" = 1 ]; then
        [ -d "${dir}" ] || die "no cached ${dir}; run once without --no-git"
        [ "$(cat "${dir}/.git/HEAD" 2>/dev/null)" = "${BUILDER_COMMIT}" ] \
            || warn "cached builder-ffmpeg HEAD is not ${BUILDER_COMMIT} (not verified: no git)"
        return 0
    fi
    if [ ! -d "${dir}/.git" ]; then
        log "Cloning builder-ffmpeg -> ${dir}"
        mkdir -p "${WORK}"
        git clone "${BUILDER_REPO}" "${dir}"
    fi
    if ! git -C "${dir}" cat-file -e "${BUILDER_COMMIT}^{commit}" 2>/dev/null; then
        log "Fetching builder-ffmpeg (commit ${BUILDER_COMMIT} not present)"
        git -C "${dir}" fetch origin
    fi
    if [ "$(git -C "${dir}" rev-parse HEAD)" != "${BUILDER_COMMIT}" ]; then
        log "Checking out builder-ffmpeg ${BUILDER_COMMIT}"
        git -C "${dir}" -c advice.detachedHead=false checkout --detach "${BUILDER_COMMIT}"
    fi
}

ensure_pristine() {
    local dir="${WORK}/jellyfin-ffmpeg.pristine"
    if [ "${FFP_NO_GIT:-0}" = 1 ]; then
        [ -d "${dir}" ] || die "no cached ${dir}; run once without --no-git"
        return 0
    fi
    if [ ! -d "${dir}/.git" ]; then
        log "Cloning jellyfin-ffmpeg ${FF_REF} -> ${dir}"
        mkdir -p "${WORK}"
        git clone --depth 1 --branch "${FF_REF}" "${FF_REPO}" "${dir}"
    fi
}

# Files copied by a previous run are removed first so deleted
# patches do not linger; tracked via a manifest.
sync_ours_into_builder() {
    local b="${WORK}/builder-ffmpeg" m="${WORK}/builder-ffmpeg/.ffplay-ours" f
    if [ -f "${m}" ]; then
        while IFS= read -r f; do rm -f "${b}/${f}"; done < "${m}"
    fi
    : > "${m}"
    mkdir -p "${b}/patches" "${b}/source-patches"
    for f in "${FFP_ROOT}"/patches/*.patch "${FFP_ROOT}"/source-patches/*.patch; do
        [ -e "$f" ] || continue
        local rel
        rel="$(basename "$(dirname "$f")")/$(basename "$f")"
        if [ -e "${b}/${rel}" ]; then
            if cmp -s "$f" "${b}/${rel}"; then
                log "Reusing identical builder-ffmpeg file ${rel}"
                continue
            fi
            die "our ${rel} collides with a builder-ffmpeg file of the same name; renumber it"
        fi
        cp "$f" "${b}/${rel}"
        echo "${rel}" >> "${m}"
    done
}

# DEST := fresh copy of the pristine jellyfin checkout, then exactly what builder's
# build.yaml "Apply patches" step does (patches git-applied in sorted order,
# source-patches copied + appended to debian/patches/series).
stage_tree() {
    local dest="$1" keep_orig="${2:-0}" b="${WORK}/builder-ffmpeg" p
    rm -rf "${dest}"
    if [ "${FFP_NO_GIT:-0}" = 1 ]; then
        mkdir -p "${dest}"
        tar --exclude=./.git -C "${WORK}/jellyfin-ffmpeg.pristine" -cf - . | tar -C "${dest}" -xf -
        for p in $(cd "${b}/patches" && ls *.patch 2>/dev/null | LC_ALL=C sort); do
            log "patch -p1 -F0 patches/${p}"
            ( cd "${dest}" && patch -p1 -F0 -s --no-backup-if-mismatch < "${b}/patches/${p}" ) \
                || die "build patch ${p} failed to apply"
        done
    else
        cp -a "${WORK}/jellyfin-ffmpeg.pristine" "${dest}"
        for p in $(cd "${b}/patches" && ls *.patch 2>/dev/null | LC_ALL=C sort); do
            log "git apply patches/${p}"
            git -C "${dest}" apply "${b}/patches/${p}" || die "build patch ${p} failed to apply"
        done
        # git apply may rewrite files as 0644 when core.fileMode=false (clones living on /mnt/c);
        # restore the executable bit recorded in the index (the image symlinks docker-build-win64.sh
        # as its ENTRYPOINT, so it must stay executable).
        git -C "${dest}" ls-files --stage | sed -n 's/^100755 [0-9a-f]* 0\t//p' \
            | while IFS= read -r p; do chmod +x "${dest}/${p}"; done
    fi
    [ "${keep_orig}" = 1 ] && cp "${dest}/debian/patches/series" "${dest}/debian/patches/series.orig"
    for p in $(cd "${b}/source-patches" && ls *.patch 2>/dev/null | LC_ALL=C sort); do
        log "stage source-patch ${p} -> debian/patches/series"
        cp "${b}/source-patches/${p}" "${dest}/debian/patches/"
        echo "${p}" >> "${dest}/debian/patches/series"
    done
}

docker_diagnose() {
    local wsl=0
    grep -qi microsoft /proc/version 2>/dev/null && wsl=1
    if ! command -v docker >/dev/null 2>&1; then
        if [ "${wsl}" = 1 ]; then
            cat >&2 <<'EOT'
Docker is not installed in this WSL distro. Install one of:
  * Docker Engine in WSL:   sudo apt-get install -y docker.io && sudo usermod -aG docker "$USER"
                            (then restart the WSL session: `wsl --shutdown`; start the daemon if needed:
                             sudo service docker start)
  * Docker Desktop for Windows with "WSL integration" enabled for this distro (Ubuntu-26.04).
This script never installs anything itself.
EOT
        else
            echo "Docker is not installed. Install Docker Engine and make it usable without sudo (docker group)." >&2
        fi
        return 1
    fi
    if ! docker info >/dev/null 2>&1; then
        if [ "${wsl}" = 1 ]; then
            cat >&2 <<'EOT'
`docker` exists but the daemon is not usable without sudo from this user.
  * start it:            sudo service docker start      (Docker Engine in WSL)
  * or start Docker Desktop and enable WSL integration for this distro
  * permission denied?   sudo usermod -aG docker "$USER" ; then `wsl --shutdown` and reopen
EOT
        else
            echo "'docker' exists but the daemon is not usable without sudo from this user (start dockerd / join the docker group)." >&2
        fi
        return 1
    fi
    return 0
}
