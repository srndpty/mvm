[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$SourceRoot)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$runner=Get-Content -LiteralPath (Join-Path $SourceRoot 'scripts/inventory-p2-d5-2-w2-c21-required-intent-domain.ps1') -Raw -Encoding utf8
$core=Get-Content -LiteralPath (Join-Path $SourceRoot 'scripts/p2-d5-2-w2-c21-required-intent-domain-core.ps1') -Raw -Encoding utf8
$proofCore=Get-Content -LiteralPath (Join-Path $SourceRoot 'scripts/p2-d5-2-w2-c21-from-c1-core.ps1') -Raw -Encoding utf8
function Require([string]$Text,[string]$Pattern,[string]$Message){if($Text-notmatch$Pattern){throw $Message}}
Require -Text $runner -Pattern 'check-p2-d5-2-w2-c14-sealed-mapping-replay\.ps1' -Message 'C1 sealed authorityをconsumeしていません'
Require -Text $core -Pattern 'required_intent_set_exact[\s\S]+required_intent_ordinals' -Message 'scheduler exact set provenanceを要求していません'
Require -Text $core -Pattern 'REQUIRED_INTENT_MEMBERSHIP_PROVENANCE_MISSING' -Message 'membership欠損をfail-closeしていません'
Require -Text $core -Pattern 'SCHEDULER_DECISION_QPC_PROVENANCE_MISSING' -Message 'decision QPC欠損をfail-closeしていません'
Require -Text $core -Pattern 'MEASUREMENT_BOUNDARY_RELATION_UNRESOLVED' -Message 'boundary relation欠損をfail-closeしていません'
Require -Text $core -Pattern 'presented_population_used_to_derive_required_set=\$false' -Message 'Presentedからrequired setを復元しています'
Require -Text $core -Pattern 'nearest_qpc_or_tolerance_used=\$false' -Message 'QPC heuristic不使用が固定されていません'
Require -Text $core -Pattern 'source_frame_identity_used=\$false[\s\S]+producer_changed=\$false' -Message 'C2.1禁止事項が固定されていません'
Require -Text $proofCore -Pattern 'branch_a_established=\$false[\s\S]+branch_b_established=\$false' -Message 'authority未確定なのにA/Bを選択しています'
Write-Output 'W2-C2.1 required intent domain architecture: PASS'
