param(
    [Parameter(Mandatory)][string]$Header,
    [Parameter(Mandatory)][string]$Def
)
$text = Get-Content -Raw -LiteralPath $Header
$names = [regex]::Matches($text, 'FFP_API[^;(]*?FFP_CALL\s+(ffp_\w+)\s*\(') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
if (-not $names) { throw "No FFP_API declarations found in $Header" }
$lines = @('LIBRARY libffplay', 'EXPORTS') + ($names | ForEach-Object { "    $_" })
Set-Content -LiteralPath $Def -Value $lines -Encoding ascii
