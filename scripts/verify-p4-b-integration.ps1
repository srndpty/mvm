<#
.SYNOPSIS
Phase 4 / B の integration sanity raw を独立に再計算して検査する。

.DESCRIPTION
**これは Phase 4 の contract checker ではない。**
docs/phase4-plan.md §10.3 の smoke contract と §10.4 の formal contract は
transition pixel probe を必須にしており、4/B ではまだ実装していない。
このスクリプトは「audio master -> schedule resolve -> atomic adoption ->
exact pair -> compose -> actual display ledger」という経路が成立したことだけを見る。

producer が出した mismatch counter や verdict は根拠にしない。
canonical schedule 文字列を独立に parse し、display ledger の 1 record ずつから
expected state / expected epoch / A/B layer identity を再計算する。

pixel probe field が数値で入っていたら失敗させる。0 を「mismatch 0」と
読み替えられる形で通すと、未実装が PASS に化ける。
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Json
)
$ErrorActionPreference = 'Stop'

# docs/phase4-plan.md §3.8 の freeze 値。raw から読まずにここへ literal で書く。
$expectedCanonical = '0:S0;200:S1;400:S2'
$expectedSha256 = '418ae09f4bb9349aa7ac53ca38028782aef074ca1696338758ccfa6b4e4398e8'
$expectedFrames = 600
$boundaries = @(200, 400)

$failures = New-Object System.Collections.Generic.List[string]
function Fail([string]$message) { $script:failures.Add($message) }
function Need($object, [string]$name) {
    if (-not $object.PSObject.Properties.Name.Contains($name)) {
        Fail "field が欠落しています: $name"
        return $false
    }
    if ($null -eq $object.$name) {
        Fail "field が null です: $name"
        return $false
    }
    return $true
}
function NeedZero($object, [string]$name) {
    if (Need $object $name) {
        if ($object.$name -ne 0) { Fail "$name が 0 ではありません: $($object.$name)" }
    }
}
function NeedTrue($object, [string]$name) {
    if (Need $object $name) {
        if ($object.$name -isnot [bool]) { Fail "$name が Boolean ではありません" }
        elseif (-not $object.$name) { Fail "$name が false です" }
    }
}

if (-not (Test-Path -LiteralPath $Json)) { Write-Error "raw がありません: $Json" }
$raw = Get-Content -LiteralPath $Json -Raw | ConvertFrom-Json

# --- schema / verdict -------------------------------------------------------
if ($raw.schema -ne 'mvm-p4-b-integration-1') { Fail "schema が違います: $($raw.schema)" }
if ($raw.phase -ne 'P4-B') { Fail "phase が違います: $($raw.phase)" }
if ($raw.schedule_kind -ne 'smoke') { Fail "schedule_kind が smoke ではありません" }
if ($raw.formal_verdict -ne 'NOT_RUN') { Fail 'formal_verdict が NOT_RUN ではありません' }
if ($raw.smoke_contract_verdict -ne 'NOT_RUN') {
    Fail 'smoke_contract_verdict が NOT_RUN ではありません'
}

# --- pixel probe は未実装。数値で埋めた raw を通さない ----------------------
if ($raw.transition_pixel_probe_status -ne 'NOT_IMPLEMENTED') {
    Fail 'transition_pixel_probe_status が NOT_IMPLEMENTED ではありません'
}
foreach ($probeField in @('transition_probe_checked_count', 'transition_probe_mismatch_count',
                          'transition_probe_render_thread_blocking_wait_count')) {
    if (-not $raw.PSObject.Properties.Name.Contains($probeField)) {
        Fail "probe field が欠落しています: $probeField"
    } elseif ($null -ne $raw.$probeField) {
        Fail "未実装の $probeField に値が入っています: $($raw.$probeField)"
    }
}

