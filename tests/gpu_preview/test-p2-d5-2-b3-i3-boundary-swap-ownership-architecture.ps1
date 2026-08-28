param([Parameter(Mandatory=$true)][string]$RepoRoot)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

function Read-Source([string]$RelativePath){Get-Content -Raw -LiteralPath (Join-Path $RepoRoot $RelativePath)}
function Require([string]$Text,[string]$Pattern,[string]$Message){if($Text-notmatch$Pattern){throw $Message}}
function Reject([string]$Text,[string]$Pattern,[string]$Message){if($Text-match$Pattern){throw $Message}}

$header=Read-Source 'src/app/preview/compositor_rhi_item.h'
$renderer=Read-Source 'src/app/preview/compositor_rhi_item.cpp'
$controller=Read-Source 'apps/compositor_spike/compositor_spike_controller.cpp'

Require $header 'formalOpportunityIgnoreNextSwap\{false\}' 'ignore-next-swap初期値がfalseではありません'
if(([regex]::Matches($renderer,'formalOpportunityIgnoreNextSwap\.store\(true')).Count-ne1){throw 'ignore-next-swap producerがexactly 1箹所ではありません'}
if(([regex]::Matches($renderer,'formalOpportunityIgnoreNextSwap\.exchange\(false')).Count-ne1){throw 'ignore-next-swap consume/resetがexactly 1箹所ではありません'}
Require $renderer 'ignoreNextSwapPublicationSerial\.fetch_add[\s\S]+IgnoreNextSwapPublished[\s\S]+RENDER_MEASUREMENT_START' 'flag publicationのserial/QPC/thread/phase記録がありません'
Require $renderer 'scopeRecord\.tokenSerial != receipt\.tokenSerial[\s\S]+callbackScopeMatchCount == 1' 'frameSwapped scopeをexact token serialで結合していません'
Require $renderer 'BoundarySwapEventKind::FrameSwapped[\s\S]+ignorePreValue[\s\S]+ignoreConsumed[\s\S]+receipt\.presentSerial[\s\S]+receipt\.tokenSerial[\s\S]+activeReservation' 'frameSwapped raw attributionが不足しています'
Require $controller 'measurementStartRequested\.store\(true[\s\S]+MeasurementStartRequestPublished' 'measurement start publication eventがありません'
foreach($eventName in @('MeasurementStartConsumed','RequiredQueueStarted','FirstReservation','IgnoreNextSwapPublished')){
    Require $renderer ([regex]::Escape("BoundarySwapEventKind::$eventName")) "ordering eventが不足しています: $eventName"
}
foreach($fieldName in @('identity_join_uses_present_and_token_serial','nearest_qpc_used','event_serial_is_identity_authority','foreign_callback_relation','positional_contract_expresses_boundary_identity','boundary_swap_ownership_attribution')){
    Require $controller ([regex]::Escape($fieldName)) "B3-I3 artifact fieldが不足しています: $fieldName"
}
Require $controller 'nearest_qpc_used", false[\s\S]+callback_index_used_as_identity", false[\s\S]+event_serial_is_identity_authority", false' 'QPC/callback/event serialをidentity authorityから排除していません'
Require $controller 'product_semantics_changed", false[\s\S]+queue_semantics_changed", false[\s\S]+join_accept_reject_changed", false' 'diagnostics-only contractが固定されていません'
Require $renderer 'ignoredFormalBoundarySwap[\s\S]+hasActiveReservation\(\)[\s\S]+BoundarySwapRequiresNoActiveReservation[\s\S]+failQualifiedJoin' '既存boundary FATAL semanticsが維持されていません'
Reject $renderer '(?i)(nearestBoundary|closestBoundary|callbackIndex.*intent)' 'rendererがnearest/callback indexからboundary identityを推定しています'
Reject $controller '(?i)(nearestBoundary|closestBoundary)' 'controllerがnearest QPCからboundary identityを推定しています'

Write-Output 'P2-D5-2 B3-I3 boundary-swap ownership architecture: PASS'
