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

Write-Host "Test media ready in $OutDir"
Write-Host "Serve over HTTP with: scripts\serve-testmedia.ps1  (http://127.0.0.1:$HttpPort/hls/index.m3u8, /http.m3u)"
