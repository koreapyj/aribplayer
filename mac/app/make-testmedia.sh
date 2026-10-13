#!/bin/bash
# Synthetic fixtures for mac/app/selftest.sh and mac/presenter's test harness.
# Usage: mac/app/make-testmedia.sh OUT_DIR (new or empty directory)
# OUT_DIR holds the five video clips; OUT_DIR/subs holds clip.ts and its subtitle fixtures.
# Requires the darwin dist, python3 and hardware VideoToolbox H.264 / HEVC Main 10 encoding (including 8K).
# HLG is a tagged test pattern, not calibrated HDR imagery. These clips do not replace broadcast-TS tests.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
usage() { printf 'Usage: %s OUT_DIR\nCreates synthetic app/presenter media in a new or empty directory, with subtitles in OUT_DIR/subs.\n' "$0"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
if [[ "${1:-}" == --help || "${1:-}" == -h ]]; then usage; exit 0; fi
[[ $# -eq 1 && -n "$1" ]] || { usage >&2; exit 2; }
FFMPEG="$ROOT/ffmpeg/dist/darwin-arm64/ffmpeg"
FFPROBE="$ROOT/ffmpeg/dist/darwin-arm64/ffprobe"
[[ -x "$FFMPEG" && -x "$FFPROBE" ]] || die 'Build ffmpeg/darwin/build-ffplay.sh first (ffmpeg and ffprobe are required).'
command -v python3 >/dev/null || die 'python3 is required.'
mkdir -p "$1"
OUT="$(cd "$1" && pwd)"
shopt -s nullglob dotglob
contents=("$OUT"/*)
[[ ${#contents[@]} -eq 0 ]] || die "Output directory must be empty: $OUT"
shopt -u nullglob dotglob
mkdir "$OUT/subs"
trap 'printf "Generation failed; incomplete media remains in %s. Retry with an empty directory.\n" "$OUT" >&2' ERR
ffmpeg() { "$FFMPEG" -hide_banner -loglevel warning -nostdin -n "$@"; }
sdr=(-color_range tv -colorspace bt709 -color_primaries bt709 -color_trc bt709)
# Set frame metadata as well as encoder options: the encoder takes primaries / transfer from its input frames.
sdr_filter='setparams=range=limited:color_primaries=bt709:color_trc=bt709:colorspace=bt709'
hevc=(-c:v hevc_videotoolbox -allow_sw 0 -profile:v main10 -pix_fmt p010le -tag:v hvc1 -bf 0 -g 30)

printf 'Generating 1080i MPEG-2 (240 film frames -> 300 TFF telecined frames)...\n'
ffmpeg -f lavfi -i 'testsrc2=size=1920x1080:rate=24000/1001' \
    -vf "telecine=first_field=top:pattern=23,setfield=tff,$sdr_filter" -frames:v 300 \
    -c:v mpeg2video -pix_fmt yuv420p -flags +ildct+ilme -g 15 -bf 2 \
    -b:v 12M -maxrate 18M -bufsize 1835008 "${sdr[@]}" -an "$OUT/tc_1080i_mpeg2.ts"
printf 'Generating 1080p HEVC Main 10...\n'
ffmpeg -f lavfi -i 'testsrc2=size=1920x1080:rate=30000/1001,format=yuv420p10le,format=p010le' -frames:v 360 \
    -vf "$sdr_filter" "${hevc[@]}" -b:v 8M "${sdr[@]}" -an -movflags +faststart "$OUT/tc_1080p_hevc10.mp4"
printf 'Generating HLG-tagged HEVC Main 10...\n'
ffmpeg -f lavfi -i 'testsrc2=size=1920x1080:rate=60000/1001,format=yuv420p10le,format=p010le' -frames:v 180 \
    -vf 'setparams=range=limited:color_primaries=bt2020:color_trc=arib-std-b67:colorspace=bt2020nc' \
    "${hevc[@]}" -b:v 8M -color_range tv -colorspace bt2020nc -color_primaries bt2020 -color_trc arib-std-b67 \
    -an -movflags +faststart "$OUT/hlg_tagged.mp4"
printf 'Generating 30-second AVC / AAC transport stream...\n'
# Explicit PMT/video PIDs are the input contract of make-arib-caption-clip.py.
# The app seeks to 20 seconds and persists a 12-second resume point, so this clip must outlast both.
ffmpeg -f lavfi -i 'testsrc2=size=1920x1080:rate=30000/1001' \
    -f lavfi -i 'sine=frequency=440:sample_rate=48000' -t 30.03 -map 0:v -map 1:a \
    -vf "$sdr_filter" -c:v h264_videotoolbox -allow_sw 0 -profile:v high -pix_fmt yuv420p -bf 0 -g 30 -b:v 8M "${sdr[@]}" \
    -c:a aac -b:a 192k -ac 2 -ar 48000 -mpegts_pmt_start_pid 4096 -streamid 0:256 -streamid 1:257 \
    "$OUT/av_1080p_h264_aac.ts"
printf 'Generating 8K HEVC Main 10 (presenter test h)...\n'
ffmpeg -f lavfi -i 'testsrc2=size=7680x4320:rate=60000/1001,format=yuv420p10le,format=p010le' -frames:v 60 \
    -vf "$sdr_filter" "${hevc[@]}" -b:v 40M "${sdr[@]}" -an -movflags +faststart "$OUT/tc_8k_hevc10.mp4"
cp "$OUT/av_1080p_h264_aac.ts" "$OUT/subs/clip.ts"

# Cues cover the app's 4.5-second seek plus its delayed pause; extra.srt must not auto-attach to clip.ts.
python3 -I - "$OUT/subs" <<'PY'
from pathlib import Path
import sys

out = Path(sys.argv[1])
for name, text in (("clip.ko.srt", "한국어 자막"), ("clip.en.srt", "English subtitles"),
                   ("extra.srt", "Manual-load / drag-and-drop only")):
    cues = [f"{i + 1}\n00:00:{s:02d},000 --> 00:00:{s + 1:02d},800\n{name} | {text} 00:{s:02d}\n\n"
            for i, s in enumerate(range(0, 20, 2))]
    (out / name).write_text("".join(cues), encoding="utf-8")

sami = '''<SAMI>
<HEAD>
<TITLE>clip.ja.smi - Japanese subtitle fixture</TITLE>
<STYLE TYPE="text/css">
P { font-family: sans-serif; text-align: center; color: white; }
.JPCC { Name: Japanese; lang: ja-JP; SAMIType: CC; }
</STYLE>
</HEAD>
<BODY>
'''
for s in range(0, 20, 2):
    sami += f'<SYNC Start={s * 1000}><P Class=JPCC>clip.ja.smi | 日本語字幕 00:{s:02d}</P></SYNC>\n'
    sami += f'<SYNC Start={s * 1000 + 1800}><P Class=JPCC>​</P></SYNC>\n'
(out / "clip.ja.smi").write_text(sami + '</BODY>\n</SAMI>\n', encoding="utf-8")

ass = '''[Script Info]
Title: clip.ass - ASS subtitle fixture
ScriptType: v4.00+
PlayResX: 640
PlayResY: 360

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Default,Arial,28,&H0000FFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
'''
for s in range(0, 20, 2):
    ass += f'Dialogue: 0,0:00:{s:02d}.00,0:00:{s + 1:02d}.80,Default,,0,0,0,,clip.ass | ASS styled subtitles 00:{s:02d}\n'
(out / "clip.ass").write_text(ass, encoding="utf-8")
PY

# Probe the encoded files, not the encoder's requested input format (Main 10 / HLG must survive the mux).
python3 -I - "$FFPROBE" "$OUT" <<'PY'
import json
from pathlib import Path
import subprocess
import sys

probe, root = sys.argv[1], Path(sys.argv[2])
cases = [
    ("tc_1080i_mpeg2.ts", "mpeg2video", "Main", 1920, 1080, "yuv420p", "tt", "30000/1001", 300, 10.010),
    ("tc_1080p_hevc10.mp4", "hevc", "Main 10", 1920, 1080, "yuv420p10le", "progressive", "30000/1001", 360, 12.012),
    ("hlg_tagged.mp4", "hevc", "Main 10", 1920, 1080, "yuv420p10le", "progressive", "60000/1001", 180, 3.003),
    ("av_1080p_h264_aac.ts", "h264", "High", 1920, 1080, "yuv420p", "progressive", "30000/1001", 900, 30.030),
    ("tc_8k_hevc10.mp4", "hevc", "Main 10", 7680, 4320, "yuv420p10le", "progressive", "60000/1001", 60, 1.001),
]
for name, codec, profile, width, height, fmt, field, rate, frames, duration in cases:
    data = json.loads(subprocess.check_output([probe, "-v", "error", "-count_packets", "-show_streams",
                                               "-show_programs", "-of", "json", str(root / name)]))
    video = next(s for s in data["streams"] if s["codec_type"] == "video")
    expected = dict(codec_name=codec, profile=profile, width=width, height=height, pix_fmt=fmt,
                    field_order=field, r_frame_rate=rate, nb_read_packets=str(frames), sample_aspect_ratio="1:1", color_range="tv",
                    color_space="bt2020nc" if name == "hlg_tagged.mp4" else "bt709",
                    color_transfer="arib-std-b67" if name == "hlg_tagged.mp4" else "bt709",
                    color_primaries="bt2020" if name == "hlg_tagged.mp4" else "bt709")
    # VideoToolbox's H.264 bitstream omits the square-pixel SAR; the other codecs signal it explicitly.
    if codec == "h264":
        expected.pop("sample_aspect_ratio")
    for key, value in expected.items():
        if video.get(key) != value:
            sys.exit(f"{name}: expected {key}={value}, got {video.get(key)}")
    if abs(float(video["duration"]) - duration) > 0.002:
        sys.exit(f"{name}: unexpected duration {video['duration']}")
    if codec == "h264":
        audio = next(s for s in data["streams"] if s["codec_type"] == "audio")
        if (audio["codec_name"], audio["sample_rate"], audio["channels"], video["id"]) != ("aac", "48000", 2, "0x100"):
            sys.exit(f"{name}: wrong AAC / video PID properties")
        if len(data["programs"]) != 1 or data["programs"][0]["pmt_pid"] != 4096:
            sys.exit(f"{name}: expected one program with PMT PID 0x1000")
    print(f"verified {name}: {width}x{height} {profile} {fmt} {field}, {rate} fps, {duration:.3f} s, {frames} packets")
PY
printf '\nMedia: %s\nSubtitles: %s/subs\nRun: mac/app/selftest.sh "%s"\n' "$OUT" "$OUT" "$OUT"
