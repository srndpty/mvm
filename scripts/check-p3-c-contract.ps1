[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Json,
    [ValidateSet('playback','seek','pause-resume')][string]$Mode,
    [int]$ProcessExitCode = 0,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Fail([string]$Message) { throw "P3-C contract: $Message" }
function Property([object]$Object, [string]$Name) {
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or $null -eq $property.Value) { Fail "必須 field がありません: $Name" }
    return $property.Value
}
function Is-IntegerValue([object]$Value) {
    return $Value -is [sbyte] -or $Value -is [byte] -or $Value -is [int16] -or
        $Value -is [uint16] -or $Value -is [int32] -or $Value -is [uint32] -or
        $Value -is [int64] -or $Value -is [uint64]
}
function Integer([object]$Object, [string]$Name, [bool]$Nonnegative = $true) {
    $value = Property $Object $Name
    if (-not (Is-IntegerValue $value)) { Fail "$Name は JSON integer ではありません" }
    if ($Nonnegative -and [long]$value -lt 0) { Fail "$Name は nonnegative ではありません" }
    return [long]$value
}
function Number([object]$Object, [string]$Name) {
    $value = Property $Object $Name
    if (-not ($value -is [ValueType]) -or $value -is [bool]) { Fail "$Name は数値ではありません" }
    $number = [double]$value
    if ([double]::IsNaN($number) -or [double]::IsInfinity($number)) {
        Fail "$Name は有限数ではありません"
    }
    return $number
}
function Equal-Number([double]$Actual, [double]$Expected, [string]$Name) {
    if ([math]::Abs($Actual - $Expected) -gt 0.0000005) {
        Fail "$Name の producer summary が raw 再計算値と不一致です ($Actual / $Expected)"
    }
}
function Nearest-Rank([double[]]$Sorted, [double]$P) {
    if ($Sorted.Count -eq 0) { return 0.0 }
    $index = [math]::Ceiling($Sorted.Count * $P) - 1
    return [double]$Sorted[[math]::Min($index, $Sorted.Count - 1)]
}
function Check-Distribution([object]$Distribution, [int]$ExpectedCount, [bool]$RequireValues,
                            [string]$Name) {
    $count = Integer $Distribution 'count'
    if ($count -ne $ExpectedCount) { Fail "$Name count が不正です ($count / $ExpectedCount)" }
    if ($RequireValues) {
        $valuesProperty = $Distribution.PSObject.Properties['values']
        if ($null -eq $valuesProperty -or $null -eq $valuesProperty.Value) {
            Fail "$Name raw values がありません"
        }
        $values = @($valuesProperty.Value)
        if ($values.Count -ne $ExpectedCount) { Fail "$Name raw values 件数が不正です" }
        $finite = [System.Collections.Generic.List[double]]::new()
        foreach ($value in $values) {
            if (-not ($value -is [ValueType]) -or $value -is [bool]) { Fail "$Name に非数値があります" }
            $number = [double]$value
            if ([double]::IsNaN($number) -or [double]::IsInfinity($number)) {
                Fail "$Name に NaN/Infinity があります"
            }
            $finite.Add($number)
        }
        $signed = @($finite | Sort-Object)
        $absolute = @($finite | ForEach-Object { [math]::Abs($_) } | Sort-Object)
        Equal-Number (Number $Distribution 'p50') (Nearest-Rank $signed 0.50) "$Name.p50"
        Equal-Number (Number $Distribution 'p95') (Nearest-Rank $signed 0.95) "$Name.p95"
        Equal-Number (Number $Distribution 'min') $(if ($signed.Count) {$signed[0]} else {0}) "$Name.min"
        Equal-Number (Number $Distribution 'max') $(if ($signed.Count) {$signed[-1]} else {0}) "$Name.max"
        return [pscustomobject]@{
            Values = $finite
            AbsP50 = Nearest-Rank $absolute 0.50
            AbsP95 = Nearest-Rank $absolute 0.95
            AbsP99 = Nearest-Rank $absolute 0.99
            AbsMax = $(if ($absolute.Count) {$absolute[-1]} else {0.0})
        }
    }
    return $null
}
function Check-AbsoluteSummary([object]$Summary, [object]$Raw, [string]$Name) {
    if ((Integer $Summary 'count') -ne $Raw.Values.Count) { Fail "$Name count が不正です" }
    Equal-Number (Number $Summary 'p50') $Raw.AbsP50 "$Name.p50"
    Equal-Number (Number $Summary 'p95') $Raw.AbsP95 "$Name.p95"
    Equal-Number (Number $Summary 'p99') $Raw.AbsP99 "$Name.p99"
    Equal-Number (Number $Summary 'min') $(if ($Raw.Values.Count) {
        [double](($Raw.Values | ForEach-Object {[math]::Abs($_)} | Measure-Object -Minimum).Minimum)
    } else {0}) "$Name.min"
    Equal-Number (Number $Summary 'max') $Raw.AbsMax "$Name.max"
}
function Require-Zero([object]$Object, [string[]]$Names) {
    foreach ($name in $Names) { if ((Integer $Object $name) -ne 0) { Fail "$name は 0 ではありません" } }
}

