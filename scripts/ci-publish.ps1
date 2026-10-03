#Requires -Version 7
# Called only by the serialized, contents:write release job. Never uses a local git checkout.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Version,
    [Parameter(Mandatory)][string]$ArtifactDirectory
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Invoke-Gh {
    param([Parameter(ValueFromRemainingArguments)][string[]]$Arguments)
    $output = & gh @Arguments
    if ($LASTEXITCODE -ne 0) { throw "gh failed: $($Arguments -join ' ')" }
    return $output
}

$repo = $env:GITHUB_REPOSITORY
$ref = $env:GITHUB_REF
$sha = $env:GITHUB_SHA
$run = [long]$env:GITHUB_RUN_NUMBER
if ($env:GITHUB_EVENT_NAME -ne 'push' -or ($ref -ne 'refs/heads/main' -and $ref -notlike 'refs/tags/v*')) {
    throw 'Only pushes to main or v* tags can publish a release.'
}
$msi = Join-Path $ArtifactDirectory "aribplayer-$Version-x64.msi"
if (-not (Test-Path -LiteralPath $msi -PathType Leaf)) { throw "MSI missing: $msi" }
if ($sha -notmatch '^[0-9a-fA-F]{40}$') { throw "GITHUB_SHA is not a full commit id: $sha" }
$shortSha = $sha.Substring(0, 8).ToLowerInvariant()
$isMain = $ref -eq 'refs/heads/main'
$tag = if ($isMain) { 'main' } else { $ref.Substring('refs/tags/'.Length) }

# gh release view exits nonzero for a missing release; all subsequent mutations
# still fail closed if that exit was a permission/network problem instead.
$existingText = & gh release view $tag --repo $repo --json body,assets 2>$null
$existing = if ($LASTEXITCODE -eq 0) { ($existingText -join "`n") | ConvertFrom-Json } else { $null }
if ($isMain) {
    # Job concurrency is not FIFO. Do not let an older build finishing later (or
    # an old workflow rerun) roll the tag, asset or MSI version back.
    $head = Invoke-Gh api "repos/$repo/git/ref/heads/main" --jq '.object.sha'
    if ($head -ne $sha) {
        'Skipping rolling release: a newer main commit exists.'
        return
    }
    if ($existing -and $existing.body -match '<!-- ci-run:([0-9]+) -->' -and [long]$Matches[1] -gt $run) {
        'Skipping rolling release: a newer workflow run has already published.'
        return
    }
    $null = & gh api "repos/$repo/git/ref/tags/main" 2>$null
    if ($LASTEXITCODE -eq 0) {
        $null = Invoke-Gh api --method PATCH "repos/$repo/git/refs/tags/main" -f "sha=$sha" -F 'force=true'
    } else {
        $null = Invoke-Gh api --method POST "repos/$repo/git/refs" -f 'ref=refs/tags/main' -f "sha=$sha"
    }
    $title = "main ($shortSha)"
    $notes = "ARIB Player: rolling, unsigned development build of main.`n`nCommit: $shortSha ($sha)`nMSI ProductVersion: $Version`nWorkflow run: $env:GITHUB_SERVER_URL/$repo/actions/runs/$env:GITHUB_RUN_ID`n`n<!-- ci-run:$run -->"
    # The file is named after the commit; Windows Installer still upgrades by the numeric ProductVersion.
    $asset = Join-Path $ArtifactDirectory "aribplayer-$shortSha-x64.msi"
    Copy-Item -LiteralPath $msi -Destination $asset -Force
} else {
    $title = $tag
    $notes = "ARIB Player $Version (unsigned x64, per-user installer).`n`nCommit: $sha`nWorkflow run: $env:GITHUB_SERVER_URL/$repo/actions/runs/$env:GITHUB_RUN_ID"
    $asset = $msi
}

if (-not $existing) {
    # Keep a new release draft until its asset has uploaded successfully.
    $null = Invoke-Gh release create $tag --repo $repo --verify-tag --draft --title $title --notes $notes --target $sha
}
$null = Invoke-Gh release upload $tag $asset --repo $repo --clobber
if ($isMain) {
    if ($existing) {
        $keep = Split-Path $asset -Leaf
        foreach ($old in $existing.assets) {
            if ($old.name -like '*.msi' -and $old.name -ne $keep) {
                $null = Invoke-Gh release delete-asset $tag $old.name --repo $repo --yes
            }
        }
    }
    $null = Invoke-Gh release edit $tag --repo $repo --title $title --notes $notes --target $sha --draft=false --prerelease --latest=false
} else {
    $null = Invoke-Gh release edit $tag --repo $repo --title $title --notes $notes --draft=false --prerelease=false
}
"Published $tag ($Version)."
