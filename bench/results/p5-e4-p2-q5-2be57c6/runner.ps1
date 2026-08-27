[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,
    [string]$Executable,
    [ValidateRange(1, 6)]
    [int]$MaxRuns = 6
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = Split-Path -Parent $PSScriptRoot
if (-not $Executable) { $Executable = Join-Path $repo 'build\ucrt64-release\bin\mvm_compositor_spike.exe' }
$checker = Join-Path $PSScriptRoot 'check-p2-contract.ps1'
$sourceA = Join-Path $repo 'tests\assets\benchmark\v1080p60_h264.mp4'
$sourceB = Join-Path $repo 'tests\assets\benchmark\v1080p60_hevc.mp4'
foreach ($required in @($Executable, $checker, $sourceA, $sourceB)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "P2-Q5必須fileがありません: $required" }
}
$Executable = (Resolve-Path -LiteralPath $Executable).Path
if (Test-Path -LiteralPath $OutputDirectory) { throw "既存artifactを上書きしません: $OutputDirectory" }
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Invoke-GitText([string[]]$Arguments) {
    $value = & git -C $repo @Arguments 2>$null
    if ($LASTEXITCODE -ne 0) { throw "git $($Arguments -join ' ') に失敗しました" }
    return ($value -join "`n").Trim()
}

function Get-LostMediaOpportunities([long]$RefreshDelta, [long]$Numerator,
                                    [long]$Denominator) {
    if ($RefreshDelta -le 0 -or $Numerator -le 0 -or $Denominator -le 0) { return 0L }
    $mediaIntervals = [math]::Round(
        [double]$RefreshDelta * 60.0 * [double]$Denominator / [double]$Numerator,
        [MidpointRounding]::AwayFromZero)
    return [math]::Max(0L, [long]$mediaIntervals - 1L)
}

function Assert-ClassifierSynthetic {
    $cases = @(
        @{ name = '59.94 normal'; delta = 1; num = 60000; den = 1001; expected = 0 },
        @{ name = '59.94 one loss'; delta = 2; num = 60000; den = 1001; expected = 1 },
        @{ name = '60 normal'; delta = 1; num = 60; den = 1; expected = 0 },
        @{ name = '60 two loss'; delta = 3; num = 60; den = 1; expected = 2 },
        @{ name = '120 normal'; delta = 2; num = 120; den = 1; expected = 0 },
        @{ name = '120 one loss'; delta = 4; num = 120; den = 1; expected = 1 },
        @{ name = 'duplicate'; delta = 0; num = 60; den = 1; expected = 0 }
    )
    foreach ($case in $cases) {
        $actual = Get-LostMediaOpportunities $case.delta $case.num $case.den
        if ($actual -ne $case.expected) {
            throw "presentation classifier synthetic失敗: $($case.name) actual=$actual"
        }
    }
    return $cases.Count
}

