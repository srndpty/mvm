[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$SourceRoot)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$renderer=Get-Content -LiteralPath (Join-Path $SourceRoot 'src/app/preview/compositor_rhi_item.cpp') -Raw -Encoding utf8
$scheduler=Get-Content -LiteralPath (Join-Path $SourceRoot 'src/media/gpu_preview/presentation_opportunity_scheduler.h') -Raw -Encoding utf8
$controller=Get-Content -LiteralPath (Join-Path $SourceRoot 'apps/compositor_spike/compositor_spike_controller.cpp') -Raw -Encoding utf8
$checker=Get-Content -LiteralPath (Join-Path $SourceRoot 'scripts/p2-d5-2-w2-c24-formal-transport-core.ps1') -Raw -Encoding utf8
function Require([string]$Text,[string]$Pattern,[string]$Message){if($Text-notmatch$Pattern){throw $Message}}
Require $scheduler 'requiredIntentMembershipExact[\s\S]+InvalidMembershipProvenance[\s\S]+duplicateCallback[\s\S]+SuppressDuplicateCallback[\s\S]+requiredIntentMembership[\s\S]+SuppressOutsideRequiredSet' 'membership provenance fail-closeがsuppressionより先ではありません'
Require $renderer 'formalIntentTransportDisposition[\s\S]+SuppressDuplicateCallback[\s\S]+return;[\s\S]+SuppressOutsideRequiredSet[\s\S]+return;[\s\S]+setFormalIntentOrdinal' 'formal token発行より前にtransport抑止していません'
Require $controller 'duplicate_transport_suppressed_count[\s\S]+outside_required_transport_suppressed_count' 'formal transport抑止件数を記録していません'
Require $checker 'expectedDisposition[\s\S]+TRANSPORT_DISPOSITION_SEMANTIC_MISMATCH[\s\S]+FORMAL_TRANSPORT_ELIGIBILITY_MISMATCH' 'checkerがdispositionとeligibilityを独立再計算していません'
Write-Output 'W2-C2.4 formal transport architecture: PASS'
