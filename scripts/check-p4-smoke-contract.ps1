[CmdletBinding()]
param([Parameter(Mandatory)][string]$Json)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Contract-Fail([string]$Message) { throw "Phase 4 smoke contract: $Message" }
function Property([object]$Object, [string]$Name) {
    if ($null -eq $Object) { Contract-Fail "$Name の親objectがnullです" }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or $null -eq $property.Value) {
        Contract-Fail "必須fieldがmissing/nullです: $Name"
    }
    return $property.Value
}
function Integer([object]$Object, [string]$Name) {
    $value = Property $Object $Name
    $integerTypes = @([sbyte],[byte],[int16],[uint16],[int32],[uint32],[int64],[uint64])
    if ($value.GetType() -notin $integerTypes) { Contract-Fail "$Name はJSON integerではありません" }
    return [long]$value
}
function Number([object]$Object, [string]$Name) {
    $value = Property $Object $Name
    if ($value -isnot [ValueType] -or $value -is [bool]) { Contract-Fail "$Name は数値ではありません" }
    $number = [double]$value
    if ([double]::IsNaN($number) -or [double]::IsInfinity($number)) {
        Contract-Fail "$Name は有限数ではありません"
    }
    return $number
}
function Text([object]$Object, [string]$Name) {
    $value = Property $Object $Name
    if ($value -isnot [string] -or [string]::IsNullOrEmpty($value)) {
        Contract-Fail "$Name は空でないJSON stringではありません"
    }
    return [string]$value
}
function Boolean([object]$Object, [string]$Name, [bool]$Expected) {
    $value = Property $Object $Name
    if ($value -isnot [bool]) { Contract-Fail "$Name はJSON booleanではありません" }
    if ($value -ne $Expected) { Contract-Fail "$Name が期待値 $Expected ではありません" }
    return [bool]$value
}
function Array([object]$Object, [string]$Name) {
    $value = Property $Object $Name
    if ($value -is [string] -or $value -isnot [System.Collections.IEnumerable]) {
        Contract-Fail "$Name はJSON arrayではありません"
    }
    return @($value)
}
function Zero([object]$Object, [string]$Name) {
    if ((Integer $Object $Name) -ne 0) { Contract-Fail "$Name が0ではありません" }
}
function Equal-Array([object[]]$Actual, [object[]]$Expected, [string]$Name) {
    if ($Actual.Count -ne $Expected.Count) { Contract-Fail "$Name の件数が違います" }
    for ($i = 0; $i -lt $Expected.Count; $i++) {
        if ($Actual[$i] -ne $Expected[$i]) { Contract-Fail "$Name の要素$iが違います" }
    }
}

