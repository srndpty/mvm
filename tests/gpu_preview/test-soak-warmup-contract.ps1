[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'Good',
        'NegativeWarmupIncludedInHandleWindow',
        'NegativeMeasuredIterationCountReduced',
        'NegativeHandleThresholdRelaxed',
        'NegativeWarmupFunctionalFailureIgnored')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Checker,
    [Parameter(Mandatory=$true)][string]$Output
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# S2-g1b: warmup が gate avoidance ではなく measurement-domain repair であることを固定する。
# 各 negative は自分の intended violation で落ちること。非0 exitだけでPASSにしない。

$raw = [ordered]@{
    scenario                           = 's5-five-track'
    iterations_requested               = 100
    iterations_completed               = 130
    warmup_iteration_count             = 30
    measured_iteration_count           = 100
    handle_measurement_start_iteration = 30
    quartile_size                      = 25
    handle_growth_threshold            = 8
    functional_authority_domain        = 'ALL_ITERATIONS'
    handle_retention_authority_domain  = 'MEASURED_ONLY'
}

$expectedViolation = @{
    NegativeWarmupIncludedInHandleWindow   = 'handle measurementがwarmup sampleを含んでいます: start=0'
    NegativeMeasuredIterationCountReduced  = 'measured iteration数が減っています: expected=100 actual=70'
    NegativeHandleThresholdRelaxed         = 'handle growth thresholdが変更されています: 16'
    NegativeWarmupFunctionalFailureIgnored = 'warmup中のfunctional failureが無視されています: MEASURED_ONLY'
}

$before = $raw | ConvertTo-Json -Depth 6 -Compress
switch ($Case) {
    # warmup sample を handle metric の窓へ混ぜる。
    'NegativeWarmupIncludedInHandleWindow'  { $raw.handle_measurement_start_iteration = 0 }
    # warmup を 100 の内側で消費し measured workload を減らす。
    'NegativeMeasuredIterationCountReduced' {
        $raw.measured_iteration_count = 70
        $raw.iterations_completed = 100
    }
    # threshold を緩める。
    'NegativeHandleThresholdRelaxed'        { $raw.handle_growth_threshold = 16 }
    # warmup 中の functional failure を authority から外す。
    'NegativeWarmupFunctionalFailureIgnored'{ $raw.functional_authority_domain = 'MEASURED_ONLY' }
}
$after = $raw | ConvertTo-Json -Depth 6 -Compress
if ($Case -ne 'Good' -and $before -ceq $after) {
    throw "negative mutationが適用されませんでした: $Case"
}

$raw | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $Output -Encoding utf8
try {
    & $Checker -Json $Output *> $null
    $actual = $null
} catch {
    $actual = $_.Exception.Message
}
if ($Case -eq 'Good') {
    if ($null -ne $actual) { throw "対照群が失敗しました: $actual" }
} elseif ($null -eq $actual) {
    throw "negative caseをcheckerが受理しました: $Case"
} elseif ($actual -cne $expectedViolation[$Case]) {
    throw "意図しないviolationです: case=$Case expected=$($expectedViolation[$Case]) actual=$actual"
}
Write-Output "soak warmup contract $Case : PASS"
