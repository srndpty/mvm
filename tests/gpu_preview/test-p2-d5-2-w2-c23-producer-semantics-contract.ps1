[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet('GoodExact','NegativeMissingSemantics','NegativeAmbiguousReverse',
        'NegativeZeroNotDuplicateCallback','NegativeZeroSingleDecision','NegativeZeroSharedToken',
        'Negative301MembershipTrue','Negative301PastSourceDomainFalse','Negative301NoFormalPresented')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Core,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. $Core
if(-not(Test-Path -LiteralPath $Directory)){New-Item -ItemType Directory -Path $Directory|Out-Null}
$sourcePath=Join-Path $Directory "$Case-c21.json"
function Decision([int]$Sequence,[string]$Ordinal,[bool]$Duplicate,[bool]$Past,[string]$Token){
    [pscustomobject][ordered]@{
        scheduler_decision_sequence=$Sequence;opportunity_ordinal=$Ordinal
        producer_scope='CURRENT_MEASUREMENT';decision_qpc=[int64](100+$Sequence)
        checker_derived_measurement_boundary_relation='WITHIN_CURRENT_MEASUREMENT'
        required_current_membership=$Ordinal-eq'0';required_current_membership_exact=$true
        producer_semantics_exact=$true;duplicate_callback=$Duplicate;repeat=$false
        past_source_domain=$Past;target_frame=[int64]$Ordinal
        last_finalized_opportunity_ordinal=$(if($Ordinal-eq'0'){-1}else{297})
        render_begin_qpc=$(if($Ordinal-eq'0'){100}else{100+$Sequence})
        token_serial=$Token;native_present_serial=$Token;formal_presented_count=1
        formal_presented_event_keys=@("$Token|$([int64](200+$Sequence))")
        formal_presented_reverse_attribution_exact=$true
    }
}
$decisions=@((Decision 0 '0' $false $false '1'),(Decision 1 '0' $true $false '2'),(Decision 2 '301' $false $true '3'))
if($Case-eq'NegativeMissingSemantics'){$decisions[1].producer_semantics_exact=$false}
if($Case-eq'NegativeAmbiguousReverse'){$decisions[1].formal_presented_reverse_attribution_exact=$false}
if($Case-eq'NegativeZeroNotDuplicateCallback'){$decisions[1].duplicate_callback=$false}
if($Case-eq'NegativeZeroSingleDecision'){$decisions=@($decisions[0],$decisions[2])}
if($Case-eq'NegativeZeroSharedToken'){
    $decisions[1].token_serial=$decisions[0].token_serial
    $decisions[1].native_present_serial=$decisions[0].native_present_serial
    $decisions[1].formal_presented_event_keys=$decisions[0].formal_presented_event_keys
}
if($Case-eq'Negative301MembershipTrue'){$decisions[2].required_current_membership=$true}
if($Case-eq'Negative301PastSourceDomainFalse'){$decisions[2].past_source_domain=$false}
if($Case-eq'Negative301NoFormalPresented'){
    $decisions[2].formal_presented_count=0
    $decisions[2].formal_presented_event_keys=@()
}
$source=[pscustomobject][ordered]@{authority_exact=$true;branch_a_established=$true;runs=@([pscustomobject][ordered]@{run=1;decisions=$decisions})}
$source|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $sourcePath -Encoding utf8
$result=Invoke-MvmC23ProducerSemanticsAttribution -C21ProofObject $source -C21ProofPath $sourcePath
if($Case-eq'GoodExact'){
    if(-not[bool]$result.authority_exact-or-not[bool]$result.ordinal_zero_duplicate_callback_attribution_exact-or
       -not[bool]$result.ordinal_301_scope_membership_conflict_attribution_exact){throw '正当なC2.3 attributionを拒否しました'}
}else{
    if([bool]$result.authority_exact){throw "不完全なC2.3 attributionを受理しました: $Case"}
    $expected=$(if($Case-eq'NegativeMissingSemantics'){'PRODUCER_SEMANTICS_PROVENANCE_MISSING'}
        elseif($Case-eq'NegativeAmbiguousReverse'){'TARGET_REVERSE_ATTRIBUTION_INVALID'}
        elseif($Case.StartsWith('NegativeZero')){'ORDINAL_ZERO_DUPLICATE_CALLBACK_ATTRIBUTION_INVALID'}
        else{'ORDINAL_301_SCOPE_MEMBERSHIP_ATTRIBUTION_INVALID'})
    if($expected-notin@($result.blockers)){throw "期待blockerがありません: $expected"}
}
Write-Output "W2-C2.3 contract $Case`: PASS"