# --- canonical schedule を独立に parse --------------------------------------
if ($raw.canonical_schedule -ne $expectedCanonical) {
    Fail "canonical_schedule が freeze 値と違います: $($raw.canonical_schedule)"
}
$sha = [System.BitConverter]::ToString(
    [System.Security.Cryptography.SHA256]::Create().ComputeHash(
        [System.Text.Encoding]::UTF8.GetBytes($raw.canonical_schedule))).Replace('-', '').ToLower()
if ($sha -ne $expectedSha256) { Fail "canonical_schedule の SHA-256 が freeze 値と違います: $sha" }
if ($raw.canonical_schedule_sha256 -ne $expectedSha256) {
    Fail "canonical_schedule_sha256 が freeze 値と違います: $($raw.canonical_schedule_sha256)"
}

$parsed = @()
foreach ($segment in $expectedCanonical.Split(';')) {
    $parts = $segment.Split(':')
    if ($parts.Count -ne 2) { Fail "canonical schedule を parse できません: $segment"; continue }
    $parsed += [pscustomobject]@{ boundary = [long]$parts[0]; state = $parts[1] }
}
if ($raw.schedule.Count -ne $parsed.Count) {
    Fail "schedule array の要素数が $($parsed.Count) ではありません: $($raw.schedule.Count)"
} else {
    for ($i = 0; $i -lt $parsed.Count; $i++) {
        if ([long]$raw.schedule[$i].boundary -ne $parsed[$i].boundary -or
            $raw.schedule[$i].state -ne $parsed[$i].state) {
            Fail "schedule array の要素 $i が canonical string と一致しません"
        }
    }
}

function Expected-State([long]$frame) {
    $state = $null
    foreach ($segment in $parsed) { if ($frame -ge $segment.boundary) { $state = $segment.state } }
    return $state
}
function Expected-SegmentIndex([long]$frame) {
    $index = -1
    for ($i = 0; $i -lt $parsed.Count; $i++) { if ($frame -ge $parsed[$i].boundary) { $index = $i } }
    return $index
}

# --- driver counter ---------------------------------------------------------
foreach ($name in @('composition_state_resolve_count', 'composition_state_adoption_count',
                    'composition_state_noop_count', 'composition_state_reject_count',
                    'composition_epoch_increment_count', 'composition_state_unresolved_count',
                    'source_generation_change_due_to_layout_count',
                    'measurement_baseline_composition_epoch')) {
    [void](Need $raw $name)
}
if ($raw.composition_state_adoption_count -ne 2) {
    Fail "adoption が 2 ではありません: $($raw.composition_state_adoption_count)"
}
if ($raw.composition_epoch_increment_count -ne 2) {
    Fail "epoch increment が 2 ではありません: $($raw.composition_epoch_increment_count)"
}
NeedZero $raw 'composition_state_reject_count'
NeedZero $raw 'composition_state_unresolved_count'
NeedZero $raw 'source_generation_change_due_to_layout_count'
NeedZero $raw 'phase4_adoption_failure_count'
$resolveSum = $raw.composition_state_adoption_count + $raw.composition_state_noop_count +
              $raw.composition_state_reject_count
if ($raw.composition_state_resolve_count -ne $resolveSum) {
    Fail "resolve == adoption + noop + reject が成立しません: $($raw.composition_state_resolve_count) != $resolveSum"
}

