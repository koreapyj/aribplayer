<#
.SYNOPSIS
  Serves testmedia/ over HTTP (python -m http.server) so http:// playlists and HLS can be tested.
#>
[CmdletBinding()]
param(
    [int]$Port = 8000,
    [string]$Directory = (Join-Path (Split-Path -Parent $PSScriptRoot) 'testmedia')
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Directory)) { throw "Directory not found: $Directory (run scripts\make-testmedia.ps1 first)" }
$py = Get-Command python -ErrorAction SilentlyContinue
if (-not $py) { $py = Get-Command py -ErrorAction Stop }
Write-Host "Serving $Directory on http://127.0.0.1:$Port/ (Ctrl+C to stop)"
Push-Location $Directory
try { & $py.Source -m http.server $Port --bind 127.0.0.1 } finally { Pop-Location }
