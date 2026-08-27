[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'GoodCanonicalCutover','GoodLegacyDiagnosticsRemainPresent',
        'NegativeShadowProofNotExact','NegativeShadowVerdictInvalid','NegativeHistoricalVerdictMutation',
        'NegativeLegacyAuthorityStillEnabled','NegativeLegacyMetricFeedsCanonicalVerdict',
        'NegativeLegacyMetricUnclassified','NegativeLegacyDiagnosticsDeleted',
        'NegativeCanonicalSourceFrameIdentity','NegativeCanonicalNearestQpcFallback',
        'NegativeCanonicalRecordSourceFrameField')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Core
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. $Core

# 期待値は実装の式を呼ばず fixture 側で決める。
$record=[pscustomobject][ordered]@{
    exact_event_key='11|110';intent_ordinal='0';intent_scope='CURRENT_MEASUREMENT'
    ordinal_in_required_intent_set=$true;required_intent_membership=$true
    composition_token_serial='101';native_present_serial='11';etw_sequence=11
    final_state='Presented';displayed_qpc=110;physical_vblank_ordinal=2
    in_measurement_physical_domain=$true;intent_satisfied=$true
}
$integration=[ordered]@{
    schema='mvm-p2-d5-2-w2-d-formal-v2-shadow-1'
    c1_checkpoint_sha='5034bfcd41dd9f5c860827a9594b604be5db7446'
    source_c1_proof_sha256='a'*64;source_c21_proof_sha256='b'*64
    source_c2_proof_sha256='c'*64;source_upstream_inventory_proof_sha256='d'*64
    required_intent_count=3;satisfied_intent_count=1;unsatisfied_intent_count=2
    formal_presented_event_count=1;in_domain_presented_event_count=1
    in_domain_presented_foreign_intent_count=0
    physical_vblank_opportunity_count=3;filled_physical_opportunity_count=1
    layer1a_required_accounting_identity_exact=$true
    presented_accounting_identity_exact=$true
    filled_physical_opportunity_identity_exact=$true
    physical_vblank_domain_cardinality_exact=$true
    c2_ledger_agreement_exact=$true
    source_frame_identity_used=$false;nearest_qpc_or_tolerance_used=$false
    integration_exact=$true;verdict='FORMAL_V2_SHADOW_INTEGRATION_EXACT'
    runs=@([ordered]@{
        run=1;required_intent_count=3;satisfied_intent_count=1;unsatisfied_intent_count=2
        formal_presented_event_count=1;in_domain_presented_event_count=1
        in_domain_presented_foreign_intent_count=0
        physical_vblank_opportunity_count=3;filled_physical_opportunity_count=1
        layer1a_required_accounting_identity_exact=$true
        presented_accounting_identity_exact=$true
        filled_physical_opportunity_identity_exact=$true
        physical_vblank_domain_cardinality_exact=$true
        sealed_source_sha256=[ordered]@{traced_app='e'*64}
        records=@($record)
    })
}
$retirement=[ordered]@{
    retirement_exact=$true
    legacy_metric_canonical_decision_count=0
    legacy_metric_diagnostic_integrity_count=2
    legacy_metric_unclassified_count=0
    legacy_diagnostics_retained=$true
}
$w2dVerdict='FORMAL_V2_SHADOW_INTEGRATION_EXACT'

switch($Case){
    'NegativeShadowProofNotExact'{$integration.integration_exact=$false}
    'NegativeShadowVerdictInvalid'{
        $integration.verdict='FORMAL_V2_SHADOW_INTEGRATION_INVALID'
        $w2dVerdict='FORMAL_V2_SHADOW_INTEGRATION_INVALID'
    }
    'NegativeHistoricalVerdictMutation'{$w2dVerdict='FORMAL_V2_SHADOW_INTEGRATION_INVALID'}
    'NegativeLegacyAuthorityStillEnabled'{$retirement.retirement_exact=$false}
    'NegativeLegacyMetricFeedsCanonicalVerdict'{$retirement.legacy_metric_canonical_decision_count=1}
    'NegativeLegacyMetricUnclassified'{$retirement.legacy_metric_unclassified_count=1}
    'NegativeLegacyDiagnosticsDeleted'{$retirement.legacy_diagnostics_retained=$false}
    'NegativeCanonicalSourceFrameIdentity'{$integration.source_frame_identity_used=$true}
    'NegativeCanonicalNearestQpcFallback'{$integration.nearest_qpc_or_tolerance_used=$true}
    'NegativeCanonicalRecordSourceFrameField'{
        $integration.runs[0].records=@([pscustomobject][ordered]@{
            exact_event_key='11|110';intent_ordinal='0';intent_scope='CURRENT_MEASUREMENT'
            source_frame_index=7;intent_satisfied=$true
        })
    }
}

$canonical=Invoke-MvmECanonicalAuthority -FormalV2Integration $integration `
    -W2DProofPath 'C:\fixture\w2-d.json' -W2DProofSha256 ('f'*64) -W2DVerdict $w2dVerdict `
    -RetirementInventory $retirement
