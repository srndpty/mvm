param(
    [Parameter(Mandatory)][ValidateSet('Good', 'Negative')][string]$Case,
    [Parameter(Mandatory)][string]$Checker,
    [Parameter(Mandatory)][string]$Output
)
$ErrorActionPreference = 'Stop'

$raw = [ordered]@{
    schema='mvm-p2-formal-1'; formal_contract_version='P2-D1-1'; mode='playback'
    formal_preflight=$true; process_exit_code=0
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
    measurement_layer_draw_count=240; measurement_logical_clear_count=120
    measurement_drop_scheduler_deadline=0; measurement_drop_missing_source_a=0
    measurement_drop_missing_source_b=0; measurement_drop_missing_both=0
    measurement_drop_stale_generation=0; measurement_drop_future_generation=0
    measurement_drop_stale_composition_epoch=0; measurement_drop_render_failure=0
    measurement_untracked_submission_count=0; measurement_completion_poll_failure_count=0
    measurement_partial_gpu_issue_failure_count=0; effective_fps=60.0; drop_rate=0.0
}
if ($Case -eq 'Negative') {
    # 実装と同じ式を共有せず、1 fieldだけを壊してcheckerの効力を確認する。
    $raw.measurement_layer_draw_count = 239
}
$raw | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $Output -Encoding utf8
& pwsh -NoProfile -File $Checker -Json $Output -Mode Playback -ProcessExitCode 0 -DryRun
$actual = $LASTEXITCODE
$expected = if ($Case -eq 'Good') { 0 } else { 3 }
if ($actual -ne $expected) {
    throw "$Case contract testの終了codeが違います: expected=$expected actual=$actual"
}
Write-Host "P2 contract $Case test: PASS"
