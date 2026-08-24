[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$SourceRoot)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$source=Get-Content -LiteralPath (Join-Path $SourceRoot 'apps/compositor_spike/compositor_spike_controller.cpp') -Raw -Encoding utf8
$main=Get-Content -LiteralPath (Join-Path $SourceRoot 'apps/compositor_spike/main.cpp') -Raw -Encoding utf8
function Require([string]$Pattern,[string]$Message){if($source-notmatch$Pattern){throw $Message}}
Require 'void CompositorSpikeController::attach[\s\S]{0,2500}startVBlankObserverWithPreroll\(\)[\s\S]{0,500}timer_\.start\(\)' 'startup Presentedより前のobserver開始が固定されていません'
if($main-notmatch'controller\.attach\(surface\);[\s\S]{0,500}window->setVisible\(true\);'){
    throw 'controller attachがwindow可視化より先ではありません'
}
Require 'if \(state_->nativePresentCaptureEnvelopeEnabled[\s\S]{0,500}startVBlankObserverWithPreroll\(\)[\s\S]{0,500}nativePresentEnvelopeStartRequested' 'physical observerがcandidate captureより先に開始されていません'
Require 'teardownComplete\.load\(\)[\s\S]{0,300}closeVBlankMappingSupportAfterTeardown\(\)[\s\S]{0,300}writeMetrics\(\)' 'teardown後のphysical postrollがserialize前に閉じていません'
Require 'waitForSuccessor\(vblankMappingSupportPostrollBoundaryQpc_' 'postrollの実sample witnessがありません'
if($source-match'vblankMappingSupportPostrollBoundaryQpc_[\s\S]{0,200}(tolerance|nearest|phase)'){
    throw 'mapping support closureへQPC heuristicが混入しています'
}
Write-Output 'W2-C1.1 support envelope architecture: PASS'
