Set-StrictMode -Version Latest

$script:MvmDisplayedMappingRule =
    'CLOSED_POST_WAKE_CELL: first.qpc == displayed_qpc OR previous.qpc < displayed_qpc <= sample.qpc'
$script:MvmDisplayedMappingProvenance = 'DXGI_OUTPUT_WAIT_FOR_VBLANK_POST_WAKE_QPC'

function Get-MvmFieldValues($Object,[string]$Name){
    if($null-eq$Object-or$Object.PSObject.Properties.Name-notcontains$Name){return @()}
    return @($Object.$Name)
}

function Invoke-MvmDisplayedQpcPhysicalMapping {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory=$true)][array]$Candidates,
        [Parameter(Mandatory=$true)][array]$Samples,
        [Parameter(Mandatory=$true)][int64]$PredecessorOrdinal,
        [Parameter(Mandatory=$true)][int64]$SuccessorOrdinal,
        [Parameter(Mandatory=$true)][int64]$OriginOrdinal,
        [Parameter(Mandatory=$true)][int64]$LastOrdinal,
        [Parameter(Mandatory=$true)][bool]$PhysicalAuthorityValid,
        [int64]$EtwEventsLost=0,
        [int64]$EtwBuffersLost=0,
        [int64]$PresentEventOverflowCount=0,
        # formal 母集団の呼び出しでは true。formal Presented が support 外にあれば
        # authority INVALID である。observed diagnostic の呼び出しでは false。
        [bool]$RequireAllCandidatesInsideSupport=$false
    )
    $support=@($Samples|Where-Object{
        [int64]$_.ordinal-ge$PredecessorOrdinal-and[int64]$_.ordinal-le$SuccessorOrdinal
    }|Sort-Object {[int64]$_.ordinal})
    # exact mapping support の定義域。mapping rule が解を持ちうる QPC 区間は
    # support sample の [first.qpc, last.qpc] に限られる。この外側の DisplayedQPC には
    # 対応する VBlank witness が存在しないため、mapping を要求すること自体が
    # support 外について authority を主張することになる。
    $supportFirstQpc=$(if($support.Count-gt0){[int64]$support[0].qpc}else{$null})
    $supportLastQpc=$(if($support.Count-gt0){[int64]$support[-1].qpc}else{$null})
    $records=@();$missing=0;$ambiguous=0;$cardinalityInvalid=0;$upstreamInvalid=0
    $insideSupport=0;$outsideHead=0;$outsideTail=0;$inDomainOutsideSupport=0
    foreach($candidate in $Candidates){
        $upstreamFields=@('native_exact','intent_exact','intent_scope_exact')
        $upstreamExact=$true
        foreach($field in $upstreamFields){
            if($candidate.PSObject.Properties.Name-notcontains$field-or-not[bool]$candidate.$field){
                $upstreamExact=$false
            }
        }
        if(-not$upstreamExact){++$upstreamInvalid}
        $displayed=@(Get-MvmFieldValues $candidate 'displayed_qpc')
        # support 境界は candidate の位置 (最後かどうか) や capture 長ではなく、
        # sealed physical sample から導いた QPC 区間だけで決める。
        $relation='SUPPORT_UNAVAILABLE'
        if($displayed.Count-eq1-and$null-ne$supportFirstQpc){
            $displayedQpc=[int64]$displayed[0]
            if($displayedQpc-lt$supportFirstQpc){$relation='BEFORE_PREDECESSOR'}
            elseif($displayedQpc-gt$supportLastQpc){$relation='AFTER_SUCCESSOR'}
            else{$relation='INSIDE_SUPPORT'}
        }
        $mappingRequired=$relation-eq'INSIDE_SUPPORT'
        switch($relation){
            'INSIDE_SUPPORT'{++$insideSupport}
            'BEFORE_PREDECESSOR'{++$outsideHead}
            'AFTER_SUCCESSOR'{++$outsideTail}
        }
        $solutions=@()
        if($displayed.Count-eq1){
            $qpc=[int64]$displayed[0]
            for($index=0;$index-lt$support.Count;++$index){
                $sample=$support[$index];$sampleQpc=[int64]$sample.qpc
                $cellMatches=$qpc-eq$sampleQpc
                if(-not$cellMatches-and$index-gt0){
                    $previousQpc=[int64]$support[$index-1].qpc
                    $cellMatches=$previousQpc-lt$qpc-and$qpc-lt$sampleQpc
                }
                if($cellMatches){$solutions+=,$sample}
            }
        }else{++$cardinalityInvalid}
        $solutionCount=$solutions.Count;$exact=$displayed.Count-eq1-and$solutionCount-eq1
        # missing / ambiguous は support 内の candidate についてだけ数える。
        if($mappingRequired-and$solutionCount-eq0){++$missing}
        if($mappingRequired-and$solutionCount-gt1){++$ambiguous}
        $mapped=$(if($exact){$solutions[0]}else{$null})
        $mappedOrdinal=$(if($exact){[int64]$mapped.ordinal}else{$null})
        $inDomain=$exact-and$OriginOrdinal-ge0-and$LastOrdinal-ge$OriginOrdinal-and
            $mappedOrdinal-ge$OriginOrdinal-and$mappedOrdinal-le$LastOrdinal
        if($inDomain-and-not$mappingRequired){++$inDomainOutsideSupport}
        $records+=[ordered]@{
            etw_sequence=$candidate.etw_sequence
            native_present_serial=$candidate.native_present_serial
            composition_token_serial=$candidate.composition_token_serial
            intent_ordinal=$candidate.intent_ordinal
            intent_scope=$candidate.intent_scope
            displayed_qpc=$(if($displayed.Count-eq1){[int64]$displayed[0]}else{@($displayed)})
            physical_vblank_ordinal=$mappedOrdinal
            physical_vblank_qpc=$(if($exact){[int64]$mapped.qpc}else{$null})
            physical_vblank_provenance=$(if($exact){$script:MvmDisplayedMappingProvenance}else{'UNRESOLVED'})
            in_measurement_physical_domain=$inDomain
            mapping_support_relation=$relation
            physical_vblank_mapping_required=$mappingRequired
            mapping_solution_count=$solutionCount
            mapping_exact=$exact
            upstream_candidate_exact=$upstreamExact
            layer2_cohort_member=[bool]$candidate.layer2_cohort_member
        }
    }
    $exactRecords=@($records|Where-Object{[bool]$_.mapping_exact})
    $duplicateOrdinalCount=0
    foreach($group in @($exactRecords|Group-Object {[int64]$_.physical_vblank_ordinal})){
        if($group.Count-gt1){$duplicateOrdinalCount+=$group.Count-1}
    }
    $blockers=@()
    if(-not$PhysicalAuthorityValid){$blockers+='PHYSICAL_AUTHORITY_INVALID'}
    if($EtwEventsLost-ne0-or$EtwBuffersLost-ne0-or$PresentEventOverflowCount-ne0){$blockers+='ETW_AUTHORITY_INVALID'}
    if($cardinalityInvalid-ne0){$blockers+='DISPLAYED_QPC_CARDINALITY_INVALID'}
    if($upstreamInvalid-ne0){$blockers+='UPSTREAM_CANDIDATE_AUTHORITY_INVALID'}
    if($missing-ne0){$blockers+='PHYSICAL_MAPPING_MISSING'}
    if($ambiguous-ne0){$blockers+='PHYSICAL_MAPPING_AMBIGUOUS'}
    # support 外を許すのは「formal でなく、かつ in-domain でもない」場合だけである。
    if($inDomainOutsideSupport-ne0){$blockers+='IN_DOMAIN_PRESENTED_OUTSIDE_MAPPING_SUPPORT'}
    if($RequireAllCandidatesInsideSupport-and($outsideHead+$outsideTail)-ne0){
        $blockers+='FORMAL_PRESENTED_OUTSIDE_MAPPING_SUPPORT'
    }
    if($duplicateOrdinalCount-ne0){$blockers+='DUPLICATE_PRESENTED_PHYSICAL_ORDINAL'}
    return [ordered]@{
        schema='mvm-p2-d5-2-w2-c1-displayed-physical-mapping-run-1'
        mapping_rule=$script:MvmDisplayedMappingRule
        mapping_provenance=$script:MvmDisplayedMappingProvenance
        mapping_uses_nearest_qpc=$false
        mapping_uses_arbitrary_tolerance=$false
        mapping_filters_measurement_window_before_mapping=$false
        mapping_uses_present_start_qpc=$false
        mapping_uses_present_return_qpc=$false
        mapping_uses_layer2_membership=$false
        mapping_uses_intent_scope=$false
        mapping_uses_source_frame=$false
        mapping_support_domain='CLOSED_SUPPORT_SAMPLE_QPC_INTERVAL'
        mapping_support_first_qpc=$supportFirstQpc
        mapping_support_last_qpc=$supportLastQpc
        presented_candidate_count=$Candidates.Count
        inside_mapping_support_count=$insideSupport
        outside_mapping_support_count=($outsideHead+$outsideTail)
        outside_mapping_support_head_count=$outsideHead
        outside_mapping_support_tail_count=$outsideTail
        in_domain_outside_mapping_support_count=$inDomainOutsideSupport
        formal_outside_mapping_support_required_zero=$RequireAllCandidatesInsideSupport
        mapped_exact_count=$exactRecords.Count
        in_domain_presented_event_count=@($exactRecords|Where-Object{[bool]$_.in_measurement_physical_domain}).Count
        out_of_domain_presented_event_count=@($exactRecords|Where-Object{-not[bool]$_.in_measurement_physical_domain}).Count
        missing_mapping_count=$missing
        ambiguous_mapping_count=$ambiguous
        duplicate_physical_ordinal_count=$duplicateOrdinalCount
        displayed_qpc_cardinality_invalid_count=$cardinalityInvalid
        upstream_candidate_authority_invalid_count=$upstreamInvalid
        mapping_exact=$blockers.Count-eq0
        blockers=$blockers
        records=$records
    }
}

