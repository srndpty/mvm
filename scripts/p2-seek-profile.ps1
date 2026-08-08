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
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $build 'p2-seek-profile-d4c' }
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
        $raw.seek_stage_timings.request_to_display_ms.count -ne 256 -or
        $raw.seek_overlap_count -eq 0 -or $raw.seek_display_mismatch -ne 0 -or
        $raw.seek_timeout_count -ne 0 -or $raw.seek_stale_completion_count -ne 0 -or
        $raw.seek_busy_acceptance_count -ne 0 -or $raw.software_fallback_count -ne 0 -or
        $raw.seek_completion_publish_reject_count -ne 0 -or
        $raw.seek_completion_request_mismatch_count -ne 0 -or
        $raw.cpu_full_frame_readback_count -ne 0 -or $raw.device_lost_count -ne 0 -or
        $raw.worker_join_leak_count -ne 0) {
        throw "seek profile run $run のJSON契約が不正です"
    }
    $runs += [ordered]@{
        run = $run
        raw_path = $path
        stages = $raw.seek_stage_timings
        lock_timings = $raw.d3d11_lock_timings
        mismatch = $raw.seek_display_mismatch
        timeout = $raw.seek_timeout_count
        stale_completion = $raw.seek_stale_completion_count
        busy_acceptance = $raw.seek_busy_acceptance_count
        completion_publish_reject = $raw.seek_completion_publish_reject_count
        completion_request_mismatch = $raw.seek_completion_request_mismatch_count
        overlap_count = $raw.seek_overlap_count
    }
}

$oldSummaryPath = Join-Path $build 'p2-seek-profile\summary.json'
$oldRuns = @()
if (Test-Path -LiteralPath $oldSummaryPath) {
    $oldSummary = Get-Content -LiteralPath $oldSummaryPath -Raw -Encoding utf8 | ConvertFrom-Json
    $oldRuns = @($oldSummary.runs | ForEach-Object {
        [ordered]@{
            run = $_.run
            a_p95_ms = $_.stages.seek_a_ms.p95
            b_p95_ms = $_.stages.seek_b_ms.p95
            dual_decode_ready_p95_ms = $_.stages.dual_decode_ready_ms.p95
            request_to_display_p95_ms = $_.stages.request_to_display_ms.p95
        }
    })
}

$summary = [ordered]@{
    schema = 'mvm-p2-seek-profile-summary-1'
    git_commit = (& git -C $repo rev-parse HEAD)
    seed = 20260808
    seek_count_per_run = 256
    threshold_pass_fail_applied = $false
    old_serial_summary_path = if ($oldRuns.Count) { $oldSummaryPath } else { $null }
    old_serial_runs = $oldRuns
    runs = $runs
}
$summaryPath = Join-Path $OutputDirectory 'summary.json'
$summary | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $summaryPath -Encoding utf8
Write-Host "P2 seek profile summary: $summaryPath"
