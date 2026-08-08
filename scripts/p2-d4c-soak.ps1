[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('PlaybackStartup', 'PlaybackRegression', 'SeekIntegration', 'Batch')]
    [string]$Stage,
    [ValidateSet('ucrt64-release', 'ucrt64-debug')]
    [string]$Preset = 'ucrt64-release',
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo "build\$Preset"
$compositor = Join-Path $build 'bin\mvm_compositor_spike.exe'
$dualDecode = Join-Path $build 'bin\mvm_test_p2_dual_decode.exe'
$sourceA = Join-Path $repo 'tests\assets\benchmark\v1080p60_h264.mp4'
$sourceB = Join-Path $repo 'tests\assets\benchmark\v1080p60_hevc.mp4'
$ctest = 'C:\msys64\ucrt64\bin\ctest.exe'
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $build 'p2-d4c-reliability'
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"

$results = [System.Collections.Generic.List[object]]::new()
$failed = $false

function Invoke-PlaybackSoak([int]$Count, [int]$Warmup, [int]$Measure) {
    foreach ($run in 1..$Count) {
        $rawPath = Join-Path $OutputDirectory "$($Stage.ToLowerInvariant())-run$run.json"
        Write-Host "$Stage playback run $run/$Count を開始します"
        & $compositor --source-a $sourceA --source-b $sourceB --metrics $rawPath `
            --warmup-seconds $Warmup --measure-seconds $Measure --seed 20260808 `
            --seek-count 64 --display-timeout-ms 2000 --gpu-completion fence `
            --mode playback --formal-preflight
        $exitCode = $LASTEXITCODE
        $raw = if (Test-Path -LiteralPath $rawPath) {
            Get-Content -LiteralPath $rawPath -Raw -Encoding utf8 | ConvertFrom-Json
        } else { $null }
        $ok = $exitCode -eq 0 -and $null -ne $raw -and
            $raw.formal_contract_version -eq 'P2-D4-2' -and
            $raw.configured_measurement_preroll_frames -eq 8 -and
            $raw.measurement_preroll_ok -eq $true -and
            $raw.measurement_preroll_depth_a -ge 8 -and
            $raw.measurement_preroll_depth_b -ge 8 -and
            $raw.measurement_preroll_front_a -eq 0 -and
            $raw.measurement_preroll_front_b -eq 0 -and
            $raw.measurement_first_output_frame -eq 0 -and
            $raw.measurement_missing_pair_count -eq 0 -and
            $raw.measurement_source_a_eof_count -eq 0 -and
            $raw.measurement_source_b_eof_count -eq 0 -and
            $raw.measurement_drop_missing_source_a -eq 0 -and
            $raw.measurement_drop_missing_source_b -eq 0 -and
            $raw.measurement_drop_missing_both -eq 0 -and
            $raw.measurement_drop_stale_generation -eq 0 -and
            $raw.measurement_drop_future_generation -eq 0 -and
            $raw.measurement_drop_stale_composition_epoch -eq 0 -and
            $raw.measurement_drop_render_failure -eq 0 -and
            $raw.device_lost_count -eq 0 -and $raw.worker_join_leak_count -eq 0
        if (-not $ok) { $script:failed = $true }
        $results.Add([ordered]@{
            run = $run; process_exit_code = $exitCode; passed = $ok; raw_path = $rawPath
            preroll_depth_a = if ($raw) { $raw.measurement_preroll_depth_a } else { $null }
            preroll_depth_b = if ($raw) { $raw.measurement_preroll_depth_b } else { $null }
            preroll_front_a = if ($raw) { $raw.measurement_preroll_front_a } else { $null }
            preroll_front_b = if ($raw) { $raw.measurement_preroll_front_b } else { $null }
            first_output = if ($raw) { $raw.measurement_first_output_frame } else { $null }
            scheduled = if ($raw) { $raw.measurement_scheduled_output_count } else { $null }
            displayed = if ($raw) { $raw.measurement_displayed_composition_count } else { $null }
            dropped = if ($raw) { $raw.measurement_dropped_output_count } else { $null }
            deadline_drop = if ($raw) { $raw.measurement_drop_scheduler_deadline } else { $null }
            missing_pair = if ($raw) { $raw.measurement_missing_pair_count } else { $null }
            eof_a = if ($raw) { $raw.measurement_source_a_eof_count } else { $null }
            eof_b = if ($raw) { $raw.measurement_source_b_eof_count } else { $null }
            effective_fps = if ($raw) { $raw.effective_fps } else { $null }
            drop_rate = if ($raw) { $raw.drop_rate } else { $null }
        })
    }
}

if ($Stage -eq 'PlaybackStartup') {
    if ($Preset -ne 'ucrt64-release') { throw 'Playback soakはreleaseで実行してください' }
    Invoke-PlaybackSoak 20 1 2
} elseif ($Stage -eq 'PlaybackRegression') {
    if ($Preset -ne 'ucrt64-release') { throw 'Playback soakはreleaseで実行してください' }
    Invoke-PlaybackSoak 3 5 15
} elseif ($Stage -eq 'SeekIntegration') {
    foreach ($required in @($dualDecode, $sourceA, $sourceB)) {
        if (-not (Test-Path -LiteralPath $required)) { throw "必須fileがありません: $required" }
    }
    foreach ($run in 1..20) {
        Write-Host "$Preset parallel seek integration run $run/20 を開始します"
        $output = & $dualDecode $sourceA $sourceB 2>&1
        $exitCode = $LASTEXITCODE
        $logPath = Join-Path $OutputDirectory "seek-integration-$Preset-run$run.log"
        $output | Set-Content -LiteralPath $logPath -Encoding utf8
        $ok = $exitCode -eq 0
        if (-not $ok) { $failed = $true }
        $results.Add([ordered]@{
            run = $run; process_exit_code = $exitCode; passed = $ok; log_path = $logPath
        })
    }
} else {
    $regex = 'gpu_pure_unit|p2_dual_decode_integration|p2_gpu_compositor_offscreen|p2_formal_checker_|compositor_qt_'
    $listed = & $ctest --test-dir $build -N -R $regex 2>&1
    $total = -1
    foreach ($line in $listed) {
        if ("$line" -match '^Total Tests:\s*(\d+)') { $total = [int]$Matches[1] }
    }
    if ($LASTEXITCODE -ne 0 -or $total -le 0) { throw '関連batchのtest件数を確定できません' }
    foreach ($run in 1..5) {
        Write-Host "$Preset relevant batch run $run/5 ($total tests) を開始します"
        $output = & $ctest --test-dir $build -R $regex --output-on-failure 2>&1
        $exitCode = $LASTEXITCODE
        $logPath = Join-Path $OutputDirectory "batch-$Preset-run$run.log"
        $output | Set-Content -LiteralPath $logPath -Encoding utf8
        $ok = $exitCode -eq 0
        if (-not $ok) { $failed = $true }
        $results.Add([ordered]@{
            run = $run; test_count = $total; process_exit_code = $exitCode
            passed = $ok; log_path = $logPath
        })
    }
}

$summary = [ordered]@{
    schema = 'mvm-p2-d4c-soak-summary-1'
    git_commit = (& git -C $repo rev-parse HEAD)
    dirty_worktree = -not [string]::IsNullOrWhiteSpace((& git -C $repo status --porcelain))
    stage = $Stage
    preset = $Preset
    passed = -not $failed
    run_count = $results.Count
    passed_count = @($results | Where-Object passed).Count
    runs = $results
}
$summaryPath = Join-Path $OutputDirectory "$($Stage.ToLowerInvariant())-$Preset-summary.json"
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $summaryPath -Encoding utf8
Write-Host "P2-D4C soak summary: $summaryPath"
if ($failed) { exit 3 }
