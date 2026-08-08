[CmdletBinding()]
param(
    [string]$OutputDirectory,
    [string]$SourceA,
    [string]$SourceB
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo 'build\ucrt64-release'
$exe = Join-Path $build 'bin\mvm_compositor_spike.exe'
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $build 'p2-seek-profile' }
if (-not $SourceA) { $SourceA = Join-Path $repo 'tests\assets\benchmark\v1080p60_h264.mp4' }
if (-not $SourceB) { $SourceB = Join-Path $repo 'tests\assets\benchmark\v1080p60_hevc.mp4' }
foreach ($required in @($exe, $SourceA, $SourceB)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "P2 seek profile必須fileがありません: $required" }
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"

$runs = @()
foreach ($run in 1..3) {
    $path = Join-Path $OutputDirectory "seek-run$run.json"
    if (Test-Path -LiteralPath $path) {
        $stamp = Get-Date -Format 'yyyyMMdd-HHmmssfff'
        Move-Item -LiteralPath $path -Destination "$path.previous-$stamp"
    }
    Write-Host "P2 seek profile run $run/3 を開始します"
    & $exe --source-a $SourceA --source-b $SourceB --metrics $path `
        --warmup-seconds 5 --measure-seconds 15 --seed 20260808 --seek-count 256 `
        --display-timeout-ms 2000 --gpu-completion fence --mode seek --diagnostic-timing
    if ($LASTEXITCODE -ne 0) { throw "seek profile run $run が失敗しました (exit=$LASTEXITCODE)" }
    $raw = Get-Content -LiteralPath $path -Raw -Encoding utf8 | ConvertFrom-Json
    if ($raw.schema -ne 'mvm-p2-diagnostic-1' -or
        $raw.seek_stage_timings.request_to_display_ms.count -ne 256) {
        throw "seek profile run $run のJSON契約が不正です"
    }
    $runs += [ordered]@{
        run = $run
        raw_path = $path
        stages = $raw.seek_stage_timings
        lock_timings = $raw.d3d11_lock_timings
        mismatch = $raw.seek_display_mismatch
        timeout = $raw.seek_timeout_count
    }
}

$summary = [ordered]@{
    schema = 'mvm-p2-seek-profile-summary-1'
    git_commit = (& git -C $repo rev-parse HEAD)
    seed = 20260808
    seek_count_per_run = 256
    threshold_pass_fail_applied = $false
    runs = $runs
}
$summaryPath = Join-Path $OutputDirectory 'summary.json'
$summary | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $summaryPath -Encoding utf8
Write-Host "P2 seek profile summary: $summaryPath"
