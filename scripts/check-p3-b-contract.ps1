[CmdletBinding()]
param([Parameter(Mandatory)][string]$Json)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not (Test-Path -LiteralPath $Json)) { throw "P3-B JSON がありません: $Json" }
$data = Get-Content -Raw -LiteralPath $Json | ConvertFrom-Json
$required = @(
    'schema_version','phase','formal_verdict','mode','pass','audio_master_only',
    'samples_per_video_frame','video_preroll_frames','audio_preroll_ms',
    'endpoint_prefill_frames','endpoint_first_media_sample','endpoint_start_device_position',
    'clock_anchor_media_sample','clock_anchor_device_position','audio_underflow_count',
    'audio_overflow_count','video_displayed_count','video_catchup_skip_count',
    'video_pair_wait_count','video_target_superseded_count',
    'audio_clock_video_catchup_skip_count',
    'audio_clock_video_stale_discard_a','audio_clock_video_stale_discard_b',
    'video_ahead_violation_count','clock_regression_count',
    'video_qpc_master_fallback_count','marker_a_mismatch','marker_b_mismatch',
    'mixed_pair_mismatch','mixed_generation_count','stale_composition_epoch_count',
    'application_av_delta_ms','application_av_delta_abs_ms','seeks','device_lost_count',
    'audio_render_thread_join_leak','audio_decode_thread_join_leak',
    'video_worker_a_joined','video_worker_b_joined','lifecycle_violation_count'
)
foreach ($name in $required) {
    if ($null -eq $data.PSObject.Properties[$name]) { throw "P3-B 必須 field がありません: $name" }
}
if ($data.schema_version -ne 1 -or $data.phase -ne 'P3-B' -or
    $data.formal_verdict -ne 'NOT_RUN') { throw 'P3-B schema/phase/formal verdict が不正です' }
if (-not $data.pass -or -not $data.audio_master_only) { throw 'P3-B run は成功していません' }
if ($data.samples_per_video_frame -ne 800 -or $data.video_preroll_frames -lt 8 -or
    $data.audio_preroll_ms -lt 100) { throw 'P3-B startup/mapping contract が不正です' }
if ($data.endpoint_prefill_frames -le 0 -or
    $data.endpoint_first_media_sample -ne $data.clock_anchor_media_sample) {
    throw 'endpoint の first PCM と clock anchor が一致しません'
}
$zeroFields = @(
    'audio_underflow_count','audio_overflow_count','video_ahead_violation_count',
    'clock_regression_count','video_qpc_master_fallback_count','marker_a_mismatch',
    'marker_b_mismatch','mixed_pair_mismatch','mixed_generation_count',
    'stale_composition_epoch_count','device_lost_count','audio_render_thread_join_leak',
    'audio_decode_thread_join_leak','lifecycle_violation_count'
)
foreach ($name in $zeroFields) {
    if ([long]$data.$name -ne 0) { throw "$name は 0 でなければなりません: $($data.$name)" }
}
if (-not $data.video_worker_a_joined -or -not $data.video_worker_b_joined) {
    throw 'video worker join が完了していません'
}
if ($data.video_displayed_count -le 0 -or $data.application_av_delta_ms.count -le 0 -or
    $data.application_av_delta_ms.values.Count -ne $data.application_av_delta_ms.count) {
    throw 'display/AV delta sample が空振りです'
}
if ($data.mode -eq 'seek') {
    if ($data.integrated_seek_requested -ne 64 -or $data.integrated_seek_exact -ne 64 -or
        $data.seeks.Count -ne 64) { throw 'integrated seek は 64/64 ではありません' }
    foreach ($seek in $data.seeks) {
        if ($seek.first_audio_sample -ne $seek.requested_audio_sample -or
            $seek.first_displayed_video_frame -ne $seek.requested_frame) {
            throw 'integrated seek の first audio/video identity が不一致です'
        }
    }
}
if ($data.mode -eq 'pause-resume' -and
    (-not $data.pause_clock_frozen -or -not $data.pause_video_advance_zero -or
     -not $data.pause_generation_stable)) { throw 'pause/resume contract が不正です' }
Write-Host "PASS: P3-B JSON contract ($($data.mode))"