function Classify-Presentation([object]$Raw) {
    $phase = $Raw.scheduler_phase_attribution
    $presentation = $Raw.presentation_opportunity
    $startTiming = $presentation.dwm_timing_start
    $stopTiming = $presentation.dwm_timing_stop
    $numerator = [long]$startTiming.display_refresh_numerator
    $denominator = [long]$startTiming.display_refresh_denominator
    $frequency = [long]$presentation.qpc_frequency
    $renders = @($presentation.render_records)
    $swaps = @($presentation.swap_records)
    $phaseRecords = @($phase.records)
    $authorityValid = $startTiming.available -and $stopTiming.available -and
        $startTiming.display_config_available -and $stopTiming.display_config_available -and
        $numerator -gt 0 -and $denominator -gt 0 -and
        [long]$startTiming.qpc_vblank -gt 0 -and
        $numerator -eq [long]$stopTiming.display_refresh_numerator -and
        $denominator -eq [long]$stopTiming.display_refresh_denominator -and
        [long]$presentation.render_overflow_count -eq 0 -and
        [long]$presentation.swap_overflow_count -eq 0 -and
        $renders.Count -eq [long]$Raw.measurement_present_callback_count -and
        $phaseRecords.Count -eq $renders.Count

    $swapByRender = @{}
    foreach ($swap in $swaps) {
        $key = [string][long]$swap.completed_render_ordinal
        if ($swapByRender.ContainsKey($key)) { $authorityValid = $false }
        else { $swapByRender[$key] = $swap }
    }
    $falseCount = 0L
    $trueCount = 0L
    $ambiguousCount = 0L
    $unpairedFalse = 0L
    $unpairedTrue = 0L
    $unpairedAmbiguous = 0L
    $actualLossAtSkip = 0L
    $events = [System.Collections.Generic.List[object]]::new()
    for ($index = 0; $index -lt $phaseRecords.Count; ++$index) {
        $record = $phaseRecords[$index]
        $skipped = [long]$record.decision_skipped_deadline_count
        if ($skipped -le 0) { continue }
        $render = $renders[$index]
        $isUnpaired = $record.skip_classification -eq 'UNPAIRED_SKIP'
        if ([long]$render.callback_begin_qpc -ne [long]$record.callback_qpc -or
            [long]$render.scheduler_skipped_deadline_count -ne $skipped) {
            $authorityValid = $false
        }
        $previousKey = [string]([long]$render.render_ordinal - 1L)
        $currentKey = [string][long]$render.render_ordinal
        if (-not $authorityValid -or -not $swapByRender.ContainsKey($previousKey) -or
            -not $swapByRender.ContainsKey($currentKey)) {
            $ambiguousCount += $skipped
            if ($isUnpaired) { $unpairedAmbiguous += $skipped }
            $events.Add([ordered]@{ render_ordinal = [long]$render.render_ordinal; skipped = $skipped;
                    classification = 'AMBIGUOUS'; physical_refresh_delta = $null;
                    actual_lost_opportunities = $null })
            continue
        }
        $previousSwap = $swapByRender[$previousKey]
        $currentSwap = $swapByRender[$currentKey]
        $swapDeltaQpc = [long]$currentSwap.swap_qpc - [long]$previousSwap.swap_qpc
        $refreshDelta = [long][math]::Round(
            [double]$swapDeltaQpc * [double]$numerator /
                ([double]$frequency * [double]$denominator),
            [MidpointRounding]::AwayFromZero)
        $lost = Get-LostMediaOpportunities $refreshDelta $numerator $denominator
        $actualLossAtSkip += $lost
        $trueUnits = [math]::Min($skipped, $lost)
        $falseUnits = $skipped - $trueUnits
        $trueCount += $trueUnits
        $falseCount += $falseUnits
        if ($isUnpaired) {
            $unpairedTrue += $trueUnits
            $unpairedFalse += $falseUnits
        }
        $classification = if ($trueUnits -eq $skipped) { 'TRUE_OPPORTUNITY_LOSS' }
            elseif ($falseUnits -eq $skipped) { 'FALSE_DEADLINE_SKIP' }
            else { 'MIXED' }
        $events.Add([ordered]@{ render_ordinal = [long]$render.render_ordinal; skipped = $skipped;
                classification = $classification; swap_delta_qpc = $swapDeltaQpc;
                physical_refresh_delta = $refreshDelta; actual_lost_opportunities = $lost;
                previous_swap_qpc = [long]$previousSwap.swap_qpc;
                current_swap_qpc = [long]$currentSwap.swap_qpc })
    }
    $uniqueFrames = @($swaps | Where-Object presented_output_frame -ge 0 |
        ForEach-Object presented_output_frame | Sort-Object -Unique).Count
    $anchorQpc = [long]$startTiming.qpc_vblank
    $firstOpportunity = [long][math]::Ceiling(
        ([double][long]$presentation.measurement_start_qpc - [double]$anchorQpc) *
            [double]$numerator / ([double]$frequency * [double]$denominator))
    $endOpportunity = [long][math]::Ceiling(
        ([double][long]$presentation.measurement_end_qpc_exclusive - [double]$anchorQpc) *
            [double]$numerator / ([double]$frequency * [double]$denominator))
    $opportunityCount = $endOpportunity - $firstOpportunity
    return [ordered]@{
        authority_valid = $authorityValid
        refresh_numerator = $numerator
        refresh_denominator = $denominator
        refresh_hz = [double]$numerator / [double]$denominator
        callback_count = [long]$Raw.measurement_present_callback_count
        render_record_count = $renders.Count
        swap_record_count = $swaps.Count
        present_opportunity_count = $opportunityCount
        actual_unique_presented_frame_count = $uniqueFrames
        current_synthetic_deadline_drop_count = [long]$Raw.measurement_drop_scheduler_deadline
        false_deadline_skip_count = $falseCount
        true_opportunity_loss_count = $trueCount
        ambiguous_count = $ambiguousCount
        actual_loss_units_at_skip_events = $actualLossAtSkip
        unpaired_false_deadline_skip_count = $unpairedFalse
        unpaired_true_opportunity_loss_count = $unpairedTrue
        unpaired_ambiguous_count = $unpairedAmbiguous
        events = $events
    }
}

$testedSha = Invoke-GitText @('rev-parse', 'HEAD')
if (Invoke-GitText @('status', '--porcelain')) { throw 'P2-Q5はclean worktreeでのみ実行できます' }
$syntheticCount = Assert-ClassifierSynthetic
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
Copy-Item -LiteralPath $PSCommandPath -Destination (Join-Path $OutputDirectory 'runner.ps1')
$identity = [ordered]@{
    tested_sha = $testedSha
    executable_sha256 = Get-Sha256 $Executable
    checker_sha256 = Get-Sha256 $checker
    source_a_sha256 = Get-Sha256 $sourceA
    source_b_sha256 = Get-Sha256 $sourceB
    runner_sha256 = Get-Sha256 $PSCommandPath
}
$runs = [System.Collections.Generic.List[object]]::new()
$passSeen = $false
$failSeen = $false

