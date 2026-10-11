#Requires -Version 7
<#
.SYNOPSIS
  Builds libffplay.dll / libav*.dll (+ the libavutil headers and avutil.lib) into ffmpeg\dist\win64 via WSL
  (digest-cached).
.DESCRIPTION
  Skips the build when dist\win64\.stamp equals the digest of (builder.lock + this
  script + scripts/*.sh + patches\* + source-patches\*) and libffplay.dll exists. The digest is computed by the bash script itself
  (scripts/build-ffplay.sh --print-digest) so both sides always agree.
.PARAMETER Force      Rebuild even if the stamp matches.
.PARAMETER WorkDir    Work dir passed to the script (a WSL path, or a Windows path which is converted).
                      Omit it: the script then uses its one canonical work dir (FFP_WORK_DIR_DEFAULT in
                      scripts/lib.sh, shared with scripts/author.sh). Unknown parameters and positional
                      arguments are rejected.
.PARAMETER CheckOnly  Prerequisite + patch-apply check only (no build).
.PARAMETER KeepDevImage  Keep the build container as docker image ffplay-dev:win64 (for scripts/author.sh build).
.PARAMETER Distro     WSL distro (default Ubuntu-26.04).
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [switch]$Force,
    [string]$WorkDir,
    [switch]$CheckOnly,
    [switch]$KeepDevImage,
    [string]$Distro = 'Ubuntu-26.04'
)
$ErrorActionPreference = 'Stop'

function Fail([string]$msg, [int]$code = 2) {
    [Console]::Error.WriteLine("build-ffplay: $msg")
    exit $code
}

function ConvertTo-WslPath([string]$p) {
    $full = [System.IO.Path]::GetFullPath($p)
    if ($full -match '^([A-Za-z]):[\\/](.*)$') {
        return '/mnt/' + $Matches[1].ToLowerInvariant() + '/' + ($Matches[2] -replace '\\', '/').TrimEnd('/')
    }
    Fail "cannot map '$p' to a WSL path (only drive-letter paths are supported)"
}

if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
    Fail 'wsl.exe not found. Install WSL (wsl --install) with the Ubuntu-26.04 distro.'
}
$distros = (& wsl.exe -l -q 2>$null) -join "`n" -replace "`0", ''
if ($distros -notmatch [regex]::Escape($Distro)) {
    Fail "WSL distro '$Distro' not found (installed: $(($distros -split "`n" | Where-Object { $_.Trim() }) -join ', ')). Use -Distro or install it."
}

$ffDir = $PSScriptRoot
$dist = Join-Path $ffDir 'dist\win64'
$wslScript = (ConvertTo-WslPath $ffDir) + '/scripts/build-ffplay.sh'

# Digest from the bash side (single source of truth).
$digest = ((& wsl.exe -d $Distro -- bash $wslScript --print-digest 2>$null) | Select-Object -First 1)
$digest = "$digest".Trim()
if ($LASTEXITCODE -ne 0 -or $digest -notmatch '^[0-9a-f]{64}$') {
    Fail "could not compute input digest via WSL ($wslScript --print-digest)"
}

$stampFile = Join-Path $dist '.stamp'
if (-not $Force -and -not $CheckOnly -and (Test-Path $stampFile) -and -not (Test-Path (Join-Path $dist '.dev-build')) -and
    (Test-Path (Join-Path $dist 'libffplay.dll')) -and
    (Test-Path (Join-Path $dist 'ffcapsdump.exe'))) {
    $stamp = (Get-Content -Raw $stampFile).Trim()
    if ($stamp -eq $digest) {
        Write-Host "ffplay build up to date (digest $digest)."
        exit 0
    }
}

$args2 = @($wslScript)
if ($Force)        { $args2 += '--force' }
if ($CheckOnly)    { $args2 += '--check-only' }
if ($KeepDevImage) { $args2 += '--keep-dev-image' }
if ($WorkDir) {
    $wd = if ($WorkDir -match '^[A-Za-z]:[\\/]') { ConvertTo-WslPath $WorkDir } else { $WorkDir }
    $args2 += @('--work-dir', $wd)
}

# `bash -lc` so ~ and login PATH (docker) are available; arguments are single-quoted.
$quoted = ($args2 | ForEach-Object { "'" + ($_ -replace "'", "'\''") + "'" }) -join ' '
& wsl.exe -d $Distro -- bash -lc "bash $quoted"
$rc = $LASTEXITCODE
if ($rc -ne 0) {
    [Console]::Error.WriteLine("build-ffplay: WSL build script failed with exit code $rc (see $ffDir\.work\build.log).")
    exit $rc
}
exit 0
