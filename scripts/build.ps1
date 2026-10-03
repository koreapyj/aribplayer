<#
.SYNOPSIS
  Configures and builds with CMake (Ninja, MSVC x64) inside a VS 2022 x64 developer environment.

.DESCRIPTION
  CMake and Ninja come from the VS install.

  Output: artifacts\bin\<Config>\<Target>\ (the app is artifacts\bin\Release\PlayerApp\aribplayer.exe),
  build tree in artifacts\cmake\<Preset>\.

.EXAMPLE
  .\scripts\build.ps1                       # Release
  .\scripts\build.ps1 -Preset debug
  .\scripts\build.ps1 -Test unit            # build, then ctest -L unit
  .\scripts\build.ps1 -Fresh -Test all      # reconfigure from scratch, build, then every ctest test
  .\scripts\build.ps1 -Version 1.2.3        # VERSIONINFO of aribplayer.exe (CI passes the MSI version)
#>
[CmdletBinding()]
param(
    [ValidateSet('release', 'debug')][string]$Preset = 'release',
    [ValidateSet('none', 'unit', 'all')][string]$Test = 'none',
    [string]$WindowsSdkVersion = '10.0.26100.0',
    [string]$Version,             # major.minor.build for aribplayer.exe's VERSIONINFO (default: the cached value, 0.0.0)
    [switch]$Fresh
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

function Invoke-Checked([string]$exe, [string[]]$argv) {
    Write-Host "build: $exe $($argv -join ' ')"
    & $exe @argv
    if ($LASTEXITCODE -ne 0) { throw "build: $exe failed ($LASTEXITCODE)" }
}

$sdk = ($env:WindowsSDKVersion -replace '[\\/]+$', '')
if ($env:VSCMD_ARG_TGT_ARCH -eq 'x64' -and $sdk -eq $WindowsSdkVersion) {
    Write-Host "build: using the current x64 developer environment (SDK $sdk)"
} else {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw "build: vswhere.exe not found; install Visual Studio 2022 or Build Tools" }
    $vs = & $vswhere -latest -products * -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'build: no Visual Studio 2022 with the x64 C++ tools found' }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    Write-Host "build: entering $vcvars $WindowsSdkVersion"
    $env:PATH = "$(Split-Path $vswhere);$env:PATH"   # vcvars calls vswhere by name
    $lines = & cmd.exe /d /s /c "`"$vcvars`" $WindowsSdkVersion >nul && set"
    if ($LASTEXITCODE -ne 0) { throw "build: vcvars64.bat failed ($LASTEXITCODE); is Windows SDK $WindowsSdkVersion installed?" }
    foreach ($line in $lines) {
        if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') }
    }
}

Push-Location $Root
# cl writes /showIncludes notes ("Note: including file:", localized e.g. "참고: 포함 파일:") in the console output code page,
# and Ninja records header dependencies only when they match the prefix CMake detected at configure time. Build in
# UTF-8 always, so trees configured/built from consoles with different code pages (e.g. CP949) keep dependency tracking.
$prevOutputEncoding = [Console]::OutputEncoding
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
try {
    $configure = @('--preset', $Preset)
    if ($Fresh) { $configure += '--fresh' }
    if ($Version) { $configure += "-DARIBPLAYER_VERSION=$Version" }
    Invoke-Checked 'cmake' $configure
    Invoke-Checked 'cmake' @('--build', '--preset', $Preset)
    switch ($Test) {
        'unit' { Invoke-Checked 'ctest' @('--preset', "$Preset-unit") }
        'all'  { Invoke-Checked 'ctest' @('--preset', $Preset) }
    }
} finally {
    [Console]::OutputEncoding = $prevOutputEncoding
    Pop-Location
}