# --- display ledger を 1 record ずつ再計算 ----------------------------------
$e0 = [long]$raw.measurement_baseline_composition_epoch
$records = @($raw.measurement_display_ledger)
if ($records.Count -eq 0) { Fail 'measurement display ledger が空です' }
if ($raw.measurement_display_ledger_count -ne $records.Count) {
    Fail 'measurement_display_ledger_count が record 数と一致しません'
}
$stateMismatch = 0
$epochMismatch = 0
$identityMismatch = 0
$oldStateAfterBoundary = 0
$lastFrame = -1
$unique = 0
$skipped = 0
$nonIncreasing = 0
foreach ($record in $records) {
    $frame = [long]$record.output_frame
    if ($frame -lt 0 -or $frame -ge $expectedFrames) {
        Fail "measurement 区間外の output frame が ledger にあります: $frame"
        continue
    }
    $expectedState = Expected-State $frame
    $expectedEpoch = $e0 + (Expected-SegmentIndex $frame)
    if ($record.composition_state -ne $expectedState) {
        $stateMismatch++
        foreach ($boundary in $boundaries) {
            if ($frame -ge $boundary -and $record.composition_state -eq (Expected-State ($boundary - 1))) {
                $oldStateAfterBoundary++
                break
            }
        }
    }
    if ([long]$record.composition_epoch -ne $expectedEpoch) { $epochMismatch++ }
    $sources = @($record.sources)
    if ($sources.Count -ne 2) {
        $identityMismatch++
    } else {
        $a = $sources[0]
        $b = $sources[1]
        if ([long]$a.source_id -ne 1 -or [long]$b.source_id -ne 2 -or
            [long]$a.frame -ne $frame -or [long]$b.frame -ne $frame -or
            [long]$a.source_generation -ne [long]$raw.baseline_source_generation_a -or
            [long]$b.source_generation -ne [long]$raw.baseline_source_generation_b -or
            [long]$a.resource_epoch -ne [long]$raw.baseline_resource_epoch_a -or
            [long]$b.resource_epoch -ne [long]$raw.baseline_resource_epoch_b) {
            $identityMismatch++
        }
    }
    if ($lastFrame -ge 0) {
        if ($frame -le $lastFrame) { $nonIncreasing++ } else { $skipped += $frame - $lastFrame - 1 }
    }
    if ($frame -ne $lastFrame) { $unique++ }
    $lastFrame = $frame
}
if ($lastFrame -ge 0 -and $lastFrame -lt $expectedFrames) {
    $skipped += $expectedFrames - $lastFrame - 1
}
if ([long]$records[0].output_frame -ne 0) { Fail 'first actual display が frame 0 ではありません' }
if ($records[0].composition_state -ne 'S0') { Fail 'first actual display が S0 ではありません' }
if ([long]$records[0].composition_epoch -ne $e0) { Fail 'first actual display の epoch が E0 ではありません' }
if ($stateMismatch -ne 0) { Fail "state 不一致が $stateMismatch 件あります" }
if ($epochMismatch -ne 0) { Fail "E0 相対 epoch 不一致が $epochMismatch 件あります" }
if ($identityMismatch -ne 0) { Fail "A/B layer identity 不一致が $identityMismatch 件あります" }
if ($oldStateAfterBoundary -ne 0) { Fail "boundary 後の旧 state 表示が $oldStateAfterBoundary 件あります" }
if ($nonIncreasing -ne 0) { Fail "output frame が非単調な display が $nonIncreasing 件あります" }
if ($unique + $skipped -ne $expectedFrames) {
    Fail "display 会計が合いません: unique $unique + skipped $skipped != $expectedFrames"
}

# --- activation lag を ledger から再計算 ------------------------------------
$recomputedLags = @()
foreach ($boundary in $boundaries) {
    $after = $records | Where-Object { [long]$_.output_frame -ge $boundary } |
             Sort-Object { [long]$_.output_frame } | Select-Object -First 1
    if ($null -eq $after) {
        Fail "boundary $boundary 以降の actual display がありません"
        $recomputedLags += -1
        continue
    }
    $lag = [long]$after.output_frame - $boundary
    if ($lag -lt 0 -or $lag -gt 2) { Fail "boundary $boundary の activation lag が 0..2 の外です: $lag" }
    if ($after.composition_state -ne (Expected-State $boundary)) {
        Fail "boundary $boundary 後の最初の display が新 state ではありません"
    }
    $recomputedLags += $lag
}
$rawLags = @($raw.transition_activation_lag_frames)
if ($rawLags.Count -ne $boundaries.Count) {
    Fail "transition_activation_lag_frames が $($boundaries.Count) 件ではありません: $($rawLags.Count)"
} else {
    for ($i = 0; $i -lt $rawLags.Count; $i++) {
        if ([long]$rawLags[$i] -ne $recomputedLags[$i]) {
            Fail "activation lag $i が ledger から再計算した値と違います: $($rawLags[$i]) != $($recomputedLags[$i])"
        }
    }
}

