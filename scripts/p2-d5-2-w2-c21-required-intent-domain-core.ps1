Set-StrictMode -Version Latest

function Get-MvmC21Value($Object,[string]$Name){
    if($Object-is[System.Collections.IDictionary]){
        if($Object.Contains($Name)){return $Object[$Name]}
        return $null
    }
    if($null-ne$Object-and$Object.PSObject.Properties.Name-contains$Name){return $Object.$Name}
    return $null
}

function Invoke-MvmC21RunInventory {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory=$true)]$IntentScopeAuthority,
        [Parameter(Mandatory=$true)][array]$NativePresentRecords,
        [Parameter(Mandatory=$true)][array]$FormalPresentedCandidates,
        [Parameter(Mandatory=$true)][int64]$RequiredIntentCount,
        [Parameter(Mandatory=$true)]$CaptureEnvelope,
        [string[]]$DiagnosticIntentOrdinals=@('0','301')
    )
    $blockers=@{}
    $requiredSetExact=[bool](Get-MvmC21Value $IntentScopeAuthority 'required_intent_set_exact')
    $requiredSetValues=Get-MvmC21Value $IntentScopeAuthority 'required_intent_ordinals'
    $requiredSetPresent=$null-ne$requiredSetValues
    $requiredOrdinals=@()
    if($requiredSetPresent){$requiredOrdinals=@($requiredSetValues|ForEach-Object{[string]([uint64]$_)})}
    $requiredDuplicate=0L
    foreach($group in @($requiredOrdinals|Group-Object)){if($group.Count-gt1){$requiredDuplicate+=$group.Count-1}}
    $requiredDistinct=@($requiredOrdinals|Select-Object -Unique)
    $requiredIdentityExact=$requiredSetExact-and$requiredSetPresent-and$requiredDuplicate-eq0-and
        $requiredDistinct.Count-eq$RequiredIntentCount
    if(-not$requiredSetPresent-or-not$requiredSetExact){$blockers['REQUIRED_INTENT_MEMBERSHIP_PROVENANCE_MISSING']=$true}
    if($requiredDuplicate-ne0){$blockers['REQUIRED_SCHEDULER_INTENT_SET_DUPLICATE']=$true}
    if($requiredSetExact-and$requiredDistinct.Count-ne$RequiredIntentCount){$blockers['REQUIRED_INTENT_COUNT_SET_CARDINALITY_MISMATCH']=$true}
    $requiredKeys=@{};foreach($value in $requiredDistinct){$requiredKeys[[string]$value]=$true}
    $zeroBasedCountKeys=@{};for($ordinalIndex=0L;$ordinalIndex-lt$RequiredIntentCount;++$ordinalIndex){$zeroBasedCountKeys[[string]$ordinalIndex]=$true}
    $requiredIsZeroBasedCountDomain=$requiredIdentityExact-and
        @($requiredKeys.Keys|Where-Object{-not$zeroBasedCountKeys.ContainsKey($_)}).Count-eq0-and
        @($zeroBasedCountKeys.Keys|Where-Object{-not$requiredKeys.ContainsKey($_)}).Count-eq0

    $armValue=Get-MvmC21Value $CaptureEnvelope 'measurement_arm_qpc'
    $startValue=Get-MvmC21Value $CaptureEnvelope 'measurement_start_qpc'
    $endValue=Get-MvmC21Value $CaptureEnvelope 'frozen_measurement_end_qpc'
    $armPresent=$null-ne$armValue;$startPresent=$null-ne$startValue;$endPresent=$null-ne$endValue
    if(-not$armPresent){$blockers['MEASUREMENT_ARM_QPC_MISSING']=$true}
    if(-not$startPresent){$blockers['MEASUREMENT_START_QPC_MISSING']=$true}
    if(-not$endPresent){$blockers['MEASUREMENT_END_QPC_MISSING']=$true}
    $armQpc=$(if($armPresent){[int64]$armValue}else{$null})
    $startQpc=$(if($startPresent){[int64]$startValue}else{$null})
    $endQpc=$(if($endPresent){[int64]$endValue}else{$null})
    $boundaryQpcExact=$armPresent-and$startPresent-and$endPresent-and
        $armQpc-gt0-and$armQpc-le$startQpc-and$startQpc-lt$endQpc
    if($armPresent-and$startPresent-and$endPresent-and-not$boundaryQpcExact){
        $blockers['MEASUREMENT_BOUNDARY_QPC_ORDER_INVALID']=$true
    }
    $nativeByToken=@{}
    foreach($nativeRecord in $NativePresentRecords){
        $token=Get-MvmC21Value (Get-MvmC21Value $nativeRecord 'composition_token') 'token_serial'
        if($null-eq$token){continue};$key=[string]$token
        if(-not$nativeByToken.ContainsKey($key)){$nativeByToken[$key]=@()}
        $nativeByToken[$key]=@($nativeByToken[$key])+@($nativeRecord)
    }
    $formalByToken=@{}
    foreach($candidate in $FormalPresentedCandidates){
        $key=[string](Get-MvmC21Value $candidate 'composition_token_serial')
        if(-not$formalByToken.ContainsKey($key)){$formalByToken[$key]=@()}
        $formalByToken[$key]=@($formalByToken[$key])+@($candidate)
    }
    $decisionRecords=@();$sequence=0L
    foreach($scopeRecord in @(Get-MvmC21Value $IntentScopeAuthority 'records')){
        $token=[string](Get-MvmC21Value $scopeRecord 'token_serial')
        $ordinal=[string]([uint64](Get-MvmC21Value $scopeRecord 'intent_ordinal'))
        $scope=[string](Get-MvmC21Value $scopeRecord 'intent_scope')
        $decisionQpc=Get-MvmC21Value $scopeRecord 'decision_qpc'
        $decisionQpcExact=[bool](Get-MvmC21Value $scopeRecord 'decision_qpc_exact')
        $membership=Get-MvmC21Value $scopeRecord 'required_current_membership'
        $membershipExact=[bool](Get-MvmC21Value $scopeRecord 'required_current_membership_exact')
        $expectedMembership=$scope-eq'CURRENT_MEASUREMENT'-and$requiredKeys.ContainsKey($ordinal)
        $membershipConsistent=$membershipExact-and$null-ne$membership-and
            [bool]$membership-eq$expectedMembership
        $boundaryRelation=[string](Get-MvmC21Value $scopeRecord 'measurement_boundary_relation')
        $producerSemanticsExact=[bool](Get-MvmC21Value $scopeRecord 'producer_semantics_exact')
        if([string]::IsNullOrWhiteSpace($boundaryRelation)){$boundaryRelation='UNRESOLVED_WITHOUT_DECISION_QPC_OR_MEMBERSHIP'}
        $derivedRelation='UNRESOLVED_WITHOUT_EXACT_BOUNDARY_QPC'
        if($decisionQpcExact-and$null-ne$decisionQpc-and$boundaryQpcExact){
            $decisionQpcValue=[int64]$decisionQpc
            $derivedRelation=$(if($decisionQpcValue-lt$armQpc){'PRE_MEASUREMENT_ARM'}
                elseif($decisionQpcValue-lt$startQpc){'ARMED_PRE_MEASUREMENT'}
                elseif($decisionQpcValue-lt$endQpc){'WITHIN_CURRENT_MEASUREMENT'}
                else{'POST_MEASUREMENT'})
        }
        $boundaryConsistent=$derivedRelation-ne'UNRESOLVED_WITHOUT_EXACT_BOUNDARY_QPC'-and
            $boundaryRelation-eq$derivedRelation
        $nativeMatches=@($(if($nativeByToken.ContainsKey($token)){$nativeByToken[$token]}else{@()}))
        $formalMatches=@($(if($formalByToken.ContainsKey($token)){$formalByToken[$token]}else{@()}))
        $nativeExact=$nativeMatches.Count-eq1-and
            [string](Get-MvmC21Value $nativeMatches[0] 'intent_ordinal')-eq$ordinal-and
            [bool](Get-MvmC21Value $nativeMatches[0] 'intent_ordinal_valid')
        $formalKeys=@()
        foreach($candidate in $formalMatches){
            $displayed=@(Get-MvmC21Value $candidate 'displayed_qpc')
            if($displayed.Count-eq1){$formalKeys+=,"$([int64](Get-MvmC21Value $candidate 'etw_sequence'))|$([int64]$displayed[0])"}
        }
        $decisionRecords+=,[pscustomobject][ordered]@{
            scheduler_decision_sequence=$sequence
            opportunity_ordinal=$ordinal
            decision_qpc=$(if($null-ne$decisionQpc){[int64]$decisionQpc}else{$null})
            decision_qpc_exact=$decisionQpcExact
            producer_scope=$scope
            required_current_membership=$(if($null-ne$membership){[bool]$membership}else{$null})
            required_current_membership_exact=$membershipExact
            checker_derived_required_current_membership=$expectedMembership
            required_current_membership_consistent=$membershipConsistent
            measurement_arm_qpc=$armQpc
            measurement_start_qpc=$startQpc
            measurement_end_qpc_exclusive=$endQpc
            measurement_boundary_relation=$boundaryRelation
            checker_derived_measurement_boundary_relation=$derivedRelation
            measurement_boundary_relation_consistent=$boundaryConsistent
            producer_semantics_exact=$producerSemanticsExact
            duplicate_callback=$(if($producerSemanticsExact){[bool](Get-MvmC21Value $scopeRecord 'duplicate_callback')}else{$null})
            repeat=$(if($producerSemanticsExact){[bool](Get-MvmC21Value $scopeRecord 'repeat')}else{$null})
            past_source_domain=$(if($producerSemanticsExact){[bool](Get-MvmC21Value $scopeRecord 'past_source_domain')}else{$null})
            target_frame=$(if($producerSemanticsExact){[int64](Get-MvmC21Value $scopeRecord 'target_frame')}else{$null})
            last_finalized_opportunity_ordinal=$(if($producerSemanticsExact){[int64](Get-MvmC21Value $scopeRecord 'last_finalized_opportunity_ordinal')}else{$null})
            render_begin_qpc=$(if($producerSemanticsExact){[int64](Get-MvmC21Value $scopeRecord 'render_begin_qpc')}else{$null})
            token_serial=$token
            composition_token_transport_observed=$nativeMatches.Count-ne0
            composition_token_transport_match_count=$nativeMatches.Count
            composition_token_transport_exact=$nativeExact
            native_present_transport_observed=$nativeMatches.Count-ne0
            native_present_transport_match_count=$nativeMatches.Count
            native_present_transport_exact=$nativeExact
            native_present_serial=$(if($nativeMatches.Count-eq1){[string](Get-MvmC21Value $nativeMatches[0] 'present_serial')}else{$null})
            native_present_enter_qpc=$(if($nativeMatches.Count-eq1){[int64](Get-MvmC21Value $nativeMatches[0] 'present_enter_qpc')}else{$null})
            formal_presented_count=$formalMatches.Count
            formal_presented_event_keys=$formalKeys
            formal_presented_reverse_attribution_exact=$formalMatches.Count-le1-and($formalMatches.Count-eq0-or$nativeExact)
            diagnostic_target=$ordinal-in$DiagnosticIntentOrdinals
        }
        $sequence+=1
    }
    $currentRecords=@($decisionRecords|Where-Object{$_.producer_scope-eq'CURRENT_MEASUREMENT'})
    $currentOrdinals=@($currentRecords|ForEach-Object{[uint64]$_.opportunity_ordinal})
    $currentDuplicate=0L
    foreach($group in @($currentOrdinals|Group-Object)){if($group.Count-gt1){$currentDuplicate+=$group.Count-1}}
    $currentDistinct=@($currentOrdinals|Sort-Object -Unique)
    $diagnosticGap=0L
    if($currentDistinct.Count-ne0){
        $diagnosticGap=[int64]$currentDistinct[-1]-[int64]$currentDistinct[0]+1-$currentDistinct.Count
    }
    if(@($decisionRecords|Where-Object{-not[bool]$_.decision_qpc_exact}).Count-ne0){
        $blockers['SCHEDULER_DECISION_QPC_PROVENANCE_MISSING']=$true
    }
    if(@($decisionRecords|Where-Object{-not[bool]$_.required_current_membership_exact}).Count-ne0){
        $blockers['DECISION_REQUIRED_MEMBERSHIP_PROVENANCE_MISSING']=$true
    }
    $membershipMismatchRecords=@($decisionRecords|Where-Object{
        [bool]$_.required_current_membership_exact-and
        -not[bool]$_.required_current_membership_consistent
    })
    if($membershipMismatchRecords.Count-ne0){
        $blockers['DECISION_REQUIRED_MEMBERSHIP_MISMATCH']=$true
    }
    if(-not$boundaryQpcExact-or@($decisionRecords|Where-Object{-not[bool]$_.measurement_boundary_relation_consistent}).Count-ne0){
        $blockers['MEASUREMENT_BOUNDARY_RELATION_UNRESOLVED']=$true
    }
    if($boundaryQpcExact-and@($decisionRecords|Where-Object{
        $_.checker_derived_measurement_boundary_relation-ne'UNRESOLVED_WITHOUT_EXACT_BOUNDARY_QPC'-and
        -not[bool]$_.measurement_boundary_relation_consistent}).Count-ne0){
        $blockers['MEASUREMENT_BOUNDARY_RELATION_MISMATCH']=$true
    }
    $exactMembershipOrdinals=@($decisionRecords|Where-Object{
        [bool]$_.required_current_membership_exact-and[bool]$_.required_current_membership
    }|ForEach-Object{[string]([uint64]$_.opportunity_ordinal)}|Sort-Object -Unique)
    $membershipKeys=@{};foreach($value in $exactMembershipOrdinals){$membershipKeys[[string]$value]=$true}
    $missingMembership=@($requiredKeys.Keys|Where-Object{-not$membershipKeys.ContainsKey($_)}|Sort-Object{[uint64]$_})
    $extraMembership=@($membershipKeys.Keys|Where-Object{-not$requiredKeys.ContainsKey($_)}|Sort-Object{[uint64]$_})
    $setMembershipEqual=$requiredIdentityExact-and$missingMembership.Count-eq0-and$extraMembership.Count-eq0
    if(@($decisionRecords|Where-Object{-not[bool]$_.formal_presented_reverse_attribution_exact}).Count-ne0){
        $blockers['FORMAL_PRESENTED_REVERSE_ATTRIBUTION_INVALID']=$true
    }
    $blockerList=@($blockers.Keys|Sort-Object)
    return [ordered]@{
        schema='mvm-p2-d5-2-w2-c21-required-intent-domain-run-1'
        scheduler_decision_authority='FORMAL_DECISION_SCOPE_LEDGER_AFTER_SELECT_FOR_RENDER'
        scheduler_decision_record_count=$decisionRecords.Count
        required_intent_count=$RequiredIntentCount
        required_intent_count_authority='WORKLOAD_CARDINALITY_ONLY'
        required_scheduler_intent_set_exact=$requiredIdentityExact
        required_scheduler_intent_ordinals=$(if($requiredSetPresent){$requiredOrdinals}else{$null})
        required_scheduler_intent_set_cardinality=$(if($requiredSetPresent){$requiredDistinct.Count}else{$null})
        required_scheduler_intent_set_duplicate_count=$requiredDuplicate
        required_count_equals_exact_set_cardinality=$requiredIdentityExact
        required_scheduler_intent_set_is_zero_based_count_domain=$requiredIsZeroBasedCountDomain
        exact_required_membership_ordinal_set=$exactMembershipOrdinals
        required_set_missing_membership_ordinals=$missingMembership
        membership_outside_required_set_ordinals=$extraMembership
        required_set_missing_membership_count=$missingMembership.Count
        membership_outside_required_set_count=$extraMembership.Count
        required_set_equals_exact_membership_set=$setMembershipEqual
        required_set_decision_population_equality_required=$false
        decision_required_membership_mismatch_count=$membershipMismatchRecords.Count
        decision_required_membership_mismatches=@($membershipMismatchRecords|ForEach-Object{
            [pscustomobject][ordered]@{
                scheduler_decision_sequence=$_.scheduler_decision_sequence
                opportunity_ordinal=$_.opportunity_ordinal
                producer_scope=$_.producer_scope
                recorded_membership=$_.required_current_membership
                checker_derived_membership=$_.checker_derived_required_current_membership
            }
        })
        decision_required_membership_consistent=$membershipMismatchRecords.Count-eq0-and
            @($decisionRecords|Where-Object{-not[bool]$_.required_current_membership_exact}).Count-eq0
        measurement_boundary_qpc_exact=$boundaryQpcExact
        producer_current_decision_count=$currentRecords.Count
        producer_current_distinct_ordinal_count=$currentDistinct.Count
        producer_current_ordinal_min_diagnostic=$(if($currentDistinct.Count){[string]$currentDistinct[0]}else{$null})
        producer_current_ordinal_max_diagnostic=$(if($currentDistinct.Count){[string]$currentDistinct[-1]}else{$null})
        producer_current_duplicate_ordinal_count_diagnostic=$currentDuplicate
        producer_current_gap_count_within_min_max_diagnostic=$diagnosticGap
        producer_current_set_is_required_authority=$false
        presented_population_used_to_derive_required_set=$false
        source_frame_identity_used=$false
        nearest_qpc_or_tolerance_used=$false
        producer_changed=$false
        historical_c2_artifact_changed=$false
        authority_exact=$blockerList.Count-eq0-and$boundaryQpcExact
        blockers=$blockerList
        decisions=$decisionRecords
        diagnostic_reverse_attribution=@($decisionRecords|Where-Object{[bool]$_.diagnostic_target})
    }
}