function Assert-MvmDisplayedQpcPhysicalMapping {
    [CmdletBinding()]
    param([Parameter(Mandatory=$true)]$Expected,[Parameter(Mandatory=$true)]$Actual)
    $fields=@('mapping_rule','mapping_provenance','presented_candidate_count','mapped_exact_count',
        'in_domain_presented_event_count','out_of_domain_presented_event_count',
        'missing_mapping_count','ambiguous_mapping_count','duplicate_physical_ordinal_count',
        'displayed_qpc_cardinality_invalid_count','mapping_exact',
        'mapping_support_domain','mapping_support_first_qpc','mapping_support_last_qpc',
        'inside_mapping_support_count','outside_mapping_support_count',
        'outside_mapping_support_head_count','outside_mapping_support_tail_count',
        'in_domain_outside_mapping_support_count')
    foreach($field in $fields){
        if($Expected.PSObject.Properties.Name-notcontains$field-or
           $Actual.PSObject.Properties.Name-notcontains$field-or
           [string]$Expected.$field-ne[string]$Actual.$field){
            throw "mapping ledger fieldが一致しません: $field"
        }
    }
    $expectedRecords=@($Expected.records);$actualRecords=@($Actual.records)
    if($expectedRecords.Count-ne$actualRecords.Count){throw 'mapping ledger record件数が一致しません'}
    $recordFields=@('etw_sequence','native_present_serial','composition_token_serial','intent_ordinal',
        'intent_scope','displayed_qpc','physical_vblank_ordinal','physical_vblank_qpc',
        'physical_vblank_provenance','in_measurement_physical_domain','mapping_support_relation',
        'physical_vblank_mapping_required','mapping_solution_count','mapping_exact')
    for($index=0;$index-lt$expectedRecords.Count;++$index){
        foreach($field in $recordFields){
            if($expectedRecords[$index].PSObject.Properties.Name-notcontains$field-or
               $actualRecords[$index].PSObject.Properties.Name-notcontains$field-or
               (@($expectedRecords[$index].$field)-join',')-ne(@($actualRecords[$index].$field)-join',')){
                throw "mapping ledger recordが一致しません: record=$index field=$field"
            }
        }
    }
}
