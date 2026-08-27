[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,
    [string]$Executable,
    [ValidateRange(1, 8)]
    [int]$MaxRuns = 8
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = Split-Path -Parent $PSScriptRoot
if (-not $Executable) {
    $Executable = Join-Path $repo 'build\ucrt64-release\bin\mvm_compositor_spike.exe'
}
$checker = Join-Path $PSScriptRoot 'check-p2-contract.ps1'
$sourceA = Join-Path $repo 'tests\assets\benchmark\v1080p60_h264.mp4'
$sourceB = Join-Path $repo 'tests\assets\benchmark\v1080p60_hevc.mp4'
foreach ($required in @($Executable, $checker, $sourceA, $sourceB)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "P2-Q3必須ファイルがありません: $required"
    }
}
$Executable = (Resolve-Path -LiteralPath $Executable).Path
if (Test-Path -LiteralPath $OutputDirectory) {
    throw "既存artifactを上書きしません: $OutputDirectory"
}

function Invoke-GitText([string[]]$Arguments) {
    $value = & git -C $repo @Arguments 2>$null
    if ($LASTEXITCODE -ne 0) { throw "git $($Arguments -join ' ') に失敗しました" }
    return ($value -join "`n").Trim()
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

$testedSha = Invoke-GitText @('rev-parse', 'HEAD')
$status = Invoke-GitText @('status', '--porcelain')
if ($status) { throw "P2-Q3はclean worktreeでのみ実行できます: $status" }

New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
$runs = [System.Collections.Generic.List[object]]::new()
$passSeen = $false
$failSeen = $false
$identity = [ordered]@{
    tested_sha = $testedSha
    executable = @{ path = $Executable; sha256 = Get-Sha256 $Executable }
    checker = @{ path = $checker; sha256 = Get-Sha256 $checker }
    source_a = @{ path = $sourceA; sha256 = Get-Sha256 $sourceA }
    source_b = @{ path = $sourceB; sha256 = Get-Sha256 $sourceB }
    runner = @{ path = $PSCommandPath; sha256 = Get-Sha256 $PSCommandPath }
}

function Write-Summary {
    $phase = 0L
    $gap = 0L
    $unpaired = 0L
    $boundary = 0L
    foreach ($run in $runs) {
        $phase += [long]$run.phase_pair_deadline_count
        $gap += [long]$run.long_callback_gap_deadline_count
        $unpaired += [long]$run.unpaired_skip_deadline_count
        $boundary += [long]$run.unobserved_boundary_deadline_count
    }
    $total = $phase + $gap + $unpaired + $boundary
    $dominant = if ($total -eq 0) { 'NO_DEADLINE_DROP' } elseif ($phase -gt $gap -and $phase -gt $unpaired -and $phase -gt $boundary) {
        'PHASE_PAIR'
    } elseif ($gap -gt $phase -and $gap -gt $unpaired -and $gap -gt $boundary) {
        'LONG_CALLBACK_GAP'
    } elseif ($unpaired -gt $phase -and $unpaired -gt $gap -and $unpaired -gt $boundary) {
        'UNPAIRED_SKIP'
    } elseif ($boundary -gt $phase -and $boundary -gt $gap -and $boundary -gt $unpaired) {
        'UNOBSERVED_BOUNDARY'
    } else {
        'MIXED'
    }
    [ordered]@{
        schema = 'mvm.p5-e4-p2-q3-phase.v1'
        authority = 'diagnostic_only_not_closure_evidence'
        identity = $identity
        conditions = [ordered]@{
            mode = 'playback'
            warmup_seconds = 5
            measure_seconds = 60
            seed = 20260808
            seek_count = 1000
            display_timeout_ms = 2000
            gpu_completion = 'fence'
            formal_preflight = $true
            diagnostic_timing = $false
            scheduler_phase_ring = $true
            ring_capacity = 8192
        }
        stop_rule = '最大8 run。PASS 1本とFAIL 1本を採取した時点で停止'
        pass_seen = $passSeen
        fail_seen = $failSeen
        aggregate = [ordered]@{
            phase_pair_deadline_count = [long]$phase
            long_callback_gap_deadline_count = [long]$gap
            unpaired_skip_deadline_count = [long]$unpaired
            unobserved_boundary_deadline_count = [long]$boundary
            total_deadline_count = $total
            dominant_classification = $dominant
        }
        runs = $runs
    } | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'summary.json') -Encoding utf8
}

