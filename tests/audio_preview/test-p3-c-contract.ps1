[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Case,
    [Parameter(Mandatory)][string]$Checker,
    [Parameter(Mandatory)][string]$OutputDir
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Distribution([double[]]$Values) {
    $sorted = @($Values | Sort-Object)
    $absolute = @($Values | ForEach-Object {[math]::Abs($_)} | Sort-Object)
    function Rank([object[]]$Items, [double]$P) {
        if ($Items.Count -eq 0) { return 0.0 }
        return [double]$Items[[math]::Ceiling($Items.Count * $P) - 1]
    }
    return [ordered]@{
        signed = [ordered]@{count=$Values.Count; p50=(Rank $sorted 0.5); p95=(Rank $sorted 0.95)
            min=$(if ($sorted.Count) {$sorted[0]} else {0.0}); max=$(if ($sorted.Count) {$sorted[-1]} else {0.0})
            values=@($Values)}
        absolute = [ordered]@{count=$Values.Count; p50=(Rank $absolute 0.5); p95=(Rank $absolute 0.95)
            p99=(Rank $absolute 0.99); min=$(if ($absolute.Count) {$absolute[0]} else {0.0})
            max=$(if ($absolute.Count) {$absolute[-1]} else {0.0})}
    }
}
function Common([string]$Mode) {
    return [ordered]@{
        schema='mvm-p3-formal-1'; contract_version='P3-C-1'; phase='P3-C'
        formal_verdict='NOT_RUN'; mode=$Mode; pass=$true; detail='test'; audio_master_only=$true
        configured_video_preroll_frames=8; configured_audio_preroll_ms=100
        warmup_seconds=1; measurement_seconds=1; measurement_audio_start_sample=0
        measurement_audio_end_sample=48000; observed_audio_stop_sample=48000
        required_measurement_samples=48000; required_video_frames=60
        measurement_video_first_frame=0; measurement_video_last_frame=59
        measurement_video_displayed_unique_count=60; measurement_video_skipped_frame_count=0
        measurement_duplicate_display_identity_count=0; measurement_non_increasing_display_count=0
        measurement_display_records=@(); effective_video_fps=60.0; drop_rate=0.0
        measurement_pair_wait_count=0; measurement_target_superseded_count=0
        measurement_stale_discard_a=0; measurement_stale_discard_b=0
        measurement_audio_underflow_count=0; measurement_audio_overflow_count=0
        measurement_marker_mismatch_count=0; measurement_mixed_pair_count=0
        measurement_mixed_generation_count=0; measurement_stale_composition_epoch_count=0
        measurement_video_ahead_violation_count=0; measurement_clock_regression_count=0
        measurement_video_qpc_master_fallback_count=0; measurement_audio_clock_query_failure_count=0
        application_av_delta_ms=$null; application_av_delta_abs_ms=$null
        application_av_projection_failure_count=0; endpoint_prefill_frames=480
        endpoint_first_media_sample=0; clock_anchor_media_sample=0
        integrated_seek_requested=0; integrated_seek_exact=0; integrated_seek_timeout_count=0
        integrated_seek_busy_acceptance_count=0; integrated_seek_stale_completion_count=0
        integrated_seek_generation_mismatch_count=0; seek_timeout_diagnostic=''
        seek_request_to_first_display_ms=$null; seek_first_display_application_av_delta_ms=$null
        seeks=@(); pause_clock_frozen=$false; pause_video_advance_zero=$false
        pause_generation_stable=$false; cpu_full_frame_readback_count=0
        full_frame_gpu_copy_count=0; software_video_fallback_count=0; device_lost_count=0
        lifecycle_violation_count=0; audio_render_thread_join_leak=0
        audio_decode_thread_join_leak=0; video_worker_a_joined=$true; video_worker_b_joined=$true
        teardown_success=$true; final_report_after_teardown=$true; adapter='test'
        audio_endpoint_sample_rate=48000; audio_endpoint_channels=2; audio_endpoint_sample_format='float'
    }
}
function Playback([double[]]$Values = @()) {
    $record = Common 'playback'
    if ($Values.Count -eq 0) { $Values = [double[]](1..60 | ForEach-Object {-10.0}) }
    if ($Values.Count -ne 60) { throw 'playback test values は 60 件必要です' }
    $distribution = Distribution $Values
    $record.application_av_delta_ms = $distribution.signed
    $record.application_av_delta_abs_ms = $distribution.absolute
    $record.measurement_display_records = @(0..59 | ForEach-Object {
        [ordered]@{frame=$_; display_record_qpc=1000+$_; application_av_projection_valid=$true
            application_av_delta_ms=$Values[$_]}
    })
    return $record
}
function Seek([double[]]$Latency = @(100.0,100.0), [double[]]$Av = @()) {
    $record = Common 'seek'
    if ($Av.Count -eq 0) { $Av = [double[]](1..$Latency.Count | ForEach-Object {-10.0}) }
    if ($Av.Count -ne $Latency.Count) { throw 'seek latency/AV test values の件数が不一致です' }
    $record.measurement_video_first_frame=-1; $record.measurement_video_last_frame=-1
    $record.measurement_video_displayed_unique_count=0; $record.required_video_frames=60
    $record.integrated_seek_requested=$Latency.Count; $record.integrated_seek_exact=$Latency.Count
    $record.seeks=@(0..($Latency.Count-1) | ForEach-Object {
        [ordered]@{requested_frame=$_; requested_audio_sample=$_*800; audio_generation=$_+1
            video_generation_a=$_+1; video_generation_b=$_+1; first_audio_sample=$_*800
            first_displayed_video_frame=$_; audio_ready_ms=10.0; video_a_ready_ms=10.0
            video_b_ready_ms=10.0; all_media_ready_ms=20.0; resume_to_display_ms=30.0
            request_to_first_display_ms=$Latency[$_]
            first_display_application_av_projection_valid=$true
            first_display_application_av_delta_ms=$Av[$_]}
    })
    $latencyDistribution=Distribution $Latency; $avDistribution=Distribution $Av
    $record.seek_request_to_first_display_ms=$latencyDistribution.signed
    $record.seek_first_display_application_av_delta_ms=$avDistribution.signed
    $empty=Distribution @(); $record.application_av_delta_ms=$empty.signed
    $record.application_av_delta_abs_ms=$empty.absolute
    return $record
}
function Pause([double]$Av = -10.0) {
    $record=Common 'pause-resume'; $record.pause_clock_frozen=$true
    $record.pause_video_advance_zero=$true; $record.pause_generation_stable=$true
    $distribution=Distribution @($Av,$Av)
    $record.application_av_delta_ms=$distribution.signed
    $record.application_av_delta_abs_ms=$distribution.absolute
    $empty=Distribution @(); $record.seek_request_to_first_display_ms=$empty.signed
    $record.seek_first_display_application_av_delta_ms=$empty.signed
    return $record
}

$mode = if ($Case -like 'NegativeSeek*' -or $Case -like 'Seek*') {'seek'} `
    elseif ($Case -like 'NegativePause*') {'pause-resume'} else {'playback'}
$record = switch ($mode) {'seek' {Seek} 'pause-resume' {Pause} default {Playback}}
$expectPass = $Case -eq 'Good'
switch ($Case) {
    'Good' {
        $values=[double[]](@(1..57 | ForEach-Object {-20.000}) + @(-33.334,-33.334,-33.334))
        $record=Playback $values
    }
    'NegativeFps' {$record.effective_video_fps=54.999}
    'NegativeDrop' {$record.drop_rate=0.020001}
    'NegativeAvP95' {$record=Playback ([double[]](1..60 | ForEach-Object {-20.001}))}
    'NegativeAvMax' {
        $values=[double[]](@(1..57 | ForEach-Object {-10.0}) + @(-33.335,-33.335,-33.335))
        $record=Playback $values
    }
    'NegativeAvCount' {$record.application_av_delta_ms.count=59}
    'NegativeScheduledAccounting' {$record.measurement_video_skipped_frame_count=1}
    'NegativeFirstFrame' {$record.measurement_video_first_frame=1}
    'NegativeAudioStartSample' {$record.measurement_audio_start_sample=1}
    'NegativeUnderflow' {$record.measurement_audio_underflow_count=1}
    'NegativeOverflow' {$record.measurement_audio_overflow_count=1}
    'NegativeMarker' {$record.measurement_marker_mismatch_count=1}
    'NegativeMixedPair' {$record.measurement_mixed_pair_count=1}
    'NegativeGeneration' {$record.measurement_mixed_generation_count=1}
    'NegativeAhead' {$record.measurement_video_ahead_violation_count=1}
    'NegativeClockRegression' {$record.measurement_clock_regression_count=1}
    'NegativeQpcFallback' {$record.measurement_video_qpc_master_fallback_count=1}
    'NegativeSeekCount999' {$record.integrated_seek_requested=999}
    'NegativeSeekMismatch' {$record.seeks[0].first_audio_sample=1}
    'NegativeSeekTimeout' {$record.integrated_seek_timeout_count=1}
    'NegativeSeekP95' {$record=Seek ([double[]](1..20 | ForEach-Object {150.001}))}
    'NegativeSeekMax' {
        $record=Seek ([double[]](@(1..19 | ForEach-Object {100.0}) + @(400.001)))
    }
    'NegativePauseAdvance' {$record.pause_video_advance_zero=$false}
    'NegativePauseClock' {$record.pause_clock_frozen=$false}
    'NegativeMissingProperty' {$record.Remove('audio_master_only')}
    'NegativeNaN' {$record.effective_video_fps='__NAN__'}
    default {throw "未知の case: $Case"}
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$json = Join-Path $OutputDir "$Case.json"
$text = $record | ConvertTo-Json -Depth 12
if ($Case -eq 'NegativeNaN') { $text = $text.Replace('"__NAN__"','NaN') }
Set-Content -LiteralPath $json -Value $text -Encoding utf8
& pwsh -NoProfile -File $Checker -Json $json -Mode $mode -DryRun
$actualPass = $LASTEXITCODE -eq 0
if ($actualPass -ne $expectPass) {
    Write-Error "P3-C checker case が期待と不一致です: $Case (pass=$actualPass)"
}
if ($Case -eq 'Good') {
    # seek latency p95/max と seek AV p95/max の PASS 側境界を同じ positive test で固定する。
    $latency=[double[]](@(1..19 | ForEach-Object {150.000}) + @(400.000))
    $av=[double[]](@(1..19 | ForEach-Object {-20.000}) + @(-33.334))
    $seekBoundary=Seek $latency $av
    $boundaryJson=Join-Path $OutputDir 'GoodSeekBoundary.json'
    $seekBoundary | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $boundaryJson -Encoding utf8
    & pwsh -NoProfile -File $Checker -Json $boundaryJson -Mode seek -DryRun
    if ($LASTEXITCODE -ne 0) { Write-Error 'seek threshold PASS 境界が拒否されました' }
}
