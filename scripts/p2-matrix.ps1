[CmdletBinding()]
param(
    [switch]$DryRun,
    [switch]$StopOnFailure,
    [string]$OutputDirectory,
    [string]$SourceA,
    [string]$SourceB
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo 'build\ucrt64-release'
$exe = Join-Path $build 'bin\mvm_compositor_spike.exe'
$checker = Join-Path $PSScriptRoot 'check-p2-contract.ps1'
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $build 'p2-matrix-d4' }
if (-not $SourceA) { $SourceA = Join-Path $repo 'tests\assets\benchmark\v1080p60_h264.mp4' }
if (-not $SourceB) { $SourceB = Join-Path $repo 'tests\assets\benchmark\v1080p60_hevc.mp4' }
foreach ($required in @($exe, $checker, $SourceA, $SourceB)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "P2 matrix必須fileがありません: $required" }
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
$seed = 20260808
$runCount = if ($DryRun) { 1 } else { 3 }
$warmup = if ($DryRun) { 1 } else { 5 }
$measure = if ($DryRun) { 2 } else { 60 }
$seekCount = if ($DryRun) { 16 } else { 1000 }
$displayTimeoutMs = 2000

function Git-Text([string[]]$Arguments) {
    $value = & git @Arguments 2>$null
    if ($LASTEXITCODE -ne 0) { throw "git $($Arguments -join ' ') に失敗しました" }
    return ($value -join "`n")
}