try {
    if (-not (Test-Path -LiteralPath $Json)) { Contract-Fail "rawがありません: $Json" }
    $raw = Get-Content -Raw -LiteralPath $Json | ConvertFrom-Json
    $canonical = '0:S0;200:S1;400:S2'
    $scheduleHash = '418ae09f4bb9349aa7ac53ca38028782aef074ca1696338758ccfa6b4e4398e8'
    $fixtureA = 'd398114c38806f39670df51dfabb0095d462cbc35286ea1467901d4007cf0308'
    $fixtureB = 'fe7cd1a45d101d363cb3930497601efd41dd55fd36c194acf8f24e3e4728b479'
    $segments = @(
        [pscustomobject]@{ boundary = 0; state = 'S0' },
        [pscustomobject]@{ boundary = 200; state = 'S1' },
        [pscustomobject]@{ boundary = 400; state = 'S2' })
    $boundaries = @(200L, 400L)

    if ((Text $raw 'schema') -ne 'mvm-p4-smoke-1') { Contract-Fail 'schemaが違います' }
    if ((Integer $raw 'schema_version') -ne 1) { Contract-Fail 'schema_versionが1ではありません' }
    if ((Text $raw 'contract_version') -ne 'P4-C-smoke-frozen') { Contract-Fail 'contract_versionが違います' }
    if ((Text $raw 'phase') -ne 'P4-C') { Contract-Fail 'phaseがP4-Cではありません' }
    if ((Text $raw 'schedule_kind') -ne 'smoke') { Contract-Fail 'schedule_kindがsmokeではありません' }
    if ((Text $raw 'formal_verdict') -ne 'NOT_RUN') { Contract-Fail 'formal_verdictがNOT_RUNではありません' }
    if ((Text $raw 'smoke_contract_verdict') -ne 'NOT_RUN') { Contract-Fail 'producerがsmoke verdictを出しています' }
    if ((Text $raw 'canonical_schedule') -ne $canonical) { Contract-Fail 'canonical scheduleが違います' }
    $computedHash = [Convert]::ToHexString(
        [Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($canonical))).ToLowerInvariant()
    if ($computedHash -ne $scheduleHash -or (Text $raw 'canonical_schedule_sha256') -ne $scheduleHash) {
        Contract-Fail 'schedule SHA-256が違います'
    }
    $rawSchedule = Array $raw 'schedule'
    if ($rawSchedule.Count -ne 3) { Contract-Fail 'smoke scheduleの要素数が3ではありません' }
    for ($i = 0; $i -lt 3; $i++) {
        if ((Integer $rawSchedule[$i] 'boundary') -ne $segments[$i].boundary -or
            (Text $rawSchedule[$i] 'state') -ne $segments[$i].state) {
            Contract-Fail "parsed schedule[$i]がcanonicalと一致しません"
        }
    }
    if ((Text $raw 'fixture_a_sha256') -ne $fixtureA -or
        (Text $raw 'fixture_b_sha256') -ne $fixtureB) { Contract-Fail 'fixture hashが違います' }
    if ((Text $raw 'cpu_reference_pixel_status') -ne 'PRECOMPUTED' -or
        (Integer $raw 'cpu_reference_candidate_frame_count') -ne 6 -or
        (Integer $raw 'cpu_reference_candidate_probe_count') -ne 12) {
        Contract-Fail 'CPU reference候補の事前計算契約が違います'
    }

    if ((Integer $raw 'measurement_seconds') -ne 10 -or
        (Integer $raw 'required_video_frames') -ne 600 -or
        (Integer $raw 'measurement_audio_start_sample') -ne 0 -or
        (Integer $raw 'measurement_audio_end_sample') -ne 480000) {
        Contract-Fail '10秒smoke measurement契約が違います'
    }
    [void](Boolean $raw 'audio_master_only' $true)
    $e0 = Integer $raw 'measurement_baseline_composition_epoch'
    if ((Integer $raw 'composition_state_adoption_count') -ne 2 -or
        (Integer $raw 'composition_epoch_increment_count') -ne 2) {
        Contract-Fail 'adoption/epoch incrementが2ではありません'
    }
    Zero $raw 'composition_state_reject_count'
    Zero $raw 'composition_state_unresolved_count'
    Zero $raw 'source_generation_change_due_to_layout_count'
    Zero $raw 'phase4_adoption_failure_count'
    $resolve = Integer $raw 'composition_state_resolve_count'
    $noop = Integer $raw 'composition_state_noop_count'
    if ($resolve -ne 2 + $noop) { Contract-Fail 'resolve != adoption + noop + rejectです' }

    function Expected-State([long]$Frame) {
        if ($Frame -lt 200) { return 'S0' }
        if ($Frame -lt 400) { return 'S1' }
        return 'S2'
    }
    function Expected-Epoch([long]$Frame) {
        if ($Frame -lt 200) { return $e0 }
        if ($Frame -lt 400) { return $e0 + 1 }
        return $e0 + 2
    }
    $baselineGenerationA = Integer $raw 'baseline_source_generation_a'
    $baselineGenerationB = Integer $raw 'baseline_source_generation_b'
    $baselineEpochA = Integer $raw 'baseline_resource_epoch_a'
    $baselineEpochB = Integer $raw 'baseline_resource_epoch_b'
    $ledger = Array $raw 'measurement_display_ledger'
    if ($ledger.Count -eq 0 -or (Integer $raw 'measurement_display_ledger_count') -ne $ledger.Count) {
        Contract-Fail 'display ledgerが空または件数不整合です'
    }
    $last = -1L; $unique = 0L; $skipped = 0L; $nonIncreasing = 0L
    $rawAvAbs = [Collections.Generic.List[double]]::new()
    foreach ($record in $ledger) {
        $frame = Integer $record 'output_frame'
        if ($frame -lt 0 -or $frame -ge 600) { Contract-Fail "ledger frameが[0,600)外です: $frame" }
        if ((Text $record 'composition_state') -ne (Expected-State $frame) -or
            (Integer $record 'composition_epoch') -ne (Expected-Epoch $frame)) {
            Contract-Fail "ledger state/epochが違います: frame $frame"
        }
        [void](Boolean $record 'application_av_projection_valid' $true)
        $rawAvAbs.Add([Math]::Abs((Number $record 'application_av_delta_ms')))
        $sources = Array $record 'sources'
        if ($sources.Count -ne 2) { Contract-Fail "frame $frame のsource数が2ではありません" }
        $a = $sources[0]; $b = $sources[1]
        if ((Integer $a 'source_id') -ne 1 -or (Integer $b 'source_id') -ne 2 -or
            (Integer $a 'frame') -ne $frame -or (Integer $b 'frame') -ne $frame -or
            (Integer $a 'source_generation') -ne $baselineGenerationA -or
            (Integer $b 'source_generation') -ne $baselineGenerationB -or
            (Integer $a 'resource_epoch') -ne $baselineEpochA -or
            (Integer $b 'resource_epoch') -ne $baselineEpochB) {
            Contract-Fail "frame $frame のA/B identityが違います"
        }
        if ($last -ge 0) {
            if ($frame -le $last) { $nonIncreasing++ } else { $skipped += $frame - $last - 1 }
        }
        if ($frame -ne $last) { $unique++ }
        $last = $frame
    }
    if ((Integer $ledger[0] 'output_frame') -ne 0 -or
        (Text $ledger[0] 'composition_state') -ne 'S0' -or
        (Integer $ledger[0] 'composition_epoch') -ne $e0) {
        Contract-Fail 'first displayがframe0/S0/E0ではありません'
    }
    if ($last -lt 599) { $skipped += 599 - $last }
    if ($nonIncreasing -ne 0 -or $unique + $skipped -ne 600) {
        Contract-Fail 'displayのunique/skipped/non-increasing会計が違います'
    }
    if ((Integer $raw 'measurement_video_displayed_unique_count') -ne $unique -or
        (Integer $raw 'measurement_video_skipped_frame_count') -ne $skipped -or
        (Integer $raw 'measurement_non_increasing_display_count') -ne $nonIncreasing) {
        Contract-Fail 'producer display countがledger再計算値と違います'
    }
    $recomputedFps = $unique / 10.0
    $recomputedDrop = $skipped / 600.0
    if ([Math]::Abs((Number $raw 'effective_video_fps') - $recomputedFps) -gt 0.000000001 -or
        [Math]::Abs((Number $raw 'drop_rate') - $recomputedDrop) -gt 0.000000001) {
        Contract-Fail 'producer fps/dropがledger再計算値と違います'
    }
    if ($rawAvAbs.Count -ne $unique) { Contract-Fail 'raw A/V sample件数がdisplay件数と違います' }
    $sortedAv = @($rawAvAbs | Sort-Object)
    $rawAvP95 = [double]$sortedAv[[Math]::Ceiling($sortedAv.Count * 0.95) - 1]
    $rawAvMax = [double]$sortedAv[-1]

    $lags = Array $raw 'transition_activation_lag_frames'
    if ($lags.Count -ne 2) { Contract-Fail 'activation lag rawが2件ではありません' }
    $firstAfter = @{}
    for ($i = 0; $i -lt 2; $i++) {
        $boundary = $boundaries[$i]
        $first = @($ledger | Where-Object { (Integer $_ 'output_frame') -ge $boundary } |
            Sort-Object { Integer $_ 'output_frame' } | Select-Object -First 1)
        if ($first.Count -ne 1) { Contract-Fail "boundary $boundary 後のdisplayがありません" }
        $frame = Integer $first[0] 'output_frame'
        $lag = $frame - $boundary
        if ($lag -lt 0 -or $lag -gt 2) { Contract-Fail "boundary $boundary のlagが0..2外です" }
        if ($lags[$i].GetType() -notin @([int32],[int64]) -or [long]$lags[$i] -ne $lag) {
            Contract-Fail "boundary $boundary のlag rawが再計算値と違います"
        }
        $firstAfter[$boundary] = $frame
    }
    $transitionRaw = Array $raw 'transition_boundaries'
    if ($transitionRaw.Count -ne 2) { Contract-Fail 'transition boundary rawが2件ではありません' }
    for ($i = 0; $i -lt 2; $i++) {
        $transition = $transitionRaw[$i]; $boundary = $boundaries[$i]
        $frame = $firstAfter[$boundary]
        if ((Integer $transition 'boundary') -ne $boundary -or
            (Integer $transition 'first_displayed_output_frame') -ne $frame -or
            (Integer $transition 'activation_lag_frames') -ne ($frame - $boundary) -or
            (Text $transition 'first_display_state') -ne (Expected-State $frame) -or
            (Integer $transition 'first_display_composition_epoch') -ne (Expected-Epoch $frame)) {
            Contract-Fail "boundary $boundary のfirst display rawがledger再計算値と違います"
        }
        $sources = Array $transition 'first_display_sources'
        if ($sources.Count -ne 2 -or (Integer $sources[0] 'source_id') -ne 1 -or
            (Integer $sources[1] 'source_id') -ne 2 -or
            (Integer $sources[0] 'frame') -ne $frame -or (Integer $sources[1] 'frame') -ne $frame -or
            (Integer $sources[0] 'source_generation') -ne $baselineGenerationA -or
            (Integer $sources[1] 'source_generation') -ne $baselineGenerationB -or
            (Integer $sources[0] 'resource_epoch') -ne $baselineEpochA -or
            (Integer $sources[1] 'resource_epoch') -ne $baselineEpochB) {
            Contract-Fail "boundary $boundary のfirst display identityが違います"
        }
    }
    Zero $raw 'old_state_after_boundary_count'

    if ((Text $raw 'transition_pixel_probe_status') -ne 'COMPLETE') { Contract-Fail 'probe statusがCOMPLETEではありません' }
    $probes = Array $raw 'transition_probe_records'
    if ($probes.Count -ne 4 -or (Integer $raw 'transition_probe_checked_count') -ne 4) {
        Contract-Fail 'probe record/checked countが4ではありません'
    }
    $seen = @{}
    foreach ($probe in $probes) {
        $boundary = Integer $probe 'boundary'
        if ($boundary -notin $boundaries) { Contract-Fail "probe boundaryが違います: $boundary" }
        $name = Text $probe 'probe'
        if ($name -notin @('TL','BR')) { Contract-Fail "probe名が違います: $name" }
        $key = "$boundary/$name"
        if ($seen.ContainsKey($key)) { Contract-Fail "probeが重複しています: $key" }
        $seen[$key] = $true
        $expectedX = if ($name -eq 'TL') { 480 } else { 1440 }
        $expectedY = if ($name -eq 'TL') { 270 } else { 810 }
        if ((Integer $probe 'x') -ne $expectedX -or (Integer $probe 'y') -ne $expectedY) {
            Contract-Fail "$key の座標が違います"
        }
        $frame = Integer $probe 'actual_output_frame'
        if ($frame -ne $firstAfter[$boundary] -or
            (Text $probe 'composition_state') -ne (Expected-State $frame) -or
            (Integer $probe 'composition_epoch') -ne (Expected-Epoch $frame)) {
            Contract-Fail "$key のactual frame/state/epochがledger再計算値と違います"
        }
        $referenceState = Text $probe 'cpu_reference_state'
        if ($referenceState -ne (Expected-State $boundary) -or
            $referenceState -ne (Text $probe 'composition_state')) {
            Contract-Fail "$key のCPU reference state identityがactual/canonical stateと違います"
        }
        if ((Integer $probe 'gpu_ticket') -le 0 -or (Integer $probe 'gpu_completion_serial') -le 0) {
            Contract-Fail "$key のGPU ticket/serialが不正です"
        }
        [void](Boolean $probe 'completion_observed' $true)
        if ((Integer $probe 'blocking_wait_count') -ne 0) { Contract-Fail "$key でblocking waitが発生しました" }
        $actual = Array $probe 'actual_rgba'; $expected = Array $probe 'cpu_expected_rgba'
        if ($actual.Count -ne 4 -or $expected.Count -ne 4) { Contract-Fail "$key のRGBAが4要素ではありません" }
        for ($channel = 0; $channel -lt 4; $channel++) {
            if ($actual[$channel].GetType() -notin @([int32],[int64]) -or
                $expected[$channel].GetType() -notin @([int32],[int64])) {
                Contract-Fail "$key のRGBA channelがintegerではありません"
            }
        }
        for ($channel = 0; $channel -lt 3; $channel++) {
            if ([Math]::Abs([int]$actual[$channel] - [int]$expected[$channel]) -gt 3) {
                Contract-Fail "$key のRGBが±3を超えました"
            }
        }
        if ([int]$actual[3] -ne 255 -or [int]$expected[3] -ne 255) {
            Contract-Fail "$key のalphaが255ではありません"
        }
        $sources = Array $probe 'sources'
        if ($sources.Count -ne 2 -or (Integer $sources[0] 'source_id') -ne 1 -or
            (Integer $sources[1] 'source_id') -ne 2 -or
            (Integer $sources[0] 'frame') -ne $frame -or (Integer $sources[1] 'frame') -ne $frame -or
            (Integer $sources[0] 'source_generation') -ne $baselineGenerationA -or
            (Integer $sources[1] 'source_generation') -ne $baselineGenerationB -or
            (Integer $sources[0] 'resource_epoch') -ne $baselineEpochA -or
            (Integer $sources[1] 'resource_epoch') -ne $baselineEpochB) {
            Contract-Fail "$key のprobe layer identityが違います"
        }
    }
    foreach ($boundary in $boundaries) { foreach ($name in @('TL','BR')) {
        if (-not $seen.ContainsKey("$boundary/$name")) { Contract-Fail "probeが欠落しています: $boundary/$name" }
    }}
    foreach ($name in @('transition_probe_mismatch_count',
            'transition_probe_render_thread_blocking_wait_count',
            'transition_probe_pending_after_drain_count',
            'transition_probe_completion_failure_count',
            'transition_probe_untracked_submission_count',
            'transition_probe_retirement_timeout_count',
            'transition_probe_issue_failure_count')) { Zero $raw $name }

    foreach ($name in @('composition_state_display_mismatch_count','composition_pair_identity_violation_count',
            'composition_layer_generation_mismatch_count','measurement_audio_underflow_count',
            'measurement_audio_overflow_count','measurement_marker_mismatch_count',
            'measurement_mixed_pair_count','measurement_mixed_generation_count',
            'measurement_stale_composition_epoch_count','measurement_video_ahead_violation_count',
            'measurement_clock_regression_count','measurement_video_qpc_master_fallback_count',
            'measurement_audio_clock_query_failure_count','measurement_scheduler_deadline_drop_count',
            'measurement_render_failure_count','cpu_full_frame_readback_count',
            'full_frame_gpu_copy_count','software_video_fallback_count','untracked_submission_count',
            'completion_poll_failure_count','retirement_depth_after_drain',
            'payloads_released_before_completion','retirement_timeout_count',
            'partial_gpu_issue_failure_count','device_lost_count',
            'lifecycle_violation_count','audio_render_thread_join_leak','audio_decode_thread_join_leak')) {
        Zero $raw $name
    }
    foreach ($name in @('video_worker_a_joined','video_worker_b_joined','teardown_success',
            'final_report_after_teardown','shutdown_workers_joined_before_teardown',
            'shutdown_render_teardown_requested','display_target_preflight_pass')) {
        [void](Boolean $raw $name $true)
    }
    Zero $raw 'shutdown_order_violation_count'
    $shutdown = Array $raw 'shutdown_sequence'
    Equal-Array $shutdown @('DisableSchedulers','StopAudioSink','StopAudioDecodeWorker',
        'StopVideoWorkerA','StopVideoWorkerB','DetachSharedWorkerRefs','RequestRenderTeardown') 'shutdown_sequence'

    $start = Property $raw 'display_environment_start'; $end = Property $raw 'display_environment_end'
    if ((Integer $raw 'requested_output_width') -ne 1920 -or
        (Integer $raw 'requested_output_height') -ne 1080) {
        Contract-Fail 'requested outputが1920x1080ではありません'
    }
    foreach ($environment in @($start,$end)) {
        if ((Integer $environment 'window_logical_width') -ne 1920 -or
            (Integer $environment 'window_logical_height') -ne 1080 -or
            (Integer $environment 'compositor_surface_logical_width') -ne 1920 -or
            (Integer $environment 'compositor_surface_logical_height') -ne 1080 -or
            (Integer $environment 'rhi_target_pixel_width') -ne 1920 -or
            (Integer $environment 'rhi_target_pixel_height') -ne 1080 -or
            (Number $environment 'device_pixel_ratio') -ne 1.0) {
            Contract-Fail 'display targetがP3-C-2 preflight exactではありません'
        }
    }
    foreach ($name in @('screen_name','screen_orientation','screen_geometry_width','screen_geometry_height',
            'available_geometry_width','available_geometry_height','device_pixel_ratio','window_logical_width',
            'window_logical_height','compositor_surface_logical_width','compositor_surface_logical_height',
            'rhi_target_pixel_width','rhi_target_pixel_height')) {
        if ((Property $start $name) -ne (Property $end $name)) { Contract-Fail "display environmentが変化しました: $name" }
    }

    $av = Property $raw 'application_av_delta_abs_ms'
    if ((Integer $av 'count') -ne $unique -or
        [Math]::Abs((Number $av 'p95') - $rawAvP95) -gt 0.000000001 -or
        [Math]::Abs((Number $av 'max') - $rawAvMax) -gt 0.000000001) {
        Contract-Fail 'producer A/V summaryがraw display再計算値と違います'
    }
    Write-Host ("[p4-smoke] PASS correctness/path fps={0:N2} drop={1:P2} av_p95={2:N3}ms av_max={3:N3}ms probes=4（performance値はdiagnostic）" -f `
        (Number $raw 'effective_video_fps'), (Number $raw 'drop_rate'), (Number $av 'p95'), (Number $av 'max'))
    Write-Host '[p4-smoke] formal_verdict=NOT_RUN（Phase 4 formal PASSではありません）'
    exit 0
} catch {
    Write-Host "[p4-smoke] FAIL $($_.Exception.Message)"
    exit 3
}