if (-not (Test-Path -LiteralPath $Json)) { Fail "JSON がありません: $Json" }
try { $data = Get-Content -Raw -LiteralPath $Json | ConvertFrom-Json } catch { Fail "JSON を読めません: $_" }
if ((Property $data 'schema') -ne 'mvm-p3-formal-1' -or
    (Property $data 'contract_version') -ne 'P3-C-1' -or
    (Property $data 'phase') -ne 'P3-C' -or
    (Property $data 'formal_verdict') -ne 'NOT_RUN') { Fail 'schema/contract/phase/verdict が不正です' }
if ($ProcessExitCode -ne 0) { Fail "process exit が 0 ではありません: $ProcessExitCode" }
$actualMode = [string](Property $data 'mode')
if ($Mode -and $actualMode -ne $Mode) { Fail "mode が不一致です: $actualMode / $Mode" }
if (-not (Property $data 'pass') -or -not (Property $data 'audio_master_only')) { Fail 'producer correctness が FAIL です' }
if ((Integer $data 'configured_video_preroll_frames') -ne 8 -or
    (Integer $data 'configured_audio_preroll_ms') -ne 100) { Fail '固定 pre-roll が exact 値ではありません' }
$endpointFirst = Integer $data 'endpoint_first_media_sample' $false
$clockAnchor = Integer $data 'clock_anchor_media_sample' $false
if ((Integer $data 'endpoint_prefill_frames') -le 0 -or $endpointFirst -ne $clockAnchor) {
    Fail 'endpoint first media sample と clock anchor が不一致です'
}

$zeroFields = @(
    'measurement_audio_underflow_count','measurement_audio_overflow_count',
    'measurement_marker_mismatch_count','measurement_mixed_pair_count',
    'measurement_mixed_generation_count','measurement_stale_composition_epoch_count',
    'measurement_video_ahead_violation_count','measurement_clock_regression_count',
    'measurement_video_qpc_master_fallback_count','measurement_audio_clock_query_failure_count',
    'cpu_full_frame_readback_count','full_frame_gpu_copy_count','software_video_fallback_count',
    'device_lost_count','lifecycle_violation_count','audio_render_thread_join_leak',
    'audio_decode_thread_join_leak'
)
Require-Zero $data $zeroFields
foreach ($name in @('video_worker_a_joined','video_worker_b_joined','teardown_success','final_report_after_teardown')) {
    if (-not (Property $data $name)) { Fail "$name が true ではありません" }
}