Copy-Item -LiteralPath $PSCommandPath -Destination (Join-Path $OutputDirectory 'runner.ps1')
for ($index = 1; $index -le $MaxRuns; ++$index) {
    if ((Invoke-GitText @('rev-parse', 'HEAD')) -ne $testedSha -or
        (Invoke-GitText @('status', '--porcelain', '--untracked-files=no'))) {
        throw 'campaign中にsource provenanceが変化しました'
    }
    if ((Get-Sha256 $Executable) -ne $identity.executable.sha256) {
        throw 'campaign中にexecutable SHA-256が変化しました'
    }

    $runDirectory = Join-Path $OutputDirectory "run-$index"
    New-Item -ItemType Directory -Path $runDirectory | Out-Null
    $rawPath = Join-Path $runDirectory 'playback.json'
    $stdoutPath = Join-Path $runDirectory 'stdout.txt'
    $stderrPath = Join-Path $runDirectory 'stderr.txt'
    $arguments = @(
        '--source-a', $sourceA, '--source-b', $sourceB,
        '--metrics', $rawPath, '--warmup-seconds', '5',
        '--measure-seconds', '60', '--seed', '20260808',
        '--seek-count', '1000', '--display-timeout-ms', '2000',
        '--gpu-completion', 'fence', '--mode', 'playback',
        '--formal-preflight', '--scheduler-phase-ring'
    )
    Write-Host "P2-Q3 playback run $index/$MaxRuns を開始します"
    $process = Start-Process -FilePath $Executable -ArgumentList $arguments -PassThru `
        -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
    $timedOut = -not $process.WaitForExit(180000)
    if ($timedOut) {
        & taskkill.exe /PID $process.Id /T /F | Out-Null
        $process.WaitForExit()
    }
    $processExit = if ($timedOut) { 124 } else { $process.ExitCode }
    if (-not (Test-Path -LiteralPath $rawPath)) {
        throw "run-$index はraw P2 JSONを生成しませんでした"
    }
    $checkerOutput = @(& pwsh -NoProfile -File $checker -Json $rawPath -Mode Playback `
            -ProcessExitCode $processExit 2>&1)
    $checkerExit = $LASTEXITCODE
    $checkerOutput | Set-Content -LiteralPath (Join-Path $runDirectory 'checker.txt') -Encoding utf8
    $raw = Get-Content -LiteralPath $rawPath -Raw -Encoding utf8 | ConvertFrom-Json
    $phase = $raw.scheduler_phase_attribution
    if (-not $raw.diagnostic_scheduler_phase_ring -or -not $phase.enabled) {
        throw "run-$index はscheduler phase ringを公開しませんでした"
    }
    $classified = [long]$phase.phase_pair_deadline_count +
        [long]$phase.long_callback_gap_deadline_count + [long]$phase.unpaired_skip_deadline_count
    $accounted = $classified + [long]$phase.unobserved_boundary_deadline_count
    if ([long]$phase.overflow_count -ne 0 -or
        $classified -ne [long]$phase.classified_deadline_count -or
        $accounted -ne [long]$raw.measurement_drop_scheduler_deadline) {
        throw "run-$index のring/classification整合検査に失敗しました"
    }
    $passed = $processExit -eq 0 -and $checkerExit -eq 0
    $passSeen = $passSeen -or $passed
    $failSeen = $failSeen -or (-not $passed)
    $runs.Add([ordered]@{
        run = $index
        process_exit_code = $processExit
        timed_out = $timedOut
        checker_exit_code = $checkerExit
        contract_pass = $passed
        raw_file = "run-$index/playback.json"
        raw_sha256 = Get-Sha256 $rawPath
        deadline_drop_count = [long]$raw.measurement_drop_scheduler_deadline
        deadline_drop_rate = [double]$raw.deadline_drop_rate
        present_callback_count = [long]$raw.measurement_present_callback_count
        repeated_present_count = [long]$raw.measurement_repeated_present_count
        ring_record_count = [long]$phase.record_count
        ring_overflow_count = [long]$phase.overflow_count
        phase_pair_deadline_count = [long]$phase.phase_pair_deadline_count
        long_callback_gap_deadline_count = [long]$phase.long_callback_gap_deadline_count
        unpaired_skip_deadline_count = [long]$phase.unpaired_skip_deadline_count
        unobserved_boundary_deadline_count = [long]$phase.unobserved_boundary_deadline_count
        phase_pair_samples = @($phase.phase_pairs)
    })
    Write-Summary
    if ($passSeen -and $failSeen) { break }
}

$manifestPath = Join-Path $OutputDirectory 'manifest.sha256'
$manifest = Get-ChildItem -LiteralPath $OutputDirectory -Recurse -File |
    Where-Object FullName -ne $manifestPath | Sort-Object FullName | ForEach-Object {
        $relative = $_.FullName.Substring($OutputDirectory.Length + 1).Replace('\', '/')
        "$(Get-Sha256 $_.FullName)  $relative"
    }
$manifest | Set-Content -LiteralPath $manifestPath -Encoding ascii
Write-Host "P2-Q3 campaign完了: pass=$passSeen fail=$failSeen root=$OutputDirectory"
