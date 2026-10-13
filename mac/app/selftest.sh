#!/bin/bash
# All app tests against one bundle (default .tmp/dist-mac/ARIB Player.app), from the repository root:
#   mac/app/selftest.sh MEDIA                uses the default app bundle
#   mac/app/selftest.sh APP MEDIA            or MEDIA=/path/to/media mac/app/selftest.sh [APP]
# Generate MEDIA with make-testmedia.sh OUT_DIR; logs go in .tmp/app-tests/.
# Subtitle fixtures come from MEDIA/subs (override with SUBS_DIR=/path/to/fixtures).
# 1. --selftest: the in-process self-test (engine, shell, Settings window, drag and drop, openURLs, mini player, ...);
#    its ARIB caption checks run on .tmp/app-tests/av_arib_caption.ts, made from MEDIA/av_1080p_h264_aac.ts by
#    make-arib-caption-clip.py (the test clips carry no captions);
# 2. --persist-phase 1 then 2 on one data directory: settings, the unnamed playlist and the resume position survive a
#    relaunch;
# 3. open-with: the app waits (--expect-open 2), `open -a APP clip` is run twice; both deliveries must reach that one
#    instance (no second process) and the last clip must play.
# None of them touches ~/Library/Application Support/aribplayer (temporary data directories).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
usage() { printf 'Usage: %s MEDIA\n   or: %s APP MEDIA\n   or: MEDIA=/path/to/media %s [APP]\nA single argument is MEDIA unless the MEDIA environment variable is set; then it is APP.\nSUBS_DIR overrides MEDIA/subs. Generate fixtures with mac/app/make-testmedia.sh OUT_DIR.\n' "$0" "$0" "$0"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
if [[ "${1:-}" == --help || "${1:-}" == -h ]]; then usage; exit 0; fi
[[ $# -le 2 ]] || { usage >&2; exit 2; }
APP="$ROOT/.tmp/dist-mac/ARIB Player.app"
MEDIA="${MEDIA:-}"
if [[ $# -eq 2 ]]; then
    APP="${1:-$APP}"
    MEDIA="$2"
elif [[ $# -eq 1 ]]; then
    if [[ -n "$MEDIA" ]]; then APP="$1"; else MEDIA="$1"; fi
fi
[[ -n "$MEDIA" ]] || die 'MEDIA is required as an argument or environment variable. Run mac/app/make-testmedia.sh OUT_DIR first.'
[[ -d "$MEDIA" ]] || die "Media directory does not exist: $MEDIA"
MEDIA="$(cd "$MEDIA" && pwd)" || exit 1
SUBS_DIR="${SUBS_DIR:-$MEDIA/subs}"
[[ -d "$SUBS_DIR" ]] || die "Subtitle directory does not exist: $SUBS_DIR (generate MEDIA/subs or set SUBS_DIR)."
SUBS_DIR="$(cd "$SUBS_DIR" && pwd)" || exit 1
for clip in tc_1080i_mpeg2.ts tc_1080p_hevc10.mp4 hlg_tagged.mp4 av_1080p_h264_aac.ts; do
    [[ -s "$MEDIA/$clip" ]] || die "Missing or empty media: $MEDIA/$clip"
done
for clip in clip.ts clip.ko.srt clip.en.srt clip.ja.smi clip.ass extra.srt; do
    [[ -s "$SUBS_DIR/$clip" ]] || die "Missing or empty subtitle fixture: $SUBS_DIR/$clip"
done
[[ -d "$APP" ]] || die "App bundle does not exist: $APP"
APP="$(cd "$APP" && pwd)" || exit 1
EXE="$APP/Contents/MacOS/ARIB Player"
[[ -x "$EXE" ]] || die "App executable is missing: $EXE"
for tool in python3 timeout; do
    command -v "$tool" >/dev/null || die "$tool is required (timeout is supplied by Homebrew coreutils)."
done
LOGS="$ROOT/.tmp/app-tests"
mkdir -p "$LOGS" || die "Cannot create log directory: $LOGS"
fail=0
result() { if [ "$1" -eq 0 ]; then echo "PASS $2"; else echo "FAIL $2 (see $3)"; fail=1; fi; }

CAPTION="$LOGS/av_arib_caption.ts"
# Regenerate from the selected MEDIA so the caption fixture uses the same video as this run.
python3 -I "$ROOT/mac/app/make-arib-caption-clip.py" "$MEDIA/av_1080p_h264_aac.ts" "$CAPTION" >"$LOGS/caption.log" 2>&1 ||
    die "ARIB caption generation failed; see $LOGS/caption.log"
[[ -s "$CAPTION" ]] || die "ARIB caption generator produced no clip; see $LOGS/caption.log"
timeout 420 "$EXE" --selftest --media "$MEDIA" --caption-clip "$CAPTION" --subs-dir "$SUBS_DIR" >"$LOGS/selftest.log" 2>&1
rc=$?
echo "   $(grep -c '^ok' "$LOGS/selftest.log") checks ok, $(grep -c '^FAIL' "$LOGS/selftest.log") failed$(grep -q '^skip' "$LOGS/selftest.log" && echo ", $(grep '^skip' "$LOGS/selftest.log" | head -1)")"
result $rc "self-test" "$LOGS/selftest.log"

D=$(mktemp -d /tmp/aribplayer-persist.XXXXXX)
timeout 120 "$EXE" --persist-phase 1 --data-dir "$D" --media "$MEDIA" >"$LOGS/persist-1.log" 2>&1 &&
    timeout 120 "$EXE" --persist-phase 2 --data-dir "$D" >"$LOGS/persist-2.log" 2>&1
rc=$?
grep -h -E '^(ok|FAIL)' "$LOGS/persist-1.log" "$LOGS/persist-2.log" | sed 's/^/   /'
result $rc "settings / playlist / resume across two launches" "$LOGS/persist-*.log"
rm -rf "$D"

D=$(mktemp -d /tmp/aribplayer-open.XXXXXX)
"$EXE" --expect-open 2 --data-dir "$D" >"$LOGS/open-with.log" 2>&1 &
PID=$!
for _ in $(seq 1 100); do grep -q '^READY' "$LOGS/open-with.log" && break; sleep 0.2; done
open -a "$APP" "$MEDIA/tc_1080p_hevc10.mp4"
sleep 3
instances=$(pgrep -f "$EXE" | wc -l | tr -d ' ')
open -a "$APP" "$MEDIA/av_1080p_h264_aac.ts"
wait $PID
rc=$?
[ "$instances" = "1" ] || rc=1
grep -E '^(OPEN|ok|FAIL)' "$LOGS/open-with.log" | sed 's/^/   /'
echo "   instances while the second file was opened: $instances"
result $rc "open -a: Finder / Dock opens reach the running instance" "$LOGS/open-with.log"
rm -rf "$D"
exit $fail
