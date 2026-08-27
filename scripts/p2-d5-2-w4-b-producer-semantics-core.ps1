Set-StrictMode -Version Latest

# P2-D5-2-W4-B producer semantics attribution core。
#
# W4-A は 5574 件の unsatisfied がすべて primary scheduler decision 生成以前に集中することを
# exact に閉じた。W4-B は isolated missing と double-missing boundary に対応する
# producer-side exact semantic transition を識別する段である。
#
# root cause は出さない。instrumentation A/B もしない。
#
# 最重要の禁止事項:
#   missing ordinal 自身に存在しない field を補間しない。
#   missing 側は「前後に実在した record」と「gap 幅」だけを持つ。値は作らない。
#   nearest QPC / midpoint / 周囲からの推測は一切しない。

function Get-MvmW4BValue($Object,[string]$Name){
    if($Object-is[System.Collections.IDictionary]){
        if(-not$Object.Contains($Name)){return $null}
        return $Object[$Name]
    }
    if($null-eq$Object-or$Object.PSObject.Properties.Name-notcontains$Name){return $null}
    return $Object.$Name
}

# primary decision の定義は W4-A と同一である。ここで別定義を作らない。
function Test-MvmW4BPrimaryDecision($Decision,[hashtable]$RequiredSet){
    if([string](Get-MvmW4BValue $Decision 'intent_scope')-ne'CURRENT_MEASUREMENT'){return $false}
    if(-not[bool](Get-MvmW4BValue $Decision 'required_current_membership_exact')){return $false}
    if([bool](Get-MvmW4BValue $Decision 'duplicate_callback')){return $false}
    if(-not[bool](Get-MvmW4BValue $Decision 'required_current_membership')){return $false}
    $ordinal=Get-MvmW4BValue $Decision 'intent_ordinal'
    if($null-eq$ordinal-or[string]::IsNullOrWhiteSpace([string]$ordinal)){return $false}
    return $RequiredSet.ContainsKey([string]([uint64]$ordinal))
}

function Get-MvmW4BPrimarySnapshot($Decision){
    return [ordered]@{
        intent_ordinal=[string]([uint64](Get-MvmW4BValue $Decision 'intent_ordinal'))
        decision_qpc=[int64](Get-MvmW4BValue $Decision 'decision_qpc')
        render_begin_qpc=[int64](Get-MvmW4BValue $Decision 'render_begin_qpc')
        target_frame=[int64](Get-MvmW4BValue $Decision 'target_frame')
        repeat=[bool](Get-MvmW4BValue $Decision 'repeat')
        past_source_domain=[bool](Get-MvmW4BValue $Decision 'past_source_domain')
        last_finalized_opportunity_ordinal=[int64](Get-MvmW4BValue $Decision 'last_finalized_opportunity_ordinal')
        token_serial=[string](Get-MvmW4BValue $Decision 'token_serial')
    }
}

