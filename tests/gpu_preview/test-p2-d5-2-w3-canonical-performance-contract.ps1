[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'GoodCanonicalPass','GoodCanonicalFail','GoodLayer1ALayer1BDifference',
        'NegativeAcquisitionCheckpointMismatch','NegativeAcquisitionCheckpointMissing',
        'NegativeAcquisitionNotFresh','NegativeAcquisitionWorktreeDirty',
        'NegativeAcquisitionBinaryProvenanceMissing','NegativeAcquisitionCoverageIncomplete',
        'NegativeAcquisitionIntentScopeNotExact',
        'NegativeCanonicalAuthorityNotFormalV2','NegativeCanonicalCutoverNotExact',
        'NegativeLegacyAuthorityStillCanonical','NegativeLegacyMetricFeedsCanonicalVerdict',
        'NegativeCanonicalChainIdentityInvalid','NegativeShadowNotExact',
        'NegativeAccountingIdentityViolation','NegativeAccountingSumViolation',
        'NegativeMeasurementWindowInvalid','NegativeMeasurementWindowCountMismatch',
        'NegativeQpcFrequencyInconsistent')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Core
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. $Core

$checkpoint='d659d5a8f9a47b22ccaa4d79e47eef4cb9d8efa2'
# 期待値は実装の式を呼ばず fixture 側で決める。
# 3 run x 5s = 15s、satisfied 900 なら 60fps ちょうど。
$provenance=[ordered]@{
    fresh_acquisition=$true;worktree_clean=$true;checkpoint_sha=$checkpoint
    compositor_spike_sha256='a'*64;decoder_sha256='b'*64
    qt_gui_sha256='c'*64;qt_quick_sha256='d'*64
    coverage_complete=$true;intent_scope_exact=$true;run_count=3
}
$canonical=[ordered]@{
    presentation_authority_schema='FORMAL_V2';canonical_authority=$true;cutover_exact=$true
    frame_swapped_authority=$false;dwm_frame_statistics_authority=$false
    legacy_metric_canonical_decision_count=0
    source_frame_identity_used=$false;nearest_qpc_or_tolerance_used=$false
    source_w2d_verdict='FORMAL_V2_SHADOW_INTEGRATION_EXACT'
    layer1a_required_accounting_identity_exact=$true
    presented_accounting_identity_exact=$true
    filled_physical_opportunity_identity_exact=$true
    physical_vblank_domain_cardinality_exact=$true
    c2_ledger_agreement_exact=$true
    canonical_required_intent_count=900
    canonical_satisfied_intent_count=900
    canonical_unsatisfied_intent_count=0
    canonical_physical_vblank_opportunity_count=897
}
$windows=@()
for($run=1;$run-le3;++$run){
    $windows+=,[ordered]@{run=$run;measurement_start_qpc=1000000;
        measurement_end_qpc_exclusive=51000000;qpc_frequency=10000000}
}

switch($Case){
    'GoodCanonicalFail'{
        # stage 1-3 は成立。threshold だけ割る。INVALID ではなく FAIL であること。
        $canonical.canonical_satisfied_intent_count=438
        $canonical.canonical_unsatisfied_intent_count=462
    }
    'GoodLayer1ALayer1BDifference'{
        # required 900 != physical 897。これを verdict へ接続しない。
        $canonical.canonical_physical_vblank_opportunity_count=897
    }
    'NegativeAcquisitionCheckpointMismatch'{$provenance.checkpoint_sha='0'*40}
    'NegativeAcquisitionCheckpointMissing'{$provenance.checkpoint_sha=''}
    'NegativeAcquisitionNotFresh'{$provenance.fresh_acquisition=$false}
    'NegativeAcquisitionWorktreeDirty'{$provenance.worktree_clean=$false}
    'NegativeAcquisitionBinaryProvenanceMissing'{$provenance.decoder_sha256=''}
    'NegativeAcquisitionCoverageIncomplete'{$provenance.coverage_complete=$false}
    'NegativeAcquisitionIntentScopeNotExact'{$provenance.intent_scope_exact=$false}
    'NegativeCanonicalAuthorityNotFormalV2'{$canonical.presentation_authority_schema='LEGACY'}
    'NegativeCanonicalCutoverNotExact'{$canonical.cutover_exact=$false}
    'NegativeLegacyAuthorityStillCanonical'{$canonical.frame_swapped_authority=$true}
    'NegativeLegacyMetricFeedsCanonicalVerdict'{$canonical.legacy_metric_canonical_decision_count=1}
    'NegativeCanonicalChainIdentityInvalid'{$canonical.nearest_qpc_or_tolerance_used=$true}
    'NegativeShadowNotExact'{$canonical.source_w2d_verdict='FORMAL_V2_SHADOW_INTEGRATION_INVALID'}
    'NegativeAccountingIdentityViolation'{$canonical.presented_accounting_identity_exact=$false}
    'NegativeAccountingSumViolation'{$canonical.canonical_unsatisfied_intent_count=1}
    'NegativeMeasurementWindowInvalid'{$windows[0].measurement_end_qpc_exclusive=0}
    'NegativeMeasurementWindowCountMismatch'{$windows=@($windows[0],$windows[1])}
    'NegativeQpcFrequencyInconsistent'{$windows[1].qpc_frequency=1000000}
}

