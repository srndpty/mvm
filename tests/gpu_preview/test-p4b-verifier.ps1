<#
Phase 4 / B verifier の対照群と negative test。

検査を書いただけでは、それが効いている証明にならない (AGENTS.md)。
Good を 1 件ずつ壊し、verifier が**壊した箇所で**落ちることを確かめる。
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('Good', 'NegativeWrongState', 'NegativeStaleEpoch', 'NegativeAdoptionCount',
                 'NegativeCounterSelfConsistency', 'NegativeProbeFabricated',
                 'NegativeProbeFieldMissing', 'NegativeFormalHash', 'NegativeLagOutOfRange',
                 'NegativeGenerationChange', 'NegativeLayerIdentity', 'NegativeFalseSanityPass',
                 'NegativeFirstFrame', 'NegativeShutdownOrder', 'NegativeShutdownMissingStep',
                 'NegativeTeardownBeforeJoin')]
    [string]$Case,
    [Parameter(Mandatory)][string]$Checker,
    [Parameter(Mandatory)][string]$OutputDir
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$json = Join-Path $OutputDir "$Case.json"

$e0 = 2
$generationA = 3
$generationB = 4
$resourceA = 1
$resourceB = 1

function New-Record([long]$frame) {
    if ($frame -lt 200) { $state = 'S0'; $id = 1; $segment = 0 }
    elseif ($frame -lt 400) { $state = 'S1'; $id = 2; $segment = 1 }
    else { $state = 'S2'; $id = 3; $segment = 2 }
    return [ordered]@{
        output_frame = $frame
        composition_state = $state
        composition_state_id = $id
        composition_epoch = $e0 + $segment
        display_record_qpc = 1000 + $frame
        application_av_projection_valid = $true
        application_av_delta_ms = 0.5
        sources = @(
            [ordered]@{ source_id = 1; frame = $frame; source_generation = $generationA
                        resource_epoch = $resourceA },
            [ordered]@{ source_id = 2; frame = $frame; source_generation = $generationB
                        resource_epoch = $resourceB })
    }
}

$frames = 0..599
if ($Case -eq 'NegativeLagOutOfRange') { $frames = $frames | Where-Object { $_ -lt 200 -or $_ -gt 203 } }
if ($Case -eq 'NegativeFirstFrame') { $frames = $frames | Where-Object { $_ -ne 0 } }
$ledger = @($frames | ForEach-Object { New-Record ([long]$_) })
$resolveCount = 600
$noopCount = 598

# OrderedDictionary への [0] は「先頭 entry の値」を返してしまうため使わない。
$ledger | Where-Object { $_.output_frame -eq 250 } | ForEach-Object {
    if ($Case -eq 'NegativeWrongState') { $_.composition_state = 'S0' }
    if ($Case -eq 'NegativeStaleEpoch') { $_.composition_epoch = $e0 }
}
$ledger | Where-Object { $_.output_frame -eq 300 } | ForEach-Object {
    if ($Case -eq 'NegativeLayerIdentity') { $_.sources[0].frame = 299 }
}

$lags = @(0, 0)
if ($Case -eq 'NegativeLagOutOfRange') { $lags = @(4, 0) }

