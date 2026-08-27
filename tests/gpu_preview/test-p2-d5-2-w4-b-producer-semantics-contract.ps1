[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'GoodIsolatedAndDouble','GoodEdgeCohorts','GoodLongerMissingRun',
        'NegativeMissingSetSplice','NegativeIsolatedClassifiedAsDouble',
        'NegativeDoubleEventCountMutation','NegativeMissingOrdinalInterpolatedProducerField',
        'NegativeNearestDecisionQpcUsed','NegativeCrossRunNeighborSplice',
        'NegativeRootCauseDeclared','NegativeAggregateOnlyTransitionForgery',
        'NegativeDecisionSpanUsedAsMeasurementWindow','NegativeLegacyElapsedPromotedToAuthority',
        'NegativeTailGapMutation')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Core,
    [Parameter(Mandatory=$true)][string]$SharedReplay
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. $Core
. $SharedReplay

# 期待値は実装の式を呼ばず fixture 側で決める。
# QPC 1 tick = 1/10000000 s。decision は 1 refresh (166805 tick) 間隔で並べる。
$frequency=10000000L
$period=166805L
$startQpc=1000000L
function Decision([string]$Ordinal,[int64]$Qpc,[int64]$TargetFrame,[int64]$LastFinalized,
                  [bool]$Duplicate=$false,[bool]$QpcExact=$true,[bool]$SemanticsExact=$true){
    return [pscustomobject][ordered]@{
        intent_ordinal=$Ordinal;intent_scope='CURRENT_MEASUREMENT'
        required_current_membership=$true;required_current_membership_exact=$true
        duplicate_callback=$Duplicate;formal_transport_eligible=$true
        decision_qpc=$Qpc;decision_qpc_exact=$QpcExact;render_begin_qpc=$Qpc
        target_frame=$TargetFrame;repeat=$false;past_source_domain=$false
        last_finalized_opportunity_ordinal=$LastFinalized;token_serial='1'
        producer_semantics_exact=$SemanticsExact
    }
}

# required 0..9。primary は 0,2,4,7,9 (2 と 5,6 が missing)。
#   missing 1        -> ISOLATED
#   missing 3        -> ISOLATED
#   missing 5,6      -> DOUBLE
#   missing 8        -> ISOLATED
$required=@('0','1','2','3','4','5','6','7','8','9')
$primaryOrdinals=@(0,2,4,7,9)
$missing=@('1','3','5','6','8')
$decisions=@()
for($index=0;$index-lt$primaryOrdinals.Count;++$index){
    $decisions+=,(Decision ([string]$primaryOrdinals[$index]) ($startQpc+$index*$period) `
        ([int64]$primaryOrdinals[$index]+2) ([int64]$primaryOrdinals[$index]-4))
}
$endQpc=$startQpc+10*$period
$legacyElapsed=[double](($primaryOrdinals.Count-1)*$period)/[double]$frequency

switch($Case){
    'GoodEdgeCohorts'{
        # 先頭と末尾が missing。HEAD_EDGE / TAIL_EDGE が precedence で先に取る。
        $required=@('0','1','2','3','4')
        $primaryOrdinals=@(1,2,3)
        $missing=@('0','4')
        $decisions=@()
        for($index=0;$index-lt$primaryOrdinals.Count;++$index){
            $decisions+=,(Decision ([string]$primaryOrdinals[$index]) ($startQpc+$index*$period) `
                ([int64]$primaryOrdinals[$index]+2) ([int64]$primaryOrdinals[$index]-1))
        }
    }
    'GoodLongerMissingRun'{
        # 3 連続 missing。LONGER_MISSING_RUN が DOUBLE より先に取る。
        $required=@('0','1','2','3','4','5')
        $primaryOrdinals=@(0,4,5)
        $missing=@('1','2','3')
        $decisions=@()
        for($index=0;$index-lt$primaryOrdinals.Count;++$index){
            $decisions+=,(Decision ([string]$primaryOrdinals[$index]) ($startQpc+$index*$period) `
                ([int64]$primaryOrdinals[$index]+2) ([int64]$primaryOrdinals[$index]-1))
        }
    }
    'NegativeMissingSetSplice'{
        # W4-A missing set と producer ledger の再構築が食い違う。
        $missing=@('1','3','5','6')
    }
    'NegativeNearestDecisionQpcUsed'{
        # 近傍 primary の decision_qpc が exact でない。delta を数値として出さない。
        $decisions[1]=Decision '2' ($startQpc+$period) 4 (-2) $false $false
    }
    'NegativeCrossRunNeighborSplice'{
        # run 末尾が missing。次 run の先頭 primary と接続してはならない。
        # missing set が整合していれば interior の近傍欠落は構造的に起こらないため、
        # ここでは「run 境界で splice しないこと」を検証する。
        $required=@('0','1','2','3')
        $primaryOrdinals=@(0,1,2)
        $missing=@('3')
        $decisions=@()
        for($index=0;$index-lt$primaryOrdinals.Count;++$index){
            $decisions+=,(Decision ([string]$primaryOrdinals[$index]) ($startQpc+$index*$period) `
                ([int64]$primaryOrdinals[$index]+2) ([int64]$primaryOrdinals[$index]-1))
        }
    }
}

