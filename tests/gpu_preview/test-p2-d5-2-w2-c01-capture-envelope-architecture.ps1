[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$SourceRoot)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$controller=Get-Content -LiteralPath (Join-Path $SourceRoot 'apps/compositor_spike/compositor_spike_controller.cpp') -Raw -Encoding utf8
$renderer=Get-Content -LiteralPath (Join-Path $SourceRoot 'src/app/preview/compositor_rhi_item.cpp') -Raw -Encoding utf8
$observer=Get-Content -LiteralPath (Join-Path $SourceRoot 'src/media/gpu_preview/window_output_vblank_observer.cpp') -Raw -Encoding utf8
$inventory=Get-Content -LiteralPath (Join-Path $SourceRoot 'scripts/inventory-p2-d5-2-w2-c0-display-candidates.ps1') -Raw -Encoding utf8
function Require-Pattern([string]$Text,[string]$Pattern,[string]$Message){if($Text-notmatch$Pattern){throw $Message}}
Require-Pattern $controller 'nativePresentEnvelopeStartRequested\.store\(true[\s\S]+Phase::CaptureEnvelopeStartWait' 'native envelopeのrender-thread start requestがありません'
Require-Pattern $controller 'Phase::CaptureEnvelopeStartWait[\s\S]+armMeasurementAfterCaptureEnvelopeOpen' 'capture open ackより前にmeasurementをarmしています'
Require-Pattern $controller 'frozenMeasurementEndQpc_\s*=\s*state_->measurementEndQpc\.load[\s\S]+waitForSuccessor' 'measurement end freeze後のsuccessor waitがありません'
Require-Pattern $controller 'waitForSuccessor[\s\S]+nativePresentEnvelopeStopRequested\.store\(true' 'successor waitより前にnative envelopeを閉じています'
Require-Pattern $observer 'sample\.qpc\s*>=\s*frozenMeasurementEndQpc' 'successorのQPC条件がありません'
Require-Pattern $renderer 'nativePresentEnvelopeStopRequested\.exchange[\s\S]+endCapture[\s\S]+nativePresentEnvelopeStopped\.store\(true' 'render threadでのenvelope close/ackがありません'
Require-Pattern $renderer 'formalDecision\.pastSourceDomain[\s\S]+finishMeasurement\(callbackBegin, StopArbitration::DomainTerminal' 'scheduler-produced terminal intentと同じcallbackでmeasurementを閉じていません'
Require-Pattern $renderer 'qScopeGuard[\s\S]+measurementStopCaptured[\s\S]+update\(\)' 'envelope drain中の追加Present抑止がありません'
Require-Pattern $renderer 'formalOpportunityEnvelopePrerollActive\.store\([\s\S]+formalOpportunityEnvelopePrerollActive\.exchange\([\s\S]+formalOpportunityScheduler\.close\([\s\S]+startFormalOpportunityScheduler\(\)' 'lower intent producerがB1 schedulerへ混入しています'
Require-Pattern $renderer 'setFormalIntentOrdinal\(formalDecision\.opportunityOrdinal\)[\s\S]+nativePresentIntentScopeLedger\.push_back' 'intent ordinal producerとscope producerが同じ箇所にありません'
Require-Pattern $renderer 'nativePresentToken\.tokenSerial\(\)[\s\S]+NativePresentIntentScope::ForeignPreMeasurement[\s\S]+NativePresentIntentScope::CurrentMeasurement' 'token serial keyed scope provenanceがありません'
Require-Pattern $controller 'join_key", "composition_token\.token_serial"' 'scope ledgerのexact join keyがtoken serialではありません'
Require-Pattern $controller 'scope_derived_from_present_qpc", false[\s\S]+scope_derived_from_source_frame", false[\s\S]+scope_derived_from_layer2_membership", false' 'scope非推論契約が固定されていません'
Require-Pattern $inventory "intentScope-eq'FOREIGN_PRE_MEASUREMENT'" 'foreign intentをproducer scopeから分類していません'
if($inventory-match "elseif\(-not\`$inLayer2\).*observedForeignExact"){
    throw 'Layer2 membershipからforeign intentを復元しています'
}
$endStores=[regex]::Matches($renderer,'measurementEndQpc\.store\(').Count
if($endStores-ne1){throw "measurementEndQpc producerが一意ではありません: $endStores"}
if($controller-match'measurementEndQpc\.store\('){throw 'controllerがfrozen measurement endを変更しています'}
Write-Output 'W2-C0.1 capture envelope architecture: PASS'