$result=Invoke-MvmW3CanonicalPerformance -AcquisitionProvenance $provenance `
    -CanonicalAuthority $canonical -MeasurementWindows $windows -ExpectedCheckpointSha $checkpoint

if($Case -like 'Negative*'){
    # 1〜3 が INVALID なら 4〜6 を評価しない。performance FAIL へ変換しない。
    if([string]$result.verdict-ne'AUTHORITY_OR_PROTOCOL_INVALID'){
        throw "$Case をauthority/protocol INVALIDにしていません: $($result.verdict)"
    }
    foreach($laterStage in @('stage4_metric_constructed','stage5_threshold_evaluated',
                             'stage6_canonical_verdict_evaluated','performance_evaluated')){
        if([bool]$result.$laterStage){throw "$Case で後段を評価しています: $laterStage"}
    }
    foreach($nullField in @('canonical_performance_pass','canonical_drop_rate','canonical_effective_fps')){
        if($null-ne$result.$nullField){throw "$Case でperformance値を出しています: $nullField"}
    }
    $blockers=@($result.stage1_blockers)+@($result.stage2_blockers)+@($result.stage3_blockers)
    if($blockers.Count-eq0){throw "$Case でblockerが1件も出ていません"}
    Write-Output "W3 canonical performance contract $Case`: PASS";exit 0
}

# Good 系は 1〜3 が成立し、4〜6 まで進むこと。
foreach($stage in @('stage1_acquisition_protocol_valid','stage2_canonical_authority_valid',
                    'stage3_accounting_valid','stage4_metric_constructed',
                    'stage5_threshold_evaluated','stage6_canonical_verdict_evaluated',
                    'performance_evaluated')){
    if(-not[bool]$result.$stage){throw "$Case で$stage が成立していません"}
}
if([bool]$result.legacy_presentation_authority_used){throw 'legacy authorityを使っています'}
if(-not[bool]$result.layer1a_layer1b_count_difference_is_not_a_verdict){
    throw 'Layer 1A / Layer 1B差をverdictへ接続しています'
}
if(-not[bool]$result.thresholds_frozen_unchanged){throw 'thresholdがfrozen値から変わっています'}
# 分母は W2-A physical window。3 run x 5.0s = 15.0s。
if([double]$result.canonical_measurement_seconds-ne15.0){
    throw "measurement秒数がW2-A windowと一致しません: $($result.canonical_measurement_seconds)"
}

$expected=switch($Case){
    'GoodCanonicalFail'{@{fps=29.2;drop=462/900;pass=$false;verdict='CANONICAL_PERFORMANCE_FAIL'}}
    default{@{fps=60.0;drop=0.0;pass=$true;verdict='CANONICAL_PERFORMANCE_PASS'}}
}
if([Math]::Abs([double]$result.canonical_effective_fps-[double]$expected.fps)-gt1e-9){
    throw "$Case のeffective_fpsが期待値と一致しません: $($result.canonical_effective_fps)"
}
if([Math]::Abs([double]$result.canonical_drop_rate-[double]$expected.drop)-gt1e-9){
    throw "$Case のdrop_rateが期待値と一致しません: $($result.canonical_drop_rate)"
}
if([bool]$result.canonical_performance_pass-ne[bool]$expected.pass){throw "$Case のpass判定が不正です"}
if([string]$result.verdict-ne[string]$expected.verdict){throw "$Case のverdictが不正です: $($result.verdict)"}
if($Case-eq'GoodLayer1ALayer1BDifference'){
    # required(900) != physical(897) でも PASS のままであること。
    if([int64]$canonical.canonical_required_intent_count-eq
       [int64]$canonical.canonical_physical_vblank_opportunity_count){
        throw 'Layer 1A / Layer 1B差のfixtureになっていません'
    }
    if([string]$result.verdict-ne'CANONICAL_PERFORMANCE_PASS'){
        throw 'Layer 1A / Layer 1Bのcount差をFAILへ接続しています'
    }
}
Write-Output "W3 canonical performance contract $Case`: PASS"