for ($index = 1; $index -le $MaxRuns; ++$index) {
    if ((Invoke-GitText @('rev-parse', 'HEAD')) -ne $testedSha -or
        (Invoke-GitText @('status', '--porcelain', '--untracked-files=no'))) {
        throw 'campaign中にsource provenanceが変化しました'
    }
    if ((Get-Sha256 $Executable) -ne $identity.executable_sha256) {
        throw 'campaign中にexecutable SHA-256が変化しました'
    }
    $runDirectory = Join-Path $OutputDirectory "run-$index"
    New-Item -ItemType Directory -Path $runDirectory | Out-Null
    $rawPath = Join-Path $runDirectory 'playback.json'
    $stdout = Join-Path $runDirectory 'stdout.txt'
    $stderr = Join-Path $runDirectory 'stderr.txt'
    $arguments = @('--source-a', $sourceA, '--source-b', $sourceB, '--metrics', $rawPath,
        '--warmup-seconds', '5', '--measure-seconds', '60', '--seed', '20260808',
        '--seek-count', '1000', '--display-timeout-ms', '2000', '--gpu-completion', 'fence',
        '--mode', 'playback', '--formal-preflight', '--scheduler-phase-ring',
        '--presentation-opportunity-ring')
    Write-Host "P2-Q5 playback run $index/$MaxRuns を開始します"
    $process = Start-Process -FilePath $Executable -ArgumentList $arguments -PassThru `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $timedOut = -not $process.WaitForExit(180000)
    if ($timedOut) {
        & taskkill.exe /PID $process.Id /T /F | Out-Null
        $process.WaitForExit()
    }
    $processExit = if ($timedOut) { 124 } else { $process.ExitCode }
    if (-not (Test-Path -LiteralPath $rawPath)) { throw "run-$index raw JSONがありません" }
    $checkerOutput = @(& pwsh -NoProfile -File $checker -Json $rawPath -Mode Playback `
            -ProcessExitCode $processExit 2>&1)
    $checkerExit = $LASTEXITCODE
    $checkerOutput | Set-Content -LiteralPath (Join-Path $runDirectory 'checker.txt') -Encoding utf8
    $raw = Get-Content -LiteralPath $rawPath -Raw -Encoding utf8 | ConvertFrom-Json
    $classification = Classify-Presentation $raw
    $classification | ConvertTo-Json -Depth 10 |
        Set-Content -LiteralPath (Join-Path $runDirectory 'classification.json') -Encoding utf8
    if (-not $classification.authority_valid) { throw "run-$index presentation authorityが不成立です" }
    $passed = $processExit -eq 0 -and $checkerExit -eq 0
    $passSeen = $passSeen -or $passed
    $failSeen = $failSeen -or (-not $passed)
    $runs.Add([ordered]@{ run = $index; contract_pass = $passed;
            process_exit_code = $processExit; checker_exit_code = $checkerExit;
            raw_sha256 = Get-Sha256 $rawPath; classification = $classification })
    if ($passSeen -and $failSeen) { break }
}

$falseTotal = 0L; $trueTotal = 0L; $ambiguousTotal = 0L
foreach ($run in $runs) {
    $falseTotal += [long]$run.classification.false_deadline_skip_count
    $trueTotal += [long]$run.classification.true_opportunity_loss_count
    $ambiguousTotal += [long]$run.classification.ambiguous_count
}
[ordered]@{
    schema = 'mvm.p5-e4-p2-q5-presentation.v1'
    authority = 'diagnostic_only_not_closure_evidence'
    identity = $identity
    conditions = @{ warmup_seconds = 5; measure_seconds = 60; media_rate = '60/1';
        scheduler_policy_changed = $false; threshold_changed = $false; checker_changed = $false }
    stop_rule = '最大6 run。PASSとFAILの両方を採取した時点で停止'
    synthetic_classifier_scenarios_passed = $syntheticCount
    pass_seen = $passSeen
    fail_seen = $failSeen
    aggregate = @{ false_deadline_skip_count = $falseTotal;
        true_opportunity_loss_count = $trueTotal; ambiguous_count = $ambiguousTotal }
    runs = $runs
} | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'summary.json') -Encoding utf8

$manifestPath = Join-Path $OutputDirectory 'manifest.sha256'
Get-ChildItem -LiteralPath $OutputDirectory -Recurse -File |
    Where-Object FullName -ne $manifestPath | Sort-Object FullName | ForEach-Object {
        $relative = $_.FullName.Substring($OutputDirectory.Length + 1).Replace('\', '/')
        "$(Get-Sha256 $_.FullName)  $relative"
    } | Set-Content -LiteralPath $manifestPath -Encoding ascii
Write-Host "P2-Q5 campaign完了: false=$falseTotal true=$trueTotal ambiguous=$ambiguousTotal"