function Get-Provenance {
    $status = Git-Text @('status', '--porcelain')
    $diff = Git-Text @('diff', '--no-ext-diff')
    $cachedDiff = Git-Text @('diff', '--cached', '--no-ext-diff')
    $untrackedText = Git-Text @('ls-files', '--others', '--exclude-standard')
    $untracked = @($untrackedText -split "`r?`n" | Where-Object { $_ })
    $untrackedHashes = @($untracked | ForEach-Object {
        $absolute = Join-Path $repo $_
        "$_=$((Get-FileHash -LiteralPath $absolute -Algorithm SHA256).Hash.ToLowerInvariant())"
    })
    $fingerprintText = @(
        (Git-Text @('rev-parse', 'HEAD')), $status, $diff, $cachedDiff,
        ($untrackedHashes -join "`n")
    ) -join "`n--P2-PROVENANCE--`n"
    $bytes = [Text.Encoding]::UTF8.GetBytes($fingerprintText)
    $hash = [Convert]::ToHexString(
        [Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
    return [ordered]@{
        git_commit = Git-Text @('rev-parse', 'HEAD')
        dirty_worktree = -not [string]::IsNullOrWhiteSpace($status)
        git_status_porcelain = @($status -split "`n" | Where-Object { $_ })
        git_diff_stat = Git-Text @('diff', '--stat')
        source_fingerprint_sha256 = $hash
    }
}

$startProvenance = Get-Provenance
if (-not $DryRun -and $startProvenance.dirty_worktree) {
    throw 'P2 formal matrixはclean worktreeでのみ実行できます。変更をcommitしてから再実行してください'
}
$entries = [System.Collections.Generic.List[object]]::new()
$matrixFailed = $false

function Invoke-P2Run([string]$Mode, [int]$Index) {
    $path = Join-Path $OutputDirectory "$($Mode.ToLowerInvariant())-run$Index.json"
    if (Test-Path -LiteralPath $path) {
        $stamp = Get-Date -Format 'yyyyMMdd-HHmmssfff'
        Move-Item -LiteralPath $path -Destination "$path.previous-$stamp"
    }
    $arguments = @(
        '--source-a', $SourceA, '--source-b', $SourceB,
        '--metrics', $path, '--warmup-seconds', $warmup,
        '--measure-seconds', $measure, '--seed', $seed,
        '--seek-count', $seekCount, '--display-timeout-ms', $displayTimeoutMs,
        '--gpu-completion', 'fence', '--mode', $Mode.ToLowerInvariant(),
        '--formal-preflight'
    )
    Write-Host "P2 $Mode run $Index/$runCount を開始します"
    & $exe @arguments
    $processExit = $LASTEXITCODE
    $contractExit = -1
    if (Test-Path -LiteralPath $path) {
        if ($DryRun) {
            & pwsh -NoProfile -File $checker -Json $path -Mode $Mode `
                -ProcessExitCode $processExit -DryRun
        } else {
            & pwsh -NoProfile -File $checker -Json $path -Mode $Mode `
                -ProcessExitCode $processExit
        }
        $contractExit = $LASTEXITCODE
    }
    $passed = $processExit -eq 0 -and $contractExit -eq 0
    if (-not $passed) { $script:matrixFailed = $true }
    $raw = if (Test-Path -LiteralPath $path) {
        Get-Content -LiteralPath $path -Raw -Encoding utf8 | ConvertFrom-Json
    } else { $null }
    $script:entries.Add([pscustomobject]@{
        mode = $Mode.ToLowerInvariant()
        run = $Index
        raw_path = $path
        process_exit_code = $processExit
        contract_exit_code = $contractExit
        pass = $passed
        raw = $raw
    })
    return $passed
}

$stop = $false
foreach ($mode in @('Playback', 'Seek')) {
    if ($stop) { break }
    foreach ($index in 1..$runCount) {
        $passed = Invoke-P2Run $mode $index
        if (-not $passed -and $StopOnFailure) { $stop = $true; break }
    }
}

$endProvenance = Get-Provenance
$provenanceUnchanged = $startProvenance.git_commit -eq $endProvenance.git_commit -and
    $startProvenance.source_fingerprint_sha256 -eq $endProvenance.source_fingerprint_sha256
if (-not $provenanceUnchanged) { $matrixFailed = $true }

$playbackEntries = @($entries | Where-Object mode -eq 'playback')
$seekEntries = @($entries | Where-Object mode -eq 'seek')
$playbackRaw = @($playbackEntries | Where-Object raw | ForEach-Object raw)
$seekRaw = @($seekEntries | Where-Object raw | ForEach-Object raw)

function Sum-Raw([object[]]$Raw, [string]$Name) {
    $sum = 0L
    foreach ($item in $Raw) {
        if ($item.PSObject.Properties.Name -contains $Name) { $sum += [long]$item.$Name }
    }
    return $sum
}

$allPlayback = $playbackEntries.Count -eq $runCount -and
    @($playbackEntries | Where-Object { -not $_.pass }).Count -eq 0
$allSeek = $seekEntries.Count -eq $runCount -and
    @($seekEntries | Where-Object { -not $_.pass }).Count -eq 0
$summary = [ordered]@{
    schema = 'mvm-p2-matrix-summary-1'
    formal_contract_version = 'P2-D4-2'
    dry_run = [bool]$DryRun
    git_commit = $startProvenance.git_commit
    dirty_worktree = $startProvenance.dirty_worktree
    git_status_porcelain = $startProvenance.git_status_porcelain
    git_diff_stat = $startProvenance.git_diff_stat
    provenance_unchanged_during_matrix = $provenanceUnchanged
    source_a = (Resolve-Path -LiteralPath $SourceA).Path
    source_b = (Resolve-Path -LiteralPath $SourceB).Path
    adapter = if ($playbackRaw.Count) { $playbackRaw[0].adapter_a } elseif ($seekRaw.Count) { $seekRaw[0].adapter_a } else { $null }
    playback_runs = @($playbackEntries | ForEach-Object {
        [ordered]@{ run=$_.run; raw_path=$_.raw_path; process_exit_code=$_.process_exit_code
            contract_exit_code=$_.contract_exit_code; pass=$_.pass
            effective_fps=if ($_.raw) {$_.raw.effective_fps} else {$null}
            drop_rate=if ($_.raw) {$_.raw.drop_rate} else {$null} }
    })
    seek_runs = @($seekEntries | ForEach-Object {
        [ordered]@{ run=$_.run; raw_path=$_.raw_path; process_exit_code=$_.process_exit_code
            contract_exit_code=$_.contract_exit_code; pass=$_.pass
            p95_ms=if ($_.raw) {$_.raw.dual_seek_displayed_p95_ms} else {$null}
            observed_max_ms=if ($_.raw) {$_.raw.dual_seek_displayed_observed_max_ms} else {$null} }
    })
    min_effective_fps = if ($playbackRaw.Count) { [double](($playbackRaw.effective_fps | Measure-Object -Minimum).Minimum) } else { $null }
    max_drop_rate = if ($playbackRaw.Count) { [double](($playbackRaw.drop_rate | Measure-Object -Maximum).Maximum) } else { $null }
    per_run_p95 = @($seekRaw | ForEach-Object dual_seek_displayed_p95_ms)
    per_run_observed_max = @($seekRaw | ForEach-Object dual_seek_displayed_observed_max_ms)
    global_observed_max = if ($seekRaw.Count) { [double](($seekRaw.dual_seek_displayed_observed_max_ms | Measure-Object -Maximum).Maximum) } else { $null }
    marker_mismatch_total = (Sum-Raw $playbackRaw 'marker_a_mismatch') + (Sum-Raw $playbackRaw 'marker_b_mismatch') + (Sum-Raw $seekRaw 'marker_a_mismatch') + (Sum-Raw $seekRaw 'marker_b_mismatch')
    probe_mismatch_total = (Sum-Raw $playbackRaw 'actual_target_probe_mismatch') + (Sum-Raw $seekRaw 'actual_target_probe_mismatch')
    mixed_frame_total = (Sum-Raw $playbackRaw 'mixed_source_frame_count') + (Sum-Raw $seekRaw 'mixed_source_frame_count')
    mixed_generation_total = (Sum-Raw $playbackRaw 'mixed_generation_count') + (Sum-Raw $seekRaw 'mixed_generation_count')
    stale_epoch_total = (Sum-Raw $playbackRaw 'stale_composition_epoch_count') + (Sum-Raw $seekRaw 'stale_composition_epoch_count')
    cpu_full_frame_readback_total = (Sum-Raw $playbackRaw 'cpu_full_frame_readback_count') + (Sum-Raw $seekRaw 'cpu_full_frame_readback_count')
    full_frame_gpu_copy_total = (Sum-Raw $playbackRaw 'full_frame_gpu_copy_count') + (Sum-Raw $seekRaw 'full_frame_gpu_copy_count')
    untracked_submission_total = (Sum-Raw $playbackRaw 'untracked_submission_count') + (Sum-Raw $seekRaw 'untracked_submission_count')
    completion_failure_total = (Sum-Raw $playbackRaw 'completion_poll_failure_count') + (Sum-Raw $seekRaw 'completion_poll_failure_count')
    early_release_total = (Sum-Raw $playbackRaw 'payloads_released_before_completion') + (Sum-Raw $seekRaw 'payloads_released_before_completion')
    retirement_timeout_total = (Sum-Raw $playbackRaw 'retirement_timeout_count') + (Sum-Raw $seekRaw 'retirement_timeout_count')
    retirement_after_drain_total = (Sum-Raw $playbackRaw 'retirement_depth_after_drain') + (Sum-Raw $seekRaw 'retirement_depth_after_drain')
    device_lost_total = (Sum-Raw $playbackRaw 'device_lost_count') + (Sum-Raw $seekRaw 'device_lost_count')
    lifecycle_violation_total = (Sum-Raw $playbackRaw 'lifecycle_order_violation_count') + (Sum-Raw $seekRaw 'lifecycle_order_violation_count')
    all_playback_runs_pass = $allPlayback
    all_seek_runs_pass = $allSeek
    dry_run_harness_pass = [bool]($DryRun -and $allPlayback -and $allSeek -and $provenanceUnchanged)
    p2_pass = [bool]((-not $DryRun) -and (-not $startProvenance.dirty_worktree) -and
        $allPlayback -and $allSeek -and $provenanceUnchanged)
}

$summaryPath = Join-Path $OutputDirectory 'summary.json'
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $summaryPath -Encoding utf8
Write-Host "P2 matrix summary: $summaryPath"
if ($matrixFailed -or -not $allPlayback -or -not $allSeek) { exit 3 }
exit 0
