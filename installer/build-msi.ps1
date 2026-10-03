<#
.SYNOPSIS
  Builds the per-user x64 ARIB Player MSI with the WiX dotnet tool (pinned: wix 7.0.0).

.DESCRIPTION

  Output: <OutputDirectory>\aribplayer-<Version>-x64.msi ; its absolute path is the only pipeline output.
  Everything else goes to the host stream, so `$msi = ./installer/build-msi.ps1 ...` captures just the path.

  Runtime payload (verified against the sources: src\App\features\SettingsService.cpp ExeDir()/BundledAribFontPath,
  CMakeLists.txt wvp_copy_ffplay_dlls, src\App\CMakeLists.txt font copy, dumpbin /dependents):
    aribplayer.exe                 static CRT (/MT), imports libffplay.dll
    <every *.dll in ffmpeg dist>   libffplay.dll + av*/sw* (libffplay and av* import each other)
    fonts\WLCMARU2004ARIBU.TTF     read by the app at <exe dir>\fonts
    THIRD-PARTY-NOTICES.txt        repo root
    licenses\*                     <ffmpeg dist>\licenses (FFmpeg COPYING*, LICENSE.md, ...)
  Not shipped: pdb/lib/exp/def/.dll.a, ffmpeg.exe/ffplay.exe/ffprobe.exe, ffplay.h, build debris.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Version,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$AppDirectory,
    [string]$FfmpegDirectory,
    [string]$LicensesDirectory,
    [string]$IconFile,
    [string]$WorkDirectory,
    [string]$ExpectedWixVersion = '7.0.0',
    [string]$AcceptEula = 'wix7', # OSMF EULA id for WiX v7 (passed per invocation: -acceptEula)
    [switch]$StageOnly
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $AppDirectory)      { $AppDirectory      = Join-Path $Root 'artifacts\bin\Release\PlayerApp' }
if (-not $FfmpegDirectory)   { $FfmpegDirectory   = Join-Path $Root 'ffmpeg\dist\win64' }
if (-not $LicensesDirectory) { $LicensesDirectory = Join-Path $FfmpegDirectory 'licenses' }
if (-not $IconFile)          { $IconFile          = Join-Path $Root 'assets\icon\aribplayer.ico' }

function Fail([string]$m) { throw "build-msi: $m" }
function Full([string]$p) {
    if ([System.IO.Path]::IsPathRooted($p)) { return [System.IO.Path]::GetFullPath($p) }
    [System.IO.Path]::GetFullPath((Join-Path (Get-Location).Path $p))
}

if ($Version -notmatch '^(0|[1-9]\d{0,4})\.(0|[1-9]\d{0,4})\.(0|[1-9]\d{0,5})$') { Fail "Version '$Version' is not major.minor.build" }
if ([int]$Matches[1] -gt 255 -or [int]$Matches[2] -gt 255 -or [long]$Matches[3] -gt 65535) { Fail "Version '$Version' exceeds MSI limits (255.255.65535)" }

$OutputDirectory = Full $OutputDirectory
if (-not $WorkDirectory) { $WorkDirectory = Join-Path $OutputDirectory 'obj' }
$WorkDirectory = Full $WorkDirectory
$Stage = Join-Path $WorkDirectory 'stage'
$Msi   = Join-Path $OutputDirectory "aribplayer-$Version-x64.msi"

foreach ($d in @($AppDirectory, $FfmpegDirectory, $LicensesDirectory)) {
    if (-not (Test-Path -LiteralPath $d -PathType Container)) { Fail "missing directory: $d" }
}
$AppDirectory      = (Resolve-Path -LiteralPath $AppDirectory).Path
$FfmpegDirectory   = (Resolve-Path -LiteralPath $FfmpegDirectory).Path
$LicensesDirectory = (Resolve-Path -LiteralPath $LicensesDirectory).Path

