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
    Write-Error "shadow JSONを読めません: $($_.Exception.Message)"
    exit 2
}

Require-Equal $ProcessExitCode 0 '実process exit code'
$opportunity = Require-Property $raw 'presentation_opportunity'
Require-Equal (Require-Property $opportunity 'enabled') $true 'presentation_opportunity.enabled'
$qpcFrequency = [long](Require-Property $opportunity 'qpc_frequency')
if ($qpcFrequency -le 0) { Add-Failure "qpc_frequencyは正数である必要があります (actual=$qpcFrequency)" }

$vblank = Require-Property $opportunity 'physical_vblank'
Require-Equal (Require-Property $vblank 'enabled') $true 'physical_vblank.enabled'
Require-Equal (Require-Property $vblank 'observer_started') $true 'physical_vblank.observer_started'
Require-Equal (Require-Property $vblank 'observer_error') '' 'physical_vblank.observer_error'
# normal priorityへ黙ってfallbackしていないこと。
Require-Equal (Require-Property $vblank 'time_critical_priority') $true `
    'physical_vblank.time_critical_priority'
Require-Equal (Require-Property $vblank 'window_output_stable') $true `
    'physical_vblank.window_output_stable'
Require-Equal (Require-Property $vblank 'sequence_status') 'OK' 'physical_vblank.sequence_status'
Require-Equal (Require-Property $vblank 'ring_overflow_count') 0 `
    'physical_vblank.ring_overflow_count'
Require-Equal (Require-Property $vblank 'wait_failure_count') 0 `
    'physical_vblank.wait_failure_count'
Require-Equal (Require-Property $vblank 'interval_report_ok') $true `
    'physical_vblank.interval_report_ok'
# 隣接VBlankと断定できないintervalは長短どちらもauthority invalidとする。
Require-Equal (Require-Property $vblank 'long_interval_count') 0 `
    'physical_vblank.long_interval_count'
Require-Equal (Require-Property $vblank 'short_interval_count') 0 `
    'physical_vblank.short_interval_count'
Require-Equal (Require-Property $vblank 'cumulative_consistent') $true `
    'physical_vblank.cumulative_consistent'

$startIdentity = Require-Property $vblank 'window_output_start'
$endIdentity = Require-Property $vblank 'window_output_end'
Require-Equal (Require-Property $startIdentity 'available') $true `
    'physical_vblank.window_output_start.available'
foreach ($name in @('monitor_handle', 'output_index', 'adapter_luid_low', 'adapter_luid_high',
                    'gdi_device_name', 'output_device_name', 'refresh_numerator',
                    'refresh_denominator', 'desktop_left', 'desktop_top', 'desktop_right',
                    'desktop_bottom')) {
    Require-Equal (Require-Property $endIdentity $name) (Require-Property $startIdentity $name) `
        "physical_vblank.window_output_end.$name"
}
$refreshNumerator = [long](Require-Property $startIdentity 'refresh_numerator')
$refreshDenominator = [long](Require-Property $startIdentity 'refresh_denominator')
if ($refreshNumerator -le 0 -or $refreshDenominator -le 0) {
    Add-Failure "window outputのrefresh rationalが不正です ($refreshNumerator/$refreshDenominator)"
}

$samples = @(Require-Property $vblank 'samples')
if ($samples.Count -lt 2) { Add-Failure "physical VBlank sampleが不足しています ($($samples.Count))" }
Require-Equal (Require-Property $vblank 'sample_count') $samples.Count 'physical_vblank.sample_count'

