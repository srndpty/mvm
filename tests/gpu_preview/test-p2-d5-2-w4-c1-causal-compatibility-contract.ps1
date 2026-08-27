[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'GoodCompatibility','NegativeCompletedOrdinalMutation','NegativeIntentDeltaMutation',
        'NegativeNearestQpcBinding','NegativeSourceRequiredDomainConflation',
        'NegativeCandidateForced','NegativePlannedEndHeuristic',
        'NegativeMissingStateInterpolation','NegativeRootCauseDeclared')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Core
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. $Core

function Decision([string]$Ordinal,[int64]$Qpc,[bool]$PastSource=$false){
    return [pscustomobject][ordered]@{
        intent_ordinal=$Ordinal;intent_scope='CURRENT_MEASUREMENT'
        required_current_membership=$true;required_current_membership_exact=$true
        duplicate_callback=$false;decision_qpc=$Qpc;decision_qpc_exact=$true
        render_begin_qpc=$Qpc;past_source_domain=$PastSource
        transport_disposition='TRANSPORT'
    }
}
function Authority([uint64]$Refresh){
    return [pscustomobject][ordered]@{available=$true;refresh_count=[string]$Refresh
        qpc_vblank=1;refresh_numerator=60;refresh_denominator=1}
}
function Ledger([string]$Ordinal,[int64]$Qpc,[uint64]$Refresh){
    return [pscustomobject][ordered]@{predicted_opportunity_ordinal=$Ordinal
        render_begin_qpc=$Qpc;pre_render_authority=(Authority $Refresh)}
}
function Transition([string]$Before,[string]$After,[string]$Delta){
    return [pscustomobject][ordered]@{cohort='ISOLATED_MISSING'
        before_primary=[ordered]@{intent_ordinal=$Before}
        after_primary=[ordered]@{intent_ordinal=$After}
        transition=[ordered]@{primary_decision_intent_ordinal_delta=$Delta}}
}

$decisions=@((Decision '0' 100),(Decision '2' 200),(Decision '5' 300),(Decision '7' 400 $true))
# origin=10。0はunanchored、2はcompleted=1、5はcompleted=4。terminal 7はcommitされずledgerなし。
$ledger=@((Ledger '0' 100 8),(Ledger '2' 200 11),(Ledger '5' 300 14))
$events=@((Transition '0' '2' '2'),(Transition '2' '5' '3'),(Transition '5' '7' '2'))
switch($Case){
    'NegativeCompletedOrdinalMutation'{$ledger[2].pre_render_authority.refresh_count='13'}
    'NegativeIntentDeltaMutation'{$events[1].transition.primary_decision_intent_ordinal_delta='2'}
    'NegativeNearestQpcBinding'{$ledger[1].render_begin_qpc=201}
}
$actual=Invoke-MvmW4C1CausalCompatibility -DecisionRecords $decisions -SchedulerLedger $ledger `
    -W4BEvents $events -OriginRefreshCount 10 -MeasurementStartQpc 50 `
    -MeasurementEndQpcExclusive 1000 -QpcFrequency 100 -MeasurementStopCaptured $true

$coreFailures=@('NegativeCompletedOrdinalMutation','NegativeIntentDeltaMutation',
    'NegativeNearestQpcBinding')
if($Case-in$coreFailures){
    if([bool]$actual.attribution_exact){throw "$Case をfail-closeしていません"}
    Write-Output "W4-C1 causal compatibility contract $Case : PASS";exit 0
}
if(-not[bool]$actual.attribution_exact){throw "$Case が不成立です: $(@($actual.blockers)-join', ')"}

$mutations=@{
    'NegativeSourceRequiredDomainConflation'={param($proof)
        $proof.source_domain_required_domain_conflated=$true}
    'NegativeCandidateForced'={param($proof)
        $proof.cause_b_candidates.DOMAIN_TERMINAL.status='EXECUTED_EXACT'}
    'NegativePlannedEndHeuristic'={param($proof)
        $proof.cause_b_candidates.PLANNED_WINDOW_END.tolerance_or_elapsed_heuristic_used=$true}
    'NegativeMissingStateInterpolation'={param($proof)
        $proof.missing_state_interpolated=$true}
    'NegativeRootCauseDeclared'={param($proof)
        $proof.root_cause_determined=$true}
}
if($mutations.ContainsKey($Case)){
    $mutated=$actual|ConvertTo-Json -Depth 24|ConvertFrom-Json
    & $mutations[$Case] $mutated
    $rejected=$false
    try{Assert-MvmW4C1Proof -Expected $actual -Actual $mutated}catch{$rejected=$true}
    if(-not$rejected){throw "$Case が受理されました"}
    Write-Output "W4-C1 causal compatibility contract $Case : PASS";exit 0
}

if([int64]$actual.compatible_transition_count-ne1-or
   [int64]$actual.not_observable_transition_count-ne2-or
   [int64]$actual.incompatible_transition_count-ne0){throw 'transition partitionが期待値と一致しません'}
if([string]$actual.delta_compatibility_counts.'3'-ne'1'){throw 'delta +3 compatibilityがありません'}
if([string]$actual.cause_b_candidates.DOMAIN_TERMINAL.status-ne'EXACT_CAUSAL_COMPATIBILITY'){
    throw 'source-domain terminal compatibilityが成立しません'
}
if(-not[bool]$actual.cause_b_candidates.DOMAIN_TERMINAL.required_intent_membership-or
   [bool]$actual.source_domain_required_domain_conflated){
    throw 'source domainとrequired intent domainが分離されていません'
}
if([string]$actual.cause_b_candidates.PLANNED_WINDOW_END.status-ne'INCOMPATIBLE'){
    throw 'planned window endをexact QPCで棄却していません'
}
if([string]$actual.cause_b_candidates.EXPLICIT_STOP.status-ne'NOT_OBSERVABLE'){
    throw 'explicit stopを証拠なしに確定しています'
}
Write-Output "W4-C1 causal compatibility contract $Case : PASS"