$exe = Join-Path $AppDirectory 'aribplayer.exe'
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { Fail "missing $exe" }
$font = Join-Path $AppDirectory 'fonts\WLCMARU2004ARIBU.TTF'
if (-not (Test-Path -LiteralPath $font -PathType Leaf)) { Fail "missing $font (the CMake build copies it from assets\fonts; rebuild PlayerApp)" }
if (-not (Test-Path -LiteralPath $IconFile -PathType Leaf)) { Fail "missing icon $IconFile (assets\icon\make-icon.py generates it)" }
$IconFile = (Resolve-Path -LiteralPath $IconFile).Path
$notice = Join-Path $Root 'THIRD-PARTY-NOTICES.txt'
if (-not (Test-Path -LiteralPath $notice -PathType Leaf)) { Fail "missing $notice" }

$distDlls = @(Get-ChildItem -LiteralPath $FfmpegDirectory -Filter '*.dll' -File)
if ($distDlls.Count -eq 0) { Fail "no DLLs in $FfmpegDirectory" }
if (-not ($distDlls | Where-Object Name -eq 'libffplay.dll')) { Fail "libffplay.dll missing from $FfmpegDirectory" }

# The DLLs next to the exe (copied by the CMake build) must be byte-identical to the dist DLLs we ship.
foreach ($d in $distDlls) {
    $a = Join-Path $AppDirectory $d.Name
    if (-not (Test-Path -LiteralPath $a -PathType Leaf)) { Fail "$($d.Name) is in dist but missing next to the exe ($AppDirectory)" }
    if ((Get-FileHash -LiteralPath $a -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $d.FullName -Algorithm SHA256).Hash) {
        Fail "$($d.Name) next to the exe differs from $($d.FullName); rebuild the app against this ffmpeg dist"
    }
}
$debris = '\.(pdb|lib|exp|ilk|iobj|ipdb|obj|log|tlog|idb)$|\.(locked|old)-'
$distNames = @($distDlls | ForEach-Object { $_.Name })
foreach ($f in @(Get-ChildItem -LiteralPath $AppDirectory -File)) {
    if ($f.Name -eq 'aribplayer.exe' -or $distNames -contains $f.Name -or $f.Name -match $debris) { continue }
    if ($f.Extension -ieq '.dll') { Fail "unexpected DLL $($f.Name) next to the exe and not in ffmpeg dist; add it to the payload deliberately" }
    Write-Host "build-msi: WARNING not packaged (unknown app file): $($f.Name)"
}
foreach ($d in @(Get-ChildItem -LiteralPath $AppDirectory -Directory | Where-Object Name -ne 'fonts')) {
    Write-Host "build-msi: WARNING not packaged (unknown app directory): $($d.Name)"
}
foreach ($f in @(Get-ChildItem -LiteralPath (Join-Path $AppDirectory 'fonts') -File | Where-Object Name -ne 'WLCMARU2004ARIBU.TTF')) {
    Write-Host "build-msi: WARNING not packaged (unknown font file): $($f.Name)"
}

$licFiles = @(Get-ChildItem -LiteralPath $LicensesDirectory -Recurse -File)
if ($licFiles.Count -eq 0) { Fail "no license files in $LicensesDirectory" }
if (-not ($licFiles | Where-Object { $_.Name -like 'COPYING*' })) { Fail "no COPYING* in $LicensesDirectory (FFmpeg license texts)" }
if (-not ($licFiles | Where-Object { $_.Name -ieq 'LICENSE.md' })) { Fail "no LICENSE.md in $LicensesDirectory" }