# producerのinterval/cumulative集計を信じず、raw sampleから再計算する。
$nominalPeriod = [long]([Math]::Floor(($qpcFrequency * $refreshDenominator) / $refreshNumerator))
Require-Equal (Require-Property $vblank 'nominal_period_qpc') $nominalPeriod `
    'physical_vblank.nominal_period_qpc'
$longIntervals = 0
$shortIntervals = 0
$qpcValues = @($samples | ForEach-Object { [long]$_.qpc })
$ordinals = @($samples | ForEach-Object { [long]$_.ordinal })
for ($index = 1; $index -lt $samples.Count; ++$index) {
    if ($ordinals[$index] -ne $ordinals[$index - 1] + 1) {
        Add-Failure "physical VBlank ordinalが連続していません (index=$index)"
        break
    }
    $interval = $qpcValues[$index] - $qpcValues[$index - 1]
    if ($interval -le 0) {
        Add-Failure "physical VBlank QPCが後退しました (index=$index)"
        break
    }
    if ($interval * 2 -ge $nominalPeriod * 3) { ++$longIntervals }
    if ($interval * 2 -lt $nominalPeriod) { ++$shortIntervals }
}
if ($longIntervals -ne 0) {
    Add-Failure "1.5周期以上のVBlank intervalを再計算で $longIntervals 件検出しました"
}
if ($shortIntervals -ne 0) {
    Add-Failure "0.5周期未満のVBlank intervalを再計算で $shortIntervals 件検出しました"
}
$cumulativeDeviation = ([decimal]($ordinals[-1] - $ordinals[0]) * [decimal]$qpcFrequency *
    [decimal]$refreshDenominator) -
    ([decimal]($qpcValues[-1] - $qpcValues[0]) * [decimal]$refreshNumerator)
$cumulativeTolerance = [decimal]$qpcFrequency * [decimal]$refreshDenominator
if ([Math]::Abs($cumulativeDeviation) -gt $cumulativeTolerance) {
    Add-Failure ("VBlank累積progressionがrationalと1 VBlank以内で一致しません " +
        "(deviation=$cumulativeDeviation tolerance=$cumulativeTolerance)")
}

# 各swapを V_k <= swapQpc < V_(k+1) へ独立にmappingする。QPC補間で救済しない。
$swaps = @(Require-Property $opportunity 'swap_records')
Require-Equal (Require-Property $opportunity 'swap_record_count') $swaps.Count `
    'presentation_opportunity.swap_record_count'
Require-Equal (Require-Property $opportunity 'swap_overflow_count') 0 `
    'presentation_opportunity.swap_overflow_count'
if ($swaps.Count -lt 2) { Add-Failure "swap recordが不足しています ($($swaps.Count))" }

$mapped = 0
$sameOpportunity = 0
$beforeFirst = 0
$afterLast = 0
$observerGap = 0
$ambiguous = 0
$previousOpportunity = -1
$previousSwapQpc = 0
$cursor = 0
foreach ($swap in $swaps) {
    $swapQpc = [long](Require-Property $swap 'swap_qpc')
    if ($swapQpc -le $previousSwapQpc) {
        ++$ambiguous
        continue
    }
    $previousSwapQpc = $swapQpc
    if ($swapQpc -lt $qpcValues[0]) { ++$beforeFirst; continue }
    if ($swapQpc -ge $qpcValues[-1]) { ++$afterLast; continue }
    while ($cursor + 1 -lt $qpcValues.Count -and $qpcValues[$cursor + 1] -le $swapQpc) { ++$cursor }
    if ($ordinals[$cursor + 1] -ne $ordinals[$cursor] + 1) { ++$observerGap; continue }
    $ordinal = $ordinals[$cursor]
    if ($ordinal -eq $previousOpportunity) {
        ++$sameOpportunity
    } elseif ($ordinal -lt $previousOpportunity) {
        ++$ambiguous
    } else {
        ++$mapped
        $previousOpportunity = $ordinal
    }
}

if ($ambiguous -ne 0) { Add-Failure "一意にbracketできないswapが $ambiguous 件あります" }
if ($observerGap -ne 0) { Add-Failure "observer gap内のswapが $observerGap 件あります" }
if ($beforeFirst -ne 0) { Add-Failure "observer開始前のswapが $beforeFirst 件あります" }
if ($afterLast -ne 0) {
    Add-Failure "upper bracket未確定のswapが $afterLast 件あります (post-window drain不足)"
}
if ($mapped -le 0) { Add-Failure 'physical opportunityへmappingできたswapがありません' }
Require-Equal ($mapped + $sameOpportunity) $swaps.Count 'mapped + sameOpportunity == swap数'

Write-Host ("shadow mapping: mapped=$mapped same_opportunity=$sameOpportunity " +
    "before_first=$beforeFirst after_last=$afterLast observer_gap=$observerGap " +
    "ambiguous=$ambiguous vblank_samples=$($samples.Count)")

if ($failures.Count -gt 0) {
    $failures | ForEach-Object { Write-Host $_ }
    exit 3
}
Write-Host 'P2 vblank shadow mapping contract: PASS'
exit 0