function Invoke-MvmW4BProducerSemantics {
    [CmdletBinding()]
    param(
        # W4-A が確定した exact required current intent set。
        [Parameter(Mandatory=$true)][AllowEmptyCollection()][array]$RequiredIntentOrdinals,
        # W4-A が集合差から確定した missing set。ここで作り直さない。
        [Parameter(Mandatory=$true)][AllowEmptyCollection()][array]$MissingOrdinals,
        # producer の scheduler decision ledger。
        [Parameter(Mandatory=$true)][AllowEmptyCollection()][array]$DecisionRecords,
        # canonical physical measurement window (W2-A)。decision stream から推定しない。
        [Parameter(Mandatory=$true)][int64]$MeasurementStartQpc,
        [Parameter(Mandatory=$true)][int64]$MeasurementEndQpcExclusive,
        [Parameter(Mandatory=$true)][int64]$QpcFrequency,
        # legacy elapsed は correlation として記録するだけで authority にしない。
        [double]$LegacyMeasurementElapsedSeconds=0
    )
    $blockers=@{}
    $requiredSet=@{}
    foreach($ordinal in $RequiredIntentOrdinals){
        if($null-eq$ordinal-or[string]::IsNullOrWhiteSpace([string]$ordinal)){continue}
        $requiredSet[[string]([uint64]$ordinal)]=$true
    }
    $requiredSorted=@($requiredSet.Keys|ForEach-Object{[uint64]$_}|Sort-Object)
    $missingSet=@{}
    foreach($ordinal in $MissingOrdinals){
        if($null-eq$ordinal-or[string]::IsNullOrWhiteSpace([string]$ordinal)){continue}
        $key=[string]([uint64]$ordinal)
        if(-not$requiredSet.ContainsKey($key)){$blockers['MISSING_ORDINAL_OUTSIDE_REQUIRED_SET']=$true}
        $missingSet[$key]=$true
    }

    # producer ledger から primary decision を索く。W4-A と同じ定義を使う。
    $primaryByOrdinal=@{}
    foreach($decision in $DecisionRecords){
        if(-not(Test-MvmW4BPrimaryDecision $decision $requiredSet)){continue}
        $key=[string]([uint64](Get-MvmW4BValue $decision 'intent_ordinal'))
        if($primaryByOrdinal.ContainsKey($key)){$blockers['PRIMARY_DECISION_DUPLICATE']=$true;continue}
        $primaryByOrdinal[$key]=$decision
    }
    # W4-A missing set と producer ledger の再構築が一致すること。splice を fail-close する。
    foreach($key in @($missingSet.Keys)){
        if($primaryByOrdinal.ContainsKey($key)){$blockers['MISSING_SET_SPLICE']=$true}
    }
    foreach($key in @($requiredSet.Keys)){
        if(-not$primaryByOrdinal.ContainsKey($key)-and-not$missingSet.ContainsKey($key)){
            $blockers['MISSING_SET_SPLICE']=$true
        }
    }

    # --- maximal consecutive missing run から cohort を決める ---
    # local pattern matching ではなく run 単位で分類する。double run の 2 ordinal を
    # それぞれ数えて double event 2 件にする余地を無くす。
    $runs=@();$current=@()
    foreach($ordinal in $requiredSorted){
        $key=[string]$ordinal
        if($missingSet.ContainsKey($key)){$current+=,$ordinal}
        elseif($current.Count-gt0){$runs+=,@($current);$current=@()}
    }
    if($current.Count-gt0){$runs+=,@($current)}

    $firstRequired=$(if($requiredSorted.Count-gt0){$requiredSorted[0]}else{$null})
    $lastRequired=$(if($requiredSorted.Count-gt0){$requiredSorted[-1]}else{$null})
    $cohortNames=@('HEAD_EDGE','TAIL_EDGE','LONGER_MISSING_RUN','DOUBLE_MISSING_BOUNDARY',
        'ISOLATED_MISSING','OTHER_PATTERN')
    $eventCounts=[ordered]@{};$intentCounts=[ordered]@{}
    foreach($name in $cohortNames){$eventCounts[$name]=0L;$intentCounts[$name]=0L}
    $events=@();$intervalDuplicateTotal=0L

    # producer ledger 上の record を intent ordinal 昇順で走査するため索引を作る。
    # cross-run splice を禁止するため、近傍探索はこの run 内で閉じる。
    foreach($missingRun in $runs){
        $runFirst=$missingRun[0];$runLast=$missingRun[-1]
        # precedence: HEAD/TAIL edge -> LONGER -> DOUBLE -> ISOLATED -> OTHER
        $cohort=$null
        if($null-ne$firstRequired-and$runFirst-eq$firstRequired){$cohort='HEAD_EDGE'}
        elseif($null-ne$lastRequired-and$runLast-eq$lastRequired){$cohort='TAIL_EDGE'}
        elseif($missingRun.Count-gt2){$cohort='LONGER_MISSING_RUN'}
        elseif($missingRun.Count-eq2){$cohort='DOUBLE_MISSING_BOUNDARY'}
        elseif($missingRun.Count-eq1){$cohort='ISOLATED_MISSING'}
        else{$cohort='OTHER_PATTERN'}
        $eventCounts[$cohort]+=1
        $intentCounts[$cohort]+=$missingRun.Count

        $beforeKey=[string]($runFirst-1);$afterKey=[string]($runLast+1)
        $hasBefore=$primaryByOrdinal.ContainsKey($beforeKey)
        $hasAfter=$primaryByOrdinal.ContainsKey($afterKey)
        if($cohort-in@('HEAD_EDGE','TAIL_EDGE')){
            # edge は近傍が片側しか無い。値を作らずそのまま記録する。
            $events+=,[ordered]@{
                cohort=$cohort
                missing_ordinals=@($missingRun|ForEach-Object{[string]$_})
                gap_width=$missingRun.Count
                before_primary=$(if($hasBefore){Get-MvmW4BPrimarySnapshot $primaryByOrdinal[$beforeKey]}else{$null})
                after_primary=$(if($hasAfter){Get-MvmW4BPrimarySnapshot $primaryByOrdinal[$afterKey]}else{$null})
                transition=$null
                interval_diagnostics=$null
            }
            continue
        }
        if(-not$hasBefore-or-not$hasAfter){
            # run 内に近傍が無い。cross-run で埋めない。
            $blockers['PRIMARY_NEIGHBOR_MISSING_IN_RUN']=$true
            continue
        }
        $before=$primaryByOrdinal[$beforeKey];$after=$primaryByOrdinal[$afterKey]
        # exactness の前提。片方でも exact でなければ delta を数値として出さない。
        foreach($neighbor in @($before,$after)){
            if(-not[bool](Get-MvmW4BValue $neighbor 'decision_qpc_exact')){$blockers['DECISION_QPC_NOT_EXACT']=$true}
            if(-not[bool](Get-MvmW4BValue $neighbor 'producer_semantics_exact')){$blockers['PRODUCER_SEMANTICS_NOT_EXACT']=$true}
        }
        $beforeSnapshot=Get-MvmW4BPrimarySnapshot $before
        $afterSnapshot=Get-MvmW4BPrimarySnapshot $after
        # open interval 内の duplicate callback。primary の意味を汚さず別 diagnostic にする。
        $intervalDuplicates=0L
        foreach($decision in $DecisionRecords){
            if(-not[bool](Get-MvmW4BValue $decision 'duplicate_callback')){continue}
            $qpc=[int64](Get-MvmW4BValue $decision 'decision_qpc')
            if($qpc-gt[int64]$beforeSnapshot.decision_qpc-and$qpc-lt[int64]$afterSnapshot.decision_qpc){
                $intervalDuplicates+=1
            }
        }
        $intervalDuplicateTotal+=$intervalDuplicates
        $events+=,[ordered]@{
            cohort=$cohort
            missing_ordinals=@($missingRun|ForEach-Object{[string]$_})
            gap_width=$missingRun.Count
            before_primary=$beforeSnapshot
            after_primary=$afterSnapshot
            transition=[ordered]@{
                primary_decision_intent_ordinal_delta=([uint64]$afterSnapshot.intent_ordinal-[uint64]$beforeSnapshot.intent_ordinal)
                decision_qpc_delta=([int64]$afterSnapshot.decision_qpc-[int64]$beforeSnapshot.decision_qpc)
                render_begin_qpc_delta=([int64]$afterSnapshot.render_begin_qpc-[int64]$beforeSnapshot.render_begin_qpc)
                target_frame_delta=([int64]$afterSnapshot.target_frame-[int64]$beforeSnapshot.target_frame)
                last_finalized_opportunity_ordinal_delta=([int64]$afterSnapshot.last_finalized_opportunity_ordinal-[int64]$beforeSnapshot.last_finalized_opportunity_ordinal)
                repeat_before=[bool]$beforeSnapshot.repeat
                repeat_after=[bool]$afterSnapshot.repeat
                past_source_domain_before=[bool]$beforeSnapshot.past_source_domain
                past_source_domain_after=[bool]$afterSnapshot.past_source_domain
            }
            interval_diagnostics=[ordered]@{duplicate_callback_record_count=$intervalDuplicates}
        }
    }

    # --- identity ---
    $eventSum=0L;$intentSum=0L
    foreach($name in $cohortNames){$eventSum+=[int64]$eventCounts[$name];$intentSum+=[int64]$intentCounts[$name]}
    if($intentSum-ne$missingSet.Count){$blockers['COHORT_INTENT_SUM_MISMATCH']=$true}
    if(([int64]$eventCounts['DOUBLE_MISSING_BOUNDARY']*2)-ne[int64]$intentCounts['DOUBLE_MISSING_BOUNDARY']){
        $blockers['DOUBLE_EVENT_INTENT_IDENTITY_VIOLATION']=$true
    }
    if([int64]$eventCounts['ISOLATED_MISSING']-ne[int64]$intentCounts['ISOLATED_MISSING']){
        $blockers['ISOLATED_EVENT_INTENT_IDENTITY_VIOLATION']=$true
    }
    # --- run-level time-domain diagnostic ---
    # measurement window は producer から作らない。W2-A physical authority を受け取る。
    $primarySorted=@($primaryByOrdinal.Values|Sort-Object{[int64](Get-MvmW4BValue $_ 'decision_qpc')})
    # run-level summary は全 primary decision から first/last/cadence を作る。
    # event-local neighbor だけでなく、ここでも全件の exactness を要求する。
    foreach($primary in $primarySorted){
        if(-not[bool](Get-MvmW4BValue $primary 'decision_qpc_exact')){$blockers['DECISION_QPC_NOT_EXACT']=$true}
        if(-not[bool](Get-MvmW4BValue $primary 'producer_semantics_exact')){$blockers['PRODUCER_SEMANTICS_NOT_EXACT']=$true}
    }
    $timeDomain=$null
    if($primarySorted.Count-ge2-and$QpcFrequency-gt0-and$MeasurementEndQpcExclusive-gt$MeasurementStartQpc){
        $firstQpc=[int64](Get-MvmW4BValue $primarySorted[0] 'decision_qpc')
        $lastQpc=[int64](Get-MvmW4BValue $primarySorted[-1] 'decision_qpc')
        $spanQpc=$lastQpc-$firstQpc
        $spanSeconds=[double]$spanQpc/[double]$QpcFrequency
        $windowSeconds=[double]($MeasurementEndQpcExclusive-$MeasurementStartQpc)/[double]$QpcFrequency
        # decision が N 件なら interval は N-1 個である。N / span にしない。
        $cadence=$(if($spanSeconds-gt0){[double]($primarySorted.Count-1)/$spanSeconds}else{$null})
        $trailingMissing=0L
        foreach($ordinal in $requiredSorted){
            if($missingSet.ContainsKey([string]$ordinal)){$trailingMissing+=1}else{$trailingMissing=0}
        }
        $timeDomain=[ordered]@{
            primary_decision_first_qpc=$firstQpc
            primary_decision_last_qpc=$lastQpc
            primary_decision_active_span_qpc=$spanQpc
            primary_decision_active_span_seconds=$spanSeconds
            primary_decision_count=$primarySorted.Count
            primary_decision_interdecision_cadence_hz=$cadence
            measurement_window_seconds=$windowSeconds
            primary_decision_active_span_fraction=$(if($windowSeconds-gt0){$spanSeconds/$windowSeconds}else{$null})
            head_without_primary_decision_seconds=[double]($firstQpc-$MeasurementStartQpc)/[double]$QpcFrequency
            tail_without_primary_decision_seconds=[double]($MeasurementEndQpcExclusive-$lastQpc)/[double]$QpcFrequency
            first_primary_intent_ordinal=[string]([uint64](Get-MvmW4BValue $primarySorted[0] 'intent_ordinal'))
            last_primary_intent_ordinal=[string]([uint64](Get-MvmW4BValue $primarySorted[-1] 'intent_ordinal'))
            first_required_intent_ordinal=$(if($requiredSorted.Count-gt0){[string]$requiredSorted[0]}else{$null})
            last_required_intent_ordinal=$(if($requiredSorted.Count-gt0){[string]$requiredSorted[-1]}else{$null})
            trailing_missing_required_intent_count=$trailingMissing
            # legacy elapsed は correlation。authority にはしない。
            # tolerance 付きの match bool は作らない。差そのものを保存する。
            legacy_measurement_elapsed_seconds_diagnostic=$LegacyMeasurementElapsedSeconds
            legacy_elapsed_minus_producer_span_seconds=($LegacyMeasurementElapsedSeconds-$spanSeconds)
            legacy_measurement_elapsed_used_as_authority=$false
            decision_span_used_as_measurement_window=$false
        }
    }else{
        $blockers['TIME_DOMAIN_AUTHORITY_UNAVAILABLE']=$true
    }
    $blockerList=@($blockers.Keys|Sort-Object)

    # --- isolated vs double の transition table ---
    function Get-MvmW4BTransitionSummary([array]$Events,[string]$Cohort){
        $subset=@($Events|Where-Object{$_.cohort-eq$Cohort-and$null-ne$_.transition})
        $summary=[ordered]@{event_count=$subset.Count}
        foreach($field in @('primary_decision_intent_ordinal_delta','decision_qpc_delta',
            'render_begin_qpc_delta','target_frame_delta','last_finalized_opportunity_ordinal_delta')){
            $distribution=@{}
            foreach($transitionEvent in $subset){
                $key=[string]$transitionEvent.transition.$field
                $distribution[$key]=[int64](& {if($distribution.ContainsKey($key)){$distribution[$key]}else{0}})+1
            }
            # 分布が大きい QPC delta は代表値だけを持つ。
            if($field-in@('decision_qpc_delta','render_begin_qpc_delta')){
                $values=@($subset|ForEach-Object{[int64]$_.transition.$field}|Sort-Object)
                $summary[$field]=$(if($values.Count-eq0){$null}else{[ordered]@{
                    min=$values[0];max=$values[-1]
                    median=$values[[int][Math]::Floor($values.Count/2)]
                    distinct_count=@($values|Select-Object -Unique).Count}})
            }else{
                $ordered=[ordered]@{}
                foreach($key in @($distribution.Keys|Sort-Object{[int64]$_})){$ordered[$key]=$distribution[$key]}
                $summary[$field]=$ordered
            }
        }
        foreach($field in @('repeat_before','repeat_after','past_source_domain_before','past_source_domain_after')){
            $summary[$field+'_true_count']=@($subset|Where-Object{[bool]$_.transition.$field}).Count
        }
        $summary['duplicate_callback_record_count_total']=
            [int64](@($subset|ForEach-Object{[int64]$_.interval_diagnostics.duplicate_callback_record_count}|
                Measure-Object -Sum).Sum)
        return $summary
    }

    return [ordered]@{
        schema='mvm-p2-d5-2-w4-b-producer-semantics-run-1'
        population='EXACT_REQUIRED_CURRENT_INTENT_SET'
        cohort_classification='MAXIMAL_CONSECUTIVE_MISSING_RUN'
        cohort_precedence='HEAD_EDGE/TAIL_EDGE -> LONGER_MISSING_RUN -> DOUBLE_MISSING_BOUNDARY -> ISOLATED_MISSING -> OTHER_PATTERN'
        semantic_interpretation='SCHEDULER_OPPORTUNITY_ORDINAL_DELTA_VIA_W2_B1_INTENT_IDENTITY'
        missing_ordinal_producer_field_interpolated=$false
        nearest_decision_qpc_used=$false
        cross_run_neighbor_splice_used=$false
        required_intent_count=$requiredSet.Count
        missing_intent_count=$missingSet.Count
        primary_decision_count=$primaryByOrdinal.Count
        cohort_event_counts=$eventCounts
        cohort_intent_counts=$intentCounts
        cohort_event_sum=$eventSum
        cohort_intent_sum=$intentSum
        interval_duplicate_callback_total=$intervalDuplicateTotal
        run_level_time_domain_diagnostic_present=($null-ne$timeDomain)
        time_domain_diagnostic=$timeDomain
        isolated_transition_summary=(Get-MvmW4BTransitionSummary $events 'ISOLATED_MISSING')
        double_transition_summary=(Get-MvmW4BTransitionSummary $events 'DOUBLE_MISSING_BOUNDARY')
        attribution_exact=$blockerList.Count-eq0
        blockers=$blockerList
        events=$events
    }
}
