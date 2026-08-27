Set-StrictMode -Version Latest

# P2-D5-2-W4-C1 causal compatibility replay core。
# sealed producer decision と formal opportunity ledger を exact render_begin_qpc で結び、
# scheduler が実際に受け取った pre-render refresh count から completed ordinalを再生する。
# branch discriminatorが無いのでexecution exactやroot causeへは昇格しない。

function Get-MvmW4C1Value($Object,[string]$Name){
    if($Object-is[System.Collections.IDictionary]){
        if(-not$Object.Contains($Name)){return $null}
        return $Object[$Name]
    }
    if($null-eq$Object-or$Object.PSObject.Properties.Name-notcontains$Name){return $null}
    return $Object.$Name
}

function Invoke-MvmW4C1CausalCompatibility {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory=$true)][AllowEmptyCollection()][array]$DecisionRecords,
        [Parameter(Mandatory=$true)][AllowEmptyCollection()][array]$SchedulerLedger,
        [Parameter(Mandatory=$true)][AllowEmptyCollection()][array]$W4BEvents,
        [Parameter(Mandatory=$true)][uint64]$OriginRefreshCount,
        [Parameter(Mandatory=$true)][int64]$MeasurementStartQpc,
        [Parameter(Mandatory=$true)][int64]$MeasurementEndQpcExclusive,
        [Parameter(Mandatory=$true)][int64]$QpcFrequency,
        [Parameter(Mandatory=$true)][bool]$MeasurementStopCaptured
    )
    $blockers=@{}
    if($MeasurementStartQpc-le0-or$MeasurementEndQpcExclusive-le$MeasurementStartQpc-or
       $QpcFrequency-le0){$blockers['PHYSICAL_MEASUREMENT_WINDOW_INVALID']=$true}

    # W4-Bと同じprimary definition。required membership外decisionをprimaryへ混ぜない。
    $primaryByOrdinal=@{};$primaryByRenderQpc=@{}
    foreach($decision in $DecisionRecords){
        if([string](Get-MvmW4C1Value $decision 'intent_scope')-ne'CURRENT_MEASUREMENT'){continue}
        if(-not[bool](Get-MvmW4C1Value $decision 'required_current_membership_exact')){continue}
        if(-not[bool](Get-MvmW4C1Value $decision 'required_current_membership')){continue}
        if([bool](Get-MvmW4C1Value $decision 'duplicate_callback')){continue}
        $ordinalValue=Get-MvmW4C1Value $decision 'intent_ordinal'
        $renderQpcValue=Get-MvmW4C1Value $decision 'render_begin_qpc'
        if($null-eq$ordinalValue-or$null-eq$renderQpcValue){
            $blockers['PRIMARY_DECISION_IDENTITY_MISSING']=$true;continue
        }
        if(-not[bool](Get-MvmW4C1Value $decision 'decision_qpc_exact')){
            $blockers['PRIMARY_DECISION_QPC_NOT_EXACT']=$true
        }
        $ordinalKey=[string]([uint64]$ordinalValue)
        $renderKey=[string]([int64]$renderQpcValue)
        if($primaryByOrdinal.ContainsKey($ordinalKey)-or$primaryByRenderQpc.ContainsKey($renderKey)){
            $blockers['PRIMARY_DECISION_DUPLICATE']=$true;continue
        }
        $primaryByOrdinal[$ordinalKey]=$decision
        $primaryByRenderQpc[$renderKey]=$decision
    }
    $primarySorted=@($primaryByOrdinal.Keys|ForEach-Object{[uint64]$_}|Sort-Object)
    if($primarySorted.Count-lt2){$blockers['PRIMARY_DECISION_POPULATION_TOO_SMALL']=$true}

    # nearest QPCは禁止。ledger.render_begin_qpcとdecision.render_begin_qpcの完全一致だけでjoinする。
    $completedByOrdinal=@{};$joinedLedgerCount=0L;$unanchoredDecisionCount=0L
    foreach($ledgerRecord in $SchedulerLedger){
        $renderQpc=Get-MvmW4C1Value $ledgerRecord 'render_begin_qpc'
        $predicted=Get-MvmW4C1Value $ledgerRecord 'predicted_opportunity_ordinal'
        $pre=Get-MvmW4C1Value $ledgerRecord 'pre_render_authority'
        if($null-eq$renderQpc-or$null-eq$predicted-or$null-eq$pre){
            $blockers['SCHEDULER_LEDGER_INPUT_MISSING']=$true;continue
        }
        $renderKey=[string]([int64]$renderQpc)
        if(-not$primaryByRenderQpc.ContainsKey($renderKey)){
            $blockers['EXACT_RENDER_QPC_JOIN_MISSING']=$true;continue
        }
        $decision=$primaryByRenderQpc[$renderKey]
        $decisionOrdinal=[uint64](Get-MvmW4C1Value $decision 'intent_ordinal')
        if($decisionOrdinal-ne[uint64]$predicted){
            $blockers['LEDGER_DECISION_ORDINAL_MISMATCH']=$true;continue
        }
        ++$joinedLedgerCount
        if($decisionOrdinal-eq0){
            # 最初のselectはunanchored path。originはその後のcommitSwapで確定するため、
            # final originをpre-render sampleへ逆適用しない。
            ++$unanchoredDecisionCount
            continue
        }
        if(-not[bool](Get-MvmW4C1Value $pre 'available')){
            $blockers['PRE_RENDER_AUTHORITY_UNAVAILABLE']=$true;continue
        }
        $refreshCount=[uint64](Get-MvmW4C1Value $pre 'refresh_count')
        if($refreshCount-lt$OriginRefreshCount){
            $blockers['PRE_RENDER_REFRESH_BEFORE_ORIGIN']=$true;continue
        }
        $completed=[uint64]($refreshCount-$OriginRefreshCount)
        if($completed-ge[uint64]::MaxValue-or$completed+1-ne$decisionOrdinal){
            $blockers['COMPLETED_PLUS_ONE_MISMATCH']=$true;continue
        }
        $ordinalKey=[string]$decisionOrdinal
        if($completedByOrdinal.ContainsKey($ordinalKey)){
            $blockers['COMPLETED_INPUT_DUPLICATE']=$true;continue
        }
        $completedByOrdinal[$ordinalKey]=[uint64]$completed
    }
    if($unanchoredDecisionCount-ne1){$blockers['UNANCHORED_DECISION_COUNT_INVALID']=$true}

    # W4-Bの全transitionを再検査する。missing endpointへcompleted stateを補間しない。
    $transitionResults=@();$compatibleCount=0L;$notObservableCount=0L;$deltaSummary=[ordered]@{}
    foreach($transitionEvent in $W4BEvents){
        $transition=Get-MvmW4C1Value $transitionEvent 'transition'
        if($null-eq$transition){continue}
        $before=Get-MvmW4C1Value $transitionEvent 'before_primary'
        $after=Get-MvmW4C1Value $transitionEvent 'after_primary'
        if($null-eq$before-or$null-eq$after){
            $blockers['W4B_TRANSITION_ENDPOINT_MISSING']=$true;continue
        }
        $beforeOrdinal=[uint64](Get-MvmW4C1Value $before 'intent_ordinal')
        $afterOrdinal=[uint64](Get-MvmW4C1Value $after 'intent_ordinal')
        $declaredDelta=[uint64](Get-MvmW4C1Value $transition 'primary_decision_intent_ordinal_delta')
        if($afterOrdinal-$beforeOrdinal-ne$declaredDelta){
            $blockers['W4B_INTENT_DELTA_MUTATION']=$true
        }
        $beforeKey=[string]$beforeOrdinal;$afterKey=[string]$afterOrdinal
        $status='NOT_OBSERVABLE'
        $beforeCompleted=$null;$afterCompleted=$null;$completedDelta=$null
        $unobservableReason='COMPLETED_REFRESH_INPUT_NOT_RECORDED'
        if($completedByOrdinal.ContainsKey($beforeKey)-and$completedByOrdinal.ContainsKey($afterKey)){
            $beforeCompleted=[uint64]$completedByOrdinal[$beforeKey]
            $afterCompleted=[uint64]$completedByOrdinal[$afterKey]
            $completedDelta=[uint64]($afterCompleted-$beforeCompleted)
            if($completedDelta-ne$declaredDelta){
                $blockers['INTENT_COMPLETED_DELTA_MISMATCH']=$true
                $status='INCOMPATIBLE'
            }else{
                $status='EXACT_CAUSAL_COMPATIBILITY'
                ++$compatibleCount
                $deltaKey=[string]$declaredDelta
                if(-not$deltaSummary.Contains($deltaKey)){$deltaSummary[$deltaKey]=0L}
                $deltaSummary[$deltaKey]=[int64]$deltaSummary[$deltaKey]+1
            }
            $unobservableReason=$null
        }else{++$notObservableCount}
        $transitionResults+=,[ordered]@{
            cohort=[string](Get-MvmW4C1Value $transitionEvent 'cohort')
            before_intent_ordinal=[string]$beforeOrdinal
            after_intent_ordinal=[string]$afterOrdinal
            intent_ordinal_delta=[string]$declaredDelta
            before_completed_refresh_ordinal=$(if($null-eq$beforeCompleted){$null}else{[string]$beforeCompleted})
            after_completed_refresh_ordinal=$(if($null-eq$afterCompleted){$null}else{[string]$afterCompleted})
            completed_refresh_ordinal_delta=$(if($null-eq$completedDelta){$null}else{[string]$completedDelta})
            status=$status
            unobservable_reason=$unobservableReason
        }
    }

    # Cause B候補は別々に評価する。source domainとrequired intent domainを混同しない。
    $lastPrimary=$(if($primarySorted.Count-gt0){$primaryByOrdinal[[string]$primarySorted[-1]]}else{$null})
    $lastQpc=$(if($null-eq$lastPrimary){0L}else{[int64](Get-MvmW4C1Value $lastPrimary 'decision_qpc')})
    $pastSource=$(if($null-eq$lastPrimary){$false}else{[bool](Get-MvmW4C1Value $lastPrimary 'past_source_domain')})
    $requiredMembership=$(if($null-eq$lastPrimary){$false}else{[bool](Get-MvmW4C1Value $lastPrimary 'required_current_membership')})
    $transportDisposition=$(if($null-eq$lastPrimary){''}else{[string](Get-MvmW4C1Value $lastPrimary 'transport_disposition')})
    $domainCompatible=$null-ne$lastPrimary-and$pastSource-and$requiredMembership-and
        $transportDisposition-eq'TRANSPORT'
    if(-not$domainCompatible){$blockers['SOURCE_DOMAIN_TERMINAL_WITNESS_MISSING']=$true}
    $plannedEndIncompatible=$lastQpc-gt0-and$lastQpc-lt$MeasurementEndQpcExclusive
    if(-not$plannedEndIncompatible){$blockers['PLANNED_WINDOW_COMPARISON_UNRESOLVED']=$true}

    $blockerList=@($blockers.Keys|Sort-Object)
    return [ordered]@{
        schema='mvm-p2-d5-2-w4-c1-causal-compatibility-run-1'
        attribution='EXACT_CAUSAL_COMPATIBILITY_PARTIAL_COVERAGE'
        branch_execution_exact=$false
        root_cause_determined=$false
        nearest_qpc_binding_used=$false
        missing_state_interpolated=$false
        source_domain_required_domain_conflated=$false
        origin_refresh_count=[string]$OriginRefreshCount
        primary_decision_count=$primarySorted.Count
        scheduler_ledger_record_count=$SchedulerLedger.Count
        exact_render_qpc_join_count=$joinedLedgerCount
        completed_refresh_input_witness_count=$completedByOrdinal.Count
        w4b_transition_count=$transitionResults.Count
        compatible_transition_count=$compatibleCount
        not_observable_transition_count=$notObservableCount
        incompatible_transition_count=@($transitionResults|Where-Object{$_.status-eq'INCOMPATIBLE'}).Count
        delta_compatibility_counts=$deltaSummary
        transitions=$transitionResults
        cause_b_candidates=[ordered]@{
            DOMAIN_TERMINAL=[ordered]@{
                status=$(if($domainCompatible){'EXACT_CAUSAL_COMPATIBILITY'}else{'INCOMPATIBLE'})
                branch_execution_exact=$false
                last_intent_ordinal=$(if($null-eq$lastPrimary){$null}else{[string](Get-MvmW4C1Value $lastPrimary 'intent_ordinal')})
                last_decision_qpc=$lastQpc
                past_source_domain=$pastSource
                required_intent_membership=$requiredMembership
                formal_transport_disposition=$transportDisposition
                source_domain_required_domain_separated=$true
            }
            PLANNED_WINDOW_END=[ordered]@{
                status=$(if($plannedEndIncompatible){'INCOMPATIBLE'}else{'NOT_OBSERVABLE'})
                comparison_exact=$plannedEndIncompatible
                measurement_end_qpc_exclusive=$MeasurementEndQpcExclusive
                last_decision_qpc=$lastQpc
                qpc_gap=$MeasurementEndQpcExclusive-$lastQpc
                tolerance_or_elapsed_heuristic_used=$false
            }
            EXPLICIT_STOP=[ordered]@{
                status='NOT_OBSERVABLE'
                measurement_stop_captured=$MeasurementStopCaptured
                request_source_recorded=$false
                request_qpc_recorded=$false
            }
        }
        alternative_stop_reason_excluded=$false
        c2_instrumentation_required=$true
        attribution_exact=$blockerList.Count-eq0
        blockers=$blockerList
    }
}

function Assert-MvmW4C1Proof {
    [CmdletBinding()]
    param([Parameter(Mandatory=$true)]$Expected,[Parameter(Mandatory=$true)]$Actual)
    $expectedJson=$Expected|ConvertTo-Json -Depth 24 -Compress
    $actualJson=$Actual|ConvertTo-Json -Depth 24 -Compress
    if($expectedJson-ne$actualJson){throw 'W4-C1 artifactがsealed recordsからの再構築結果と一致しません'}
}
