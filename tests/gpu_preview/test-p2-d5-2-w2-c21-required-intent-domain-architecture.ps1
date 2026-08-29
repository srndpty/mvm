[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$SourceRoot)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$runner=Get-Content -LiteralPath (Join-Path $SourceRoot 'scripts/inventory-p2-d5-2-w2-c21-required-intent-domain.ps1') -Raw -Encoding utf8
$core=Get-Content -LiteralPath (Join-Path $SourceRoot 'scripts/p2-d5-2-w2-c21-required-intent-domain-core.ps1') -Raw -Encoding utf8
$proofCore=Get-Content -LiteralPath (Join-Path $SourceRoot 'scripts/p2-d5-2-w2-c21-from-c1-core.ps1') -Raw -Encoding utf8
$requiredIntentQueue=Get-Content -LiteralPath (Join-Path $SourceRoot 'src/media/gpu_preview/required_intent_queue.cpp') -Raw -Encoding utf8
$renderer=Get-Content -LiteralPath (Join-Path $SourceRoot 'src/app/preview/compositor_rhi_item.cpp') -Raw -Encoding utf8
$controller=Get-Content -LiteralPath (Join-Path $SourceRoot 'apps/compositor_spike/compositor_spike_controller.cpp') -Raw -Encoding utf8
function Require([string]$Text,[string]$Pattern,[string]$Message){if($Text-notmatch$Pattern){throw $Message}}
Require -Text $runner -Pattern 'check-p2-d5-2-w2-c14-sealed-mapping-replay\.ps1' -Message 'C1 sealed authorityをconsumeしていません'
Require -Text $core -Pattern 'required_intent_set_exact[\s\S]+required_intent_ordinals' -Message 'scheduler exact set provenanceを要求していません'
Require -Text $core -Pattern 'REQUIRED_INTENT_MEMBERSHIP_PROVENANCE_MISSING' -Message 'membership欠損をfail-closeしていません'
Require -Text $core -Pattern 'checker_derived_required_current_membership[\s\S]+DECISION_REQUIRED_MEMBERSHIP_MISMATCH' -Message '各decisionのmembershipをrequired setから再検証していません'
Require -Text $core -Pattern 'required_set_decision_population_equality_required=\$false' -Message 'required planとobserved decision populationを集合一致させています'
Require -Text $core -Pattern 'SCHEDULER_DECISION_QPC_PROVENANCE_MISSING' -Message 'decision QPC欠損をfail-closeしていません'
Require -Text $core -Pattern 'MEASUREMENT_START_QPC_MISSING[\s\S]+MEASUREMENT_END_QPC_MISSING' -Message 'measurement boundary QPC欠損をfail-closeしていません'
Require -Text $core -Pattern 'MEASUREMENT_BOUNDARY_RELATION_UNRESOLVED' -Message 'boundary relation欠損をfail-closeしていません'
Require -Text $core -Pattern 'checker_derived_measurement_boundary_relation[\s\S]+MEASUREMENT_BOUNDARY_RELATION_MISMATCH' -Message 'boundary relationをQPCから独立再計算していません'
Require -Text $core -Pattern 'presented_population_used_to_derive_required_set=\$false' -Message 'Presentedからrequired setを復元しています'
Require -Text $core -Pattern 'nearest_qpc_or_tolerance_used=\$false' -Message 'QPC heuristic不使用が固定されていません'
Require -Text $core -Pattern 'source_frame_identity_used=\$false[\s\S]+producer_changed=\$false' -Message 'C2.1禁止事項が固定されていません'
Require -Text $proofCore -Pattern 'branch_a_established=\$branchA[\s\S]+branch_b_established=\$branchB' -Message 'producer required setからA/Bを確定していません'
Require -Text $requiredIntentQueue -Pattern 'requiredIntentOrdinals_\.push_back\(ordinal\)' -Message 'required intent setをscheduler start時点で生成していません'
Require -Text $renderer -Pattern 'callbackBegin[\s\S]+requiredIntentMembership[\s\S]+boundaryRelation' -Message 'decision authority fieldsをproducer地点で固定していません'
Require -Text $controller -Pattern 'required_intent_set_derived_from_presented", false[\s\S]+required_intent_set_exact[\s\S]+required_intent_ordinals' -Message 'producer-side required setを独立emitしていません'
Require -Text $controller -Pattern 'mvm-p2-d5-2-w2-c23-intent-authority-provenance-3' -Message 'C2.3 producer semantics追加後のschemaを識別できません'
Require -Text $controller -Pattern 'producer_semantics_exact[\s\S]+duplicate_callback[\s\S]+repeat[\s\S]+past_source_domain[\s\S]+target_frame[\s\S]+last_finalized_opportunity_ordinal[\s\S]+render_begin_qpc' -Message 'C2.3 producer semantics fieldsをemitしていません'
Write-Output 'W2-C2.1 required intent domain architecture: PASS'