# --- freeze 済み shutdown 順 (docs/phase4-plan.md §7) ------------------------
# 期待値は raw から読まずここへ literal で書く。
$expectedShutdown = @('DisableSchedulers', 'StopAudioSink', 'StopAudioDecodeWorker',
                      'StopVideoWorkerA', 'StopVideoWorkerB', 'DetachSharedWorkerRefs',
                      'RequestRenderTeardown')
$actualShutdown = @($raw.shutdown_sequence)
if ($actualShutdown.Count -ne $expectedShutdown.Count) {
    Fail "shutdown_sequence の step 数が $($expectedShutdown.Count) ではありません: $($actualShutdown.Count)"
} else {
    for ($i = 0; $i -lt $expectedShutdown.Count; $i++) {
        if ($actualShutdown[$i] -ne $expectedShutdown[$i]) {
            Fail "shutdown step $i が freeze 順と違います: $($actualShutdown[$i]) != $($expectedShutdown[$i])"
        }
    }
}
NeedZero $raw 'shutdown_order_violation_count'
NeedTrue $raw 'shutdown_workers_joined_before_teardown'
NeedTrue $raw 'shutdown_render_teardown_requested'

# --- 継承した P3 correctness / lifecycle counter ----------------------------
foreach ($name in @('measurement_audio_underflow_count', 'measurement_audio_overflow_count',
                    'measurement_marker_mismatch_count', 'measurement_mixed_pair_count',
                    'measurement_mixed_generation_count',
                    'measurement_stale_composition_epoch_count',
                    'measurement_video_ahead_violation_count',
                    'measurement_clock_regression_count',
                    'measurement_video_qpc_master_fallback_count',
                    'measurement_audio_clock_query_failure_count',
                    'composition_state_display_mismatch_count',
                    'old_state_after_boundary_count',
                    'composition_pair_identity_violation_count',
                    'composition_layer_generation_mismatch_count',
                    'measurement_non_increasing_display_count',
                    'cpu_full_frame_readback_count', 'full_frame_gpu_copy_count',
                    'software_video_fallback_count', 'device_lost_count',
                    'lifecycle_violation_count', 'audio_render_thread_join_leak',
                    'audio_decode_thread_join_leak')) {
    NeedZero $raw $name
}
foreach ($name in @('audio_master_only', 'composition_counter_self_consistent',
                    'video_worker_a_joined', 'video_worker_b_joined', 'teardown_success',
                    'final_report_after_teardown', 'display_target_preflight_pass',
                    'integration_sanity_pass')) {
    NeedTrue $raw $name
}
if ($raw.measurement_video_first_frame -ne 0) { Fail 'measurement_video_first_frame が 0 ではありません' }
if ($raw.required_video_frames -ne $expectedFrames) {
    Fail "required_video_frames が $expectedFrames ではありません: $($raw.required_video_frames)"
}

# --- performance は診断のみ。ここでは判定に使わないが、破綻は報告する --------
Write-Host ("[p4-b] fps={0:N2} drop={1:N4} av_p95={2:N3}ms av_max={3:N3}ms displays={4}" -f `
    $raw.effective_video_fps, $raw.drop_rate, $raw.application_av_delta_abs_ms.p95,
    $raw.application_av_delta_abs_ms.max, $records.Count)

if ($failures.Count -gt 0) {
    foreach ($failure in $failures) { Write-Host "[p4-b] FAIL $failure" }
    Write-Host "[p4-b] Phase 4 / B integration sanity 検査が $($failures.Count) 件失敗しました"
    exit 3
}
Write-Host '[p4-b] integration sanity OK (Phase 4 smoke contract の PASS ではない)'
exit 0
