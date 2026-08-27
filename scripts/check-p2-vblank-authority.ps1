param(
    [Parameter(Mandatory = $true)][string]$Json,
    [int]$ProcessExitCode = 0
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$failures = [System.Collections.Generic.List[string]]::new()

function Add-Failure([string]$Message) {
    $script:failures.Add($Message)
}

function Require-Property($Object, [string]$Name) {
    if ($Object.PSObject.Properties.Name -notcontains $Name) {
        Add-Failure "必須fieldがありません: $Name"
        return $null
    }
    return $Object.$Name
}

function Require-Equal($Actual, $Expected, [string]$Name) {
    if ($null -eq $Actual -or $Actual -ne $Expected) {
        Add-Failure "$Name は $Expected である必要があります (actual=$Actual)"
    }
}

try {
    $raw = Get-Content -LiteralPath $Json -Raw -Encoding utf8 | ConvertFrom-Json
} catch {
    Write-Error "probe JSONを読めません: $($_.Exception.Message)"
    exit 2
}

Require-Equal (Require-Property $raw 'schema') 'mvm-p2-vblank-authority-probe-1' 'schema'
Require-Equal $ProcessExitCode 0 '実process exit code'
Require-Equal (Require-Property $raw 'probe_pass') $true 'probe_pass'
Require-Equal (Require-Property $raw 'error') '' 'error'

$qpcFrequency = [long](Require-Property $raw 'qpc_frequency')
if ($qpcFrequency -le 0) { Add-Failure "qpc_frequencyは正数である必要があります (actual=$qpcFrequency)" }

# window outputのidentityは開始と終了で同一でなければならない。
$startIdentity = Require-Property $raw 'window_output_start'
$endIdentity = Require-Property $raw 'window_output_end'
Require-Equal (Require-Property $raw 'window_output_stable') $true 'window_output_stable'
Require-Equal (Require-Property $startIdentity 'available') $true 'window_output_start.available'
Require-Equal (Require-Property $endIdentity 'available') $true 'window_output_end.available'
foreach ($name in @('monitor_handle', 'output_index', 'adapter_luid_low', 'adapter_luid_high',
                    'gdi_device_name', 'output_device_name', 'refresh_numerator',
                    'refresh_denominator', 'desktop_left', 'desktop_top', 'desktop_right',
                    'desktop_bottom')) {
    Require-Equal (Require-Property $endIdentity $name) (Require-Property $startIdentity $name) `
        "window_output_end.$name"
}

$refreshNumerator = [long](Require-Property $startIdentity 'refresh_numerator')
$refreshDenominator = [long](Require-Property $startIdentity 'refresh_denominator')
if ($refreshNumerator -le 0 -or $refreshDenominator -le 0) {
    Add-Failure "window outputのrefresh rationalが不正です ($refreshNumerator/$refreshDenominator)"
}

# observerがVBlankを取りこぼしていないこと。ordinalは自前counterなので、
# 連続性だけでなくinterval側の異常も0でなければならない。
Require-Equal (Require-Property $raw 'vblank_sequence_status') 'OK' 'vblank_sequence_status'
Require-Equal (Require-Property $raw 'vblank_ring_overflow_count') 0 'vblank_ring_overflow_count'
Require-Equal (Require-Property $raw 'vblank_wait_failure_count') 0 'vblank_wait_failure_count'
Require-Equal (Require-Property $raw 'vblank_interval_report_ok') $true 'vblank_interval_report_ok'
Require-Equal (Require-Property $raw 'vblank_long_interval_count') 0 'vblank_long_interval_count'

$sampleCount = [long](Require-Property $raw 'vblank_sample_count')
if ($sampleCount -lt 2) { Add-Failure "vblank_sample_countが不足しています (actual=$sampleCount)" }

# preflight窓のcadence整合をproducerの真偽値ではなく生の数値から再計算する。
$preflightObserved = [long](Require-Property $raw 'preflight_observed_intervals')
$preflightElapsed = [long](Require-Property $raw 'preflight_elapsed_qpc')
$preflightDeviation = [decimal](Require-Property $raw 'preflight_deviation_numerator')
$preflightTolerance = [decimal](Require-Property $raw 'preflight_tolerance_unit')
if ($preflightObserved -le 0 -or $preflightElapsed -le 0) {
    Add-Failure 'preflight窓のVBlank観測が不足しています'
} else {
    $recalculatedDeviation = ([decimal]$preflightObserved * [decimal]$qpcFrequency *
        [decimal]$refreshDenominator) - ([decimal]$preflightElapsed * [decimal]$refreshNumerator)
    $recalculatedTolerance = [decimal]$qpcFrequency * [decimal]$refreshDenominator
    if ($preflightDeviation -ne $recalculatedDeviation) {
        Add-Failure ("preflight_deviation_numeratorが再計算と一致しません " +
            "(actual=$preflightDeviation recalculated=$recalculatedDeviation)")
    }
    if ($preflightTolerance -ne $recalculatedTolerance) {
        Add-Failure ("preflight_tolerance_unitが再計算と一致しません " +
            "(actual=$preflightTolerance recalculated=$recalculatedTolerance)")
    }
    if ([Math]::Abs($recalculatedDeviation) -gt $recalculatedTolerance) {
        Add-Failure ("window output VBlank cadenceがQueryDisplayConfig rationalと1 VBlank以内で" +
            "一致しません (deviation=$recalculatedDeviation tolerance=$recalculatedTolerance)")
    }
}
Require-Equal (Require-Property $raw 'preflight_vblank_cadence_consistent') $true `
    'preflight_vblank_cadence_consistent'

# DWM composition clockはdiagnostic-only。formal authorityへ使っていないことを、
# cadenceが一致していなくてもprobeが通ることで示す。判定材料にはしない。
$dwmStart = Require-Property $raw 'dwm_diagnostic_start'
$dwmStop = Require-Property $raw 'dwm_diagnostic_stop'
Require-Property $dwmStart 'refresh_count' | Out-Null
Require-Property $dwmStop 'refresh_count' | Out-Null
Require-Property $raw 'dwm_diagnostic_observed_hz' | Out-Null

if ($failures.Count -gt 0) {
    $failures | ForEach-Object { Write-Host $_ }
    exit 3
}
Write-Host 'P2 vblank authority probe contract: PASS'
exit 0
