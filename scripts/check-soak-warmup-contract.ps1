[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Json,
    [int]$ExpectedWarmup = 30,
    [int]$ExpectedMeasured = 100
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# S2-g1b: soak の measurement-domain contract を検査する。
#
# warmup は measurement-domain boundary であって retention の許容量ではない。
# したがって次を同時に固定する。
#   - warmup sample が handle metric の窓へ混入しないこと
#   - measured workload が warmup 分だけ減らされていないこと
#   - handle growth threshold が緩められていないこと
#   - functional correctness authority が warmup 中も有効であること

function Assert-Soak([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

$raw = Get-Content -Raw -LiteralPath $Json | ConvertFrom-Json

foreach ($field in @('warmup_iteration_count','measured_iteration_count',
                     'handle_measurement_start_iteration','quartile_size',
                     'handle_growth_threshold','functional_authority_domain',
                     'handle_retention_authority_domain','iterations_completed')) {
    Assert-Soak ($raw.PSObject.Properties.Name -contains $field) "$field がありません"
}

Assert-Soak ([int]$raw.warmup_iteration_count -eq $ExpectedWarmup) `
    "warmup iteration数が一致しません: expected=$ExpectedWarmup actual=$($raw.warmup_iteration_count)"

# measured workload を warmup で食わない。総回数は warmup + measured である。
Assert-Soak ([int]$raw.measured_iteration_count -eq $ExpectedMeasured) `
    "measured iteration数が減っています: expected=$ExpectedMeasured actual=$($raw.measured_iteration_count)"
Assert-Soak ([int]$raw.iterations_completed -eq ($ExpectedWarmup + $ExpectedMeasured)) `
    "総iteration数がwarmup+measuredと一致しません: $($raw.iterations_completed)"

# warmup sample が handle metric の窓へ入っていないこと。
Assert-Soak ([int]$raw.handle_measurement_start_iteration -eq $ExpectedWarmup) `
    "handle measurementがwarmup sampleを含んでいます: start=$($raw.handle_measurement_start_iteration)"
Assert-Soak ([int]$raw.quartile_size -eq [int][math]::Floor($ExpectedMeasured / 4)) `
    "quartile sizeがmeasured domainと一致しません: $($raw.quartile_size)"
Assert-Soak ($raw.handle_retention_authority_domain -eq 'MEASURED_ONLY') `
    "handle retention authorityのdomainが不正です: $($raw.handle_retention_authority_domain)"

# threshold は凍結値である。warmup 導入を理由に緩めない。
Assert-Soak ([int]$raw.handle_growth_threshold -eq 8) `
    "handle growth thresholdが変更されています: $($raw.handle_growth_threshold)"

# warmup 中も functional correctness は authority である。
Assert-Soak ($raw.functional_authority_domain -eq 'ALL_ITERATIONS') `
    "warmup中のfunctional failureが無視されています: $($raw.functional_authority_domain)"

Write-Output 'soak warmup measurement-domain contract: PASS'
