param(
    [Parameter(Mandatory)][string]$Exe,
    [Parameter(Mandatory)][string]$Checker,
    [Parameter(Mandatory)][string]$Output,
    [int]$DurationMs = 4000,
    [int]$PreflightVBlanks = 120
)
$ErrorActionPreference = 'Stop'

& $Exe --metrics $Output --duration-ms $DurationMs --preflight-vblanks $PreflightVBlanks
$probeExit = $LASTEXITCODE
if (-not (Test-Path -LiteralPath $Output)) {
    throw "probeがJSONを出力しませんでした (exit=$probeExit)"
}
& pwsh -NoProfile -File $Checker -Json $Output -ProcessExitCode $probeExit
if ($LASTEXITCODE -ne 0) {
    throw "P2 vblank authority probeがcontractを満たしません (checker exit=$LASTEXITCODE)"
}
Write-Host 'P2 vblank authority probe live: PASS'
