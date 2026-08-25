[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'GoodInDomain','GoodForeignIntentDisplayedInDomain','GoodUpperCurrentStraddlesEnd',
        'GoodDisplayedOutsideDomain','GoodExactBoundaries','GoodCausalCellProperty',
        'GoodHeadOutsideSupportNonFormal','GoodTailOutsideSupportNonFormal','GoodTailInsideSupportByMargin',
        'NegativeMissingDisplayedQpc','NegativeMultipleDisplayedQpc',
        'NegativeFormalPresentedOutsideSupportHead','NegativeFormalPresentedOutsideSupportTail',
        'NegativeInDomainPresentedOutsideSupport','NegativeInsideSupportMissingMappingMarkedOutside',
        'NegativeOutsideSupportTailCountMutation','NegativeSuccessorMutationMakesCandidateInside',
        'NegativeAmbiguousPhysicalMapping','NegativeDuplicatePresentedPhysicalOrdinal',
        'NegativePhysicalAuthority','NegativeEtwLoss','NegativeMissingNativeExact',
        'NegativeMissingIntentExact','NegativeMissingIntentScopeExact',
        'NegativeMappingProvenanceMutation')][string]$Case,
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
        native_exact=$true;intent_exact=$true;intent_scope_exact=$true
    }
    if($null-ne$Displayed){$value.displayed_qpc=$Displayed}
    return [pscustomobject]$value
}
$samples=@(
    [pscustomobject]@{ordinal=0;qpc=100},[pscustomobject]@{ordinal=1;qpc=200},
    [pscustomobject]@{ordinal=2;qpc=300},[pscustomobject]@{ordinal=3;qpc=400})
