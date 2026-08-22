param(
    [Parameter(Mandatory)][ValidateSet(
        'Good', 'NegativeProbeFail', 'NegativeOutputChanged', 'NegativeSequenceStatus',
        'NegativeLongInterval', 'NegativeWaitFailure', 'NegativeRingOverflow',
        'NegativeCadenceDeviation', 'NegativeDeviationRecalculation',
        'DwmClockMismatchIsDiagnosticOnly')][string]$Case,
    [Parameter(Mandatory)][string]$Checker,
    [Parameter(Mandatory)][string]$Output
)
$ErrorActionPreference = 'Stop'

$qpcFrequency = 10000000
$refreshNumerator = 59950
$refreshDenominator = 1000

# 実測と同じ 59.95 Hz / 10 MHz。120 VBlank分のpreflight窓。
$observed = 119
$elapsed = [long][Math]::Round(($observed * $qpcFrequency * $refreshDenominator) / $refreshNumerator)
$deviation = ([decimal]$observed * $qpcFrequency * $refreshDenominator) -
    ([decimal]$elapsed * $refreshNumerator)
$tolerance = [decimal]$qpcFrequency * $refreshDenominator

$identity = [ordered]@{
    available = $true; monitor_handle = '65537'; output_index = 0
    adapter_luid_low = 59807; adapter_luid_high = 0
    gdi_device_name = '\\.\DISPLAY1'; output_device_name = '\\.\DISPLAY1'
    refresh_numerator = $refreshNumerator; refresh_denominator = $refreshDenominator
    desktop_left = 0; desktop_top = 0; desktop_right = 1920; desktop_bottom = 1200
}
$endIdentity = [ordered]@{}
foreach ($key in $identity.Keys) { $endIdentity[$key] = $identity[$key] }

$raw = [ordered]@{
    schema = 'mvm-p2-vblank-authority-probe-1'
    probe_pass = $true
    error = ''
    configured_duration_ms = 60000
    configured_preflight_vblanks = 120
    qpc_frequency = $qpcFrequency
    window_output_start = $identity
    window_output_end = $endIdentity
    window_output_stable = $true
    vblank_sample_count = 3598
    vblank_sequence_status = 'OK'
    vblank_ring_overflow_count = 0
    vblank_wait_failure_count = 0
    vblank_head = @()
    preflight_vblank_cadence_consistent = $true
    preflight_observed_intervals = $observed
    preflight_elapsed_qpc = $elapsed
    preflight_deviation_numerator = $deviation
    preflight_tolerance_unit = $tolerance
    full_vblank_cadence_consistent = $true
    full_observed_intervals = 3597
    full_elapsed_qpc = 600000000
    full_deviation_numerator = 0
    full_tolerance_unit = $tolerance
    vblank_interval_report_ok = $true
    vblank_interval_count = 3597
    vblank_long_interval_count = 0
    vblank_max_interval_qpc = 171428
    vblank_min_interval_qpc = 163038
    vblank_nominal_period_qpc = 166805
    window_output_observed_hz = 59.9501936391255
    # DWM composition clockはwindow outputと一致しない。diagnostic-onlyなので
    # 判定へは持ち込まない。
    dwm_diagnostic_start = [ordered]@{ available = $true; refresh_count = 2
        qpc_vblank = 1663406221813; qpc_refresh_period = 166804
        rate_refresh_numerator = 10000000; rate_refresh_denominator = 166804
        sampled_qpc = 1663406203858 }
    dwm_diagnostic_stop = [ordered]@{ available = $true; refresh_count = 3
        qpc_vblank = 1663456263794; qpc_refresh_period = 166815
        rate_refresh_numerator = 10000000; rate_refresh_denominator = 166815
        sampled_qpc = 1663456202711 }
    dwm_diagnostic_observed_hz = 0.0166668448630163
}

switch ($Case) {
    # 各caseで1 fieldだけを壊し、checkerの効力を確認する。
    'NegativeProbeFail' { $raw.probe_pass = $false }
    'NegativeOutputChanged' { $raw.window_output_end.output_index = 1 }
    'NegativeSequenceStatus' { $raw.vblank_sequence_status = 'ORDINAL_GAP' }
    'NegativeLongInterval' { $raw.vblank_long_interval_count = 1 }
    'NegativeWaitFailure' { $raw.vblank_wait_failure_count = 1 }
    'NegativeRingOverflow' { $raw.vblank_ring_overflow_count = 1 }
    # 約123.7 Hz clockをordinal authorityにした場合の窓。1 VBlankを大きく超える。
    'NegativeCadenceDeviation' {
        $raw.preflight_elapsed_qpc = [long][Math]::Round(($observed * $qpcFrequency * 1000) / 123700)
        $raw.preflight_deviation_numerator = ([decimal]$observed * $qpcFrequency *
            $refreshDenominator) - ([decimal]$raw.preflight_elapsed_qpc * $refreshNumerator)
    }
    'NegativeDeviationRecalculation' { $raw.preflight_deviation_numerator = $deviation + 1 }
    # DWM clockがwindow outputと一致しないこと自体はprobeの合否に影響しない。
    'DwmClockMismatchIsDiagnosticOnly' {
        $raw.dwm_diagnostic_observed_hz = 123.7
        $raw.dwm_diagnostic_stop.refresh_count = 7423
    }
}

$raw | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $Output -Encoding utf8
& pwsh -NoProfile -File $Checker -Json $Output -ProcessExitCode 0
$actual = $LASTEXITCODE
$expected = if ($Case -in @('Good', 'DwmClockMismatchIsDiagnosticOnly')) { 0 } else { 3 }
if ($actual -ne $expected) {
    throw "$Case contract testの終了codeが違います: expected=$expected actual=$actual"
}
Write-Host "P2 vblank authority $Case test: PASS"
