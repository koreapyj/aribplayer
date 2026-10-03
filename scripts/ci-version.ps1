#Requires -Version 7
[CmdletBinding()]
param(
    [string]$Ref = $env:GITHUB_REF,
    [long]$RunNumber = $env:GITHUB_RUN_NUMBER
)
$ErrorActionPreference = 'Stop'

if ($Ref -like 'refs/tags/v*') {
    # Windows Installer compares three fields only: 0..255, 0..255, 0..65535.
    # Reject suffixes, fourth fields and leading zeroes instead of silently truncating.
    if ($Ref -notmatch '^refs/tags/v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') {
        throw 'Release tags must be vMAJOR.MINOR.BUILD (no prerelease suffix or fourth field).'
    }
    $parts = @($Matches[1], $Matches[2], $Matches[3])
    if ([decimal]$parts[0] -gt 255 -or [decimal]$parts[1] -gt 255 -or [decimal]$parts[2] -gt 65535) {
        throw 'Release tag exceeds MSI limits (255.255.65535).'
    }
    $version = $parts -join '.'
} else {
    # Reserve major 0 for CI. Carry into minor after 65535 rather than overflow
    # MSI's build field. Reruns keep their version; new workflow runs increase it.
    # Tagged stable releases should start at 1.x.y: returning to main after one
    # intentionally requires uninstall, not a downgrade of an installed release.
    if ($RunNumber -lt 1 -or $RunNumber -gt 16777215) {
        throw 'CI run number must be 1..16777215; change the version policy before this range is exhausted.'
    }
    $minor = [long][Math]::Floor($RunNumber / 65536.0)
    $build = $RunNumber % 65536
    $version = "0.$minor.$build"
}

if ($env:GITHUB_OUTPUT) {
    "version=$version" | Out-File -LiteralPath $env:GITHUB_OUTPUT -Append -Encoding utf8
}
$version
