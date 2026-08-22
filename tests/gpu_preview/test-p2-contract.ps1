param(
    [Parameter(Mandatory)][ValidateSet(
        'Good', 'FormalGood', 'NegativeInvariant', 'NegativeSameDevice',
        'NegativeOutputSize', 'NegativeBackend', 'NegativeSeed',
        'NegativeFormalTiming', 'NegativeScheduledHigh', 'NegativeScheduledLow',
        'NegativeCoverage', 'NegativeSourceFrameCount', 'NegativeMissingPair',
        'NegativeEofA', 'NegativeEofB', 'NegativePrerollConfigured',
        'NegativePrerollOk', 'NegativePrerollDepthA', 'NegativePrerollDepthB',
        'NegativePrerollFrontA', 'NegativePrerollFrontB', 'SeekGood',
        'NegativeSeekPublishReject', 'NegativeSeekRequestMismatch',
        'NegativeSeekStaleCompletion', 'NegativeSeekBusyAcceptance',
        'NegativeSeekStoppedSuperseded', 'NegativeSeekConcurrencyCount',
        'SeekExecutionNonoverlapGood', 'FormalSeekGood',
        'NegativeParallelDispatchCount', 'NegativeParallelDispatchSample',
        'NegativeBRequestResult', 'NegativeDispatchAfterFirstReady',
        'NegativeOpportunityLedgerMutation', 'NegativeOpportunityOrdinalGapIgnored',
        'NegativeOpportunityRefreshChange', 'NegativeOpportunityAuthority',
        'NegativeOpportunityTail', 'ReconciledGood',
        'NegativeForwardDropIgnored', 'NegativeMeasurementDelta')][string]$Case,
    [Parameter(Mandatory)][string]$Checker,
    [Parameter(Mandatory)][string]$Output
)
$ErrorActionPreference = 'Stop'