$blockers=@($canonical.blockers)

$expectedBlockers=@(switch($Case){
    'NegativeShadowProofNotExact'{@('FORMAL_V2_SHADOW_NOT_EXACT')}
    'NegativeShadowVerdictInvalid'{@('FORMAL_V2_SHADOW_NOT_EXACT')}
    'NegativeHistoricalVerdictMutation'{@('HISTORICAL_VERDICT_REWRITTEN')}
    'NegativeLegacyAuthorityStillEnabled'{@('LEGACY_AUTHORITY_STILL_CANONICAL')}
    'NegativeLegacyMetricFeedsCanonicalVerdict'{@('LEGACY_METRIC_FEEDS_CANONICAL_VERDICT')}
    'NegativeLegacyMetricUnclassified'{@('LEGACY_METRIC_FAILURE_SITE_UNCLASSIFIED')}
    'NegativeLegacyDiagnosticsDeleted'{@('LEGACY_DIAGNOSTIC_SOURCE_MISSING')}
    'NegativeCanonicalSourceFrameIdentity'{@('CANONICAL_SOURCE_FRAME_IDENTITY')}
    'NegativeCanonicalNearestQpcFallback'{@('CANONICAL_NEAREST_QPC_FALLBACK')}
    'NegativeCanonicalRecordSourceFrameField'{@('CANONICAL_SOURCE_FRAME_IDENTITY')}
    default{@()}
})
foreach($expected in $expectedBlockers){
    if($expected-notin$blockers){throw "$Case で期待したblockerが出ません: $expected (actual=$($blockers-join', '))"}
}
if($expectedBlockers.Count-ne0){
    if([bool]$canonical.cutover_exact){throw "$Case をfail-closeしていません"}
    if([string]$canonical.verdict-ne'CANONICAL_CUTOVER_INVALID'){throw "$Case のverdictが不正です"}
    Write-Output "W2-E canonical authority contract $Case`: PASS";exit 0
}
if(-not[bool]$canonical.cutover_exact){throw "$Case が不成立です: $($blockers-join', ')"}
if([string]$canonical.verdict-ne'CANONICAL_PRESENTATION_AUTHORITY_FORMAL_V2'){throw "$Case のverdictが不正です"}

# cutover したこと。
foreach($trueFlag in @('canonical_authority','legacy_presentation_authority_retired','legacy_diagnostics_retained')){
    if(-not[bool]$canonical.$trueFlag){throw "$Case で$trueFlag がtrueではありません"}
}
# していないこと。W2-E は performance verdict の段ではない。
foreach($falseFlag in @('frame_swapped_authority','dwm_frame_statistics_authority',
    'retirement_means_deletion','performance_threshold_evaluated','canonical_verdict_evaluated',
    'historical_verdicts_rewritten','source_frame_identity_used','nearest_qpc_or_tolerance_used')){
    if([bool]$canonical.$falseFlag){throw "$Case で$falseFlag がtrueです"}
}
if([string]$canonical.presentation_authority_schema-ne'FORMAL_V2'){throw 'authority schemaがFORMAL_V2ではありません'}
if([string]$canonical.canonical_performance_verdict_deferred_to-ne'W3'){throw 'performance verdictの保留先がW3ではありません'}
if([string]$canonical.canonical_source-notmatch'intent -> composition_token -> native_present -> exact_present_event -> final_state -> displayed_qpc -> physical_vblank_ordinal'){
    throw 'canonical sourceがformal-v2 exact chainではありません'
}
foreach($identity in @(
    @('canonical_required_intent_count',3),@('canonical_satisfied_intent_count',1),
    @('canonical_unsatisfied_intent_count',2),@('canonical_physical_vblank_opportunity_count',3),
    @('canonical_filled_physical_opportunity_count',1),@('legacy_metric_canonical_decision_count',0))){
    if([int64]$canonical.$($identity[0])-ne[int64]$identity[1]){
        throw "$Case のcanonical accountingが期待値と一致しません: $($identity[0])"
    }
}
# Layer 1A (3) と Layer 1B (3) の一致は要求しない契約であることの確認。
if(-not[bool]$canonical.layer1a_layer1b_count_difference_is_not_a_verdict){
    throw 'Layer 1A / Layer 1Bのcount差がverdict化されています'
}
if($Case-eq'GoodLegacyDiagnosticsRemainPresent'){
    # retirement = deletion ではない。diagnostic が残っていて non-authoritative なら PASS。
    if([int64]$canonical.legacy_metric_diagnostic_integrity_count-le0){
        throw 'legacy diagnosticの整合検査が残っていません'
    }
    if([bool]$canonical.frame_swapped_authority-or[bool]$canonical.dwm_frame_statistics_authority){
        throw 'legacy diagnosticがauthoritativeになっています'
    }
}
Write-Output "W2-E canonical authority contract $Case`: PASS"
