<#
.SYNOPSIS
  Generates test media into testmedia/ (gitignored) with ffmpeg lavfi sources.
.PARAMETER Ffmpeg
  Path to ffmpeg.exe (default: jellyfin-ffmpeg in %LOCALAPPDATA%\Programs, then PATH).
.PARAMETER Force
  Regenerate files that already exist.
#>
[CmdletBinding()]
param(
    [string]$Ffmpeg = (Join-Path $env:LOCALAPPDATA 'Programs\jellyfin-ffmpeg\ffmpeg.exe'),
    [string]$OutDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'testmedia'),
    [int]$HttpPort = 8000,
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Ffmpeg)) {
    $cmd = Get-Command ffmpeg -ErrorAction SilentlyContinue
    if (-not $cmd) { throw "ffmpeg.exe not found at $Ffmpeg and not on PATH" }
    $Ffmpeg = $cmd.Source
}
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path

function Invoke-Ff {
    param([string]$Target, [string[]]$FfArgs)
    $path = Join-Path $OutDir $Target
    if ((Test-Path $path) -and -not $Force) { Write-Host "skip   $Target"; return }
    $dir = Split-Path -Parent $path
    New-Item -ItemType Directory -Force $dir | Out-Null
    Write-Host "make   $Target"
    & $Ffmpeg -hide_banner -loglevel error -y @FfArgs $path
    if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed for $Target" }
}

function Video($size, $dur) { @('-f', 'lavfi', '-i', "testsrc2=size=${size}:rate=25:duration=$dur") }
function Sine($dur) { @('-f', 'lavfi', '-i', "sine=frequency=440:sample_rate=48000:duration=$dur") }
$h264 = @('-c:v', 'libx264', '-preset', 'veryfast', '-pix_fmt', 'yuv420p', '-g', '50', '-c:a', 'aac', '-b:a', '128k')

Invoke-Ff 'testsrc2-720p.mp4'  ((Video '1280x720' 10)  + (Sine 10) + $h264 + @('-movflags', '+faststart', '-shortest'))
Invoke-Ff 'testsrc2-1080p.mp4' ((Video '1920x1080' 10) + (Sine 10) + $h264 + @('-movflags', '+faststart', '-shortest'))
Invoke-Ff 'testsrc2-2160p.mp4' ((Video '3840x2160' 4)  + (Sine 4)  + $h264 + @('-movflags', '+faststart', '-shortest'))
Invoke-Ff 'testsrc2-720p.ts'   ((Video '1280x720' 10)  + (Sine 10) + $h264 + @('-f', 'mpegts', '-shortest'))
Invoke-Ff 'hevc-720p.mp4'      ((Video '1280x720' 10)  + (Sine 10) + @('-c:v', 'libx265', '-preset', 'ultrafast', '-pix_fmt', 'yuv420p', '-tag:v', 'hvc1', '-c:a', 'aac', '-shortest'))
Invoke-Ff 'sar-720x576.mp4'    ((Video '720x576' 10)   + (Sine 10) + $h264 + @('-vf', 'setsar=16/15', '-shortest'))
Invoke-Ff 'testsrc2-360p-short.mp4' ((Video '640x360' 3) + (Sine 3) + $h264 + @('-shortest'))
Invoke-Ff 'hls\index.m3u8' ((Video '1280x720' 10) + (Sine 10) + $h264 + @(
    '-f', 'hls', '-hls_time', '2', '-hls_playlist_type', 'vod',
    '-hls_segment_filename', (Join-Path $OutDir 'hls\seg%03d.ts'), '-shortest'))
Invoke-Ff 'audio.m4a' ((Sine 10) + @('-c:a', 'aac', '-b:a', '128k'))
Invoke-Ff 'audio.mp3' ((Sine 10) + @('-c:a', 'libmp3lame', '-b:a', '128k'))

function Write-Utf8([string]$name, [string[]]$lines) {
    $path = Join-Path $OutDir $name
    [IO.File]::WriteAllText($path, (($lines -join "`r`n") + "`r`n"), (New-Object Text.UTF8Encoding($false)))
    Write-Host "write  $name"
}
Write-Utf8 'local-relative.m3u' @(
    '#EXTM3U',
    '#EXTINF:10,Test 720p',      'testsrc2-720p.mp4',
    '#EXTINF:10,Test TS',        'testsrc2-720p.ts',
    '#EXTINF:3,Short clip',      'testsrc2-360p-short.mp4',
    '#EXTINF:10,Audio only',     'audio.m4a')
$abs = { param($n) (Join-Path $OutDir $n) }
Write-Utf8 'local-absolute.m3u' @(
    '#EXTM3U',
    '#EXTINF:10,Test 720p',  (& $abs 'testsrc2-720p.mp4'),
    '#EXTINF:3,Short clip',  (& $abs 'testsrc2-360p-short.mp4'),
    '#EXTINF:10,Audio only', (& $abs 'audio.mp3'))
$base = "http://127.0.0.1:$HttpPort"
Write-Utf8 'http.m3u' @(
    '#EXTM3U',
    '#EXTINF:10,HTTP 720p mp4', "$base/testsrc2-720p.mp4",
    '#EXTINF:10,HTTP TS',       "$base/testsrc2-720p.ts",
    '#EXTINF:3,HTTP short',     "$base/testsrc2-360p-short.mp4")

