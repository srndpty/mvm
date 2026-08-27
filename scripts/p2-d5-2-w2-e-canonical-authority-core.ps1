Set-StrictMode -Version Latest

# P2-D5-2-W2-E.1 canonical cutover contract。
#
# W2-E は性能評価の段ではない。authority selector を切り替える段である。
#
#   Before: canonical presentation authority = historical frameSwapped / DWM path
#           formal-v2                        = validated shadow only
#   After : canonical presentation authority = formal-v2 exact chain
#           historical frameSwapped / DWM    = diagnostic / non-authoritative only
#
# canonical performance verdict (fps / drop / threshold / PASS/FAIL) はここでは出さない。
# W3 の formal-v2 fresh acquisition まで保留する。
#
# W2-D shadow artifact の boolean を反転させるのではなく、W2-D checker を再実行した
# うえで sealed authority から canonical statement を独立構築する。

function Get-MvmEValue($Object,[string]$Name){
    if($Object-is[System.Collections.IDictionary]){
        if(-not$Object.Contains($Name)){return $null}
        return $Object[$Name]
    }
    if($null-eq$Object-or$Object.PSObject.Properties.Name-notcontains$Name){return $null}
    return $Object.$Name
}

function Get-MvmERequired($Object,[string]$Name){
    if($Object-is[System.Collections.IDictionary]){
        if(-not$Object.Contains($Name)){throw "W2-E必須fieldがありません: $Name"}
        return $Object[$Name]
    }
    if($null-eq$Object-or$Object.PSObject.Properties.Name-notcontains$Name){throw "W2-E必須fieldがありません: $Name"}
    return $Object.$Name
}

# canonical chain の宣言。ここで新しい identity を作らない。
$script:MvmECanonicalSource='intent -> composition_token -> native_present -> exact_present_event -> final_state -> displayed_qpc -> physical_vblank_ordinal'

