[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Case,
    [Parameter(Mandatory)][string]$Checker,
    [Parameter(Mandatory)][string]$OutputDir
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$json = Join-Path $OutputDir "$Case.json"
$e0 = 7L
function State([long]$Frame) { if ($Frame -lt 200) { 'S0' } elseif ($Frame -lt 400) { 'S1' } else { 'S2' } }
function Epoch([long]$Frame) { if ($Frame -lt 200) { $e0 } elseif ($Frame -lt 400) { $e0 + 1 } else { $e0 + 2 } }
function Record([long]$Frame) {
    [ordered]@{
        output_frame = $Frame; composition_state = State $Frame; composition_epoch = Epoch $Frame
        application_av_projection_valid = $true; application_av_delta_ms = 0.0
        sources = @(
            [ordered]@{source_id=1;frame=$Frame;source_generation=11;resource_epoch=21},
            [ordered]@{source_id=2;frame=$Frame;source_generation=12;resource_epoch=22})
    }
}
$ledger = @(0..599 | ForEach-Object { Record $_ })
function Probe([long]$Boundary, [string]$Name) {
    $frame = $Boundary
    $rgba = if ($Name -eq 'TL') { @(80,90,100,255) } else { @(110,120,130,255) }
    [ordered]@{
        boundary=$Boundary; actual_output_frame=$frame; composition_state=(State $frame)
        cpu_reference_state=(State $frame)
        composition_epoch=(Epoch $frame); probe=$Name
        x=$(if ($Name -eq 'TL') {480} else {1440}); y=$(if ($Name -eq 'TL') {270} else {810})
        actual_rgba=@($rgba); cpu_expected_rgba=@($rgba); gpu_ticket=$Boundary
        gpu_completion_serial=$Boundary; completion_observed=$true; blocking_wait_count=0
        sources=@(
            [ordered]@{source_id=1;frame=$frame;source_generation=11;resource_epoch=21},
            [ordered]@{source_id=2;frame=$frame;source_generation=12;resource_epoch=22})
    }
}
$environment = [ordered]@{
    screen_name='test';screen_orientation='landscape';screen_geometry_width=1920
    screen_geometry_height=1080;available_geometry_width=1920;available_geometry_height=1040
    device_pixel_ratio=1.0;window_logical_width=1920;window_logical_height=1080
    compositor_surface_logical_width=1920;compositor_surface_logical_height=1080
    rhi_target_pixel_width=1920;rhi_target_pixel_height=1080
}
$raw = [ordered]@{
    schema='mvm-p4-smoke-1';schema_version=1;contract_version='P4-C-smoke-frozen';phase='P4-C'
    schedule_kind='smoke';formal_verdict='NOT_RUN';smoke_contract_verdict='NOT_RUN'
    canonical_schedule='0:S0;200:S1;400:S2'
    canonical_schedule_sha256='418ae09f4bb9349aa7ac53ca38028782aef074ca1696338758ccfa6b4e4398e8'
    schedule=@(@{boundary=0;state='S0'},@{boundary=200;state='S1'},@{boundary=400;state='S2'})
    fixture_a_sha256='d398114c38806f39670df51dfabb0095d462cbc35286ea1467901d4007cf0308'
    fixture_b_sha256='fe7cd1a45d101d363cb3930497601efd41dd55fd36c194acf8f24e3e4728b479'
    cpu_reference_pixel_status='PRECOMPUTED';cpu_reference_candidate_frame_count=6
    cpu_reference_candidate_probe_count=12;measurement_seconds=10;required_video_frames=600
    measurement_audio_start_sample=0;measurement_audio_end_sample=480000;audio_master_only=$true
    measurement_baseline_composition_epoch=$e0;composition_state_resolve_count=600
    composition_state_adoption_count=2;composition_state_noop_count=598
    composition_state_reject_count=0;composition_state_unresolved_count=0
    composition_epoch_increment_count=2;source_generation_change_due_to_layout_count=0
    phase4_adoption_failure_count=0;baseline_source_generation_a=11;baseline_source_generation_b=12
    baseline_resource_epoch_a=21;baseline_resource_epoch_b=22
    measurement_display_ledger=$ledger;measurement_display_ledger_count=600
    measurement_video_displayed_unique_count=600;measurement_video_skipped_frame_count=0
    measurement_non_increasing_display_count=0;transition_activation_lag_frames=@(0,0)
    transition_boundaries=@(
        [ordered]@{boundary=200;expected_state='S1';first_displayed_output_frame=200
            activation_lag_frames=0;first_display_state='S1';first_display_composition_epoch=($e0+1)
            first_display_sources=@(
                [ordered]@{source_id=1;frame=200;source_generation=11;resource_epoch=21},
                [ordered]@{source_id=2;frame=200;source_generation=12;resource_epoch=22})},
        [ordered]@{boundary=400;expected_state='S2';first_displayed_output_frame=400
            activation_lag_frames=0;first_display_state='S2';first_display_composition_epoch=($e0+2)
            first_display_sources=@(
                [ordered]@{source_id=1;frame=400;source_generation=11;resource_epoch=21},
                [ordered]@{source_id=2;frame=400;source_generation=12;resource_epoch=22})})
    old_state_after_boundary_count=0;transition_pixel_probe_status='COMPLETE'
    transition_probe_records=@((Probe 200 'TL'),(Probe 200 'BR'),(Probe 400 'TL'),(Probe 400 'BR'))
    transition_probe_checked_count=4;transition_probe_mismatch_count=0
    transition_probe_render_thread_blocking_wait_count=0;transition_probe_pending_after_drain_count=0
    transition_probe_completion_failure_count=0;transition_probe_untracked_submission_count=0
    transition_probe_retirement_timeout_count=0;transition_probe_issue_failure_count=0
    composition_state_display_mismatch_count=0;composition_pair_identity_violation_count=0
    composition_layer_generation_mismatch_count=0;measurement_audio_underflow_count=0
    measurement_audio_overflow_count=0;measurement_marker_mismatch_count=0
    measurement_mixed_pair_count=0;measurement_mixed_generation_count=0
    measurement_stale_composition_epoch_count=0;measurement_video_ahead_violation_count=0
    measurement_clock_regression_count=0;measurement_video_qpc_master_fallback_count=0
    measurement_audio_clock_query_failure_count=0;measurement_audio_clock_catchup_skip_count=0
    measurement_scheduler_deadline_drop_count=0;measurement_render_failure_count=0
    cpu_full_frame_readback_count=0
    full_frame_gpu_copy_count=0;software_video_fallback_count=0;untracked_submission_count=0
    completion_poll_failure_count=0;retirement_depth_after_drain=0
    payloads_released_before_completion=0;retirement_timeout_count=0
    partial_gpu_issue_failure_count=0;device_lost_count=0
    lifecycle_violation_count=0;audio_render_thread_join_leak=0;audio_decode_thread_join_leak=0
    video_worker_a_joined=$true;video_worker_b_joined=$true;teardown_success=$true
    final_report_after_teardown=$true;shutdown_workers_joined_before_teardown=$true
    shutdown_render_teardown_requested=$true;shutdown_order_violation_count=0
    shutdown_sequence=@('DisableSchedulers','StopAudioSink','StopAudioDecodeWorker','StopVideoWorkerA',
        'StopVideoWorkerB','DetachSharedWorkerRefs','RequestRenderTeardown')
    display_target_preflight_pass=$true;requested_output_width=1920;requested_output_height=1080
    display_environment_start=$environment
    display_environment_end=([ordered]@{} + $environment);effective_video_fps=60.0;drop_rate=0.0
    application_av_delta_abs_ms=@{count=600;p50=0.0;p95=0.0;p99=0.0;min=0.0;max=0.0}
}

function Set-PerformanceDiagnosticOnly {
    # transition/probe frameを避けてledger自体から51 frameを除く。
    # fps/dropとA/Vの旧performance thresholdをすべて超えるが、summaryはrawと一致させる。
    $raw.measurement_display_ledger = @($raw.measurement_display_ledger |
        Where-Object { $_.output_frame -lt 500 -or $_.output_frame -gt 550 })
    foreach ($record in @($raw.measurement_display_ledger | Select-Object -First 28)) {
        $record.application_av_delta_ms = 40.0
    }
    $unique = 549
    $skipped = 51
    $raw.measurement_display_ledger_count = $unique
    $raw.measurement_video_displayed_unique_count = $unique
    $raw.measurement_video_skipped_frame_count = $skipped
    $raw.composition_state_resolve_count = $unique
    $raw.composition_state_noop_count = $unique - 2
    $raw.effective_video_fps = $unique / 10.0
    $raw.drop_rate = $skipped / 600.0
    $raw.measurement_audio_clock_catchup_skip_count = $skipped
    $raw.application_av_delta_abs_ms = @{count=$unique;p50=0.0;p95=40.0;p99=40.0;min=0.0;max=40.0}
}

switch ($Case) {
    Good {}
    SmokePerformanceDiagnosticOnly { Set-PerformanceDiagnosticOnly }
    WrongProbeRgb { $raw.transition_probe_records[0].actual_rgba[0] += 4 }
    WrongProbeAlpha { $raw.transition_probe_records[0].actual_rgba[3] = 254 }
    ProbeMissing { $raw.transition_probe_records = @($raw.transition_probe_records | Select-Object -Skip 1); $raw.transition_probe_checked_count=3 }
    ProbeDuplicate { $raw.transition_probe_records[1] = $raw.transition_probe_records[0] }
    ProbeWrongCoordinate { $raw.transition_probe_records[0].x = 481 }
    ProbeWrongBoundary { $raw.transition_probe_records[0].boundary = 300 }
    ProbeWrongActualFrame { $raw.transition_probe_records[0].actual_output_frame = 201 }
    ProbeWrongState { $raw.transition_probe_records[0].composition_state = 'S0' }
    ProbeWrongEpoch { $raw.transition_probe_records[0].composition_epoch = $e0 }
    ProbeCount3 { $raw.transition_probe_records = @($raw.transition_probe_records[0..2]); $raw.transition_probe_checked_count=3 }
    ProbeCount5 { $raw.transition_probe_records += (Probe 400 'BR'); $raw.transition_probe_checked_count=5 }
    ProbeBlockingWait { $raw.transition_probe_render_thread_blocking_wait_count=1 }
    ProbePendingAfterDrain { $raw.transition_probe_pending_after_drain_count=1 }
    ProbeCompletionFailure { $raw.transition_probe_completion_failure_count=1 }
    ProbeUntrackedCompletion { $raw.transition_probe_untracked_submission_count=1 }
    WrongCpuReference { $raw.transition_probe_records[0].cpu_expected_rgba[1] += 4 }
    CpuReferenceWrongState { $raw.transition_probe_records[0].cpu_reference_state = 'S2' }
    WrongScheduleHash { $raw.canonical_schedule_sha256 = '00' * 32 }
    FormalScheduleInSmoke { $raw.canonical_schedule='0:S0;600:S1;1200:S2;1800:S3;2400:S0;3000:S1' }
    FormalCountInSmoke { $raw.composition_state_adoption_count=5 }
    WrongState { $raw.measurement_display_ledger[250].composition_state='S0' }
    StaleEpoch { $raw.measurement_display_ledger[250].composition_epoch=$e0 }
    OldStateAfterBoundary { $raw.old_state_after_boundary_count=1 }
    Lag3 { $raw.transition_activation_lag_frames[0]=3 }
    GenerationChanged { $raw.measurement_display_ledger[250].sources[0].source_generation=13 }
    ShutdownOrder { $raw.shutdown_sequence[0]='StopAudioSink' }
    TeardownBeforeJoin { $raw.shutdown_workers_joined_before_teardown=$false }
    MissingBoolean { $raw.Remove('teardown_success') }
    StringFalseBoolean { $raw.teardown_success='false' }
    AvRawBadSummaryGood { $raw.measurement_display_ledger[0].application_av_delta_ms=40.0 }
    SmokeFpsSummaryMismatch { $raw.effective_video_fps=59.0 }
    SmokeDropSummaryMismatch { $raw.drop_rate=0.01 }
    SmokeAvSummaryMismatch { $raw.application_av_delta_abs_ms.max=1.0 }
    default { throw "未知caseです: $Case" }
}
$raw | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $json -Encoding utf8
& $Checker -Json $json
$actualExit = $LASTEXITCODE
$expectedExit = if ($Case -in @('Good','SmokePerformanceDiagnosticOnly')) { 0 } else { 3 }
if ($actualExit -ne $expectedExit) {
    Write-Error "checker終了コードが違います: case=$Case actual=$actualExit expected=$expectedExit"
    exit 1
}
Write-Host "[p4-smoke-test] $Case expected exit $expectedExit を確認しました"
exit 0
