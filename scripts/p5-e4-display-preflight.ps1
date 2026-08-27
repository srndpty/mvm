[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [ValidateRange(2,20)][int]$Samples = 5,
    [ValidateRange(0,10)][int]$IntervalSeconds = 2,
    [ValidateRange(1000,30000)][int]$TimeoutMs = 3000,
    [switch]$RequireCleanGit
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = Split-Path -Parent $PSScriptRoot
$checker = Join-Path $PSScriptRoot 'check-p5-e4-display-preflight.ps1'
foreach ($path in @($Executable,$checker)) {
    if (-not (Test-Path -LiteralPath $path)) { throw "display preflight必須fileがありません: $path" }
}
if (Test-Path -LiteralPath $OutputDirectory) {
    throw "display preflight artifactを上書きしません: $OutputDirectory"
}
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
$gitCommit = (& git -C $repo rev-parse HEAD).Trim()
$gitStatus = @(& git -C $repo status --porcelain)
$gitClean = $LASTEXITCODE -eq 0 -and $gitStatus.Count -eq 0

$paths = [System.Collections.Generic.List[string]]::new()
$runs = [System.Collections.Generic.List[object]]::new()
if (-not $RequireCleanGit -or $gitClean) { foreach ($index in 1..$Samples) {
    $path = Join-Path $OutputDirectory "probe-$index.json"
    Write-Host "ATTR-Q2B display preflight $index/$Samples を開始します" -ForegroundColor Cyan
    & $Executable --metrics $path --timeout-ms $TimeoutMs
    $exitCode = $LASTEXITCODE
    $paths.Add($path)
    $runs.Add([pscustomobject]@{sample=$index;path=$path;process_exit_code=$exitCode})
    if ($exitCode -ne 0 -or -not (Test-Path -LiteralPath $path)) { break }
    if ($index -lt $Samples -and $IntervalSeconds -gt 0) { Start-Sleep -Seconds $IntervalSeconds }
} }

$checkerExit = -1
if ($paths.Count -eq $Samples -and @($runs | Where-Object {$_.process_exit_code -ne 0}).Count -eq 0) {
    & pwsh -NoProfile -File $checker -JsonDirectory $OutputDirectory -ExpectedSamples $Samples
    $checkerExit = $LASTEXITCODE
}
$completedSamples = @(Get-ChildItem -LiteralPath $OutputDirectory -Filter 'probe-*.json' -File).Count
$summary = [ordered]@{
    schema='mvm-p5-e4-display-preflight-1'
    authority='DIAGNOSTIC_GATE_ONLY'
    workload_started=$false
    passed=($paths.Count -eq $Samples -and $checkerExit -eq 0)
    expected_samples=$Samples
    attempted_samples=$runs.Count
    completed_samples=$completedSamples
    interval_seconds=$IntervalSeconds
    timeout_ms=$TimeoutMs
    probe_executable=$Executable
    probe_executable_sha256=(Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash.ToLowerInvariant()
    checker=$checker
    checker_sha256=(Get-FileHash -LiteralPath $checker -Algorithm SHA256).Hash.ToLowerInvariant()
    checker_exit_code=$checkerExit
    git_commit=$gitCommit
    dirty_worktree=(-not $gitClean)
    git_status_porcelain=$gitStatus
    clean_git_required=[bool]$RequireCleanGit
    runs=@($runs)
}
$summaryPath = Join-Path $OutputDirectory 'summary.json'
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $summaryPath -Encoding utf8
if ($RequireCleanGit -and -not $gitClean) {
    Write-Host 'display preflightはclean committed probeでだけ実行します' -ForegroundColor Red
    exit 4
}
if (-not $summary.passed) { exit 4 }
exit 0
