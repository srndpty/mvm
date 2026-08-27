[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'GoodRepeatedSourceFrameDifferentIntents','GoodForeignPresentedInDomain',
        'GoodCurrentIntentSatisfiedOnce','NegativeDuplicateCurrentIntent',
        'NegativeCurrentIntentOutsideRequiredDomain','NegativeMissingIntentOrdinal',
        'NegativeMissingIntentScope','NegativeIntentScopeNotExact',
        'NegativeTwoPresentedSamePhysicalOrdinal','NegativeSatisfiedForeignMiscount',
        'NegativeAccountingIdentityMutation','NegativePhysicalFillCountMutation')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Core
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. $Core
function Record([long]$Sequence,[string]$Scope,$Ordinal,[long]$Physical,[long]$SourceFrame){
    [pscustomobject][ordered]@{
        exact_event_key="$Sequence|$($Sequence*100)";etw_sequence=$Sequence
        intent_scope=$Scope;intent_scope_exact=$true;intent_ordinal=$Ordinal
        intent_ordinal_valid=$true;intent_ordinal_exact=$true
        physical_vblank_ordinal=$Physical;in_measurement_physical_domain=$true
        # C2がこの値を見ないことをGoodRepeatedSourceFrameDifferentIntentsで固定する。
        source_frame_identity=$SourceFrame
    }
}
$formal=@(Record 1 'CURRENT_MEASUREMENT' '10' 100 5)
$required=@('10','11')
switch($Case){
    'GoodRepeatedSourceFrameDifferentIntents'{$formal+=, (Record 2 'CURRENT_MEASUREMENT' '11' 101 5)}
    'GoodForeignPresentedInDomain'{$formal+=, (Record 2 'FOREIGN_PRE_MEASUREMENT' '99' 101 8)}
    'GoodCurrentIntentSatisfiedOnce'{}
    'NegativeDuplicateCurrentIntent'{$formal+=, (Record 2 'CURRENT_MEASUREMENT' '10' 101 5)}
    'NegativeCurrentIntentOutsideRequiredDomain'{$formal[0].intent_ordinal='9'}
    'NegativeMissingIntentOrdinal'{$formal[0].intent_ordinal=$null;$formal[0].intent_ordinal_valid=$false;$formal[0].intent_ordinal_exact=$false}
    'NegativeMissingIntentScope'{$formal[0].intent_scope=$null;$formal[0].intent_scope_exact=$false}
    'NegativeIntentScopeNotExact'{$formal[0].intent_scope_exact=$false}
    'NegativeTwoPresentedSamePhysicalOrdinal'{$formal+=, (Record 2 'CURRENT_MEASUREMENT' '11' 100 6)}
}
$ledger=Invoke-MvmC2IntentSatisfactionLedger -FormalPresentedRecords $formal `
    -RequiredCurrentIntentOrdinals $required
$good=$Case.StartsWith('Good')
if($good){
    if(-not[bool]$ledger.ledger_exact){throw "正当なC2 ledgerを拒否しました: $(@($ledger.blockers)-join', ')"}
    if($Case-eq'GoodRepeatedSourceFrameDifferentIntents'-and[int]$ledger.satisfied_intent_count-ne2){
        throw '同一source frameの異なるintentを別々にsatisfiedとして数えていません'
    }
    if($Case-eq'GoodForeignPresentedInDomain'-and(
        [int]$ledger.satisfied_intent_count-ne1-or[int]$ledger.in_domain_presented_foreign_intent_count-ne1)){
        throw 'FOREIGN Presentedのphysical fill / current satisfaction分離が不正です'
    }
}elseif($Case-in@('NegativeSatisfiedForeignMiscount','NegativeAccountingIdentityMutation','NegativePhysicalFillCountMutation')){
    if(-not[bool]$ledger.ledger_exact){throw 'mutation前のledgerが不正です'}
    $actual=$ledger|ConvertTo-Json -Depth 10|ConvertFrom-Json
    if($Case-eq'NegativeSatisfiedForeignMiscount'){$actual.in_domain_presented_foreign_intent_count=1}
    elseif($Case-eq'NegativeAccountingIdentityMutation'){$actual.satisfaction_accounting_identity_exact=$false}
    else{$actual.filled_physical_opportunity_count=[int64]$actual.filled_physical_opportunity_count+1}
    $failed=$false;try{Assert-MvmC2IntentSatisfactionLedger -Expected $ledger -Actual $actual}catch{$failed=$true}
    if(-not$failed){throw "改変C2 ledgerを受理しました: $Case"}
}else{
    if([bool]$ledger.ledger_exact){throw "壊れたC2 ledgerを受理しました: $Case"}
    $expectedBlocker=switch($Case){
        'NegativeDuplicateCurrentIntent'{'DUPLICATE_CURRENT_INTENT_SATISFACTION'}
        'NegativeCurrentIntentOutsideRequiredDomain'{'CURRENT_INTENT_OUTSIDE_REQUIRED_DOMAIN'}
        'NegativeMissingIntentOrdinal'{'INTENT_PROVENANCE_MISSING'}
        'NegativeMissingIntentScope'{'INTENT_PROVENANCE_MISSING'}
        'NegativeIntentScopeNotExact'{'INTENT_PROVENANCE_AMBIGUOUS'}
        'NegativeTwoPresentedSamePhysicalOrdinal'{'MULTIPLE_FORMAL_PRESENTED_PER_PHYSICAL_ORDINAL'}
    }
    if($expectedBlocker-notin@($ledger.blockers)){throw "期待blockerがありません: $expectedBlocker"}
}
Write-Output "W2-C2 contract $Case`: PASS"