if ($actualMode -eq 'playback') {
    if ($endpointFirst -ne 0) { Fail 'playback endpoint/clock anchor が sample 0 ではありません' }
    $requiredSamples = Integer $data 'required_measurement_samples'
    $requiredFrames = Integer $data 'required_video_frames'
    if ((Integer $data 'measurement_audio_start_sample') -ne 0 -or
        (Integer $data 'measurement_audio_end_sample') -ne $requiredSamples -or
        $requiredSamples -ne (Integer $data 'measurement_seconds') * 48000 -or
        $requiredFrames -ne [long]($requiredSamples / 800)) { Fail 'audio sample interval が不正です' }
    $displayed = Integer $data 'measurement_video_displayed_unique_count'
    $skipped = Integer $data 'measurement_video_skipped_frame_count'
    if ((Integer $data 'measurement_video_first_frame' $false) -ne 0) { Fail 'first frame が 0 ではありません' }
    if ($displayed + $skipped -ne $requiredFrames) { Fail 'displayed + skipped accounting が不正です' }
    Require-Zero $data @('measurement_duplicate_display_identity_count','measurement_non_increasing_display_count')
    $records = @(Property $data 'measurement_display_records')
    if ($records.Count -ne $displayed) { Fail 'display ledger と unique count が不一致です' }
    $previous = -1L
    foreach ($record in $records) {
        $frame = Integer $record 'frame'
        if ($frame -le $previous) { Fail 'display ledger が strictly increasing ではありません' }
        $previous = $frame
        if (-not (Property $record 'application_av_projection_valid')) { Fail 'display AV projection が無効です' }
        [void](Number $record 'application_av_delta_ms')
    }
    $raw = Check-Distribution (Property $data 'application_av_delta_ms') $displayed $true 'application_av_delta_ms'
    Check-AbsoluteSummary (Property $data 'application_av_delta_abs_ms') $raw 'application_av_delta_abs_ms'
    if ((Integer $data 'application_av_projection_failure_count') -ne 0) { Fail 'AV projection failure があります' }
    if ((Number $data 'effective_video_fps') -lt 55.0) { Fail 'effective video fps が 55 未満です' }
    if ((Number $data 'drop_rate') -gt 0.02) { Fail 'drop rate が 0.02 を超えています' }
    if ($raw.AbsP95 -gt 20.000) { Fail 'application AV abs p95 が 20.000ms を超えています' }
    if ($raw.AbsMax -gt 33.334) { Fail 'application AV abs max が 33.334ms を超えています' }
}
elseif ($actualMode -eq 'seek') {
    $requested = Integer $data 'integrated_seek_requested'
    if (-not $DryRun -and $requested -ne 1000) { Fail 'formal seek count が 1000 ではありません' }
    if ((Integer $data 'integrated_seek_exact') -ne $requested) { Fail 'seek exact count が不一致です' }
    Require-Zero $data @('integrated_seek_timeout_count','integrated_seek_busy_acceptance_count',
        'integrated_seek_stale_completion_count','integrated_seek_generation_mismatch_count')
    $seeks = @(Property $data 'seeks')
    if ($seeks.Count -ne $requested) { Fail "seek raw count が不正です: $($seeks.Count) / $requested" }
    foreach ($seek in $seeks) {
        $frame = Integer $seek 'requested_frame'
        if ((Integer $seek 'requested_audio_sample') -ne $frame * 800 -or
            (Integer $seek 'first_audio_sample') -ne $frame * 800 -or
            (Integer $seek 'first_displayed_video_frame') -ne $frame) { Fail 'seek identity が不一致です' }
        if (-not (Property $seek 'first_display_application_av_projection_valid')) { Fail 'seek AV projection が無効です' }
        [void](Number $seek 'request_to_first_display_ms')
        [void](Number $seek 'first_display_application_av_delta_ms')
    }
    $latency = Check-Distribution (Property $data 'seek_request_to_first_display_ms') $requested $true 'seek latency'
    if ((Nearest-Rank @($latency.Values | Sort-Object) 0.95) -gt 150.000) { Fail 'seek p95 が 150.000ms を超えています' }
    if (($latency.Values | Measure-Object -Maximum).Maximum -gt 400.000) { Fail 'seek max が 400.000ms を超えています' }
    $sync = Check-Distribution (Property $data 'seek_first_display_application_av_delta_ms') $requested $true 'seek AV'
    if ($sync.AbsP95 -gt 20.000 -or $sync.AbsMax -gt 33.334) { Fail 'seek AV threshold を超えています' }
}
elseif ($actualMode -eq 'pause-resume') {
    foreach ($name in @('pause_clock_frozen','pause_video_advance_zero','pause_generation_stable')) {
        if (-not (Property $data $name)) { Fail "$name が false です" }
    }
    $count = Integer (Property $data 'application_av_delta_ms') 'count'
    if ($count -le 0) { Fail 'pause/resume AV sample が 0 件です' }
    $raw = Check-Distribution (Property $data 'application_av_delta_ms') $count $true 'pause AV'
    Check-AbsoluteSummary (Property $data 'application_av_delta_abs_ms') $raw 'pause AV abs'
    if ($raw.AbsP95 -gt 20.000 -or $raw.AbsMax -gt 33.334) { Fail 'pause/resume AV threshold を超えています' }
}
else { Fail "未知の mode です: $actualMode" }

Write-Host "PASS: P3-C-1 contract ($actualMode$(if ($DryRun) {', DryRun'}))" -ForegroundColor Green