function Invoke-MvmECanonicalAuthority {
    [CmdletBinding()]
    param(
        # W2-D checker を再実行したうえで sealed authority から再構築した integration。
        [Parameter(Mandatory=$true)]$FormalV2Integration,
        [Parameter(Mandatory=$true)][string]$W2DProofPath,
        [Parameter(Mandatory=$true)][string]$W2DProofSha256,
        [Parameter(Mandatory=$true)][string]$W2DVerdict,
        [Parameter(Mandatory=$true)]$RetirementInventory
    )
    $blockers=@{}
    # shadow が exact でなければ canonical へ昇格させない。
    if(-not[bool](Get-MvmERequired $FormalV2Integration 'integration_exact')){
        $blockers['FORMAL_V2_SHADOW_NOT_EXACT']=$true
    }
    if([string](Get-MvmERequired $FormalV2Integration 'verdict')-ne'FORMAL_V2_SHADOW_INTEGRATION_EXACT'){
        $blockers['FORMAL_V2_SHADOW_NOT_EXACT']=$true
    }
    # historical verdict を書き換えない。W2-D artifact が主張した verdict をそのまま運ぶ。
    if($W2DVerdict-ne[string](Get-MvmERequired $FormalV2Integration 'verdict')){
        $blockers['HISTORICAL_VERDICT_REWRITTEN']=$true
    }
    # legacy authority が canonical verdict へ到達していないこと。
    if(-not[bool](Get-MvmERequired $RetirementInventory 'retirement_exact')){
        $blockers['LEGACY_AUTHORITY_STILL_CANONICAL']=$true
    }
    if([int64](Get-MvmERequired $RetirementInventory 'legacy_metric_canonical_decision_count')-ne0){
        $blockers['LEGACY_METRIC_FEEDS_CANONICAL_VERDICT']=$true
    }
    if([int64](Get-MvmERequired $RetirementInventory 'legacy_metric_unclassified_count')-ne0){
        $blockers['LEGACY_METRIC_FAILURE_SITE_UNCLASSIFIED']=$true
    }
    # retirement = deletion ではない。diagnostic が消えていたらそれも不正である。
    if(-not[bool](Get-MvmERequired $RetirementInventory 'legacy_diagnostics_retained')){
        $blockers['LEGACY_DIAGNOSTIC_SOURCE_MISSING']=$true
    }
    # canonical chain へ持ち込んではいけない identity / matching。
    if([bool](Get-MvmERequired $FormalV2Integration 'source_frame_identity_used')){
        $blockers['CANONICAL_SOURCE_FRAME_IDENTITY']=$true
    }
    if([bool](Get-MvmERequired $FormalV2Integration 'nearest_qpc_or_tolerance_used')){
        $blockers['CANONICAL_NEAREST_QPC_FALLBACK']=$true
    }
    foreach($run in @(Get-MvmERequired $FormalV2Integration 'runs')){
        foreach($record in @(Get-MvmEValue $run 'records')){
            if($record.PSObject.Properties.Name-match'^source_frame'){
                $blockers['CANONICAL_SOURCE_FRAME_IDENTITY']=$true
            }
        }
    }
    $blockerList=@($blockers.Keys|Sort-Object)

    $runs=@()
    foreach($run in @(Get-MvmERequired $FormalV2Integration 'runs')){
        $runs+=,[ordered]@{
            run=[int](Get-MvmERequired $run 'run')
            canonical_required_intent_count=[int64](Get-MvmERequired $run 'required_intent_count')
            canonical_satisfied_intent_count=[int64](Get-MvmERequired $run 'satisfied_intent_count')
            canonical_unsatisfied_intent_count=[int64](Get-MvmERequired $run 'unsatisfied_intent_count')
            canonical_formal_presented_event_count=[int64](Get-MvmERequired $run 'formal_presented_event_count')
            canonical_in_domain_presented_event_count=[int64](Get-MvmERequired $run 'in_domain_presented_event_count')
            canonical_in_domain_presented_foreign_intent_count=[int64](Get-MvmERequired $run 'in_domain_presented_foreign_intent_count')
            canonical_physical_vblank_opportunity_count=[int64](Get-MvmERequired $run 'physical_vblank_opportunity_count')
            canonical_filled_physical_opportunity_count=[int64](Get-MvmERequired $run 'filled_physical_opportunity_count')
            layer1a_required_accounting_identity_exact=[bool](Get-MvmERequired $run 'layer1a_required_accounting_identity_exact')
            presented_accounting_identity_exact=[bool](Get-MvmERequired $run 'presented_accounting_identity_exact')
            filled_physical_opportunity_identity_exact=[bool](Get-MvmERequired $run 'filled_physical_opportunity_identity_exact')
            physical_vblank_domain_cardinality_exact=[bool](Get-MvmERequired $run 'physical_vblank_domain_cardinality_exact')
            sealed_source_sha256=Get-MvmERequired $run 'sealed_source_sha256'
            records=@(Get-MvmERequired $run 'records')
        }
    }
    return [ordered]@{
        schema='mvm-p2-d5-2-w2-e-canonical-authority-1';stage='P2-D5-2-W2-E'
        presentation_authority_schema='FORMAL_V2'
        canonical_authority=$true
        canonical_source=$script:MvmECanonicalSource
        # 旧 authority は diagnostic として残るが canonical ではない。
        frame_swapped_authority=$false
        dwm_frame_statistics_authority=$false
        legacy_presentation_authority_retired=$true
        legacy_diagnostics_retained=[bool](Get-MvmERequired $RetirementInventory 'legacy_diagnostics_retained')
        retirement_means_deletion=$false
        legacy_metric_canonical_decision_count=[int64](Get-MvmERequired $RetirementInventory 'legacy_metric_canonical_decision_count')
        legacy_metric_diagnostic_integrity_count=[int64](Get-MvmERequired $RetirementInventory 'legacy_metric_diagnostic_integrity_count')
        legacy_metric_unclassified_count=[int64](Get-MvmERequired $RetirementInventory 'legacy_metric_unclassified_count')
        # provenance
        source_w2d_proof=$W2DProofPath
        source_w2d_proof_sha256=$W2DProofSha256
        source_w2d_verdict=$W2DVerdict
        c1_checkpoint_sha=[string](Get-MvmERequired $FormalV2Integration 'c1_checkpoint_sha')
        source_c1_proof_sha256=[string](Get-MvmERequired $FormalV2Integration 'source_c1_proof_sha256')
        source_c21_proof_sha256=[string](Get-MvmERequired $FormalV2Integration 'source_c21_proof_sha256')
        source_c2_proof_sha256=[string](Get-MvmERequired $FormalV2Integration 'source_c2_proof_sha256')
        source_upstream_inventory_proof_sha256=[string](Get-MvmERequired $FormalV2Integration 'source_upstream_inventory_proof_sha256')
        # canonical accounting (W2-D の値をコピーせず再構築結果から取る)
        run_count=$runs.Count
        canonical_required_intent_count=[int64](Get-MvmERequired $FormalV2Integration 'required_intent_count')
        canonical_satisfied_intent_count=[int64](Get-MvmERequired $FormalV2Integration 'satisfied_intent_count')
        canonical_unsatisfied_intent_count=[int64](Get-MvmERequired $FormalV2Integration 'unsatisfied_intent_count')
        canonical_formal_presented_event_count=[int64](Get-MvmERequired $FormalV2Integration 'formal_presented_event_count')
        canonical_in_domain_presented_event_count=[int64](Get-MvmERequired $FormalV2Integration 'in_domain_presented_event_count')
        canonical_in_domain_presented_foreign_intent_count=[int64](Get-MvmERequired $FormalV2Integration 'in_domain_presented_foreign_intent_count')
        canonical_physical_vblank_opportunity_count=[int64](Get-MvmERequired $FormalV2Integration 'physical_vblank_opportunity_count')
        canonical_filled_physical_opportunity_count=[int64](Get-MvmERequired $FormalV2Integration 'filled_physical_opportunity_count')
        layer1a_required_accounting_identity_exact=[bool](Get-MvmERequired $FormalV2Integration 'layer1a_required_accounting_identity_exact')
        presented_accounting_identity_exact=[bool](Get-MvmERequired $FormalV2Integration 'presented_accounting_identity_exact')
        filled_physical_opportunity_identity_exact=[bool](Get-MvmERequired $FormalV2Integration 'filled_physical_opportunity_identity_exact')
        physical_vblank_domain_cardinality_exact=[bool](Get-MvmERequired $FormalV2Integration 'physical_vblank_domain_cardinality_exact')
        c2_ledger_agreement_exact=[bool](Get-MvmERequired $FormalV2Integration 'c2_ledger_agreement_exact')
        layer1a_layer1b_count_difference_is_not_a_verdict=$true
        source_frame_identity_used=$false
        nearest_qpc_or_tolerance_used=$false
        # W2-E で出さないもの。
        performance_threshold_evaluated=$false
        canonical_verdict_evaluated=$false
        canonical_performance_verdict_deferred_to='W3'
        historical_verdicts_rewritten=$false
        cutover_exact=$blockerList.Count-eq0
        blockers=$blockerList
        runs=$runs
        verdict=$(if($blockerList.Count-eq0){'CANONICAL_PRESENTATION_AUTHORITY_FORMAL_V2'}else{'CANONICAL_CUTOVER_INVALID'})
    }
}

function Assert-MvmECanonicalProof {
    [CmdletBinding()]
    param([Parameter(Mandatory=$true)]$Expected,[Parameter(Mandatory=$true)]$Actual)
    $expectedJson=$Expected|ConvertTo-Json -Depth 16 -Compress
    $actualJson=$Actual|ConvertTo-Json -Depth 16 -Compress
    if($expectedJson-ne$actualJson){throw 'W2-E canonical artifactがclosed authorityからの再構築結果と一致しません'}
}
