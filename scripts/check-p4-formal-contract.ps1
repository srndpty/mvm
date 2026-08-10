[CmdletBinding()]
param([Parameter(Mandatory)][string]$Json)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Fail([string]$Message) { throw "Phase 4 formal contract: $Message" }
function Prop([object]$Object, [string]$Name) {
    if ($null -eq $Object) { Fail "$Name の親objectがnullです" }
    $p = $Object.PSObject.Properties[$Name]
    if ($null -eq $p -or $null -eq $p.Value) { Fail "必須fieldがmissing/nullです: $Name" }
    $p.Value
}
function Int([object]$Object, [string]$Name) {
    $v = Prop $Object $Name
    if ($v.GetType() -notin @([sbyte],[byte],[int16],[uint16],[int32],[uint32],[int64],[uint64])) {
        Fail "$Name はJSON integerではありません"
    }
    [long]$v
}
function IntElement([object]$Value, [string]$Name) {
    if ($null -eq $Value -or
        $Value.GetType() -notin @([sbyte],[byte],[int16],[uint16],[int32],[uint32],[int64],[uint64])) {
        Fail "$Name はJSON integerではありません"
    }
    [long]$Value
}
function Num([object]$Object, [string]$Name) {
    $v = Prop $Object $Name
    if ($v -isnot [ValueType] -or $v -is [bool]) { Fail "$Name は数値ではありません" }
    $n = [double]$v
    if ([double]::IsNaN($n) -or [double]::IsInfinity($n)) { Fail "$Name は有限数ではありません" }
    $n
}
function Str([object]$Object, [string]$Name) {
    $v = Prop $Object $Name
    if ($v -isnot [string] -or [string]::IsNullOrEmpty($v)) { Fail "$Name は空でないstringではありません" }
    [string]$v
}
function Bool([object]$Object, [string]$Name, [bool]$Expected) {
    $v = Prop $Object $Name
    if ($v -isnot [bool]) { Fail "$Name はJSON booleanではありません" }
    if ($v -ne $Expected) { Fail "$Name が期待値 $Expected ではありません" }
}
function Arr([object]$Object, [string]$Name) {
    $v = Prop $Object $Name
    if ($v -is [string] -or $v -isnot [System.Collections.IEnumerable]) { Fail "$Name はarrayではありません" }
    @($v)
}
function Zero([object]$Object, [string]$Name) { if ((Int $Object $Name) -ne 0) { Fail "$Name が0ではありません" } }
function Close([double]$A, [double]$B, [string]$Name) {
    if ([Math]::Abs($A - $B) -gt 0.000000001) { Fail "$Name がraw再計算値と一致しません" }
}
function Expected-State([long]$Frame) {
    if ($Frame -lt 600) { 'S0' } elseif ($Frame -lt 1200) { 'S1' }
    elseif ($Frame -lt 1800) { 'S2' } elseif ($Frame -lt 2400) { 'S3' }
    elseif ($Frame -lt 3000) { 'S0' } else { 'S1' }
}
function Segment([long]$Frame) { [Math]::Floor($Frame / 600) }