$raw = [ordered]@{
    schema = 'mvm-p4-b-integration-1'
    schema_version = 1
    contract_version = 'P4-B'
    phase = 'P4-B'
    schedule_kind = 'smoke'
    formal_verdict = 'NOT_RUN'
    smoke_contract_verdict = 'NOT_RUN'
    integration_sanity_pass = $true
    detail = '10 秒 integration sanity 区間完了'
    canonical_schedule = '0:S0;200:S1;400:S2'
    canonical_schedule_sha256 = '418ae09f4bb9349aa7ac53ca38028782aef074ca1696338758ccfa6b4e4398e8'
    schedule = @(
        [ordered]@{ boundary = 0; state = 'S0' },
        [ordered]@{ boundary = 200; state = 'S1' },
        [ordered]@{ boundary = 400; state = 'S2' })
    audio_master_only = $true
    required_video_frames = 600
    measurement_baseline_composition_epoch = $e0
    composition_state_resolve_count = $resolveCount
    composition_state_adoption_count = 2
    composition_state_noop_count = $noopCount
    composition_state_reject_count = 0
    composition_state_unresolved_count = 0
    composition_epoch_increment_count = 2
    composition_counter_self_consistent = $true
    composition_state_display_mismatch_count = 0
    old_state_after_boundary_count = 0
    composition_pair_identity_violation_count = 0
    composition_layer_generation_mismatch_count = 0
    source_generation_change_due_to_layout_count = 0
    phase4_adoption_failure_count = 0
    transition_activation_lag_frames = $lags
    measurement_display_ledger = $ledger
    measurement_display_ledger_count = $ledger.Count
    transition_pixel_probe_status = 'NOT_IMPLEMENTED'
    transition_probe_checked_count = $null
    transition_probe_mismatch_count = $null
    transition_probe_render_thread_blocking_wait_count = $null
    cpu_reference_pixel_status = 'NOT_IMPLEMENTED'
    measurement_video_first_frame = 0
    measurement_video_last_frame = 599
    measurement_video_displayed_unique_count = $ledger.Count
    measurement_video_skipped_frame_count = 600 - $ledger.Count
    measurement_non_increasing_display_count = 0
    effective_video_fps = 60.0
    drop_rate = 0.0
    measurement_audio_underflow_count = 0
    measurement_audio_overflow_count = 0
    measurement_marker_mismatch_count = 0
    measurement_mixed_pair_count = 0
    measurement_mixed_generation_count = 0
    measurement_stale_composition_epoch_count = 0
    measurement_video_ahead_violation_count = 0
    measurement_clock_regression_count = 0
    measurement_video_qpc_master_fallback_count = 0
    measurement_audio_clock_query_failure_count = 0
    application_av_delta_abs_ms = [ordered]@{ count = 600; p50 = 1.0; p95 = 2.0; p99 = 3.0
                                              min = 0.0; max = 4.0 }
    baseline_source_generation_a = $generationA
    baseline_source_generation_b = $generationB
    baseline_resource_epoch_a = $resourceA
    baseline_resource_epoch_b = $resourceB
    cpu_full_frame_readback_count = 0
    full_frame_gpu_copy_count = 0
    software_video_fallback_count = 0
    device_lost_count = 0
    lifecycle_violation_count = 0
    audio_render_thread_join_leak = 0
    audio_decode_thread_join_leak = 0
    video_worker_a_joined = $true
    video_worker_b_joined = $true
    teardown_success = $true
    final_report_after_teardown = $true
    display_target_preflight_pass = $true
    shutdown_sequence = @('DisableSchedulers', 'StopAudioSink', 'StopAudioDecodeWorker',
                          'StopVideoWorkerA', 'StopVideoWorkerB', 'DetachSharedWorkerRefs',
                          'RequestRenderTeardown')
    shutdown_workers_joined_before_teardown = $true
    shutdown_render_teardown_requested = $true
    shutdown_order_violation_count = 0
}

switch ($Case) {
    'NegativeAdoptionCount' { $raw.composition_state_adoption_count = 3 }
    'NegativeCounterSelfConsistency' { $raw.composition_state_noop_count = 597 }
    'NegativeProbeFabricated' { $raw.transition_probe_mismatch_count = 0 }
    'NegativeProbeFieldMissing' { $raw.Remove('transition_probe_mismatch_count') }
    'NegativeFormalHash' {
        $raw.canonical_schedule_sha256 =
            '5b66543f43f98ad261a5a96e961332ef4a3d5b21f8f30b1713b4ff420a855f79'
    }
    'NegativeGenerationChange' { $raw.source_generation_change_due_to_layout_count = 1 }
    'NegativeFalseSanityPass' { $raw.integration_sanity_pass = $false }
    'NegativeShutdownOrder' {
        # Phase 4/B の実装が実際に持っていた誤り: audio worker を sink より先に停止する。
        $raw.shutdown_sequence = @('DisableSchedulers', 'StopAudioDecodeWorker', 'StopAudioSink',
                                   'StopVideoWorkerA', 'StopVideoWorkerB',
                                   'DetachSharedWorkerRefs', 'RequestRenderTeardown')
    }
    'NegativeShutdownMissingStep' {
        $raw.shutdown_sequence = @('DisableSchedulers', 'StopAudioSink', 'StopAudioDecodeWorker',
                                   'StopVideoWorkerA', 'StopVideoWorkerB',
                                   'RequestRenderTeardown')
    }
    'NegativeTeardownBeforeJoin' { $raw.shutdown_workers_joined_before_teardown = $false }
}

# ConvertTo-Json は $null を持つ key を落とさない。probe field の null は raw に残る。
$raw | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $json -Encoding utf8
& pwsh -NoProfile -File $Checker -Json $json
$checkerExit = $LASTEXITCODE
if ($Case -eq 'Good') {
    if ($checkerExit -ne 0) { Write-Error "対照群が落ちました (exit $checkerExit)" }
} else {
    if ($checkerExit -eq 0) { Write-Error "$Case が空振りしました" }
}
