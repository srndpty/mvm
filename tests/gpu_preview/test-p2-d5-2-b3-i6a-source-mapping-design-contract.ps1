[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'Good',
        'NegativeProductionChanged',
        'NegativeCanonicalW3Allowed',
        'NegativeDisplayAxisSelected',
        'NegativeRefreshDependentTargetAllowed',
        'NegativeSourceFixtureExtensionAllowed',
        'NegativeRequiredSetShrinkAllowed',
        'NegativeThresholdChanged',
        'NegativeDenominatorChanged',
        'NegativeCoveragePreflightInvariantDropped',
        'NegativeClampTargetAllowed',
        'NegativeCoverageFatalAsPerformanceDrop',
        'NegativeIntentOrdinalFromPhysicalVblank',
        'NegativeSingleIntentRateProducerDropped',
        'NegativeHistoricalMismatchReclassified')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Contract,
    [Parameter(Mandatory=$true)][string]$RepoRoot
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

function Require([bool]$Condition,[string]$Message){if(-not$Condition){throw $Message}}

function Validate-Design([object]$Design){
    Require ($Design.schema-eq'mvm-p2-d5-2-b3-i6a-source-mapping-authority-design-1') 'schemaが不正です'
    Require ($Design.status-eq'DESIGN_ONLY') 'I6Aがdesign-onlyではありません'
    foreach($flag in @('production_behavior_changed','queue_semantics_changed','join_accept_reject_changed',
                       'threshold_changed','denominator_changed','required_population_changed',
                       'source_fixture_changed','canonical_w3_allowed')){
        Require (-not$Design.$flag) "I6Aで変更してはならない対象がtrueです: $flag"
    }

    # 観測事実は測定値であってcanonical metricではない。
    Require (-not$Design.observed_failure.metrics_are_canonical) 'INVALID runの数値をcanonicalとして扱っています'
    Require ($Design.observed_failure.primary_error-eq'SOURCE_COVERAGE_INSUFFICIENT') 'primary failureがsource coverageではありません'

    # authority inventory。1A ordinalと1B rateの混在を明示していること。
    $authorities=@($Design.authorities|ForEach-Object{$_.authority})
    foreach($name in @('required_intent_count','required_intent_ordinal','physical_vblank_ordinal',
                       'target_source_frame','source_coverage_preflight')){
        Require ($authorities-contains$name) "authority inventoryが不足しています: $name"
    }
    $ordinalAuthority=$Design.authorities|Where-Object{$_.authority-eq'required_intent_ordinal'}
    Require ($ordinalAuthority.layer-eq'1A_WORKLOAD_INTENT') 'intent ordinalをLayer 1Aとして固定していません'
    $physical=$Design.authorities|Where-Object{$_.authority-eq'physical_vblank_ordinal'}
    Require ($physical.layer-eq'1B_PHYSICAL_OPPORTUNITY') 'physical ordinalをLayer 1Bとして固定していません'
    $target=$Design.authorities|Where-Object{$_.authority-eq'target_source_frame'}
    Require (-not[string]::IsNullOrWhiteSpace($target.conflict)) 'target mappingの1A/1B混在を記録していません'

    # 候補は2つとも記録し、片方だけを選定する。
    $candidates=@($Design.candidates|ForEach-Object{$_.name})
    foreach($name in @('WORKLOAD_INTENT_TIME_AXIS','DISPLAY_OPPORTUNITY_TIME_AXIS')){
        Require ($candidates-contains$name) "候補semanticsが不足しています: $name"
    }
    $selected=$Design.candidates|Where-Object{$_.name-eq$Design.selected_semantics}
    Require ($null-ne$selected) '選定semanticsが候補に存在しません'
    Require (-not$selected.target_depends_on_display_refresh) '選定semanticsがdisplay refresh依存のmappingです'
    Require (-not$selected.requires_source_fixture_change) '選定semanticsがsource fixture変更を要求しています'
    Require (-not$selected.required_set_size_changed-and-not$selected.denominator_changed) '選定semanticsがrequired set/分母を変更しています'
    $rejected=$Design.candidates|Where-Object{$_.name-ne$Design.selected_semantics}
    Require (@($rejected.rejection_reasons).Count-ge1) '非選定候補のrejection根拠がありません'

    foreach($basis in @('W1_LAYER_1A_1B_SEPARATION_FROZEN',
                        'B3_I1_ORDINAL_PRODUCER_IS_REQUIRED_INTENT_QUEUE_HEAD',
                        'CANONICAL_INPUT_MUST_NOT_DEPEND_ON_DISPLAY_REFRESH')){
        Require (@($Design.selection_basis)-contains$basis) "選定根拠が不足しています: $basis"
    }

    foreach($invariant in @('REQUIRED_INTENT_ORDINAL_IS_LAYER_1A',
                            'TARGET_MAPPING_INDEPENDENT_OF_DISPLAY_REFRESH',
                            'INTENT_RATE_HAS_SINGLE_PRODUCER',
                            'SOURCE_COVERAGE_PREFLIGHT_USES_MAX_TARGET_OVER_REQUIRED_SET',
                            'MAX_TARGET_OVER_REQUIRED_SET_LESS_THAN_SOURCE_FRAME_COUNT',
                            'REQUIRED_SET_SIZE_UNCHANGED','DROP_RATE_DENOMINATOR_UNCHANGED',
                            'SOURCE_COVERAGE_FAILURE_IS_PROTOCOL_FATAL_NOT_PERFORMANCE_DROP')){
        Require (@($Design.frozen_invariants)-contains$invariant) "frozen invariantが不足しています: $invariant"
    }
    foreach($prohibited in @('EXTEND_SOURCE_FIXTURE_TO_HIDE_MAPPING_DEFECT','SHRINK_REQUIRED_SET',
                             'CHANGE_FROZEN_THRESHOLD','CHANGE_DROP_RATE_DENOMINATOR','SKIP_TAIL_INTENTS',
                             'CLAMP_TARGET_TO_LAST_SOURCE_FRAME',
                             'TREAT_SOURCE_COVERAGE_FATAL_AS_PERFORMANCE_DROP',
                             'DERIVE_INTENT_ORDINAL_FROM_PHYSICAL_VBLANK_ORDINAL')){
        Require (@($Design.prohibited_resolutions)-contains$prohibited) "禁止解法が不足しています: $prohibited"
    }

    Require $Design.preroll_mapping.unchanged_by_selection 'preroll mappingの不変を固定していません'
    Require ($Design.canonical_w3.status-eq'HOLD') 'canonical W3をHOLDにしていません'
    Require ($Design.canonical_w3.historical_verdict_unchanged-eq'FAIL') 'historical W3 verdictを変更しています'
    Require ($Design.historical_composition_token_mismatch.status-eq'UNRESOLVED_HISTORICAL_RUNTIME_FAILURE') 'historical mismatchを未解決として保持していません'
    Require (-not$Design.historical_composition_token_mismatch.reclassified_as_i6_mapping_failure) 'historical mismatchをI6 mapping failureへ再分類しています'
}

