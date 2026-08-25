Set-StrictMode -Version Latest

# W2-D formal-v2 shadow integration core。
# ここで新しい identity を発明しない。A/B/C で freeze 済みの
#   intent -> composition token -> native Present -> exact PresentEvent
#   -> FinalState -> DisplayedQPC -> physical ordinal
# を 1 record へ束ね、Layer 1A / Layer 1B の accounting を records から再計算する。
# threshold / fps / drop / canonical verdict は評価しない。
# source frame identity は入力にも出力にも持たない。

function Get-MvmDValue($Object,[string]$Name){
    if($Object-is[System.Collections.IDictionary]){
        if(-not$Object.Contains($Name)){return $null}
        return $Object[$Name]
    }
    if($null-eq$Object-or$Object.PSObject.Properties.Name-notcontains$Name){return $null}
    return $Object.$Name
}

function Test-MvmDBlank($Value){
    return ($null-eq$Value)-or([string]::IsNullOrWhiteSpace([string]$Value))
}

function Invoke-MvmDFormalV2ShadowIntegration {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory=$true)][AllowEmptyCollection()][array]$FormalPresentedRecords,
        [Parameter(Mandatory=$true)][AllowEmptyCollection()][array]$RequiredIntentOrdinals,
        [Parameter(Mandatory=$true)][int64]$PhysicalOpportunityCount,
        [Parameter(Mandatory=$true)][int64]$PhysicalDomainOriginOrdinal,
        [Parameter(Mandatory=$true)][int64]$PhysicalDomainLastOrdinal
    )
    $blockers=@{}
    $requiredSet=@{}
    foreach($requiredOrdinal in $RequiredIntentOrdinals){
        if(Test-MvmDBlank $requiredOrdinal){$blockers['REQUIRED_INTENT_ORDINAL_MISSING']=$true;continue}
        $requiredKey=[string]([uint64]$requiredOrdinal)
        if($requiredSet.ContainsKey($requiredKey)){$blockers['REQUIRED_INTENT_SET_DUPLICATE']=$true}
        $requiredSet[$requiredKey]=$true
    }
    if($requiredSet.Count-eq0){$blockers['REQUIRED_INTENT_SET_EMPTY']=$true}

    $records=@();$provenanceMissing=0L;$scopeInvalid=0L;$finalStateInvalid=0L
    $outsideRequired=0L;$foreignInsideRequired=0L
    foreach($sourceRecord in $FormalPresentedRecords){
        $key=Get-MvmDValue $sourceRecord 'exact_event_key'
        $ordinalValue=Get-MvmDValue $sourceRecord 'intent_ordinal'
        $scope=[string](Get-MvmDValue $sourceRecord 'intent_scope')
        $tokenSerial=Get-MvmDValue $sourceRecord 'composition_token_serial'
        $nativeSerial=Get-MvmDValue $sourceRecord 'native_present_serial'
        $etwSequence=Get-MvmDValue $sourceRecord 'etw_sequence'
        $finalState=[string](Get-MvmDValue $sourceRecord 'final_state')
        $displayedQpc=Get-MvmDValue $sourceRecord 'displayed_qpc'
        $physicalOrdinal=Get-MvmDValue $sourceRecord 'physical_vblank_ordinal'
        $inDomain=[bool](Get-MvmDValue $sourceRecord 'in_measurement_physical_domain')

        # frozen chain の環が 1 つでも欠けた record は fail-close する。
        $chainMissing=(Test-MvmDBlank $key)-or(Test-MvmDBlank $ordinalValue)-or(Test-MvmDBlank $scope)-or
            (Test-MvmDBlank $tokenSerial)-or(Test-MvmDBlank $nativeSerial)-or(Test-MvmDBlank $etwSequence)-or
            (Test-MvmDBlank $finalState)-or(Test-MvmDBlank $displayedQpc)-or(Test-MvmDBlank $physicalOrdinal)
        if($chainMissing){$provenanceMissing+=1}
        if((-not(Test-MvmDBlank $scope))-and$scope-notin@('CURRENT_MEASUREMENT','FOREIGN_PRE_MEASUREMENT')){$scopeInvalid+=1}
        if((-not(Test-MvmDBlank $finalState))-and$finalState-ne'Presented'){$finalStateInvalid+=1}

        $ordinal=$(if(Test-MvmDBlank $ordinalValue){$null}else{[string]([uint64]$ordinalValue)})
        $membership=$(if($null-eq$ordinal){$false}else{$requiredSet.ContainsKey($ordinal)})
        if($scope-eq'CURRENT_MEASUREMENT'-and$null-ne$ordinal-and-not$membership){$outsideRequired+=1}
        if($scope-eq'FOREIGN_PRE_MEASUREMENT'-and$membership){$foreignInsideRequired+=1}
        $satisfied=$inDomain-and$scope-eq'CURRENT_MEASUREMENT'-and$membership-and-not$chainMissing

        $records+=,[pscustomobject][ordered]@{
            exact_event_key=$(if(Test-MvmDBlank $key){$null}else{[string]$key})
            intent_ordinal=$ordinal
            intent_scope=$(if(Test-MvmDBlank $scope){$null}else{$scope})
            required_intent_membership=$membership
            composition_token_serial=$(if(Test-MvmDBlank $tokenSerial){$null}else{[string]$tokenSerial})
            native_present_serial=$(if(Test-MvmDBlank $nativeSerial){$null}else{[string]$nativeSerial})
            etw_sequence=$(if(Test-MvmDBlank $etwSequence){$null}else{[int64]$etwSequence})
            final_state=$(if(Test-MvmDBlank $finalState){$null}else{$finalState})
            displayed_qpc=$(if(Test-MvmDBlank $displayedQpc){$null}else{[int64]$displayedQpc})
            physical_vblank_ordinal=$(if(Test-MvmDBlank $physicalOrdinal){$null}else{[int64]$physicalOrdinal})
            in_measurement_physical_domain=$inDomain
            intent_satisfied=$satisfied
        }
    }

    $eventKeys=@($records|ForEach-Object{[string]$_.exact_event_key})
    if(@($eventKeys|Where-Object{[string]::IsNullOrWhiteSpace($_)}).Count-ne0-or
       @($eventKeys|Group-Object|Where-Object{$_.Count-gt1}).Count-ne0){
        $blockers['FORMAL_V2_EXACT_EVENT_KEY_INVALID']=$true
    }
    $physicalDuplicate=0L
    foreach($group in @($records|Group-Object {[string]$_.physical_vblank_ordinal})){
        if([string]::IsNullOrWhiteSpace([string]$group.Name)){continue}
        if($group.Count-gt1){$physicalDuplicate+=$group.Count-1}
    }

    $inDomainRecords=@($records|Where-Object{[bool]$_.in_measurement_physical_domain})
    $satisfiedRecords=@($records|Where-Object{[bool]$_.intent_satisfied})
    $foreignInDomain=@($inDomainRecords|Where-Object{$_.intent_scope-eq'FOREIGN_PRE_MEASUREMENT'})
    $satisfiedOrdinals=@{}
    foreach($satisfiedRecord in $satisfiedRecords){$satisfiedOrdinals[[string]$satisfiedRecord.intent_ordinal]=$true}
    $duplicateSatisfied=$satisfiedRecords.Count-$satisfiedOrdinals.Count
    $satisfiedCount=$satisfiedOrdinals.Count
    # unsatisfied は required set 側から数える。required - satisfied の引き算で作らない。
    $unsatisfiedCount=@($requiredSet.Keys|Where-Object{-not$satisfiedOrdinals.ContainsKey($_)}).Count
    $filled=@($inDomainRecords|Where-Object{$null-ne$_.physical_vblank_ordinal}|
        ForEach-Object{[string]$_.physical_vblank_ordinal}|Select-Object -Unique).Count

    # Layer 1B の cardinality は W2-A domain の [origin, last] からも再計算して突き合わせる。
    $domainCardinality=$(if($PhysicalOpportunityCount-gt0){$PhysicalDomainLastOrdinal-$PhysicalDomainOriginOrdinal+1}else{0})
    $cardinalityExact=$domainCardinality-eq$PhysicalOpportunityCount-and
        ($PhysicalOpportunityCount-gt0-or($PhysicalDomainOriginOrdinal-eq-1-and$PhysicalDomainLastOrdinal-eq-1))
    $layer1aExact=$requiredSet.Count-eq($satisfiedCount+$unsatisfiedCount)
    $presentedExact=($satisfiedCount+$foreignInDomain.Count)-eq$inDomainRecords.Count
    $filledExact=$filled-eq$inDomainRecords.Count
    $filledWithinDomain=$filled-le$PhysicalOpportunityCount

    if($provenanceMissing-ne0){$blockers['FORMAL_V2_CHAIN_PROVENANCE_MISSING']=$true}
    if($scopeInvalid-ne0){$blockers['INTENT_SCOPE_INVALID']=$true}
    if($finalStateInvalid-ne0){$blockers['FORMAL_V2_FINAL_STATE_NOT_PRESENTED']=$true}
    if($outsideRequired-ne0){$blockers['CURRENT_INTENT_OUTSIDE_REQUIRED_INTENT_SET']=$true}
    if($foreignInsideRequired-ne0){$blockers['FOREIGN_INTENT_INSIDE_REQUIRED_INTENT_SET']=$true}
    if($duplicateSatisfied-ne0){$blockers['DUPLICATE_SATISFIED_INTENT']=$true}
    if($physicalDuplicate-ne0){$blockers['MULTIPLE_FORMAL_PRESENTED_PER_PHYSICAL_ORDINAL']=$true}
    if(-not$layer1aExact){$blockers['LAYER1A_REQUIRED_ACCOUNTING_IDENTITY_VIOLATION']=$true}
    if(-not$presentedExact){$blockers['PRESENTED_ACCOUNTING_IDENTITY_VIOLATION']=$true}
    if(-not$filledExact){$blockers['FILLED_PHYSICAL_OPPORTUNITY_IDENTITY_VIOLATION']=$true}
    if(-not$cardinalityExact){$blockers['PHYSICAL_VBLANK_DOMAIN_CARDINALITY_INVALID']=$true}
    if(-not$filledWithinDomain){$blockers['FILLED_EXCEEDS_PHYSICAL_VBLANK_OPPORTUNITY']=$true}
    $blockerList=@($blockers.Keys|Sort-Object)

    return [ordered]@{
        schema='mvm-p2-d5-2-w2-d-formal-v2-shadow-run-1'
        formal_presented_population='C1_FORMAL_PRESENTED_ONLY'
        required_intent_authority='C2.1_EXACT_SCHEDULER_REQUIRED_INTENT_SET'
        physical_domain_authority='W2-A_EXACT_PHYSICAL_VBLANK_DOMAIN'
        required_intent_ordinals=@($RequiredIntentOrdinals|ForEach-Object{[string]([uint64]$_)})
        required_intent_count=$requiredSet.Count
        satisfied_intent_count=$satisfiedCount
        unsatisfied_intent_count=$unsatisfiedCount
        formal_presented_event_count=$records.Count
        in_domain_presented_event_count=$inDomainRecords.Count
        in_domain_presented_foreign_intent_count=$foreignInDomain.Count
        physical_vblank_opportunity_count=$PhysicalOpportunityCount
        physical_domain_origin_ordinal=$PhysicalDomainOriginOrdinal
        physical_domain_last_ordinal=$PhysicalDomainLastOrdinal
        filled_physical_opportunity_count=$filled
        duplicate_satisfied_intent_count=$duplicateSatisfied
        current_intent_outside_required_intent_set_count=$outsideRequired
        foreign_intent_inside_required_intent_set_count=$foreignInsideRequired
        formal_v2_chain_provenance_missing_count=$provenanceMissing
        intent_scope_invalid_count=$scopeInvalid
        final_state_not_presented_count=$finalStateInvalid
        multiple_formal_presented_per_physical_ordinal_count=$physicalDuplicate
        layer1a_required_accounting_identity_exact=$layer1aExact
        presented_accounting_identity_exact=$presentedExact
        filled_physical_opportunity_identity_exact=$filledExact
        physical_vblank_domain_cardinality_exact=$cardinalityExact
        # Layer 1A と Layer 1B は母集団が異なる。count 差は verdict ではない。
        layer1a_layer1b_count_difference_is_not_a_verdict=$true
        source_frame_identity_used=$false
        nearest_qpc_or_tolerance_used=$false
        shadow_only=$true
        canonical_authority=$false
        performance_threshold_evaluated=$false
        canonical_verdict_evaluated=$false
        frame_swapped_retirement_changed=$false
        integration_exact=$blockerList.Count-eq0
        blockers=$blockerList
        records=$records
    }
}