# Only this build's own staging directory is cleared (recursive delete confined to <WorkDirectory>\stage).
if (Test-Path -LiteralPath $Stage) {
    if ((Split-Path $Stage -Leaf) -ne 'stage' -or $Stage.Length -lt 12) { Fail "refusing to clean $Stage" }
    Remove-Item -LiteralPath $Stage -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $Stage, (Join-Path $Stage 'fonts'), (Join-Path $Stage 'licenses'), $OutputDirectory | Out-Null
Copy-Item -LiteralPath $exe -Destination $Stage
foreach ($d in $distDlls) { Copy-Item -LiteralPath $d.FullName -Destination $Stage }
Copy-Item -LiteralPath $font -Destination (Join-Path $Stage 'fonts')
Copy-Item -LiteralPath $notice -Destination $Stage
foreach ($f in $licFiles) {
    $rel = $f.FullName.Substring($LicensesDirectory.Length).TrimStart('\')
    $dst = Join-Path (Join-Path $Stage 'licenses') $rel
    New-Item -ItemType Directory -Force -Path (Split-Path $dst) | Out-Null
    Copy-Item -LiteralPath $f.FullName -Destination $dst
}

$ns = [guid]'B254C43A-9A0B-46A8-A17A-0513B6C077CD'   # namespace for deterministic GUIDs (= UpgradeCode)
function New-NameGuid([string]$name) {               # RFC 4122 v5 (SHA-1) over namespace + name
    $nb = $ns.ToByteArray()
    [Array]::Reverse($nb, 0, 4); [Array]::Reverse($nb, 4, 2); [Array]::Reverse($nb, 6, 2)
    $data = [Text.Encoding]::UTF8.GetBytes($name)
    $h = [Security.Cryptography.SHA1]::Create().ComputeHash([byte[]]($nb + $data))
    $g = [byte[]]$h[0..15]
    $g[6] = ($g[6] -band 0x0F) -bor 0x50
    $g[8] = ($g[8] -band 0x3F) -bor 0x80
    [Array]::Reverse($g, 0, 4); [Array]::Reverse($g, 4, 2); [Array]::Reverse($g, 6, 2)
    ([guid]::new($g)).ToString().ToUpperInvariant()
}
function New-Id([string]$prefix, [string]$rel) {
    $san = ($rel -replace '[^A-Za-z0-9_]', '_')
    if ($san.Length -gt 40) { $san = $san.Substring($san.Length - 40) }
    $hash = (New-NameGuid ('id:' + $rel)).Replace('-', '').Substring(0, 8)
    "${prefix}_${san}_$hash"
}
function X([string]$s) { [Security.SecurityElement]::Escape($s) }
function DirId([string]$rel) { if ($rel -eq '') { 'INSTALLFOLDER' } else { New-Id 'Dir' $rel } }
function RelKey([string]$rel) { if ($rel -eq '') { '(root)' } else { $rel } }

$dirs = @('') + @(Get-ChildItem -LiteralPath $Stage -Recurse -Directory | ForEach-Object { $_.FullName.Substring($Stage.Length).TrimStart('\') } | Sort-Object)
$q = '"'
$sb = New-Object Text.StringBuilder
[void]$sb.AppendLine('<?xml version="1.0" encoding="utf-8"?>')
[void]$sb.AppendLine('<!-- GENERATED by installer/build-msi.ps1. Do not edit. -->')
[void]$sb.AppendLine('<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs">')
foreach ($rel in $dirs) {
    if ($rel -eq '') { continue }
    $parent = Split-Path $rel -Parent
    [void]$sb.AppendLine("  <Fragment><DirectoryRef Id=$q$(DirId $parent)$q><Directory Id=$q$(DirId $rel)$q Name=$q$(X (Split-Path $rel -Leaf))$q /></DirectoryRef></Fragment>")
}
[void]$sb.AppendLine('  <Fragment>')
[void]$sb.AppendLine('    <ComponentGroup Id="PayloadComponents">')
$fileCount = 0
foreach ($rel in $dirs) {
    $files = @(Get-ChildItem -LiteralPath (Join-Path $Stage $rel) -File | Sort-Object Name)
    # Even directories containing only subdirectories need RemoveFolder for ICE64.
    $cid  = New-Id 'Cmp' (RelKey $rel)
    $guid = New-NameGuid ('component:' + (RelKey $rel))
    [void]$sb.AppendLine("      <Component Id=$q$cid$q Directory=$q$(DirId $rel)$q Guid=$q$guid$q>")
    foreach ($f in $files) {
        $fid = New-Id 'Fil' (('{0}/{1}' -f $rel, $f.Name).TrimStart('/'))
        [void]$sb.AppendLine("        <File Id=$q$fid$q Name=$q$(X $f.Name)$q Source=$q$(X $f.FullName)$q />")
        $fileCount++
    }
    [void]$sb.AppendLine("        <RemoveFolder Id=$q$(New-Id 'Rmf' (RelKey $rel))$q Directory=$q$(DirId $rel)$q On=${q}uninstall$q />")
    # HKCU key path: required for components in the user profile (ICE38); the value is removed on uninstall.
    [void]$sb.AppendLine("        <RegistryValue Root=${q}HKCU$q Key=${q}Software\aribplayer\Installer$q Name=$q$cid$q Type=${q}integer$q Value=${q}1$q KeyPath=${q}yes$q />")
    [void]$sb.AppendLine('      </Component>')
}
[void]$sb.AppendLine('    </ComponentGroup>')
[void]$sb.AppendLine('  </Fragment>')
[void]$sb.AppendLine('</Wix>')
$payloadWxs = Join-Path $WorkDirectory 'Payload.wxs'
[IO.File]::WriteAllText($payloadWxs, $sb.ToString(), (New-Object Text.UTF8Encoding($false)))
[xml](Get-Content -LiteralPath $payloadWxs -Raw) | Out-Null
[xml](Get-Content -LiteralPath (Join-Path $PSScriptRoot 'Product.wxs') -Raw) | Out-Null
Write-Host "build-msi: staged $fileCount files in $Stage ; generated $payloadWxs"
Write-Host "build-msi: icon (shortcut + ARPPRODUCTICON) $IconFile"

if ($StageOnly) { Write-Host "build-msi: -StageOnly, wix not run. Would write $Msi"; return }

if (-not (Get-Command wix -ErrorAction SilentlyContinue)) { Fail "wix not found on PATH (dotnet tool install --global wix --version $ExpectedWixVersion)" }
# Accept before even querying the version, so a fresh WiX 7 runner cannot hit WIX7015.
# The OSMF fee does not apply: GPL, non-commercial.
& wix eula accept $AcceptEula | Out-Host
if ($LASTEXITCODE -ne 0) { Fail "wix EULA acceptance failed ($LASTEXITCODE)" }
$versionOutput = @(& wix --version)
if ($LASTEXITCODE -ne 0 -or $versionOutput.Count -eq 0) { Fail 'could not query wix version' }
$wv = $versionOutput[0].ToString().Trim()
if (($wv -split '[+\s]')[0] -ne $ExpectedWixVersion) { Fail "wix $wv found, expected $ExpectedWixVersion" }

if (Test-Path -LiteralPath $Msi) { Remove-Item -LiteralPath $Msi -Force }
& wix build -arch x64 -acceptEula $AcceptEula -pdbtype none `
    -intermediatefolder (Join-Path $WorkDirectory 'wixobj') `
    -d "ProductVersion=$Version" -d "IconFile=$IconFile" `
    -o $Msi (Join-Path $PSScriptRoot 'Product.wxs') $payloadWxs | Out-Host
if ($LASTEXITCODE -ne 0) { Fail "wix build failed ($LASTEXITCODE)" }
if (-not (Test-Path -LiteralPath $Msi -PathType Leaf)) { Fail "wix reported success but $Msi does not exist" }
Write-Host "build-msi: built $Msi ($([math]::Round((Get-Item -LiteralPath $Msi).Length / 1MB, 1)) MB, WiX $wv)"
$Msi
