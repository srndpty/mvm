param(
    [Parameter(Mandatory = $true)][string]$Json,
    [Parameter(Mandatory = $true)][ValidateSet('Playback', 'Seek')][string]$Mode,
    [int]$ProcessExitCode = 0,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$failures = [System.Collections.Generic.List[string]]::new()

function Add-Failure([string]$Message) {
    $script:failures.Add($Message)
}

function Require-Property($Object, [string]$Name) {
    if ($Object.PSObject.Properties.Name -notcontains $Name) {
        Add-Failure "必須fieldがありません: $Name"
        return $null
    }
    return $Object.$Name
}

function Require-Equal($Actual, $Expected, [string]$Name) {
    if ($null -eq $Actual -or $Actual -ne $Expected) {
        Add-Failure "$Name は $Expected である必要があります (actual=$Actual)"
    }
}

function Require-Zero($Object, [string]$Name) {
    $value = Require-Property $Object $Name
    if ($null -ne $value -and [double]$value -ne 0) {
        Add-Failure "$Name は0である必要があります (actual=$value)"
    }
}

try {
    $raw = Get-Content -LiteralPath $Json -Raw -Encoding utf8 | ConvertFrom-Json
} catch {
    Write-Error "P2 raw JSONを読めません: $($_.Exception.Message)"
    exit 2
}

Require-Equal (Require-Property $raw 'schema') 'mvm-p2-formal-2' 'schema'
Require-Equal (Require-Property $raw 'formal_contract_version') 'P2-D5-2' 'formal_contract_version'
Require-Equal (Require-Property $raw 'mode') $Mode.ToLowerInvariant() 'mode'
Require-Equal (Require-Property $raw 'process_exit_code') 0 'JSON process_exit_code'
Require-Equal $ProcessExitCode 0 '実process exit code'
Require-Equal (Require-Property $raw 'formal_preflight') $true 'formal_preflight'
Require-Equal (Require-Property $raw 'same_device_a') $true 'same_device_a'
Require-Equal (Require-Property $raw 'same_device_b') $true 'same_device_b'
Require-Equal (Require-Property $raw 'actual_output_width') 1920 'actual_output_width'
Require-Equal (Require-Property $raw 'actual_output_height') 1080 'actual_output_height'
Require-Equal (Require-Property $raw 'actual_gpu_completion_backend') 'fence' `
    'actual_gpu_completion_backend'
Require-Equal (Require-Property $raw 'configured_seed') 20260808 'configured_seed'
$configuredWarmup = Require-Property $raw 'configured_warmup_seconds'
$configuredMeasure = Require-Property $raw 'configured_measure_seconds'
$configuredSeekCount = Require-Property $raw 'configured_seek_count'
if (-not $DryRun) {
    Require-Equal $configuredWarmup 5 'configured_warmup_seconds'
    Require-Equal $configuredMeasure 60 'configured_measure_seconds'
    Require-Equal $configuredSeekCount 1000 'configured_seek_count'
}

Require-Equal (Require-Property $raw 'marker_a_checked_count') 7 'marker_a_checked_count'
Require-Equal (Require-Property $raw 'marker_b_checked_count') 7 'marker_b_checked_count'
Require-Zero $raw 'marker_a_mismatch'
Require-Zero $raw 'marker_b_mismatch'
Require-Equal (Require-Property $raw 'actual_target_probe_checked_count') 4 'actual_target_probe_checked_count'
Require-Zero $raw 'actual_target_probe_mismatch'
Require-Zero $raw 'mixed_source_frame_count'
Require-Zero $raw 'mixed_generation_count'
Require-Zero $raw 'stale_composition_epoch_count'
Require-Zero $raw 'cpu_full_frame_readback_count'
Require-Zero $raw 'full_frame_gpu_copy_count'
Require-Zero $raw 'payloads_released_before_completion'
Require-Zero $raw 'retirement_timeout_count'
Require-Zero $raw 'retirement_depth_after_drain'
Require-Zero $raw 'device_lost_count'
Require-Zero $raw 'lifecycle_order_violation_count'
Require-Equal (Require-Property $raw 'teardown_success') $true 'teardown_success'
Require-Equal (Require-Property $raw 'final_report_written_after_teardown') $true 'final_report_written_after_teardown'

if ($Mode -eq 'Playback') {
    Require-Equal (Require-Property $raw 'configured_measurement_preroll_frames') 8 `
        'configured_measurement_preroll_frames'
    Require-Equal (Require-Property $raw 'measurement_preroll_ok') $true `
        'measurement_preroll_ok'
    $prerollDepthA = Require-Property $raw 'measurement_preroll_depth_a'
    $prerollDepthB = Require-Property $raw 'measurement_preroll_depth_b'
    if ($null -ne $prerollDepthA -and [long]$prerollDepthA -lt 8) {
        Add-Failure "measurement_preroll_depth_aは8以上が必要です (actual=$prerollDepthA)"
    }
    if ($null -ne $prerollDepthB -and [long]$prerollDepthB -lt 8) {
        Add-Failure "measurement_preroll_depth_bは8以上が必要です (actual=$prerollDepthB)"
    }
    Require-Equal (Require-Property $raw 'measurement_preroll_front_a') 0 `
        'measurement_preroll_front_a'
    Require-Equal (Require-Property $raw 'measurement_preroll_front_b') 0 `
        'measurement_preroll_front_b'
    $sourceAFrames = Require-Property $raw 'source_a_frame_count'
    $sourceBFrames = Require-Property $raw 'source_b_frame_count'
    $requiredFrames = Require-Property $raw 'required_measurement_frame_count'
    Require-Equal (Require-Property $raw 'source_coverage_ok') $true 'source_coverage_ok'
    if (-not $DryRun) {
        Require-Equal $requiredFrames 3600 'required_measurement_frame_count'
        if ($null -ne $sourceAFrames -and [long]$sourceAFrames -lt 3600) {
            Add-Failure "source_a_frame_countは3600以上が必要です (actual=$sourceAFrames)"
        }
        if ($null -ne $sourceBFrames -and [long]$sourceBFrames -lt 3600) {
            Add-Failure "source_b_frame_countは3600以上が必要です (actual=$sourceBFrames)"
        }
    }
    $scheduled = Require-Property $raw 'measurement_scheduled_output_count'
    $displayed = Require-Property $raw 'measurement_displayed_composition_count'
    $dropped = Require-Property $raw 'measurement_dropped_output_count'
    $submission = Require-Property $raw 'measurement_gpu_submission_count'
    $layers = Require-Property $raw 'measurement_layer_draw_count'
    $clears = Require-Property $raw 'measurement_logical_clear_count'
    Require-Equal (Require-Property $raw 'formal_opportunity_authority_valid') $true `
        'formal_opportunity_authority_valid'
    Require-Equal (Require-Property $raw 'formal_opportunity_error') 'NONE' `
        'formal_opportunity_error'
    $refreshNumerator = Require-Property $raw 'formal_refresh_numerator'
    $refreshDenominator = Require-Property $raw 'formal_refresh_denominator'
    $sourceFpsNumerator = Require-Property $raw 'formal_source_fps_numerator'
    $sourceFpsDenominator = Require-Property $raw 'formal_source_fps_denominator'
    Require-Equal $sourceFpsNumerator 60 'formal_source_fps_numerator'
    Require-Equal $sourceFpsDenominator 1 'formal_source_fps_denominator'
    if ($null -eq $refreshNumerator -or [long]$refreshNumerator -le 0) {
        Add-Failure "formal_refresh_numeratorは正数である必要があります (actual=$refreshNumerator)"
    }
    if ($null -eq $refreshDenominator -or [long]$refreshDenominator -le 0) {
        Add-Failure "formal_refresh_denominatorは正数である必要があります (actual=$refreshDenominator)"
    }

    # producer summaryを信じず、raw opportunity ledgerから全accountingを再計算する。
    $ledger = @(Require-Property $raw 'formal_opportunity_ledger')
    if ($ledger.Count -eq 0) { Add-Failure 'formal_opportunity_ledgerが空です' }
    $previousOrdinal = -1L
    $previousSwapQpc = 0L
    $previousUnique = -1L
    $displayedUnique = 0L
    $repeated = 0L
    $gapTrueDrop = 0L
    for ($index = 0; $index -lt $ledger.Count; ++$index) {
        $record = $ledger[$index]
        $prefix = "formal_opportunity_ledger[$index]"
        $ordinal = [long](Require-Property $record 'opportunity_ordinal')
        $swapQpc = [long](Require-Property $record 'presentation_swap_qpc')
        $recordNumerator = [long](Require-Property $record 'refresh_numerator')
        $recordDenominator = [long](Require-Property $record 'refresh_denominator')
        $expected = [long](Require-Property $record 'expected_source_frame')
        $presented = [long](Require-Property $record 'presented_source_frame')
        $recordedRepeat = [bool](Require-Property $record 'repeat')
        $recordedDrop = [long](Require-Property $record 'true_drop_before_this_opportunity')

        if ($index -eq 0) { Require-Equal $ordinal 0 "$prefix.opportunity_ordinal" }
        elseif ($ordinal -le $previousOrdinal) {
            Add-Failure "$prefix.opportunity_ordinalが単調増加ではありません"
        }
        if ($swapQpc -le $previousSwapQpc) {
            Add-Failure "$prefix.presentation_swap_qpcが単調増加ではありません"
        }
        Require-Equal $recordNumerator $refreshNumerator "$prefix.refresh_numerator"
        Require-Equal $recordDenominator $refreshDenominator "$prefix.refresh_denominator"
        if ([long]$refreshNumerator -gt 0 -and [long]$refreshDenominator -gt 0) {
            $recalculatedTarget = [long][decimal]::Floor(
                ([decimal]$ordinal * [decimal]$sourceFpsNumerator *
                    [decimal]$refreshDenominator) /
                    ([decimal]$sourceFpsDenominator * [decimal]$refreshNumerator))
            Require-Equal $expected $recalculatedTarget "$prefix.expected_source_frame"
        }
        if ($expected -lt 0 -or $expected -ge [long]$requiredFrames) {
            Add-Failure "$prefix.expected_source_frameがsource domain外です (actual=$expected)"
        }
        Require-Equal $presented $expected "$prefix.presented_source_frame"
        $isRepeat = $expected -eq $previousUnique
        Require-Equal $recordedRepeat $isRepeat "$prefix.repeat"
        $trueDropBefore = if ($isRepeat) { 0L } else { $expected - $previousUnique - 1L }
        if ($trueDropBefore -lt 0) {
            Add-Failure "$prefixでtarget regressionを検出しました"
        }
        Require-Equal $recordedDrop $trueDropBefore `
            "$prefix.true_drop_before_this_opportunity"
        if ($isRepeat) { ++$repeated }
        else {
            ++$displayedUnique
            $gapTrueDrop += $trueDropBefore
            $previousUnique = $expected
        }
        $previousOrdinal = $ordinal
        $previousSwapQpc = $swapQpc
    }
    $tailTrueDrop = [long]$requiredFrames - $previousUnique - 1L
    if ($tailTrueDrop -lt 0) { Add-Failure 'tail_true_dropが負になりました' }
    $trueDrop = $gapTrueDrop + $tailTrueDrop
    Require-Equal (Require-Property $raw 'formal_displayed_unique_count') $displayedUnique `
        'formal_displayed_unique_count'
    Require-Equal (Require-Property $raw 'formal_repeated_opportunity_count') $repeated `
        'formal_repeated_opportunity_count'
    Require-Equal (Require-Property $raw 'formal_gap_true_drop_count') $gapTrueDrop `
        'formal_gap_true_drop_count'
    Require-Equal (Require-Property $raw 'tail_true_drop') $tailTrueDrop 'tail_true_drop'
    Require-Equal (Require-Property $raw 'formal_true_opportunity_drop_count') $trueDrop `
        'formal_true_opportunity_drop_count'
    Require-Property $raw 'diagnostic_synthetic_deadline_drop_count' | Out-Null
    Require-Zero $raw 'measurement_drop_scheduler_deadline'
    if ($null -ne $scheduled) { Require-Equal ([long]$scheduled) ([long]$requiredFrames) `
            'measurement_scheduled_output_count' }
    if ($null -ne $displayed) { Require-Equal ([long]$displayed) $displayedUnique `
            'measurement_displayed_composition_count' }
    if ($null -ne $dropped) { Require-Equal ([long]$dropped) $trueDrop `
            'measurement_dropped_output_count' }
    Require-Equal ($displayedUnique + $trueDrop) ([long]$requiredFrames) `
        'displayedUnique + trueDrop == required domain'
    if ($null -ne $submission -and $null -ne $displayed) {
        Require-Equal ([long]$submission) ([long]$displayed) 'gpu submission == displayed'
    }
    if ($null -ne $layers -and $null -ne $displayed) {
        Require-Equal ([long]$layers) ([long]$displayed * 2) 'layer draw == displayed * 2'
    }
    if ($null -ne $clears -and $null -ne $displayed) {
        Require-Equal ([long]$clears) ([long]$displayed) 'logical clear == displayed'
    }
    Require-Zero $raw 'measurement_missing_pair_count'
    Require-Zero $raw 'measurement_source_a_eof_count'
    Require-Zero $raw 'measurement_source_b_eof_count'
    Require-Zero $raw 'measurement_drop_missing_source_a'
    Require-Zero $raw 'measurement_drop_missing_source_b'
    Require-Zero $raw 'measurement_drop_missing_both'
    Require-Zero $raw 'measurement_drop_stale_generation'
    Require-Zero $raw 'measurement_drop_future_generation'
    Require-Zero $raw 'measurement_drop_stale_composition_epoch'
    Require-Zero $raw 'measurement_drop_render_failure'
    Require-Zero $raw 'measurement_untracked_submission_count'
    Require-Zero $raw 'measurement_completion_poll_failure_count'
    Require-Zero $raw 'measurement_partial_gpu_issue_failure_count'
    if (-not $DryRun) {
        Require-Equal (Require-Property $raw 'measurement_first_output_frame') 0 `
            'measurement_first_output_frame'
        $fps = Require-Property $raw 'effective_fps'
        $dropRate = Require-Property $raw 'drop_rate'
        if ($null -ne $fps -and [double]$fps -lt 55) {
            Add-Failure "effective_fpsは55以上が必要です (actual=$fps)"
        }
        if ($null -ne $dropRate -and [double]$dropRate -gt 0.02) {
            Add-Failure "drop_rateは0.02以下が必要です (actual=$dropRate)"
        }
        $recalculatedDropRate = [double]$trueDrop / [double]$requiredFrames
        if ($null -ne $dropRate -and
            [math]::Abs([double]$dropRate - $recalculatedDropRate) -gt 1e-12) {
            Add-Failure "drop_rateがledger再計算と一致しません (actual=$dropRate recalculated=$recalculatedDropRate)"
        }
    }
} else {
    $displayedValues = @(Require-Property $raw 'dual_seek_displayed_ms')
    $decodeValues = @(Require-Property $raw 'dual_seek_decode_ready_ms')
    $expectedCount = if ($DryRun) { [int]$configuredSeekCount } else { 1000 }
    Require-Equal $displayedValues.Count $expectedCount 'dual_seek_displayed_ms.Count'
    Require-Equal $decodeValues.Count $expectedCount 'dual_seek_decode_ready_ms.Count'
    Require-Zero $raw 'seek_display_mismatch'
    Require-Zero $raw 'seek_timeout_count'
    Require-Zero $raw 'seek_stale_completion_count'
    Require-Zero $raw 'seek_busy_acceptance_count'
    Require-Zero $raw 'seek_completion_publish_reject_count'
    Require-Zero $raw 'seek_completion_request_mismatch_count'
    Require-Zero $raw 'seek_completion_stopped_superseded_count'
    Require-Zero $raw 'software_fallback_count'
    Require-Zero $raw 'worker_join_leak_count'
    Require-Zero $raw 'untracked_submission_count'
    Require-Zero $raw 'completion_poll_failure_count'

    $concurrencySamples = @(Require-Property $raw 'seek_concurrency_samples')
    Require-Equal $concurrencySamples.Count $expectedCount 'seek_concurrency_samples.Count'
    $parallelDispatchValidCount = 0
    $executionOverlapCount = 0
    for ($sampleIndex = 0; $sampleIndex -lt $concurrencySamples.Count; ++$sampleIndex) {
        $sample = $concurrencySamples[$sampleIndex]
        $prefix = "seek_concurrency_samples[$sampleIndex]"
        $requestStart = Require-Property $sample 'request_start_qpc'
        $aRequest = Require-Property $sample 'a_request_qpc'
        $bRequest = Require-Property $sample 'b_request_qpc'
        $dispatchComplete = Require-Property $sample 'dispatch_complete_qpc'
        $aBegin = Require-Property $sample 'a_begin_qpc'
        $bBegin = Require-Property $sample 'b_begin_qpc'
        $aReady = Require-Property $sample 'a_ready_qpc'
        $bReady = Require-Property $sample 'b_ready_qpc'
        $aRequestId = Require-Property $sample 'a_request_id'
        $bRequestId = Require-Property $sample 'b_request_id'
        $aResult = Require-Property $sample 'a_request_result'
        $bResult = Require-Property $sample 'b_request_result'
        $recordedParallel = Require-Property $sample 'parallel_dispatch_valid'
        $recordedOverlap = Require-Property $sample 'execution_overlap'

        Require-Equal $aResult 'Accepted' "$prefix.a_request_result"
        Require-Equal $bResult 'Accepted' "$prefix.b_request_result"
        if ($null -ne $aRequestId -and [long]$aRequestId -le 0) {
            Add-Failure "$prefix.a_request_idは正数である必要があります (actual=$aRequestId)"
        }
        if ($null -ne $bRequestId -and [long]$bRequestId -le 0) {
            Add-Failure "$prefix.b_request_idは正数である必要があります (actual=$bRequestId)"
        }

        $timestampsPresent = $null -notin @(
            $requestStart, $aRequest, $bRequest, $dispatchComplete,
            $aBegin, $bBegin, $aReady, $bReady)
        $parallel = $false
        $executionOverlap = $false
        if ($timestampsPresent) {
            $parallel = [long]$requestStart -gt 0 -and
                [long]$aRequest -ge [long]$requestStart -and
                [long]$bRequest -ge [long]$requestStart -and
                [long]$dispatchComplete -ge [long]$aRequest -and
                [long]$dispatchComplete -ge [long]$bRequest -and
                [long]$dispatchComplete -le [math]::Min([long]$aReady, [long]$bReady) -and
                $aResult -eq 'Accepted' -and $bResult -eq 'Accepted'
            $executionOverlap = [math]::Max([long]$aBegin, [long]$bBegin) -lt
                [math]::Min([long]$aReady, [long]$bReady)
        }
        Require-Equal $recordedParallel $parallel "$prefix.parallel_dispatch_valid"
        Require-Equal $recordedParallel $true "$prefix.parallel_dispatch_valid"
        Require-Equal $recordedOverlap $executionOverlap "$prefix.execution_overlap"
        if ($parallel) { ++$parallelDispatchValidCount }
        if ($executionOverlap) { ++$executionOverlapCount }
    }
    Require-Equal (Require-Property $raw 'parallel_dispatch_valid_count') $expectedCount `
        'parallel_dispatch_valid_count'
    Require-Equal $parallelDispatchValidCount $expectedCount `
        '再計算parallel_dispatch_valid_count'
    Require-Equal (Require-Property $raw 'execution_overlap_count') $executionOverlapCount `
        'execution_overlap_count'
    Require-Equal (Require-Property $raw 'execution_nonoverlap_count') `
        ($expectedCount - $executionOverlapCount) 'execution_nonoverlap_count'

    if ($displayedValues.Count -gt 0) {
        [double[]]$sorted = $displayedValues | ForEach-Object { [double]$_ } | Sort-Object
        $index = [math]::Ceiling($sorted.Count * 0.95) - 1
        $nearestRank = $sorted[$index]
        $recordedP95 = [double](Require-Property $raw 'dual_seek_displayed_p95_ms')
        $recordedMax = [double](Require-Property $raw 'dual_seek_displayed_observed_max_ms')
        if ([math]::Abs($nearestRank - $recordedP95) -gt 1e-9) {
            Add-Failure "nearest-rank p95がraw配列と一致しません (recalculated=$nearestRank recorded=$recordedP95)"
        }
        if ([math]::Abs($sorted[-1] - $recordedMax) -gt 1e-9) {
            Add-Failure "observed maxがraw配列と一致しません (recalculated=$($sorted[-1]) recorded=$recordedMax)"
        }
        if (-not $DryRun) {
            if ($recordedP95 -gt 150) {
                Add-Failure "dual seek p95は150ms以下が必要です (actual=$recordedP95)"
            }
            if ($recordedMax -gt 400) {
                Add-Failure "dual seek observed maxは400ms以下が必要です (actual=$recordedMax)"
            }
        }
    }
}

if ($failures.Count -ne 0) {
    foreach ($failure in $failures) { [Console]::Error.WriteLine($failure) }
    exit 3
}

$kind = if ($DryRun) { 'dry-run harness' } else { 'formal' }
Write-Host "P2 $kind contract: PASS ($Mode)"
exit 0
