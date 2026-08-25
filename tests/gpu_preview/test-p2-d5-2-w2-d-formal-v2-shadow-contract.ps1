[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'GoodExactIntegration','GoodForeignPresentedInDomain','GoodLayer1ACountDiffersFromLayer1B',
        'GoodOutOfDomainPresented','GoodEmptyPhysicalDomain',
        'NegativeRequiredIntentSetMutation','NegativeSatisfiedIntentMutation','NegativePhysicalOrdinalMutation',
        'NegativeForeignCurrentMutation','NegativeScopeMutation','NegativeFinalStateMutation',
        'NegativeMissingCompositionToken','NegativeMissingNativePresentSerial','NegativeMissingPhysicalOrdinal',
        'NegativeDuplicateExactEventKey','NegativeEmptyRequiredSet','NegativeDuplicateRequiredIntent',
        'NegativePhysicalDomainCardinalityMutation','NegativeFilledExceedsPhysicalOpportunity')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Core
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. $Core

# 期待値は実装の式を呼ばずに、この fixture 側で独立に決める。
function New-Record([string]$Key,[string]$Ordinal,[string]$Scope,[int64]$Etw,[int64]$Displayed,
                    $PhysicalOrdinal,[bool]$InDomain,[string]$FinalState='Presented',
                    $TokenSerial='100',$NativeSerial='10'){
    return [pscustomobject][ordered]@{
        exact_event_key=$Key;intent_ordinal=$Ordinal;intent_scope=$Scope
        composition_token_serial=$TokenSerial;native_present_serial=$NativeSerial
        etw_sequence=$Etw;final_state=$FinalState;displayed_qpc=$Displayed
        physical_vblank_ordinal=$PhysicalOrdinal;in_measurement_physical_domain=$InDomain
    }
}
$first=New-Record '11|110' '0' 'CURRENT_MEASUREMENT' 11 110 2 $true 'Presented' '101' '11'
$second=New-Record '12|120' '1' 'CURRENT_MEASUREMENT' 12 120 3 $true 'Presented' '102' '12'
$records=@($first,$second)
$required=@('0','1','2')
$physicalCount=[int64]3;$origin=[int64]1;$last=[int64]3

switch($Case){
    'GoodForeignPresentedInDomain'{
        $records=@($first,(New-Record '12|120' '900' 'FOREIGN_PRE_MEASUREMENT' 12 120 3 $true 'Presented' '102' '12'))
    }
    'GoodLayer1ACountDiffersFromLayer1B'{$physicalCount=[int64]10;$origin=[int64]1;$last=[int64]10}
    'GoodOutOfDomainPresented'{
        $records=@($first,(New-Record '12|120' '1' 'CURRENT_MEASUREMENT' 12 120 9 $false))
    }
    'GoodEmptyPhysicalDomain'{$records=@();$physicalCount=[int64]0;$origin=[int64]-1;$last=[int64]-1}
    'NegativeRequiredIntentSetMutation'{$required=@('0','2')}
    'NegativeSatisfiedIntentMutation'{
        $records=@($first,(New-Record '12|120' '0' 'CURRENT_MEASUREMENT' 12 120 3 $true 'Presented' '102' '12'))
    }
    'NegativePhysicalOrdinalMutation'{
        $records=@($first,(New-Record '12|120' '1' 'CURRENT_MEASUREMENT' 12 120 2 $true 'Presented' '102' '12'))
    }
    'NegativeForeignCurrentMutation'{
        $records=@($first,(New-Record '12|120' '1' 'FOREIGN_PRE_MEASUREMENT' 12 120 3 $true 'Presented' '102' '12'))
    }
    'NegativeScopeMutation'{$records[1].intent_scope='MEASUREMENT'}
    'NegativeFinalStateMutation'{$records[1].final_state='Discarded'}
    'NegativeMissingCompositionToken'{$records[1].composition_token_serial=$null}
    'NegativeMissingNativePresentSerial'{$records[1].native_present_serial=$null}
    'NegativeMissingPhysicalOrdinal'{$records[1].physical_vblank_ordinal=$null}
    'NegativeDuplicateExactEventKey'{$records[1].exact_event_key='11|110'}
    'NegativeEmptyRequiredSet'{$required=@()}
    'NegativeDuplicateRequiredIntent'{$required=@('0','1','1')}
    'NegativePhysicalDomainCardinalityMutation'{$physicalCount=[int64]5}
    'NegativeFilledExceedsPhysicalOpportunity'{$physicalCount=[int64]1;$origin=[int64]3;$last=[int64]3}
}

$integration=Invoke-MvmDFormalV2ShadowIntegration -FormalPresentedRecords $records `
    -RequiredIntentOrdinals $required -PhysicalOpportunityCount $physicalCount `
    -PhysicalDomainOriginOrdinal $origin -PhysicalDomainLastOrdinal $last
$blockers=@($integration.blockers)

