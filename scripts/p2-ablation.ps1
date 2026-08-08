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
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $build 'p2-ablation' }
if (-not $SourceA) { $SourceA = Join-Path $repo 'tests\assets\benchmark\v1080p60_h264.mp4' }
if (-not $SourceB) { $SourceB = Join-Path $repo 'tests\assets\benchmark\v1080p60_hevc.mp4' }
foreach ($required in @($exe, $SourceA, $SourceB)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "P2 ablation必須fileがありません: $required" }
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"

$entries = [System.Collections.Generic.List[object]]::new()
foreach ($case in @('a', 'b', 'c', 'd')) {
    foreach ($run in 1..3) {
        $path = Join-Path $OutputDirectory "$case-run$run.json"
        if (Test-Path -LiteralPath $path) {
            $stamp = Get-Date -Format 'yyyyMMdd-HHmmssfff'
            Move-Item -LiteralPath $path -Destination "$path.previous-$stamp"
        }
        Write-Host "P2 ablation $case run $run/3 を開始します"
        & $exe --source-a $SourceA --source-b $SourceB --metrics $path `
            --warmup-seconds 5 --measure-seconds 15 --seed 20260808 --seek-count 256 `
            --display-timeout-ms 2000 --gpu-completion fence --mode playback `
            --diagnostic-case $case
        if ($LASTEXITCODE -ne 0) { throw "ablation $case run $run が失敗しました (exit=$LASTEXITCODE)" }
        $raw = Get-Content -LiteralPath $path -Raw -Encoding utf8 | ConvertFrom-Json
        if ($raw.schema -ne 'mvm-p2-diagnostic-1' -or $raw.diagnostic_case -ne $case) {
            throw "ablation $case run $run のJSON契約が不正です"
        }
        $entries.Add([pscustomobject]@{ case=$case; run=$run; path=$path; raw=$raw })
    }
}

$cases = @($entries | Group-Object case | ForEach-Object {
    $runs = @($_.Group | ForEach-Object {
        $r = $_.raw
        [ordered]@{
            run = $_.run
            raw_path = $_.path
            effective_fps = $r.effective_fps
            drop_rate = $r.drop_rate
            deadline_drop_rate = $r.deadline_drop_rate
            deadline_drop = $r.measurement_drop_scheduler_deadline
            missing_pair_count = $r.missing_pair_drop_count
            decoded_a = $r.measurement_decoded_a_count
            decoded_b = $r.measurement_decoded_b_count
            wait_for_space_a = $r.measurement_wait_for_space_a_count
            wait_for_space_b = $r.measurement_wait_for_space_b_count
            buffer_depth_a = $r.render_stage_timings.buffer_depth_a
            buffer_depth_b = $r.render_stage_timings.buffer_depth_b
            render_callback_interval_us = $r.render_stage_timings.render_callback_interval_us
            stage_timings = $r.render_stage_timings
            lock_timings = $r.d3d11_lock_timings
            retirement_depth_peak = $r.retirement_depth_peak
        }
    })
    [ordered]@{
        case = $_.Name
        runs = $runs
        min_fps = [double](($runs.effective_fps | Measure-Object -Minimum).Minimum)
        max_fps = [double](($runs.effective_fps | Measure-Object -Maximum).Maximum)
        min_drop = [double](($runs.drop_rate | Measure-Object -Minimum).Minimum)
        max_drop = [double](($runs.drop_rate | Measure-Object -Maximum).Maximum)
    }
})

$summary = [ordered]@{
    schema = 'mvm-p2-ablation-summary-1'
    git_commit = (& git -C $repo rev-parse HEAD)
    seed = 20260808
    warmup_seconds = 5
    measure_seconds = 15
    threshold_pass_fail_applied = $false
    cases = $cases
}
$summaryPath = Join-Path $OutputDirectory 'summary.json'
$summary | ConvertTo-Json -Depth 15 | Set-Content -LiteralPath $summaryPath -Encoding utf8
Write-Host "P2 ablation summary: $summaryPath"