$design=Get-Content -Raw -LiteralPath $Contract -Encoding utf8|ConvertFrom-Json
switch($Case){
    'Good'{}
    'NegativeProductionChanged'{$design.production_behavior_changed=$true}
    'NegativeCanonicalW3Allowed'{$design.canonical_w3_allowed=$true}
    'NegativeDisplayAxisSelected'{$design.selected_semantics='DISPLAY_OPPORTUNITY_TIME_AXIS'}
    'NegativeRefreshDependentTargetAllowed'{
        ($design.candidates|Where-Object{$_.name-eq'WORKLOAD_INTENT_TIME_AXIS'}).target_depends_on_display_refresh=$true}
    'NegativeSourceFixtureExtensionAllowed'{
        $design.prohibited_resolutions=@($design.prohibited_resolutions|Where-Object{$_-ne'EXTEND_SOURCE_FIXTURE_TO_HIDE_MAPPING_DEFECT'})}
    'NegativeRequiredSetShrinkAllowed'{
        $design.prohibited_resolutions=@($design.prohibited_resolutions|Where-Object{$_-ne'SHRINK_REQUIRED_SET'})}
    'NegativeThresholdChanged'{$design.threshold_changed=$true}
    'NegativeDenominatorChanged'{$design.denominator_changed=$true}
    'NegativeCoveragePreflightInvariantDropped'{
        $design.frozen_invariants=@($design.frozen_invariants|Where-Object{$_-ne'SOURCE_COVERAGE_PREFLIGHT_USES_MAX_TARGET_OVER_REQUIRED_SET'})}
    'NegativeClampTargetAllowed'{
        $design.prohibited_resolutions=@($design.prohibited_resolutions|Where-Object{$_-ne'CLAMP_TARGET_TO_LAST_SOURCE_FRAME'})}
    'NegativeCoverageFatalAsPerformanceDrop'{
        $design.prohibited_resolutions=@($design.prohibited_resolutions|Where-Object{$_-ne'TREAT_SOURCE_COVERAGE_FATAL_AS_PERFORMANCE_DROP'})}
    'NegativeIntentOrdinalFromPhysicalVblank'{
        ($design.authorities|Where-Object{$_.authority-eq'required_intent_ordinal'}).layer='1B_PHYSICAL_OPPORTUNITY'}
    'NegativeSingleIntentRateProducerDropped'{
        $design.frozen_invariants=@($design.frozen_invariants|Where-Object{$_-ne'INTENT_RATE_HAS_SINGLE_PRODUCER'})}
    'NegativeHistoricalMismatchReclassified'{
        $design.historical_composition_token_mismatch.reclassified_as_i6_mapping_failure=$true}
}

$failed=$false
try{Validate-Design $design}catch{$failed=$true}
if($Case-eq'Good'){
    if($failed){Validate-Design $design}
    # design記述が実sourceのinventoryと一致していることを固定する。
    $scheduler=Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src/media/gpu_preview/presentation_opportunity_scheduler.cpp')
    $controller=Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'apps/compositor_spike/compositor_spike_controller.cpp')
    $queue=Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src/media/gpu_preview/required_intent_queue.cpp')
    Require ($scheduler-match'config_\.sourceFpsNumerator[\s\S]{0,200}config_\.refreshDenominator') '現行targetForがrefresh比を使っている事実を確認できません'
    Require ($scheduler-match'target >= config_\.requiredFrameCount') 'source domain判定siteを確認できません'
    Require ($controller-match'requiredMeasurementFrameCount_ = static_cast<long long>\(config_\.measureSeconds\) \* 60') 'required countのworkload rate producerを確認できません'
    Require ($controller-match'sourceCoverageOk_ = sourceAFrameCount_ >= requiredMeasurementFrameCount_') '現行preflightがrequired countと比較している事実を確認できません'
    Require ($queue-match'requiredIntentOrdinals_\.push_back\(ordinal\)') 'required setのqueue producerを確認できません'
    Write-Output 'P2-D5-2 B3-I6A source mapping authority design contract Good: PASS'
    exit 0
}
if(-not$failed){throw "$Case をB3-I6A design contractが検出できませんでした"}
Write-Output "P2-D5-2 B3-I6A source mapping authority design contract $Case`: PASS"
