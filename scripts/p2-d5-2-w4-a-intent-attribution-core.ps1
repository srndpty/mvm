Set-StrictMode -Version Latest

# P2-D5-2-W4-A unsatisfied intent attribution core。
#
# W3 は canonical performance verdict を FAIL で確定した (drop 51.611%)。W4-A は
# その 5574 件の unsatisfied が chain のどこで失われたかを exact に分類する段である。
# 原因判定はしない。partition を作るだけである。
#
# 母集団は各 run の exact required current intent set だけである。
# 1 intent identity につき必ずちょうど 1 bucket へ入る。
#
#   REQUIRED CURRENT INTENT
#   ├─ A. NO_PRIMARY_SCHEDULER_DECISION
#   └─ primary decision exists
#        ├─ C. NO_FORMAL_TRANSPORT_ELIGIBLE_PRIMARY
#        ├─ D. NO_NATIVE_PRESENT
#        ├─ E. NO_EXACT_FORMAL_PRESENTED
#        ├─ F. FORMAL_PRESENTED_OUTSIDE_DOMAIN
#        └─ G. SATISFIED_IN_DOMAIN
#
# B (MULTIPLE_PRIMARY_DECISIONS) は bucket にしない。同一 current intent が複数回
# decision 生成されるのは producer semantic corruption であり、performance loss へ
# 混ぜずに authority / provenance INVALID として fail-close する。
#
# duplicate callback / outside-required decision は母集団外の diagnostic である。
# 「satisfied + suppressed」の 2 intent として数えない。

function Get-MvmW4Value($Object,[string]$Name){
    if($Object-is[System.Collections.IDictionary]){
        if(-not$Object.Contains($Name)){return $null}
        return $Object[$Name]
    }
    if($null-eq$Object-or$Object.PSObject.Properties.Name-notcontains$Name){return $null}
    return $Object.$Name
}

function Get-MvmW4Ordinal($Value){
    if($null-eq$Value-or[string]::IsNullOrWhiteSpace([string]$Value)){return $null}
    return [string]([uint64]$Value)
}

# missing ordinal set の構造を出す。原因判定はしない。結果として読むだけである。
function Get-MvmW4MissingStructure([string[]]$MissingOrdinals,[string[]]$PresentOrdinals){
    $missing=@($MissingOrdinals|ForEach-Object{[uint64]$_}|Sort-Object)
    $present=@($PresentOrdinals|ForEach-Object{[uint64]$_}|Sort-Object)
    $deltas=@{}
    for($index=1;$index-lt$missing.Count;++$index){
        $delta=[string]($missing[$index]-$missing[$index-1])
        $deltas[$delta]=[int64](& {if($deltas.ContainsKey($delta)){$deltas[$delta]}else{0}})+1
    }
    # 連続 missing の run-length。
    $runLengths=@{};$currentRun=0
    for($index=0;$index-lt$missing.Count;++$index){
        if($index-eq0-or$missing[$index]-ne$missing[$index-1]+1){
            if($currentRun-gt0){
                $key=[string]$currentRun
                $runLengths[$key]=[int64](& {if($runLengths.ContainsKey($key)){$runLengths[$key]}else{0}})+1
            }
            $currentRun=1
        }else{$currentRun+=1}
    }
    if($currentRun-gt0){
        $key=[string]$currentRun
        $runLengths[$key]=[int64](& {if($runLengths.ContainsKey($key)){$runLengths[$key]}else{0}})+1
    }
    # 周期性。仮説から判定を作らない。分布をそのまま出す。
    $modulo=[ordered]@{}
    foreach($base in 2..8){
        $buckets=@{}
        foreach($ordinal in $missing){
            $key=[string]([int]($ordinal % [uint64]$base))
            $buckets[$key]=[int64](& {if($buckets.ContainsKey($key)){$buckets[$key]}else{0}})+1
        }
        $modulo["mod_$base"]=[ordered]@{}
        foreach($key in @($buckets.Keys|Sort-Object{[int]$_})){$modulo["mod_$base"][$key]=$buckets[$key]}
    }
    return [ordered]@{
        missing_count=$missing.Count
        first_missing_ordinal=$(if($missing.Count-gt0){[string]$missing[0]}else{$null})
        last_missing_ordinal=$(if($missing.Count-gt0){[string]$missing[-1]}else{$null})
        min_present_ordinal=$(if($present.Count-gt0){[string]$present[0]}else{$null})
        max_present_ordinal=$(if($present.Count-gt0){[string]$present[-1]}else{$null})
        missing_delta_distribution=[ordered]@{}+(& {
            $ordered=[ordered]@{}
            foreach($key in @($deltas.Keys|Sort-Object{[uint64]$_})){$ordered[$key]=$deltas[$key]}
            $ordered})
        consecutive_missing_run_length_distribution=[ordered]@{}+(& {
            $ordered=[ordered]@{}
            foreach($key in @($runLengths.Keys|Sort-Object{[int]$_})){$ordered[$key]=$runLengths[$key]}
            $ordered})
        missing_ordinal_modulo_distribution=$modulo
    }
}

