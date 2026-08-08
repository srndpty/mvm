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
        'NegativeSeekStoppedSuperseded', 'NegativeSeekOverlapCount',
        'NegativeSeekConcurrencyCount', 'NegativeSeekSampleOverlap')][string]$Case,
    [Parameter(Mandatory)][string]$Checker,
    [Parameter(Mandatory)][string]$Output
)
$ErrorActionPreference = 'Stop'

$raw = [ordered]@{
    schema='mvm-p2-formal-1'; formal_contract_version='P2-D4-2'; mode='playback'
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
    marker_a_checked_count=7; marker_b_checked_count=7
    marker_a_mismatch=0; marker_b_mismatch=0
    actual_target_probe_checked_count=4; actual_target_probe_mismatch=0
    mixed_source_frame_count=0; mixed_generation_count=0; stale_composition_epoch_count=0
    cpu_full_frame_readback_count=0; full_frame_gpu_copy_count=0
    payloads_released_before_completion=0; retirement_timeout_count=0
    retirement_depth_after_drain=0; device_lost_count=0; lifecycle_order_violation_count=0
    teardown_success=$true; final_report_written_after_teardown=$true
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
    dual_seek_displayed_ms=@(1.0) * 16; dual_seek_decode_ready_ms=@(0.5) * 16
    dual_seek_displayed_p95_ms=1.0; dual_seek_displayed_observed_max_ms=1.0
    seek_display_mismatch=0; seek_timeout_count=0; untracked_submission_count=0
    completion_poll_failure_count=0; seek_completion_publish_reject_count=0
    seek_completion_request_mismatch_count=0; seek_stale_completion_count=0
    seek_busy_acceptance_count=0; seek_completion_stopped_superseded_count=0
    software_fallback_count=0; worker_join_leak_count=0; seek_overlap_count=16
    seek_concurrency_samples=@(1..16 | ForEach-Object { [ordered]@{ overlap=$true } })
}
$seekCases = @(
    'SeekGood', 'NegativeSeekPublishReject', 'NegativeSeekRequestMismatch',
    'NegativeSeekStaleCompletion', 'NegativeSeekBusyAcceptance',
    'NegativeSeekStoppedSuperseded', 'NegativeSeekOverlapCount',
    'NegativeSeekConcurrencyCount', 'NegativeSeekSampleOverlap'
)
if ($Case -in $seekCases) { $raw.mode = 'seek' }
$formalCases = @(
    'FormalGood', 'NegativeFormalTiming', 'NegativeScheduledHigh',
    'NegativeScheduledLow', 'NegativeCoverage', 'NegativeSourceFrameCount',
    'NegativeMissingPair', 'NegativeEofA', 'NegativeEofB'
)
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
    'NegativeSeekOverlapCount' { $raw.seek_overlap_count = 15 }
    'NegativeSeekConcurrencyCount' {
        $raw.seek_concurrency_samples = @($raw.seek_concurrency_samples | Select-Object -First 15)
    }
    'NegativeSeekSampleOverlap' { $raw.seek_concurrency_samples[7].overlap = $false }
}
$raw | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $Output -Encoding utf8
$formal = $Case -in $formalCases
if ($formal) {
    & pwsh -NoProfile -File $Checker -Json $Output -Mode Playback -ProcessExitCode 0
} else {
    $mode = if ($Case -in $seekCases) { 'Seek' } else { 'Playback' }
    & pwsh -NoProfile -File $Checker -Json $Output -Mode $mode -ProcessExitCode 0 -DryRun
}
$actual = $LASTEXITCODE
$expected = if ($Case -in @('Good', 'FormalGood', 'SeekGood')) { 0 } else { 3 }
if ($actual -ne $expected) {
    throw "$Case contract testの終了codeが違います: expected=$expected actual=$actual"
}
Write-Host "P2 contract $Case test: PASS"