try {
    if (-not (Test-Path -LiteralPath $Json)) { Fail "rawがありません: $Json" }
    $raw = Get-Content -Raw -LiteralPath $Json | ConvertFrom-Json
    $canonical = '0:S0;600:S1;1200:S2;1800:S3;2400:S0;3000:S1'
    $hash = '5b66543f43f98ad261a5a96e961332ef4a3d5b21f8f30b1713b4ff420a855f79'
    $states = @('S0','S1','S2','S3','S0','S1')
    $boundaries = @(600L,1200L,1800L,2400L,3000L)
    if ((Str $raw 'schema') -ne 'mvm-p4-formal-1' -or (Int $raw 'schema_version') -ne 1 -or
        (Str $raw 'contract_version') -ne 'P4-formal-frozen' -or (Str $raw 'phase') -ne 'P4-D' -or
        (Str $raw 'schedule_kind') -ne 'formal' -or (Str $raw 'formal_verdict') -ne 'NOT_RUN') {
        Fail 'schema/contract/phase/schedule_kind/verdictが不正です'
    }
    if ((Int $raw 'process_exit_code') -ne 0) { Fail 'producer process exitが0ではありません' }
    if ((Str $raw 'canonical_schedule') -ne $canonical) { Fail 'canonical scheduleが違います' }
    $computed = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData(
        [Text.Encoding]::UTF8.GetBytes($canonical))).ToLowerInvariant()
    if ($computed -ne $hash -or (Str $raw 'canonical_schedule_sha256') -ne $hash) { Fail 'schedule hashが違います' }
    $schedule = Arr $raw 'schedule'
    if ($schedule.Count -ne 6) { Fail 'parsed scheduleが6要素ではありません' }
    for ($i=0; $i -lt 6; $i++) {
        if ((Int $schedule[$i] 'boundary') -ne 600L*$i -or (Str $schedule[$i] 'state') -ne $states[$i]) {
            Fail "parsed schedule[$i]がcanonicalと違います"
        }
    }
    if ((Str $raw 'fixture_a_sha256') -ne 'd398114c38806f39670df51dfabb0095d462cbc35286ea1467901d4007cf0308' -or
        (Str $raw 'fixture_b_sha256') -ne 'fe7cd1a45d101d363cb3930497601efd41dd55fd36c194acf8f24e3e4728b479') { Fail 'fixture hashが違います' }
    if ((Int $raw 'warmup_seconds') -ne 5 -or (Int $raw 'measurement_seconds') -ne 60 -or
        (Int $raw 'required_video_frames') -ne 3600 -or (Int $raw 'measurement_audio_start_sample') -ne 0 -or
        (Int $raw 'measurement_audio_end_sample') -ne 2880000) { Fail '固定workload値が違います' }
    if ((Str $raw 'cpu_reference_pixel_status') -ne 'PRECOMPUTED' -or
        (Int $raw 'cpu_reference_candidate_frame_count') -ne 15 -or
        (Int $raw 'cpu_reference_candidate_probe_count') -ne 30) { Fail 'CPU reference候補契約が違います' }
    Bool $raw 'audio_master_only' $true

    $e0 = Int $raw 'measurement_baseline_composition_epoch'
    $genA=Int $raw 'baseline_source_generation_a'; $genB=Int $raw 'baseline_source_generation_b'
    $resA=Int $raw 'baseline_resource_epoch_a'; $resB=Int $raw 'baseline_resource_epoch_b'
    $ledger = Arr $raw 'measurement_display_ledger'
    if ($ledger.Count -eq 0 -or (Int $raw 'measurement_display_ledger_count') -ne $ledger.Count) { Fail 'ledgerが空または件数不整合です' }
    $last=-1L; $unique=0L; $skipped=0L
    $signedDeltas = [Collections.Generic.List[double]]::new()
    $deltas = [Collections.Generic.List[double]]::new()
    foreach ($record in $ledger) {
        $frame=Int $record 'output_frame'
        if ($frame -lt 0 -or $frame -ge 3600 -or $frame -le $last) { Fail "ledger frame範囲/単調性違反: $frame" }
        if ((Str $record 'composition_state') -ne (Expected-State $frame) -or
            (Int $record 'composition_epoch') -ne ($e0 + (Segment $frame))) { Fail "frame $frame のstate/epochが違います" }
        $sources=Arr $record 'sources'
        if ($sources.Count -ne 2 -or (Int $sources[0] 'source_id') -ne 1 -or (Int $sources[1] 'source_id') -ne 2 -or
            (Int $sources[0] 'frame') -ne $frame -or (Int $sources[1] 'frame') -ne $frame -or
            (Int $sources[0] 'source_generation') -ne $genA -or (Int $sources[1] 'source_generation') -ne $genB -or
            (Int $sources[0] 'resource_epoch') -ne $resA -or (Int $sources[1] 'resource_epoch') -ne $resB) {
            Fail "frame $frame のA/B identityが違います"
        }
        Bool $record 'application_av_projection_valid' $true
        $signedDelta=Num $record 'application_av_delta_ms'
        $signedDeltas.Add($signedDelta); $deltas.Add([Math]::Abs($signedDelta))
        if ($last -ge 0) { $skipped += $frame-$last-1 }; $unique++; $last=$frame
    }
    if ((Int $ledger[0] 'output_frame') -ne 0) { Fail 'first frameが0ではありません' }
    if ($last -lt 3599) { $skipped += 3599-$last }
    if ($unique+$skipped -ne 3600 -or $deltas.Count -ne $unique) { Fail 'display/A-V sample会計が違います' }
    if ((Int $raw 'measurement_video_displayed_unique_count') -ne $unique -or
        (Int $raw 'measurement_video_skipped_frame_count') -ne $skipped -or
        (Int $raw 'measurement_non_increasing_display_count') -ne 0) { Fail 'producer display summaryが再計算値と違います' }
    $fps=$unique/60.0; $drop=$skipped/3600.0
    Close (Num $raw 'effective_video_fps') $fps 'effective_video_fps'; Close (Num $raw 'drop_rate') $drop 'drop_rate'
    if ($fps -lt 55.0) { Fail 'recomputed effective fpsが55未満です' }
    if ($drop -gt 0.02) { Fail 'recomputed drop rateが2%を超えています' }
    $sorted=@($deltas | Sort-Object); $signedSorted=@($signedDeltas | Sort-Object)
    $rank=[Math]::Ceiling($sorted.Count*0.95)-1
    $p95=[double]$sorted[$rank]; $max=[double]$sorted[-1]
    $abs=Prop $raw 'application_av_delta_abs_ms'
    if ((Int $abs 'count') -ne $unique) { Fail 'A/V summary countが違います' }
    Close (Num $abs 'p95') $p95 'A/V p95'; Close (Num $abs 'max') $max 'A/V max'
    Close (Num $abs 'p50') ([double]$sorted[[Math]::Ceiling($sorted.Count*0.50)-1]) 'A/V abs p50'
    Close (Num $abs 'p99') ([double]$sorted[[Math]::Ceiling($sorted.Count*0.99)-1]) 'A/V abs p99'
    Close (Num $abs 'min') ([double]$sorted[0]) 'A/V abs min'
    $signed=Prop $raw 'application_av_delta_ms'
    if((Int $signed 'count')-ne$unique){Fail 'signed A/V summary countが違います'}
    Close (Num $signed 'p50') ([double]$signedSorted[[Math]::Ceiling($signedSorted.Count*0.50)-1]) 'A/V signed p50'
    Close (Num $signed 'p95') ([double]$signedSorted[$rank]) 'A/V signed p95'
    Close (Num $signed 'min') ([double]$signedSorted[0]) 'A/V signed min'
    Close (Num $signed 'max') ([double]$signedSorted[-1]) 'A/V signed max'
    if ($p95 -gt 20.000 -or $max -gt 33.334) { Fail 'A/V threshold違反です' }
    Zero $raw 'application_av_projection_failure_count'

    if ((Int $raw 'composition_state_adoption_count') -ne 5 -or
        (Int $raw 'composition_epoch_increment_count') -ne 5) { Fail 'adoption/epoch incrementが5ではありません' }
    foreach ($n in @('composition_state_reject_count','composition_state_unresolved_count','old_state_after_boundary_count')) { Zero $raw $n }
    $resolve=Int $raw 'composition_state_resolve_count'; $noop=Int $raw 'composition_state_noop_count'
    if ($resolve -ne 5+$noop) { Fail 'resolve != adoption + noop + rejectです' }
    $lags=Arr $raw 'transition_activation_lag_frames'; $transitions=Arr $raw 'transition_boundaries'
    if ($lags.Count -ne 5 -or $transitions.Count -ne 5) { Fail 'transition/lagが5件ではありません' }
    $firstAfter=@{}
    for ($i=0;$i -lt 5;$i++) {
        $boundary=$boundaries[$i]; $record=@($ledger | Where-Object { (Int $_ 'output_frame') -ge $boundary } | Select-Object -First 1)
        if ($record.Count -ne 1) { Fail "boundary $boundary 後のdisplayがありません" }
        $frame=Int $record[0] 'output_frame'; $lag=$frame-$boundary
        $rawLag=IntElement $lags[$i] "transition_activation_lag_frames[$i]"
        if ($lag -lt 0 -or $lag -gt 2 -or $rawLag -ne $lag) { Fail "boundary $boundary のlagが不正です" }
        $firstAfter[$boundary]=$frame; $t=$transitions[$i]
        if ((Int $t 'boundary') -ne $boundary -or (Str $t 'expected_state') -ne (Expected-State $boundary) -or
            (Int $t 'first_displayed_output_frame') -ne $frame -or
            (Int $t 'activation_lag_frames') -ne $lag -or (Str $t 'first_display_state') -ne (Expected-State $frame) -or
            (Int $t 'first_display_composition_epoch') -ne ($e0+(Segment $frame))) { Fail "boundary $boundary のtransition rawが違います" }
        $s=Arr $t 'first_display_sources'
        if ($s.Count-ne 2 -or (Int $s[0] 'source_id')-ne 1 -or (Int $s[1] 'source_id')-ne 2 -or
            (Int $s[0] 'frame')-ne $frame -or (Int $s[1] 'frame')-ne $frame -or
            (Int $s[0] 'source_generation')-ne $genA -or (Int $s[1] 'source_generation')-ne $genB -or
            (Int $s[0] 'resource_epoch')-ne $resA -or (Int $s[1] 'resource_epoch')-ne $resB) { Fail "boundary $boundary のidentityが違います" }
    }

    if((Str $raw 'transition_pixel_probe_status')-ne'COMPLETE'){Fail 'probe statusがCOMPLETEではありません'}
    $probes=Arr $raw 'transition_probe_records'
    if ($probes.Count-ne 10 -or (Int $raw 'transition_probe_checked_count')-ne 10) { Fail 'probeがexactly 10ではありません' }
    $seen=@{}
    foreach ($p in $probes) {
        $boundary=Int $p 'boundary'; $name=Str $p 'probe'; $key="$boundary/$name"
        if ($boundary -notin $boundaries -or $name -notin @('TL','BR') -or $seen.ContainsKey($key)) { Fail "probe keyが不正です: $key" }
        $seen[$key]=$true; $frame=Int $p 'actual_output_frame'
        if ($frame-ne $firstAfter[$boundary] -or (Str $p 'composition_state')-ne (Expected-State $frame) -or
            (Str $p 'cpu_reference_state')-ne (Expected-State $frame) -or
            (Int $p 'composition_epoch')-ne ($e0+(Segment $frame))) { Fail "$key のframe/state/epochが違います" }
        $x=if($name-eq'TL'){480}else{1440};$y=if($name-eq'TL'){270}else{810}
        if ((Int $p 'x')-ne$x -or (Int $p 'y')-ne$y -or (Int $p 'gpu_ticket')-le 0 -or
            (Int $p 'gpu_completion_serial')-le 0 -or (Int $p 'blocking_wait_count')-ne 0) { Fail "$key の座標/ticket/serial/waitが不正です" }
        Bool $p 'completion_observed' $true
        $actual=Arr $p 'actual_rgba';$expected=Arr $p 'cpu_expected_rgba'
        if($actual.Count-ne4 -or $expected.Count-ne4){Fail "$key のRGBA件数が違います"}
        $actualChannels=@();$expectedChannels=@()
        for($c=0;$c-lt4;$c++){
            $actualChannel=IntElement $actual[$c] "$key actual_rgba[$c]"
            $expectedChannel=IntElement $expected[$c] "$key cpu_expected_rgba[$c]"
            if($actualChannel-lt0 -or $actualChannel-gt255 -or $expectedChannel-lt0 -or $expectedChannel-gt255){Fail "$key のRGBAが0..255外です"}
            $actualChannels+=$actualChannel;$expectedChannels+=$expectedChannel
        }
        for($c=0;$c-lt3;$c++){if([Math]::Abs($actualChannels[$c]-$expectedChannels[$c])-gt3){Fail "$key のRGBが±3外です"}}
        if($actualChannels[3]-ne255 -or $expectedChannels[3]-ne255){Fail "$key のalphaが255ではありません"}
        $s=Arr $p 'sources'; if($s.Count-ne2 -or (Int $s[0] 'source_id')-ne1 -or (Int $s[1] 'source_id')-ne2 -or
            (Int $s[0] 'frame')-ne$frame -or (Int $s[1] 'frame')-ne$frame -or
            (Int $s[0] 'source_generation')-ne$genA -or (Int $s[1] 'source_generation')-ne$genB -or
            (Int $s[0] 'resource_epoch')-ne$resA -or (Int $s[1] 'resource_epoch')-ne$resB){Fail "$key のsource identityが違います"}
    }
    foreach($b in $boundaries){foreach($n in @('TL','BR')){if(-not$seen.ContainsKey("$b/$n")){Fail "probeがmissingです: $b/$n"}}}
    foreach($n in @('transition_probe_mismatch_count','transition_probe_render_thread_blocking_wait_count',
        'transition_probe_untracked_submission_count','transition_probe_completion_failure_count','transition_probe_retirement_timeout_count',
        'transition_probe_pending_after_drain_count','transition_probe_issue_failure_count','composition_state_display_mismatch_count',
        'composition_pair_identity_violation_count','composition_layer_generation_mismatch_count','source_generation_change_due_to_layout_count',
        'phase4_adoption_failure_count','measurement_audio_underflow_count','measurement_audio_overflow_count','measurement_marker_mismatch_count',
        'measurement_mixed_pair_count','measurement_mixed_generation_count','measurement_stale_composition_epoch_count',
        'measurement_video_ahead_violation_count','measurement_clock_regression_count','measurement_video_qpc_master_fallback_count',
        'measurement_audio_clock_query_failure_count','measurement_scheduler_deadline_drop_count','measurement_render_failure_count',
        'cpu_full_frame_readback_count','full_frame_gpu_copy_count','software_video_fallback_count',
        'untracked_submission_count','completion_poll_failure_count','retirement_depth_after_drain',
        'payloads_released_before_completion','retirement_timeout_count','partial_gpu_issue_failure_count',
        'device_lost_count','lifecycle_violation_count','audio_render_thread_join_leak','audio_decode_thread_join_leak')){Zero $raw $n}
    foreach($n in @('video_worker_a_joined','video_worker_b_joined','teardown_success','final_report_after_teardown',
        'shutdown_workers_joined_before_teardown','shutdown_render_teardown_requested','display_target_preflight_pass')){Bool $raw $n $true}
    $order=@('DisableSchedulers','StopAudioSink','StopAudioDecodeWorker','StopVideoWorkerA','StopVideoWorkerB','DetachSharedWorkerRefs','RequestRenderTeardown')
    $actualOrder=Arr $raw 'shutdown_sequence'; if(($actualOrder -join '|')-ne($order -join '|')){Fail 'shutdown順が違います'}
    Zero $raw 'shutdown_order_violation_count'
    if((Int $raw 'requested_output_width')-ne1920 -or (Int $raw 'requested_output_height')-ne1080){Fail 'requested display sizeが1920x1080ではありません'}
    $start=Prop $raw 'display_environment_start';$end=Prop $raw 'display_environment_end'
    foreach($env in @($start,$end)){if((Int $env 'window_logical_width')-ne1920 -or (Int $env 'window_logical_height')-ne1080 -or
        (Int $env 'compositor_surface_logical_width')-ne1920 -or (Int $env 'compositor_surface_logical_height')-ne1080 -or
        (Int $env 'rhi_target_pixel_width')-ne1920 -or (Int $env 'rhi_target_pixel_height')-ne1080 -or (Num $env 'device_pixel_ratio')-ne1.0){Fail 'display exact preflight違反です'}}
    foreach($n in @('screen_name','screen_orientation','screen_geometry_width','screen_geometry_height','available_geometry_width','available_geometry_height',
        'device_pixel_ratio','window_logical_width','window_logical_height','compositor_surface_logical_width','compositor_surface_logical_height','rhi_target_pixel_width','rhi_target_pixel_height')){
        if((Prop $start $n)-ne(Prop $end $n)){Fail "display environmentが変化しました: $n"}}
    [void](Str $raw 'adapter')
    if((Int $raw 'audio_endpoint_sample_rate')-ne48000 -or (Int $raw 'audio_endpoint_channels')-ne2 -or
        (Str $raw 'audio_endpoint_sample_format')-ne'flt'){Fail 'audio endpointがP3-C-2 exact 48kHz/stereo/fltではありません'}
    Write-Host ("[p4-formal] CHECKER PASS fps={0:N3} drop={1:P3} av_p95={2:N3}ms av_max={3:N3}ms probes=10" -f $fps,$drop,$p95,$max)
    exit 0
} catch { Write-Host "[p4-formal] FAIL $($_.Exception.Message)"; exit 3 }
