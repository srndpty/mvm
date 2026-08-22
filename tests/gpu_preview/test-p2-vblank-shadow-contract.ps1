param(
    [Parameter(Mandatory)][ValidateSet(
        'Good', 'SupersedeGood', 'NegativeAmbiguousSwap', 'NegativeObserverGap',
        'NegativeAfterLast', 'NegativeBeforeFirst', 'NegativeShortInterval',
        'NegativeLongInterval', 'NegativeCumulativeDrift', 'NegativeNormalPriority',
        'NegativeOutputChanged', 'NegativeSampleCount')][string]$Case,
    [Parameter(Mandatory)][string]$Checker,
    [Parameter(Mandatory)][string]$Output
)
$ErrorActionPreference = 'Stop'

$qpcFrequency = 10000000
$refreshNumerator = 59950
$refreshDenominator = 1000
$period = [long][Math]::Floor(($qpcFrequency * $refreshDenominator) / $refreshNumerator)
$baseQpc = 1000000
$sampleCount = 240

# 59.95 Hz outputのVBlank列。exact rationalから作り、実装の式とは独立に置く。
$samples = @(0..($sampleCount - 1) | ForEach-Object {
    $qpc = $baseQpc + [long][Math]::Round(($_ * [double]$qpcFrequency * $refreshDenominator) /
        $refreshNumerator)
    [ordered]@{ ordinal = $_; qpc = $qpc }
})

# 各physical VBlankの内側に1本ずつswapを置く。
$swaps = @(0..($sampleCount - 3) | ForEach-Object {
    [ordered]@{
        swap_qpc = $samples[$_].qpc + [long]($period / 3)
        swap_ordinal = $_
        completed_render_ordinal = $_
        submitted_render_ordinal = $_
        presented_output_frame = $_
    }
})

$identity = [ordered]@{
    available = $true; monitor_handle = '65537'; output_index = 0
    adapter_luid_low = 59807; adapter_luid_high = 0
    gdi_device_name = '\\.\DISPLAY1'; output_device_name = '\\.\DISPLAY1'
    refresh_numerator = $refreshNumerator; refresh_denominator = $refreshDenominator
    desktop_left = 0; desktop_top = 0; desktop_right = 1920; desktop_bottom = 1200
}
$endIdentity = [ordered]@{}
foreach ($key in $identity.Keys) { $endIdentity[$key] = $identity[$key] }

switch ($Case) {
    # 同一physical VBlank内の2本目。supersede候補であってambiguousではない。
    'SupersedeGood' {
        $extra = [ordered]@{
            swap_qpc = $samples[9].qpc + [long]($period / 2)
            swap_ordinal = 9; completed_render_ordinal = 9
            submitted_render_ordinal = 9; presented_output_frame = 9
        }
        $swaps = @($swaps[0..9]) + @($extra) + @($swaps[10..($swaps.Count - 1)])
    }
    'NegativeAmbiguousSwap' { $swaps[10].swap_qpc = $samples[5].qpc + [long]($period / 3) }
    'NegativeObserverGap' {
        $samples = @($samples[0..19]) + @($samples[21..($samples.Count - 1)])
    }
    'NegativeAfterLast' {
        $swaps = @($swaps) + @([ordered]@{
            swap_qpc = $samples[$samples.Count - 1].qpc + 10
            swap_ordinal = 999; completed_render_ordinal = 999
            submitted_render_ordinal = 999; presented_output_frame = 999
        })
    }
    'NegativeBeforeFirst' { $swaps[0].swap_qpc = $samples[0].qpc - 10 }
    'NegativeShortInterval' { $samples[60].qpc = $samples[59].qpc + [long]($period / 4) }
    'NegativeLongInterval' { $samples[60].qpc = $samples[59].qpc + [long]($period * 2) }
    # 1 intervalあたり0.5%程度の遅れ。interval gate (0.5T/1.5T) は通るが、
    # 累積では1 VBlankを超える。cumulative consistencyでのみ検出できる。
    'NegativeCumulativeDrift' {
        for ($index = 1; $index -lt $samples.Count; ++$index) {
            $samples[$index].qpc = $samples[$index - 1].qpc + $period + 900
        }
    }
    'NegativeNormalPriority' { }
    'NegativeOutputChanged' { $endIdentity.output_index = 1 }
    'NegativeSampleCount' { }
}

$vblank = [ordered]@{
    enabled = $true
    observer_started = $true
    observer_error = ''
    time_critical_priority = ($Case -ne 'NegativeNormalPriority')
    window_output_start = $identity
    window_output_end = $endIdentity
    window_output_stable = ($Case -ne 'NegativeOutputChanged')
    sample_count = $(if ($Case -eq 'NegativeSampleCount') { $samples.Count + 1 } else { $samples.Count })
    ring_overflow_count = 0
    wait_failure_count = 0
    sequence_status = 'OK'
    interval_report_ok = $true
    interval_count = $samples.Count - 1
    long_interval_count = 0
    short_interval_count = 0
    max_interval_qpc = $period
    min_interval_qpc = $period
    nominal_period_qpc = $period
    cumulative_deviation_numerator = 0
    cumulative_tolerance_unit = ([decimal]$qpcFrequency * $refreshDenominator)
    cumulative_consistent = $true
    samples = @($samples)
}

$raw = [ordered]@{
    schema = 'mvm-p2-formal-2'
    presentation_opportunity = [ordered]@{
        enabled = $true
        qpc_frequency = $qpcFrequency
        render_record_count = $swaps.Count
        swap_record_count = $swaps.Count
        swap_overflow_count = 0
        render_overflow_count = 0
        physical_vblank = $vblank
        swap_records = @($swaps)
    }
}

$raw | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Output -Encoding utf8
& pwsh -NoProfile -File $Checker -Json $Output -ProcessExitCode 0
$actual = $LASTEXITCODE
$expected = if ($Case -in @('Good', 'SupersedeGood')) { 0 } else { 3 }
if ($actual -ne $expected) {
    throw "$Case shadow contract testの終了codeが違います: expected=$expected actual=$actual"
}
Write-Host "P2 vblank shadow $Case test: PASS"