$raw = [ordered]@{
    schema='mvm-p2-formal-2'; formal_contract_version='P2-D5-2'; mode='playback'
    formal_preflight=$true; process_exit_code=0
    same_device_a=$true; same_device_b=$true
    actual_output_width=1920; actual_output_height=1080
    actual_gpu_completion_backend='fence'; configured_seed=20260808
    configured_warmup_seconds=1; configured_measure_seconds=2; configured_seek_count=16
    configured_measurement_preroll_frames=8; measurement_preroll_ok=$true
    measurement_preroll_depth_a=8; measurement_preroll_depth_b=8
    measurement_preroll_front_a=0; measurement_preroll_front_b=0
    source_a_frame_count=3600; source_b_frame_count=3600
    required_measurement_frame_count=120; source_coverage_ok=$true
    measurement_started=$true; measurement_stop_captured=$true; measurement_available=$true
    marker_a_checked_count=7; marker_b_checked_count=7
    marker_a_mismatch=0; marker_b_mismatch=0
    actual_target_probe_checked_count=4; actual_target_probe_mismatch=0
    mixed_source_frame_count=0; mixed_generation_count=0; stale_composition_epoch_count=0
    cpu_full_frame_readback_count=0; full_frame_gpu_copy_count=0
    payloads_released_before_completion=0; retirement_timeout_count=0
    retirement_depth_after_drain=0; device_lost_count=0; lifecycle_order_violation_count=0
    teardown_success=$true; final_report_written_after_teardown=$true
    measurement_composition_requested_count=120; measurement_composition_drawn_count=120
    measurement_scheduled_output_count=120; measurement_displayed_composition_count=120
    measurement_dropped_output_count=0; measurement_gpu_submission_count=120
    measurement_missing_pair_count=0; measurement_source_a_eof_count=0
    measurement_source_b_eof_count=0; measurement_first_output_frame=0
    measurement_layer_draw_count=240; measurement_logical_clear_count=120
    measurement_drop_scheduler_deadline=0; measurement_drop_missing_source_a=0
    measurement_drop_missing_source_b=0; measurement_drop_missing_both=0
    measurement_drop_stale_generation=0; measurement_drop_future_generation=0
    measurement_drop_stale_composition_epoch=0; measurement_drop_render_failure=0
    measurement_untracked_submission_count=0; measurement_completion_poll_failure_count=0
    measurement_partial_gpu_issue_failure_count=0; effective_fps=60.0; drop_rate=0.0
    measurement_present_callback_count=120; measurement_repeated_present_count=0
    formal_opportunity_authority_valid=$true; formal_opportunity_error='NONE'
    formal_refresh_numerator=60; formal_refresh_denominator=1
    formal_source_fps_numerator=60; formal_source_fps_denominator=1
    formal_qpc_frequency=60000
    formal_displayed_unique_count=120; formal_repeated_opportunity_count=0
    formal_gap_true_drop_count=0; tail_true_drop=0
    formal_true_opportunity_drop_count=0
    formal_forward_reconciliation_count=0; formal_lost_opportunity_count=0
    formal_first_reconciliation_event=[ordered]@{ captured=$false; classification='NONE' }
    diagnostic_synthetic_deadline_drop_count=0
    dual_seek_displayed_ms=@(1.0) * 16; dual_seek_decode_ready_ms=@(0.5) * 16
    dual_seek_displayed_p95_ms=1.0; dual_seek_displayed_observed_max_ms=1.0
    seek_display_mismatch=0; seek_timeout_count=0; untracked_submission_count=0
    completion_poll_failure_count=0; seek_completion_publish_reject_count=0
    seek_completion_request_mismatch_count=0; seek_stale_completion_count=0
    seek_busy_acceptance_count=0; seek_completion_stopped_superseded_count=0
    software_fallback_count=0; worker_join_leak_count=0
    parallel_dispatch_valid_count=16; execution_overlap_count=16
    execution_nonoverlap_count=0
    seek_concurrency_samples=@(1..16 | ForEach-Object {
        [ordered]@{
            request_start_qpc=100; a_request_qpc=101; b_request_qpc=102
            dispatch_complete_qpc=103; a_begin_qpc=104; a_ready_qpc=106
            b_begin_qpc=105; b_ready_qpc=107; a_request_id=$_; b_request_id=$_
            a_request_result='Accepted'; b_request_result='Accepted'
            parallel_dispatch_valid=$true; execution_overlap=$true
        }
    })
}
$seekCases = @(
    'SeekGood', 'NegativeSeekPublishReject', 'NegativeSeekRequestMismatch',
    'NegativeSeekStaleCompletion', 'NegativeSeekBusyAcceptance',
    'NegativeSeekStoppedSuperseded', 'NegativeSeekConcurrencyCount',
    'SeekExecutionNonoverlapGood', 'FormalSeekGood',
    'NegativeParallelDispatchCount', 'NegativeParallelDispatchSample',
    'NegativeBRequestResult', 'NegativeDispatchAfterFirstReady'
)
if ($Case -in $seekCases) { $raw.mode = 'seek' }
$formalPlaybackCases = @(
    'FormalGood', 'NegativeFormalTiming', 'NegativeScheduledHigh',
    'NegativeScheduledLow', 'NegativeCoverage', 'NegativeSourceFrameCount',
    'NegativeMissingPair', 'NegativeEofA', 'NegativeEofB'
)
$formalSeekCases = @('FormalSeekGood', 'NegativeParallelDispatchCount')
$formalCases = @($formalPlaybackCases) + @($formalSeekCases)
if ($Case -in $formalCases) {
    $raw.configured_warmup_seconds = 5
    $raw.configured_measure_seconds = 60
    $raw.configured_seek_count = 1000
    $raw.required_measurement_frame_count = 3600
    $raw.measurement_scheduled_output_count = 3600
    $raw.measurement_displayed_composition_count = 3600
    $raw.measurement_gpu_submission_count = 3600
    $raw.measurement_layer_draw_count = 7200
    $raw.measurement_logical_clear_count = 3600
    $raw.formal_displayed_unique_count = 3600
}
if ($Case -in $formalSeekCases) {
    $raw.dual_seek_displayed_ms = @(1.0) * 1000
    $raw.dual_seek_decode_ready_ms = @(0.5) * 1000
    $raw.parallel_dispatch_valid_count = 1000
    $raw.execution_overlap_count = 1000
    $raw.execution_nonoverlap_count = 0
    $raw.seek_concurrency_samples = @(1..1000 | ForEach-Object {
        [ordered]@{
            request_start_qpc=100; a_request_qpc=101; b_request_qpc=102
            dispatch_complete_qpc=103; a_begin_qpc=104; a_ready_qpc=106
            b_begin_qpc=105; b_ready_qpc=107; a_request_id=$_; b_request_id=$_
            a_request_result='Accepted'; b_request_result='Accepted'
            parallel_dispatch_valid=$true; execution_overlap=$true
        }
    })
}
$raw.formal_opportunity_ledger = @(0..([int]$raw.required_measurement_frame_count - 1) |
    ForEach-Object {
        [ordered]@{
            last_committed_opportunity_ordinal=$_ - 1
            predicted_opportunity_ordinal=$_; actual_opportunity_ordinal=$_
            render_begin_qpc=if ($_ -eq 0) { 900 } else { $_ * 1000 + 1 }
            render_end_qpc=if ($_ -eq 0) { 950 } else { $_ * 1000 + 2 }
            presentation_swap_qpc=($_ + 1) * 1000
            render_ordinal=$_; swap_ordinal=$_
            refresh_numerator=60; refresh_denominator=1
            pre_render_authority=[ordered]@{ available=$true; refresh_count=100 + $_
                qpc_vblank=500 + $_ * 10; refresh_numerator=60; refresh_denominator=1 }
            post_swap_authority=[ordered]@{ available=$true; refresh_count=101 + $_
                qpc_vblank=505 + $_ * 10; refresh_numerator=60; refresh_denominator=1 }
            authority_continuous=$true; predicted_source_frame=$_
            expected_source_frame=$_; presented_source_frame=$_
            repeat=$false; true_drop_before_this_opportunity=0
            lost_opportunity_count=0; classification='EXACT'
        }
    })
