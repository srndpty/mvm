[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'GoodInDomain','GoodForeignIntentDisplayedInDomain','GoodUpperCurrentStraddlesEnd',
        'GoodDisplayedOutsideDomain','GoodExactBoundaries','GoodCausalCellProperty','NegativeMissingDisplayedQpc',
        'NegativeMultipleDisplayedQpc','NegativeNoPhysicalMapping',
        'NegativeAmbiguousPhysicalMapping','NegativeDuplicatePresentedPhysicalOrdinal',
        'NegativePhysicalAuthority','NegativeEtwLoss','NegativeMappingProvenanceMutation')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Core
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. $Core
function Candidate([int64]$Sequence,$Displayed,[string]$Scope='CURRENT_MEASUREMENT',[bool]$Layer2=$true){
    $value=[ordered]@{
        etw_sequence=$Sequence;native_present_serial=[string](100+$Sequence)
        composition_token_serial=[string](200+$Sequence);intent_ordinal=[string]$Sequence
        intent_scope=$Scope;layer2_cohort_member=$Layer2
    }
    if($null-ne$Displayed){$value.displayed_qpc=$Displayed}
    return [pscustomobject]$value
}
$samples=@(
    [pscustomobject]@{ordinal=0;qpc=100},[pscustomobject]@{ordinal=1;qpc=200},
    [pscustomobject]@{ordinal=2;qpc=300},[pscustomobject]@{ordinal=3;qpc=400})
$candidates=@(Candidate 1 150);$physicalValid=$true;$etwLost=0;$expectedBlocker=$null
$predecessor=0L;$successor=3L;$origin=1L;$last=2L
switch($Case){
    'GoodForeignIntentDisplayedInDomain'{$candidates=@(Candidate 1 150 'FOREIGN_PRE_MEASUREMENT' $false)}
    'GoodUpperCurrentStraddlesEnd'{$candidates=@(Candidate 1 250 'CURRENT_MEASUREMENT' $false)}
    'GoodDisplayedOutsideDomain'{$candidates=@(Candidate 1 350)}
    'GoodExactBoundaries'{
        $candidates=@();$candidates+=Candidate 1 100;$candidates+=Candidate 2 200;$candidates+=Candidate 3 400
    }
    'GoodCausalCellProperty'{
        $samples=@();$candidates=@()
        foreach($ordinal in 0..20){
            $samples+=,[pscustomobject]@{ordinal=$ordinal;qpc=1000+$ordinal*100}
            $qpc=if($ordinal-eq0){1000}else{1000+$ordinal*100-1}
            $candidates+=Candidate $ordinal $qpc
        }
        $successor=20;$last=19
    }
    'NegativeMissingDisplayedQpc'{$candidates=@(Candidate 1 $null);$expectedBlocker='DISPLAYED_QPC_CARDINALITY_INVALID'}
    'NegativeMultipleDisplayedQpc'{$candidates=@(Candidate 1 @(150,160));$expectedBlocker='DISPLAYED_QPC_CARDINALITY_INVALID'}
    'NegativeNoPhysicalMapping'{$candidates=@(Candidate 1 50);$expectedBlocker='PHYSICAL_MAPPING_MISSING'}
    'NegativeAmbiguousPhysicalMapping'{
        $samples=@([pscustomobject]@{ordinal=0;qpc=100},[pscustomobject]@{ordinal=1;qpc=200},
            [pscustomobject]@{ordinal=2;qpc=200},[pscustomobject]@{ordinal=3;qpc=400})
        $candidates=@(Candidate 1 200);$expectedBlocker='PHYSICAL_MAPPING_AMBIGUOUS'
    }
    'NegativeDuplicatePresentedPhysicalOrdinal'{
        $candidates=@();$candidates+=Candidate 1 150;$candidates+=Candidate 2 160
        $expectedBlocker='DUPLICATE_PRESENTED_PHYSICAL_ORDINAL'
    }
    'NegativePhysicalAuthority'{$physicalValid=$false;$expectedBlocker='PHYSICAL_AUTHORITY_INVALID'}
    'NegativeEtwLoss'{$etwLost=1;$expectedBlocker='ETW_AUTHORITY_INVALID'}
}
$actual=Invoke-MvmDisplayedQpcPhysicalMapping -Candidates $candidates -Samples $samples `
    -PredecessorOrdinal $predecessor -SuccessorOrdinal $successor -OriginOrdinal $origin -LastOrdinal $last `
    -PhysicalAuthorityValid $physicalValid -EtwEventsLost $etwLost
if($Case-eq'NegativeMappingProvenanceMutation'){
    $mutated=$actual|ConvertTo-Json -Depth 10|ConvertFrom-Json
    $mutated.records[0].physical_vblank_provenance='MUTATED'
    $rejected=$false
    try{Assert-MvmDisplayedQpcPhysicalMapping -Expected $actual -Actual $mutated}catch{$rejected=$true}
    if(-not$rejected){throw 'mapping provenance mutationが受理されました'}
}elseif($Case-like'Negative*'){
    if([bool]$actual.mapping_exact-or$expectedBlocker-notin@($actual.blockers)){
        throw "$Case がfail-closeされませんでした: $(@($actual.blockers)-join',')"
    }
}else{
    if(-not[bool]$actual.mapping_exact-or$actual.mapped_exact_count-ne$candidates.Count){throw "$Case exact mappingが成立しません"}
    switch($Case){
        'GoodInDomain'{if($actual.in_domain_presented_event_count-ne1){throw 'in-domain mappingが不正です'}}
        'GoodForeignIntentDisplayedInDomain'{if(-not$actual.records[0].in_measurement_physical_domain-or$actual.records[0].intent_scope-ne'FOREIGN_PRE_MEASUREMENT'){throw 'foreign physical fill候補が保持されません'}}
        'GoodUpperCurrentStraddlesEnd'{if(-not$actual.records[0].in_measurement_physical_domain-or$actual.records[0].layer2_cohort_member){throw 'upper current straddle mappingが不正です'}}
        'GoodDisplayedOutsideDomain'{if($actual.out_of_domain_presented_event_count-ne1){throw 'out-of-domain mappingが不正です'}}
        'GoodExactBoundaries'{
            if((@($actual.records|ForEach-Object{$_.physical_vblank_ordinal})-join',')-ne'0,1,3'){throw 'closed support boundary mappingが不正です'}
        }
        'GoodCausalCellProperty'{
            if((@($actual.records|ForEach-Object{$_.physical_vblank_ordinal})-join',')-ne(0..20-join',')){
                throw 'closed causal cell propertyが全ordinalで成立しません'
            }
        }
    }
}
Write-Output "W2-C1 $Case contract: PASS"