$candidates=@(Candidate 1 150);$physicalValid=$true;$etwLost=0;$expectedBlocker=$null
$predecessor=0L;$successor=3L;$origin=1L;$last=2L
# exact mapping support は support sample の [first.qpc, last.qpc] = [100, 400]。
# support 外を許すのは formal でなく in-domain でもない場合だけである。
$requireInsideSupport=$false
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
    # support 外 (head/tail) は mapping を要求しない。non-formal かつ out-of-domain なら PASS。
    'GoodHeadOutsideSupportNonFormal'{$candidates=@(Candidate 1 50)}
    'GoodTailOutsideSupportNonFormal'{$candidates=@(Candidate 1 500)}
    # fresh-7 型: successor のわずか内側は mapping required のまま exact であること。
    'GoodTailInsideSupportByMargin'{$candidates=@(Candidate 1 400)}
    'NegativeFormalPresentedOutsideSupportHead'{
        $candidates=@(Candidate 1 50);$requireInsideSupport=$true
        $expectedBlocker='FORMAL_PRESENTED_OUTSIDE_MAPPING_SUPPORT'
    }
    'NegativeFormalPresentedOutsideSupportTail'{
        $candidates=@(Candidate 1 500);$requireInsideSupport=$true
        $expectedBlocker='FORMAL_PRESENTED_OUTSIDE_MAPPING_SUPPORT'
    }
    'NegativeInDomainPresentedOutsideSupport'{$candidates=@(Candidate 1 500)}
    'NegativeInsideSupportMissingMappingMarkedOutside'{$candidates=@(Candidate 1 150)}
    'NegativeOutsideSupportTailCountMutation'{$candidates=@(Candidate 1 500)}
    'NegativeSuccessorMutationMakesCandidateInside'{$candidates=@(Candidate 1 500)}
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
    'NegativeMissingNativeExact'{
        $candidates[0].PSObject.Properties.Remove('native_exact')
        $expectedBlocker='UPSTREAM_CANDIDATE_AUTHORITY_INVALID'
    }
    'NegativeMissingIntentExact'{
        $candidates[0].PSObject.Properties.Remove('intent_exact')
        $expectedBlocker='UPSTREAM_CANDIDATE_AUTHORITY_INVALID'
    }
    'NegativeMissingIntentScopeExact'{
        $candidates[0].PSObject.Properties.Remove('intent_scope_exact')
        $expectedBlocker='UPSTREAM_CANDIDATE_AUTHORITY_INVALID'
    }
}
$actual=Invoke-MvmDisplayedQpcPhysicalMapping -Candidates $candidates -Samples $samples `
    -PredecessorOrdinal $predecessor -SuccessorOrdinal $successor -OriginOrdinal $origin -LastOrdinal $last `
    -PhysicalAuthorityValid $physicalValid -EtwEventsLost $etwLost `
    -RequireAllCandidatesInsideSupport $requireInsideSupport
# artifact の outside 判定を信用せず、sealed sample からの再構築と一致させる。
$supportMutations=@{
    'NegativeInDomainPresentedOutsideSupport'={param($m)$m.records[0].in_measurement_physical_domain=$true}
    'NegativeInsideSupportMissingMappingMarkedOutside'={param($m)
        $m.records[0].mapping_support_relation='AFTER_SUCCESSOR'
        $m.records[0].physical_vblank_mapping_required=$false}
    'NegativeOutsideSupportTailCountMutation'={param($m)$m.outside_mapping_support_tail_count=0}
}
if($supportMutations.ContainsKey($Case)){
    $mutated=$actual|ConvertTo-Json -Depth 10|ConvertFrom-Json
    & $supportMutations[$Case] $mutated
    $rejected=$false
    try{Assert-MvmDisplayedQpcPhysicalMapping -Expected $actual -Actual $mutated}catch{$rejected=$true}
    if(-not$rejected){throw "$Case が受理されました"}
    Write-Output "W2-C1 mapping contract $Case : PASS";exit 0
}
if($Case-eq'NegativeSuccessorMutationMakesCandidateInside'){
    # successor を伸ばして support 外 candidate を内側へ入れ替えた replay は
    # sealed support からの再構築と一致しない。
    $widened=Invoke-MvmDisplayedQpcPhysicalMapping -Candidates $candidates `
        -Samples (@($samples)+@([pscustomobject]@{ordinal=4;qpc=600})) `
        -PredecessorOrdinal $predecessor -SuccessorOrdinal 4 -OriginOrdinal $origin -LastOrdinal $last `
        -PhysicalAuthorityValid $physicalValid -EtwEventsLost $etwLost
    if([string]$widened.records[0].mapping_support_relation-ne'INSIDE_SUPPORT'){
        throw 'successor拡張でcandidateが内側になっていません'
    }
    $rejected=$false
    try{Assert-MvmDisplayedQpcPhysicalMapping -Expected $actual -Actual ($widened|ConvertTo-Json -Depth 10|ConvertFrom-Json)}catch{$rejected=$true}
    if(-not$rejected){throw 'successor mutationが受理されました'}
    Write-Output "W2-C1 mapping contract $Case : PASS";exit 0
}
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
}elseif($Case-in@('GoodHeadOutsideSupportNonFormal','GoodTailOutsideSupportNonFormal')){
    if(-not[bool]$actual.mapping_exact){throw "$Case がfail-closeされました: $(@($actual.blockers)-join',')"}
    $record=$actual.records[0]
    $expectedRelation=$(if($Case-eq'GoodHeadOutsideSupportNonFormal'){'BEFORE_PREDECESSOR'}else{'AFTER_SUCCESSOR'})
    if([string]$record.mapping_support_relation-ne$expectedRelation){throw "$Case のrelationが不正です"}
    if([bool]$record.physical_vblank_mapping_required){throw "$Case でmappingを要求しています"}
    if([bool]$record.mapping_exact-or[bool]$record.in_measurement_physical_domain){throw "$Case のrecordが不正です"}
    if($actual.outside_mapping_support_count-ne1){throw "$Case のoutside countが不正です"}
    if($actual.in_domain_outside_mapping_support_count-ne0){throw "$Case のin-domain outside countが不正です"}
    if($actual.missing_mapping_count-ne0){throw "$Case でsupport外にmissingを数えています"}
    Write-Output "W2-C1 mapping contract $Case : PASS";exit 0
}else{
    if(-not[bool]$actual.mapping_exact-or$actual.mapped_exact_count-ne$candidates.Count){throw "$Case exact mappingが成立しません"}
    if($actual.outside_mapping_support_count-ne0){throw "$Case でsupport内candidateをoutside扱いしています"}
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