$measurementEnd=$endQpc
if($Case-eq'NegativeCrossRunNeighborSplice'){$measurementEnd=$startQpc+3*$period}

$actual=Invoke-MvmW4BProducerSemantics -RequiredIntentOrdinals $required `
    -MissingOrdinals $missing -DecisionRecords $decisions `
    -MeasurementStartQpc $startQpc -MeasurementEndQpcExclusive $measurementEnd `
    -QpcFrequency $frequency -LegacyMeasurementElapsedSeconds $legacyElapsed

# --- core が fail-close すべき case ---
$coreBlockers=@{
    'NegativeMissingSetSplice'='MISSING_SET_SPLICE'
    'NegativeNearestDecisionQpcUsed'='DECISION_QPC_NOT_EXACT'
}
if($Case-eq'NegativeCrossRunNeighborSplice'){
    # run 境界の missing は TAIL_EDGE に入り、after_primary を作らないこと。
    # cross-run で埋めていれば after_primary が非 null になる。
    if(-not[bool]$actual.attribution_exact){throw "$Case が不成立です: $(@($actual.blockers)-join',')"}
    if([int64]$actual.cohort_event_counts.TAIL_EDGE-ne1){throw 'run末尾missingがTAIL_EDGEへ入っていません'}
    $tailEvent=@($actual.events|Where-Object{$_.cohort-eq'TAIL_EDGE'})[0]
    if($null-ne$tailEvent.after_primary){throw 'run境界を越えて近傍primaryを接続しています'}
    if($null-ne$tailEvent.transition){throw 'run境界でtransitionを作っています'}
    if($null-eq$tailEvent.before_primary){throw 'run内のbefore primaryが失われています'}
    Write-Output "W4-B producer semantics contract $Case : PASS";exit 0
}
if($coreBlockers.ContainsKey($Case)){
    if([bool]$actual.attribution_exact){throw "$Case をfail-closeしていません"}
    if($coreBlockers[$Case]-notin@($actual.blockers)){
        throw "$Case で期待したblockerが出ません: $($coreBlockers[$Case]) (actual=$(@($actual.blockers)-join','))"
    }
    Write-Output "W4-B producer semantics contract $Case : PASS";exit 0
}

# --- artifact 改変系。sealed producer records からの再構築と一致しないことを固定する ---
$mutations=@{
    'NegativeIsolatedClassifiedAsDouble'={param($m)
        $m.cohort_event_counts.ISOLATED_MISSING-=1
        $m.cohort_event_counts.DOUBLE_MISSING_BOUNDARY+=1}
    'NegativeDoubleEventCountMutation'={param($m)$m.cohort_event_counts.DOUBLE_MISSING_BOUNDARY+=1}
    'NegativeMissingOrdinalInterpolatedProducerField'={param($m)
        # missing 側に producer semantic field を作る。
        $m.events[0]|Add-Member -NotePropertyName interpolated_target_frame -NotePropertyValue 99}
    'NegativeRootCauseDeclared'={param($m)
        $m|Add-Member -NotePropertyName root_cause_determined -NotePropertyValue $true}
    'NegativeAggregateOnlyTransitionForgery'={param($m)
        $m.isolated_transition_summary.event_count=999}
    'NegativeDecisionSpanUsedAsMeasurementWindow'={param($m)
        $m.time_domain_diagnostic.measurement_window_seconds=
            $m.time_domain_diagnostic.primary_decision_active_span_seconds
        $m.time_domain_diagnostic.decision_span_used_as_measurement_window=$true}
    'NegativeLegacyElapsedPromotedToAuthority'={param($m)
        $m.time_domain_diagnostic.legacy_measurement_elapsed_used_as_authority=$true}
    'NegativeTailGapMutation'={param($m)
        $m.time_domain_diagnostic.tail_without_primary_decision_seconds=0.0}
}
if($mutations.ContainsKey($Case)){
    $mutated=$actual|ConvertTo-Json -Depth 24|ConvertFrom-Json
    & $mutations[$Case] $mutated
    $rejected=$false
    try{Assert-MvmW4BProof -Expected $actual -Actual $mutated}catch{$rejected=$true}
    if(-not$rejected){throw "$Case が受理されました"}
    Write-Output "W4-B producer semantics contract $Case : PASS";exit 0
}

# --- Good 系 ---
if(-not[bool]$actual.attribution_exact){throw "$Case が不成立です: $(@($actual.blockers)-join',')"}
$expected=switch($Case){
    'GoodEdgeCohorts'{@{HEAD_EDGE=1;TAIL_EDGE=1;LONGER_MISSING_RUN=0;DOUBLE_MISSING_BOUNDARY=0;ISOLATED_MISSING=0;OTHER_PATTERN=0}}
    'GoodLongerMissingRun'{@{HEAD_EDGE=0;TAIL_EDGE=0;LONGER_MISSING_RUN=1;DOUBLE_MISSING_BOUNDARY=0;ISOLATED_MISSING=0;OTHER_PATTERN=0}}
    default{@{HEAD_EDGE=0;TAIL_EDGE=0;LONGER_MISSING_RUN=0;DOUBLE_MISSING_BOUNDARY=1;ISOLATED_MISSING=3;OTHER_PATTERN=0}}
}
foreach($name in @($expected.Keys)){
    if([int64]$actual.cohort_event_counts.$name-ne[int64]$expected[$name]){
        throw "$Case の$name event countが期待値と一致しません: $($actual.cohort_event_counts.$name) (expected $($expected[$name]))"
    }
}
# event / intent identity。
if(([int64]$actual.cohort_event_counts.DOUBLE_MISSING_BOUNDARY*2)-ne
   [int64]$actual.cohort_intent_counts.DOUBLE_MISSING_BOUNDARY){
    throw "$Case でdouble event x 2 = double intent が成立しません"
}
if([int64]$actual.cohort_event_counts.ISOLATED_MISSING-ne
   [int64]$actual.cohort_intent_counts.ISOLATED_MISSING){
    throw "$Case でisolated event = isolated intent が成立しません"
}
if([int64]$actual.cohort_intent_sum-ne$missing.Count){throw "$Case のcohort intent sumがmissing countと一致しません"}
# missing 側に producer semantic field を作っていないこと。
foreach($eventRecord in @($actual.events)){
    foreach($forbidden in @('target_frame','decision_qpc','repeat','past_source_domain')){
        if($eventRecord.Contains($forbidden)){throw "$Case でmissing eventにproducer fieldを作っています: $forbidden"}
    }
}
# time-domain は W2-A window から作り、decision span を分母に昇格させないこと。
if($Case-eq'GoodIsolatedAndDouble'){
    $time=$actual.time_domain_diagnostic
    if($null-eq$time){throw 'time-domain diagnosticがありません'}
    if([bool]$time.decision_span_used_as_measurement_window){throw 'decision spanをmeasurement windowにしています'}
    if([bool]$time.legacy_measurement_elapsed_used_as_authority){throw 'legacy elapsedをauthorityにしています'}
    $expectedWindow=[double]($measurementEnd-$startQpc)/[double]$frequency
    if([double]$time.measurement_window_seconds-ne$expectedWindow){throw 'measurement windowがW2-A由来ではありません'}
    # cadence = (N-1)/span
    $expectedCadence=[double]($primaryOrdinals.Count-1)/[double]$time.primary_decision_active_span_seconds
    if([double]$time.primary_decision_interdecision_cadence_hz-ne$expectedCadence){
        throw 'cadenceが (N-1)/span で計算されていません'
    }
}
Write-Output "W4-B producer semantics contract $Case : PASS"