# Subtitles: cues.srt (2-4 s, 6-8 s), styled.ass (overlapping 1-5 s top / 3-7 s bottom), both muxed into an MKV,
# and box.sup (hand-written PGS: a 200x40 white box 2-4 s and 6-8 s).
New-Item -ItemType Directory -Force (Join-Path $OutDir 'subtitles') | Out-Null
Write-Utf8 'subtitles\cues.srt' @(
    '1', '00:00:02,000 --> 00:00:04,000', 'Hello 안녕하세요', '',
    '2', '00:00:06,000 --> 00:00:08,000', 'Second cue', '')
Write-Utf8 'subtitles\styled.ass' @(
    '[Script Info]', 'ScriptType: v4.00+', 'PlayResX: 640', 'PlayResY: 360', '',
    '[V4+ Styles]',
    'Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding',
    'Style: Default,Arial,28,&H0000FFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1', '',
    '[Events]',
    'Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text',
    'Dialogue: 0,0:00:01.00,0:00:05.00,Default,,0,0,0,,{\an8}Top event',
    'Dialogue: 0,0:00:03.00,0:00:07.00,Default,,0,0,0,,Bottom event')
Invoke-Ff 'subtitles\video-subs.mkv' ((Video '640x360' 10) + @(
    '-i', (Join-Path $OutDir 'subtitles\cues.srt'), '-i', (Join-Path $OutDir 'subtitles\styled.ass'),
    '-map', '0:v', '-map', '1', '-map', '2', '-c:v', 'libx264', '-preset', 'veryfast', '-pix_fmt', 'yuv420p', '-g', '5',
    '-c:s', 'copy', '-metadata:s:s:0', 'language=eng', '-metadata:s:s:1', 'language=kor', '-metadata:s:s:1', 'title=Styled'))

function Write-Sup([string]$name, [int[][]]$cues) {
    $path = Join-Path $OutDir $name
    if ((Test-Path $path) -and -not $Force) { Write-Host "skip   $name"; return }
    $out = [System.Collections.Generic.List[byte]]::new()
    $u16 = { param($v) [byte[]]@((($v -shr 8) -band 255), ($v -band 255)) }
    $seg = {
        param([int]$ms, [int]$type, [byte[]]$payload)
        $pts = $ms * 90
        $out.AddRange([byte[]](0x50, 0x47, (($pts -shr 24) -band 255), (($pts -shr 16) -band 255),
                               (($pts -shr 8) -band 255), ($pts -band 255), 0, 0, 0, 0, $type))
        $out.AddRange([byte[]](& $u16 $payload.Length))
        $out.AddRange($payload)
    }
    $w = 200; $h = 40; $x = 220; $y = 300; $n = 0
    $rle = [System.Collections.Generic.List[byte]]::new()
    for ($i = 0; $i -lt $h; $i++) { $rle.AddRange([byte[]](0x00, (0xC0 -bor ($w -shr 8)), ($w -band 255), 0x01, 0x00, 0x00)) }
    $window = [byte[]](@(1, 0) + (& $u16 $x) + (& $u16 $y) + (& $u16 $w) + (& $u16 $h))
    foreach ($c in $cues) {
        # display set: PCS (epoch start, one object), WDS, PDS (entry 1 = opaque white), ODS, END
        & $seg $c[0] 0x16 ([byte[]]((& $u16 640) + (& $u16 360) + @(0x10) + (& $u16 $n) + @(0x80, 0, 0, 1) +
                                    (& $u16 0) + @(0, 0) + (& $u16 $x) + (& $u16 $y)))
        & $seg $c[0] 0x17 $window
        & $seg $c[0] 0x14 ([byte[]](0, 0, 1, 235, 128, 128, 255))
        $len = 4 + $rle.Count
        & $seg $c[0] 0x15 ([byte[]]((& $u16 0) + @(0, 0xC0, (($len -shr 16) -band 255), (($len -shr 8) -band 255), ($len -band 255)) +
                                    (& $u16 $w) + (& $u16 $h) + $rle.ToArray()))
        & $seg $c[0] 0x80 ([byte[]]@())
        $n++
        # clear set: PCS without objects, WDS, END
        & $seg $c[1] 0x16 ([byte[]]((& $u16 640) + (& $u16 360) + @(0x10) + (& $u16 $n) + @(0, 0, 0, 0)))
        & $seg $c[1] 0x17 $window
        & $seg $c[1] 0x80 ([byte[]]@())
        $n++
    }
    [IO.File]::WriteAllBytes($path, $out.ToArray())
    Write-Host "write  $name"
}
Write-Sup 'subtitles\box.sup' @(@(2000, 4000), @(6000, 8000))
Invoke-Ff 'subtitles\video-pgs.mkv' @('-copyts', '-i',(Join-Path $OutDir 'subtitles\video-subs.mkv'), '-i', (Join-Path $OutDir 'subtitles\box.sup'),
    '-map', '0:v', '-map', '1', '-c', 'copy')

Write-Host "Test media ready in $OutDir"
Write-Host "Serve over HTTP with: scripts\serve-testmedia.ps1  (http://127.0.0.1:$HttpPort/hls/index.m3u8, /http.m3u)"
