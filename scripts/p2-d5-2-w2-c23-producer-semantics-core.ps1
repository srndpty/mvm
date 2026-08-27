Set-StrictMode -Version Latest

function Invoke-MvmC23ProducerSemanticsAttribution {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory=$true)]$C21ProofObject,
        [Parameter(Mandatory=$true)][string]$C21ProofPath
    )
    $blockers=@{}
    if(-not[bool]$C21ProofObject.authority_exact-or-not[bool]$C21ProofObject.branch_a_established){
        $blockers['C21_BRANCH_A_AUTHORITY_INVALID']=$true
    }
    $runs=@()
    foreach($sourceRun in @($C21ProofObject.runs)){
        $targets=@($sourceRun.decisions|Where-Object{
            $_.producer_scope-eq'CURRENT_MEASUREMENT'-and$_.opportunity_ordinal-in@('0','301')
        })
        if(@($targets|Where-Object{-not[bool]$_.producer_semantics_exact}).Count-ne0){
            $blockers['PRODUCER_SEMANTICS_PROVENANCE_MISSING']=$true
        }
        if(@($targets|Where-Object{-not[bool]$_.formal_presented_reverse_attribution_exact}).Count-ne0){
            $blockers['TARGET_REVERSE_ATTRIBUTION_INVALID']=$true
        }
        $zero=@($targets|Where-Object{$_.opportunity_ordinal-eq'0'})
        $ordinal301=@($targets|Where-Object{$_.opportunity_ordinal-eq'301'})
        $zeroTokens=@($zero.token_serial|Select-Object -Unique)
        $zeroNative=@($zero.native_present_serial|Select-Object -Unique)
        $zeroFormal=@($zero.formal_presented_event_keys|ForEach-Object{$_}|Select-Object -Unique)
        $zeroDuplicateAttributed=$zero.Count-eq2-and
            @($zero|Where-Object{[bool]$_.duplicate_callback}).Count-eq1-and
            @($zero|Where-Object{-not[bool]$_.duplicate_callback}).Count-eq1-and
            @($zero.render_begin_qpc|Select-Object -Unique).Count-eq1-and
            @($zero.decision_qpc|Select-Object -Unique).Count-eq2-and
            $zeroTokens.Count-eq2-and$zeroNative.Count-eq2-and$zeroFormal.Count-eq2-and
            @($zero|Where-Object{[int64]$_.formal_presented_count-ne1}).Count-eq0
        $outsideScopeAttributed=$ordinal301.Count-eq1-and
            -not[bool]$ordinal301[0].required_current_membership-and
            [bool]$ordinal301[0].required_current_membership_exact-and
            [bool]$ordinal301[0].past_source_domain-and
            $ordinal301[0].checker_derived_measurement_boundary_relation-eq'WITHIN_CURRENT_MEASUREMENT'-and
            [int64]$ordinal301[0].formal_presented_count-eq1
        if(-not$zeroDuplicateAttributed){
            $blockers['ORDINAL_ZERO_DUPLICATE_CALLBACK_ATTRIBUTION_INVALID']=$true
        }
        if(-not$outsideScopeAttributed){
            $blockers['ORDINAL_301_SCOPE_MEMBERSHIP_ATTRIBUTION_INVALID']=$true
        }
        $runs+=,[ordered]@{
            run=[int]$sourceRun.run
            ordinal_zero_current_decision_count=$zero.Count
            ordinal_zero_duplicate_callback_attribution_exact=$zeroDuplicateAttributed
            ordinal_zero_decisions=$zero
            ordinal_301_current_decision_count=$ordinal301.Count
            ordinal_301_current_outside_required_set_attribution_exact=$outsideScopeAttributed
            ordinal_301_decisions=$ordinal301
        }
    }
    $blockerList=@($blockers.Keys|Sort-Object)
    return [ordered]@{
        schema='mvm-p2-d5-2-w2-c23-producer-semantics-attribution-1'
        stage='P2-D5-2-W2-C2.3'
        source_c21_proof=(Resolve-Path -LiteralPath $C21ProofPath).Path
        source_c21_proof_sha256=(Get-FileHash -LiteralPath $C21ProofPath -Algorithm SHA256).Hash.ToLowerInvariant()
        input_authority='C21_BRANCH_A_EXACT_DECISIONS_WITH_C1_REVERSE_ATTRIBUTION'
        run_count=$runs.Count
        ordinal_zero_duplicate_callback_attribution_exact=@($runs|Where-Object{-not[bool]$_.ordinal_zero_duplicate_callback_attribution_exact}).Count-eq0
        ordinal_301_scope_membership_conflict_attribution_exact=@($runs|Where-Object{-not[bool]$_.ordinal_301_current_outside_required_set_attribution_exact}).Count-eq0
        product_fix_evaluated=$false
        performance_integration_evaluated=$false
        authority_exact=$blockerList.Count-eq0
        blockers=$blockerList
        runs=$runs
        verdict=$(if($blockerList.Count-eq0){'PRODUCER_SEMANTICS_ATTRIBUTION_EXACT'}else{'PRODUCER_SEMANTICS_ATTRIBUTION_INVALID'})
    }
}

function Assert-MvmC23Proof {
    [CmdletBinding()]
    param([Parameter(Mandatory=$true)]$Expected,[Parameter(Mandatory=$true)]$Actual)
    if(($Expected|ConvertTo-Json -Depth 16 -Compress)-ne($Actual|ConvertTo-Json -Depth 16 -Compress)){
        throw 'C2.3 artifactがsealed C2.1 sourceからの再計算結果と一致しません'
    }
}