if ($Case -in @('ReconciledGood', 'NegativeForwardDropIgnored')) {
    $raw.formal_opportunity_ledger = @($raw.formal_opportunity_ledger | Select-Object -First 119)
    for ($index = 57; $index -lt $raw.formal_opportunity_ledger.Count; ++$index) {
        $record = $raw.formal_opportunity_ledger[$index]
        if ($index -eq 57) {
            $record.actual_opportunity_ordinal = 58
            $record.expected_source_frame = 58
            $record.presentation_swap_qpc = 59000
            $record.lost_opportunity_count = 1
            $record.classification = 'FORWARD_OPPORTUNITY_LOSS'
        } else {
            $ordinal = $index + 1
            $record.last_committed_opportunity_ordinal = $ordinal - 1
            $record.predicted_opportunity_ordinal = $ordinal
            $record.actual_opportunity_ordinal = $ordinal
            $record.render_begin_qpc = ($index + 1) * 1000 + 1
            $record.render_end_qpc = ($index + 1) * 1000 + 2
            $record.presentation_swap_qpc = ($index + 2) * 1000
            $record.predicted_source_frame = $ordinal
            $record.expected_source_frame = $ordinal
            $record.presented_source_frame = $ordinal
            if ($index -eq 58) { $record.true_drop_before_this_opportunity = 1 }
        }
    }
    $first = $raw.formal_opportunity_ledger[57]
    $raw.formal_displayed_unique_count = 119
    $raw.formal_gap_true_drop_count = 1
    $raw.formal_true_opportunity_drop_count = 1
    $raw.formal_forward_reconciliation_count = 1
    $raw.formal_lost_opportunity_count = 1
    $raw.measurement_displayed_composition_count = 119
    $raw.measurement_gpu_submission_count = 119
    $raw.measurement_layer_draw_count = 238
    $raw.measurement_logical_clear_count = 119
    $raw.measurement_dropped_output_count = 1
    $raw.formal_first_reconciliation_event = [ordered]@{
        captured=$true; classification='FORWARD_OPPORTUNITY_LOSS'
        predicted_opportunity_ordinal=$first.predicted_opportunity_ordinal
        actual_opportunity_ordinal=$first.actual_opportunity_ordinal
    }
}
switch ($Case) {
    # 実装と同じ式を共有せず、各caseで1 fieldだけを壊してcheckerの効力を確認する。
    'NegativeInvariant' { $raw.measurement_layer_draw_count = 239 }
    'NegativeSameDevice' { $raw.Remove('same_device_b') }
    'NegativeOutputSize' { $raw.actual_output_width = 1919 }
    'NegativeBackend' { $raw.actual_gpu_completion_backend = 'event_query' }
    'NegativeSeed' { $raw.configured_seed = 1 }
    'NegativeFormalTiming' { $raw.configured_warmup_seconds = 4 }
    'NegativeScheduledHigh' { $raw.measurement_scheduled_output_count = 3601 }
    'NegativeScheduledLow' { $raw.measurement_scheduled_output_count = 3599 }
    'NegativeCoverage' { $raw.source_coverage_ok = $false }
    'NegativeSourceFrameCount' { $raw.source_a_frame_count = 3599 }
    'NegativeMissingPair' { $raw.measurement_missing_pair_count = 1 }
    'NegativeEofA' { $raw.measurement_source_a_eof_count = 1 }
    'NegativeEofB' { $raw.measurement_source_b_eof_count = 1 }
    'NegativePrerollConfigured' { $raw.configured_measurement_preroll_frames = 7 }
    'NegativePrerollOk' { $raw.measurement_preroll_ok = $false }
    'NegativePrerollDepthA' { $raw.measurement_preroll_depth_a = 7 }
    'NegativePrerollDepthB' { $raw.measurement_preroll_depth_b = 7 }
    'NegativePrerollFrontA' { $raw.measurement_preroll_front_a = 1 }
    'NegativePrerollFrontB' { $raw.measurement_preroll_front_b = 1 }
    'NegativeSeekPublishReject' { $raw.seek_completion_publish_reject_count = 1 }
    'NegativeSeekRequestMismatch' { $raw.seek_completion_request_mismatch_count = 1 }
    'NegativeSeekStaleCompletion' { $raw.seek_stale_completion_count = 1 }
    'NegativeSeekBusyAcceptance' { $raw.seek_busy_acceptance_count = 1 }
    'NegativeSeekStoppedSuperseded' { $raw.seek_completion_stopped_superseded_count = 1 }
    'NegativeSeekConcurrencyCount' {
        $raw.seek_concurrency_samples = @($raw.seek_concurrency_samples | Select-Object -First 15)
    }
    'SeekExecutionNonoverlapGood' {
        $raw.seek_concurrency_samples[7].a_ready_qpc = 105
        $raw.seek_concurrency_samples[7].b_begin_qpc = 106
        $raw.seek_concurrency_samples[7].b_ready_qpc = 107
        $raw.seek_concurrency_samples[7].execution_overlap = $false
        $raw.execution_overlap_count = 15
        $raw.execution_nonoverlap_count = 1
    }
    'NegativeParallelDispatchCount' { $raw.parallel_dispatch_valid_count = 999 }
    'NegativeParallelDispatchSample' {
        $raw.seek_concurrency_samples[7].parallel_dispatch_valid = $false
    }
    'NegativeBRequestResult' { $raw.seek_concurrency_samples[7].b_request_result = 'RejectedBusy' }
    'NegativeDispatchAfterFirstReady' {
        $raw.seek_concurrency_samples[7].dispatch_complete_qpc = 107
    }
    'NegativeOpportunityLedgerMutation' {
        $raw.formal_opportunity_ledger[57].presented_source_frame = 999
    }
    'NegativeOpportunityOrdinalGapIgnored' {
        $raw.formal_opportunity_ledger[57].actual_opportunity_ordinal = 58
    }
    'NegativeOpportunityRefreshChange' {
        $raw.formal_opportunity_ledger[57].refresh_numerator = 59950
        $raw.formal_opportunity_ledger[57].refresh_denominator = 1000
    }
    'NegativeOpportunityAuthority' { $raw.formal_opportunity_authority_valid = $false }
    'NegativeOpportunityTail' { $raw.tail_true_drop = 1 }
    'NegativeForwardDropIgnored' {
        $raw.formal_opportunity_ledger[58].true_drop_before_this_opportunity = 0
    }
    'NegativeMeasurementDelta' { $raw.measurement_present_callback_count = -1 }
}
$raw | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $Output -Encoding utf8
$formal = $Case -in $formalCases
if ($formal) {
    $mode = if ($Case -in $seekCases) { 'Seek' } else { 'Playback' }
    & pwsh -NoProfile -File $Checker -Json $Output -Mode $mode -ProcessExitCode 0
} else {
    $mode = if ($Case -in $seekCases) { 'Seek' } else { 'Playback' }
    & pwsh -NoProfile -File $Checker -Json $Output -Mode $mode -ProcessExitCode 0 -DryRun
}
$actual = $LASTEXITCODE
$expected = if ($Case -in @(
    'Good', 'FormalGood', 'SeekGood', 'SeekExecutionNonoverlapGood', 'FormalSeekGood',
    'ReconciledGood')) { 0 } else { 3 }
if ($actual -ne $expected) {
    throw "$Case contract testの終了codeが違います: expected=$expected actual=$actual"
}
Write-Host "P2 contract $Case test: PASS"
