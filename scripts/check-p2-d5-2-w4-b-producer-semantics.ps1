[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Proof,
    [string]$SourceRoot=(Split-Path -Parent $PSScriptRoot),
    [string]$W4AChecker=(Join-Path $PSScriptRoot 'check-p2-d5-2-w4-a-intent-attribution.ps1'),
    [switch]$SkipW4AReplay,
    [string]$WorkDirectory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
if(-not(Test-Path -LiteralPath $Proof)){Fail "W4-B proofがありません: $Proof"}
$actual=Get-Content -LiteralPath $Proof -Raw -Encoding utf8|ConvertFrom-Json
if([string]$actual.schema-ne'mvm-p2-d5-2-w4-b-producer-semantics-1'){Fail 'W4-B schemaが不正です'}

# W4-B は attribution の段であり root cause の段ではない。
foreach($falseFlag in @('root_cause_determined','new_capture_performed',
                        'producer_instrumentation_changed','missing_ordinal_producer_field_interpolated',
                        'nearest_decision_qpc_used','cross_run_neighbor_splice_used',
                        'legacy_measurement_elapsed_used_as_authority',
                        'decision_span_used_as_measurement_window')){
    if($actual.PSObject.Properties.Name-notcontains$falseFlag){Fail "W4-B flagがありません: $falseFlag"}
    if([bool]$actual.$falseFlag){Fail "W4-Bで禁止されているflagがtrueです: $falseFlag"}
}
if(-not[bool]$actual.attribution_only){Fail 'W4-Bはattribution onlyでなければなりません'}
if(-not[bool]$actual.run_level_time_domain_diagnostic_present){Fail 'run-level time-domain diagnosticがありません'}

# provenance splice。
if($actual.PSObject.Properties.Name-notcontains'source_w4a_proof'){Fail 'W4-B provenanceがありません: source_w4a_proof'}
$w4aPath=[string]$actual.source_w4a_proof
if(-not(Test-Path -LiteralPath $w4aPath)){Fail "W4-A proofがありません: $w4aPath"}
$w4aHash=(Get-FileHash -LiteralPath $w4aPath -Algorithm SHA256).Hash.ToLowerInvariant()
if([string]$actual.source_w4a_proof_sha256-ne$w4aHash){Fail 'W4-Bが別のW4-A proofを参照しています'}

# cohort identity を artifact 上でも先に検査する。
$cohortNames=@('HEAD_EDGE','TAIL_EDGE','LONGER_MISSING_RUN','DOUBLE_MISSING_BOUNDARY',
    'ISOLATED_MISSING','OTHER_PATTERN')
$intentSum=0L
foreach($name in $cohortNames){
    if($actual.cohort_intent_counts.PSObject.Properties.Name-notcontains$name){Fail "W4-B cohortがありません: $name"}
    $intentSum+=[int64]$actual.cohort_intent_counts.$name
}
if($intentSum-ne[int64]$actual.missing_intent_count){Fail 'W4-B cohort intent sumがmissing countと一致しません'}
if([int64]$actual.missing_intent_count-ne[int64]$actual.w4a_unsatisfied_intent_count){
    Fail 'W4-B missing countがW4-A unsatisfied countと一致しません'
}
if(([int64]$actual.cohort_event_counts.DOUBLE_MISSING_BOUNDARY*2)-ne
   [int64]$actual.cohort_intent_counts.DOUBLE_MISSING_BOUNDARY){
    Fail 'double event count x 2 = double intent count が成立しません'
}
if([int64]$actual.cohort_event_counts.ISOLATED_MISSING-ne
   [int64]$actual.cohort_intent_counts.ISOLATED_MISSING){
    Fail 'isolated event count = isolated intent count が成立しません'
}
# time-domain summary が decision span を measurement window にしていないこと。
foreach($run in @($actual.runs)){
    $time=$run.time_domain_diagnostic
    if($null-eq$time){Fail "run $($run.run) のtime-domain diagnosticがありません"}
    if([double]$time.measurement_window_seconds-le[double]$time.primary_decision_active_span_seconds-and
       [double]$time.tail_without_primary_decision_seconds-gt0){
        Fail "run $($run.run) のmeasurement windowがdecision spanから作られています"
    }
    if([bool]$time.legacy_measurement_elapsed_used_as_authority-or
       [bool]$time.decision_span_used_as_measurement_window){
        Fail "run $($run.run) でlegacy/decision spanをauthorityにしています"
    }
}

# time-domain も artifact aggregate を信用しない。sealed producer QPC と
# W2-A physical window から active_span / cadence / head / tail / fraction / legacy delta を
# 独立再計算する。
$w4a=Get-Content -LiteralPath $w4aPath -Raw -Encoding utf8|ConvertFrom-Json
$cohortDirectory=[string]$w4a.source_cohort_directory
foreach($run in @($actual.runs)){
    $runNumber=[int]$run.run
    $appPath=Join-Path (Join-Path $cohortDirectory "run-$runNumber") 'traced-app.json'
    if(-not(Test-Path -LiteralPath $appPath)){Fail "run $runNumber のtraced-appがありません: $appPath"}
    $app=Get-Content -LiteralPath $appPath -Raw -Encoding utf8|ConvertFrom-Json
    $shadow=$app.presentation_opportunity.physical_vblank_domain_shadow
    $frequency=[int64]$app.formal_qpc_frequency
    $startQpc=[int64]$shadow.measurement_start_qpc
    $endQpc=[int64]$shadow.measurement_end_qpc_exclusive
    $requiredSet=@{}
    foreach($ordinal in @(($w4a.runs|Where-Object{[int]$_.run-eq$runNumber}).required_intent_ordinals)){
        $requiredSet[[string]([uint64]$ordinal)]=$true
    }
    $primaryQpc=@()
    foreach($decision in @($app.native_present_hook.intent_scope_provenance.records)){
        if([string]$decision.intent_scope-ne'CURRENT_MEASUREMENT'){continue}
        if(-not[bool]$decision.required_current_membership_exact){continue}
        if([bool]$decision.duplicate_callback){continue}
        if(-not[bool]$decision.required_current_membership){continue}
        if(-not$requiredSet.ContainsKey([string]([uint64]$decision.intent_ordinal))){continue}
        $primaryQpc+=,[int64]$decision.decision_qpc
    }
    $primaryQpc=@($primaryQpc|Sort-Object)
    if($primaryQpc.Count-lt2){Fail "run $runNumber のprimary decisionが2件未満です"}
    $time=$run.time_domain_diagnostic
    $spanQpc=$primaryQpc[-1]-$primaryQpc[0]
    $spanSeconds=[double]$spanQpc/[double]$frequency
    $expectedTime=[ordered]@{
        primary_decision_first_qpc=$primaryQpc[0]
        primary_decision_last_qpc=$primaryQpc[-1]
        primary_decision_active_span_qpc=$spanQpc
        primary_decision_active_span_seconds=$spanSeconds
        primary_decision_count=$primaryQpc.Count
        primary_decision_interdecision_cadence_hz=([double]($primaryQpc.Count-1)/$spanSeconds)
        measurement_window_seconds=([double]($endQpc-$startQpc)/[double]$frequency)
        head_without_primary_decision_seconds=([double]($primaryQpc[0]-$startQpc)/[double]$frequency)
        tail_without_primary_decision_seconds=([double]($endQpc-$primaryQpc[-1])/[double]$frequency)
        legacy_elapsed_minus_producer_span_seconds=([double]$app.measurement_elapsed_seconds-$spanSeconds)
    }
    $expectedTime['primary_decision_active_span_fraction']=
        $spanSeconds/[double]$expectedTime.measurement_window_seconds
    foreach($field in @($expectedTime.Keys)){
        if([string]$time.$field-ne[string]$expectedTime[$field]){
            Fail "run $runNumber のtime-domainがsealed sourceからの再計算と一致しません: $field"
        }
    }
}

if([string]::IsNullOrWhiteSpace($WorkDirectory)){
    $WorkDirectory=Join-Path (Split-Path -Parent (Resolve-Path -LiteralPath $Proof).Path) `
        ((Split-Path -Leaf $Proof)+'.w4b-check')
}
. (Join-Path $PSScriptRoot 'p2-d5-2-w4-b-shared-replay.ps1')
$expected=Invoke-MvmW4BFromAttribution -W4AProofPath $w4aPath -SourceRoot $SourceRoot `
    -W4AChecker $W4AChecker -WorkDirectory $WorkDirectory -ReplayW4AChecker (-not $SkipW4AReplay)
Assert-MvmW4BProof -Expected $expected -Actual $actual
if(-not[bool]$expected.attribution_exact){Fail "W4-B attributionがINVALIDです: $(@($expected.blockers)-join', ')"}
Write-Host ("P2-D5-2 W4-B checker: PASS missing={0} isolated={1} double={2}" -f `
    $expected.missing_intent_count,$expected.cohort_event_counts.ISOLATED_MISSING,`
    $expected.cohort_event_counts.DOUBLE_MISSING_BOUNDARY)