function Invoke-MvmW4IntentAttribution {
    [CmdletBinding()]
    param(
        # C2.1 の exact required current intent set。
        [Parameter(Mandatory=$true)][AllowEmptyCollection()][array]$RequiredIntentOrdinals,
        # producer の scheduler decision ledger (intent_scope_provenance.records)。
        [Parameter(Mandatory=$true)][AllowEmptyCollection()][array]$DecisionRecords,
        # native Present ring の record。
        [Parameter(Mandatory=$true)][AllowEmptyCollection()][array]$NativePresentRecords,
        # C1 formal mapping record (exact PresentEvent -> physical ordinal)。
        [Parameter(Mandatory=$true)][AllowEmptyCollection()][array]$FormalMappingRecords
    )
    $blockers=@{}
    $requiredSet=[ordered]@{}
    foreach($ordinal in $RequiredIntentOrdinals){
        $key=Get-MvmW4Ordinal $ordinal
        if($null-eq$key){$blockers['REQUIRED_INTENT_ORDINAL_MISSING']=$true;continue}
        if($requiredSet.Contains($key)){$blockers['REQUIRED_INTENT_SET_DUPLICATE']=$true}
        $requiredSet[$key]=$true
    }
    if($requiredSet.Count-eq0){$blockers['REQUIRED_INTENT_SET_EMPTY']=$true}

    # --- primary scheduler decision の exact な定義 ---
    # 「ordinal X の decision record が存在する」では弱い。membership が exact に
    # 立っていて duplicate callback でないものだけを primary とする。
    $primaryByOrdinal=@{}
    $duplicateCallbackSuppressed=0L;$outsideRequiredDecision=0L;$nonCurrentDecision=0L
    $membershipNotExact=0L
    foreach($decision in $DecisionRecords){
        $scope=[string](Get-MvmW4Value $decision 'intent_scope')
        $ordinal=Get-MvmW4Ordinal (Get-MvmW4Value $decision 'intent_ordinal')
        if($scope-ne'CURRENT_MEASUREMENT'){++$nonCurrentDecision;continue}
        if(-not[bool](Get-MvmW4Value $decision 'required_current_membership_exact')){
            ++$membershipNotExact;continue
        }
        if([bool](Get-MvmW4Value $decision 'duplicate_callback')){++$duplicateCallbackSuppressed;continue}
        if(-not[bool](Get-MvmW4Value $decision 'required_current_membership')){
            ++$outsideRequiredDecision;continue
        }
        if($null-eq$ordinal-or-not$requiredSet.Contains($ordinal)){++$outsideRequiredDecision;continue}
        if(-not$primaryByOrdinal.ContainsKey($ordinal)){$primaryByOrdinal[$ordinal]=@()}
        $primaryByOrdinal[$ordinal]+=,$decision
    }
    if($membershipNotExact-ne0){$blockers['REQUIRED_INTENT_MEMBERSHIP_NOT_EXACT']=$true}
    $multiplePrimary=0L
    foreach($ordinal in @($primaryByOrdinal.Keys)){
        if($primaryByOrdinal[$ordinal].Count-gt1){$multiplePrimary+=1}
    }
    # B は bucket にしない。producer semantic corruption として fail-close する。
    if($multiplePrimary-ne0){$blockers['REQUIRED_INTENT_PRIMARY_DECISION_DUPLICATE']=$true}

    # --- downstream evidence を intent ordinal で索く ---
    $nativeByOrdinal=@{}
    foreach($native in $NativePresentRecords){
        if(-not[bool](Get-MvmW4Value $native 'intent_ordinal_valid')){continue}
        $ordinal=Get-MvmW4Ordinal (Get-MvmW4Value $native 'intent_ordinal')
        if($null-eq$ordinal){continue}
        if(-not$nativeByOrdinal.ContainsKey($ordinal)){$nativeByOrdinal[$ordinal]=0}
        $nativeByOrdinal[$ordinal]+=1
    }
    $formalByOrdinal=@{}
    foreach($mapping in $FormalMappingRecords){
        if(-not[bool](Get-MvmW4Value $mapping 'mapping_exact')){continue}
        if([string](Get-MvmW4Value $mapping 'intent_scope')-ne'CURRENT_MEASUREMENT'){continue}
        $ordinal=Get-MvmW4Ordinal (Get-MvmW4Value $mapping 'intent_ordinal')
        if($null-eq$ordinal){continue}
        if(-not$formalByOrdinal.ContainsKey($ordinal)){$formalByOrdinal[$ordinal]=@()}
        $formalByOrdinal[$ordinal]+=,$mapping
    }

    # --- 排他的 partition ---
    $bucketNames=@('A_NO_PRIMARY_SCHEDULER_DECISION','C_NO_FORMAL_TRANSPORT_ELIGIBLE_PRIMARY',
        'D_NO_NATIVE_PRESENT','E_NO_EXACT_FORMAL_PRESENTED','F_FORMAL_PRESENTED_OUTSIDE_DOMAIN',
        'G_SATISFIED_IN_DOMAIN')
    $buckets=[ordered]@{}
    foreach($name in $bucketNames){$buckets[$name]=0L}
    $bucketOfOrdinal=@{}
    foreach($ordinal in @($requiredSet.Keys)){
        $bucket=$null
        if(-not$primaryByOrdinal.ContainsKey($ordinal)){
            $bucket='A_NO_PRIMARY_SCHEDULER_DECISION'
        }else{
            $primary=$primaryByOrdinal[$ordinal][0]
            if(-not[bool](Get-MvmW4Value $primary 'formal_transport_eligible')){
                $bucket='C_NO_FORMAL_TRANSPORT_ELIGIBLE_PRIMARY'
            }elseif(-not$nativeByOrdinal.ContainsKey($ordinal)){
                $bucket='D_NO_NATIVE_PRESENT'
            }elseif(-not$formalByOrdinal.ContainsKey($ordinal)){
                $bucket='E_NO_EXACT_FORMAL_PRESENTED'
            }elseif(@($formalByOrdinal[$ordinal]|Where-Object{
                [bool](Get-MvmW4Value $_ 'in_measurement_physical_domain')}).Count-eq0){
                $bucket='F_FORMAL_PRESENTED_OUTSIDE_DOMAIN'
            }else{
                $bucket='G_SATISFIED_IN_DOMAIN'
            }
        }
        $buckets[$bucket]+=1
        $bucketOfOrdinal[$ordinal]=$bucket
    }

    # --- identity ---
    $bucketSum=0L
    foreach($name in $bucketNames){$bucketSum+=[int64]$buckets[$name]}
    $satisfied=[int64]$buckets['G_SATISFIED_IN_DOMAIN']
    $unsatisfied=$bucketSum-$satisfied
    $partitionExact=$bucketSum-eq$requiredSet.Count
    if(-not$partitionExact){$blockers['REQUIRED_INTENT_PARTITION_NOT_EXHAUSTIVE']=$true}
    if($bucketOfOrdinal.Count-ne$requiredSet.Count){$blockers['REQUIRED_INTENT_PARTITION_NOT_EXCLUSIVE']=$true}

    # --- missing set は集合差そのものから作る ---
    $missing=@();$present=@()
    foreach($ordinal in @($requiredSet.Keys)){
        if($primaryByOrdinal.ContainsKey($ordinal)){$present+=,$ordinal}else{$missing+=,$ordinal}
    }
    $structure=Get-MvmW4MissingStructure -MissingOrdinals $missing -PresentOrdinals $present
    if([int64]$structure.missing_count-ne[int64]$buckets['A_NO_PRIMARY_SCHEDULER_DECISION']){
        $blockers['MISSING_SET_BUCKET_MISMATCH']=$true
    }
    $blockerList=@($blockers.Keys|Sort-Object)

    return [ordered]@{
        schema='mvm-p2-d5-2-w4-a-intent-attribution-run-1'
        population='EXACT_REQUIRED_CURRENT_INTENT_SET'
        primary_decision_definition='CURRENT_MEASUREMENT AND membership_exact AND membership AND NOT duplicate_callback AND ordinal IN required_set'
        multiple_primary_decision_is_authority_invalid=$true
        duplicate_callback_counted_as_intent=$false
        outside_required_decision_counted_as_intent=$false
        required_intent_count=$requiredSet.Count
        buckets=$buckets
        bucket_sum=$bucketSum
        satisfied_intent_count=$satisfied
        unsatisfied_intent_count=$unsatisfied
        partition_exhaustive=$partitionExact
        primary_decision_count=$primaryByOrdinal.Count
        multiple_primary_decision_count=$multiplePrimary
        # 母集団外 diagnostic。drop 原因に混ぜない。
        duplicate_callback_suppressed_count=$duplicateCallbackSuppressed
        outside_required_decision_count=$outsideRequiredDecision
        non_current_scope_decision_count=$nonCurrentDecision
        missing_primary_decision_structure=$structure
        # W4-B が exact に追えるよう、集計値だけでなく ordinal set 自体を保存する。
        required_intent_ordinals=@($requiredSet.Keys|ForEach-Object{[uint64]$_}|Sort-Object|ForEach-Object{[string]$_})
        primary_decision_ordinals=@($present|ForEach-Object{[uint64]$_}|Sort-Object|ForEach-Object{[string]$_})
        missing_primary_decision_ordinals=@($missing|ForEach-Object{[uint64]$_}|Sort-Object|ForEach-Object{[string]$_})
        satisfied_intent_ordinals=@($bucketOfOrdinal.Keys|Where-Object{
            $bucketOfOrdinal[$_]-eq'G_SATISFIED_IN_DOMAIN'}|ForEach-Object{[uint64]$_}|Sort-Object|ForEach-Object{[string]$_})
        attribution_exact=$blockerList.Count-eq0
        blockers=$blockerList
    }
}
