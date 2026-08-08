[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Good','NegativeFallback')][string]$Case,
    [Parameter(Mandatory)][string]$Checker,
    [Parameter(Mandatory)][string]$OutputDir
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$json = Join-Path $OutputDir "$Case.json"
$record = [ordered]@{
    schema_version=1; phase='P3-B'; formal_verdict='NOT_RUN'; mode='playback'; pass=$true
    audio_master_only=$true; samples_per_video_frame=800; video_preroll_frames=8
    audio_preroll_ms=100; endpoint_prefill_frames=480
    endpoint_first_media_sample=0; endpoint_start_device_position=0
    clock_anchor_media_sample=0; clock_anchor_device_position=0
    audio_underflow_count=0; audio_overflow_count=0; video_displayed_count=1
    video_catchup_skip_count=0; video_pair_wait_count=0; video_target_superseded_count=0
    audio_clock_video_catchup_skip_count=0
    audio_clock_video_stale_discard_a=0; audio_clock_video_stale_discard_b=0
    video_ahead_violation_count=0; clock_regression_count=0
    video_qpc_master_fallback_count=0; marker_a_mismatch=0; marker_b_mismatch=0
    mixed_pair_mismatch=0; mixed_generation_count=0; stale_composition_epoch_count=0
    application_av_delta_ms=[ordered]@{count=1;p50=-1;p95=-1;min=-1;max=-1;values=@(-1)}
    application_av_delta_abs_ms=[ordered]@{count=1;p50=1;p95=1;p99=1;min=1;max=1}
    integrated_seek_requested=0; integrated_seek_exact=0; seeks=@()
    pause_clock_frozen=$false; pause_video_advance_zero=$false; pause_generation_stable=$false
    device_lost_count=0; audio_render_thread_join_leak=0; audio_decode_thread_join_leak=0
    video_worker_a_joined=$true; video_worker_b_joined=$true; lifecycle_violation_count=0
}
if ($Case -eq 'NegativeFallback') { $record.video_qpc_master_fallback_count = 1 }
$record | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $json -Encoding utf8
& pwsh -NoProfile -File $Checker -Json $json
if ($Case -eq 'Good') {
    if ($LASTEXITCODE -ne 0) { exit 1 }
} else {
    if ($LASTEXITCODE -eq 0) { Write-Error 'QPC fallback negative が空振りしました' }
}