$expectedBlockers=@(switch($Case){
    'NegativeRequiredIntentSetMutation'{@('CURRENT_INTENT_OUTSIDE_REQUIRED_INTENT_SET')}
    'NegativeSatisfiedIntentMutation'{@('DUPLICATE_SATISFIED_INTENT')}
    'NegativePhysicalOrdinalMutation'{@('MULTIPLE_FORMAL_PRESENTED_PER_PHYSICAL_ORDINAL','FILLED_PHYSICAL_OPPORTUNITY_IDENTITY_VIOLATION')}
    'NegativeForeignCurrentMutation'{@('FOREIGN_INTENT_INSIDE_REQUIRED_INTENT_SET')}
    'NegativeScopeMutation'{@('INTENT_SCOPE_INVALID')}
    'NegativeFinalStateMutation'{@('FORMAL_V2_FINAL_STATE_NOT_PRESENTED')}
    'NegativeMissingCompositionToken'{@('FORMAL_V2_CHAIN_PROVENANCE_MISSING')}
    'NegativeMissingNativePresentSerial'{@('FORMAL_V2_CHAIN_PROVENANCE_MISSING')}
    'NegativeMissingPhysicalOrdinal'{@('FORMAL_V2_CHAIN_PROVENANCE_MISSING')}
    'NegativeDuplicateExactEventKey'{@('FORMAL_V2_EXACT_EVENT_KEY_INVALID')}
    'NegativeEmptyRequiredSet'{@('REQUIRED_INTENT_SET_EMPTY')}
    'NegativeDuplicateRequiredIntent'{@('REQUIRED_INTENT_SET_DUPLICATE')}
    'NegativePhysicalDomainCardinalityMutation'{@('PHYSICAL_VBLANK_DOMAIN_CARDINALITY_INVALID')}
    'NegativeFilledExceedsPhysicalOpportunity'{@('FILLED_EXCEEDS_PHYSICAL_VBLANK_OPPORTUNITY')}
    default{@()}
})
foreach($expected in $expectedBlockers){
    if($expected-notin$blockers){throw "$Case で期待したblockerが出ません: $expected (actual=$($blockers-join', '))"}
}
if($expectedBlockers.Count-ne0){
    if([bool]$integration.integration_exact){throw "$Case をfail-closeしていません"}
    Write-Output "W2-D formal-v2 shadow contract $Case`: PASS";exit 0
}
if(-not[bool]$integration.integration_exact){throw "$Case が不成立です: $($blockers-join', ')"}

# Good 系は accounting を fixture 側の期待値と突き合わせる。
$expectedCounts=switch($Case){
    'GoodForeignPresentedInDomain'{@{required=3;satisfied=1;unsatisfied=2;inDomain=2;foreign=1;filled=2;physical=3}}
    'GoodLayer1ACountDiffersFromLayer1B'{@{required=3;satisfied=2;unsatisfied=1;inDomain=2;foreign=0;filled=2;physical=10}}
    'GoodOutOfDomainPresented'{@{required=3;satisfied=1;unsatisfied=2;inDomain=1;foreign=0;filled=1;physical=3}}
    'GoodEmptyPhysicalDomain'{@{required=3;satisfied=0;unsatisfied=3;inDomain=0;foreign=0;filled=0;physical=0}}
    default{@{required=3;satisfied=2;unsatisfied=1;inDomain=2;foreign=0;filled=2;physical=3}}
}
foreach($identity in @(
    @('required_intent_count',$expectedCounts.required),
    @('satisfied_intent_count',$expectedCounts.satisfied),
    @('unsatisfied_intent_count',$expectedCounts.unsatisfied),
    @('in_domain_presented_event_count',$expectedCounts.inDomain),
    @('in_domain_presented_foreign_intent_count',$expectedCounts.foreign),
    @('filled_physical_opportunity_count',$expectedCounts.filled),
    @('physical_vblank_opportunity_count',$expectedCounts.physical))){
    if([int64]$integration[$identity[0]]-ne[int64]$identity[1]){
        throw "$Case のaccountingが期待値と一致しません: $($identity[0]) (expected=$($identity[1]) actual=$($integration[$identity[0]]))"
    }
}
if([int64]$integration.required_intent_count-ne([int64]$integration.satisfied_intent_count+[int64]$integration.unsatisfied_intent_count)){
    throw "$Case のLayer 1A accounting identityが成立しません"
}
if(([int64]$integration.satisfied_intent_count+[int64]$integration.in_domain_presented_foreign_intent_count)-ne
   [int64]$integration.in_domain_presented_event_count){
    throw "$Case のPresented accounting identityが成立しません"
}
foreach($record in @($integration.records)){
    if($record.PSObject.Properties.Name-contains'source_frame'){throw 'recordにsource frame identityがあります'}
}
Write-Output "W2-D formal-v2 shadow contract $Case`: PASS"
