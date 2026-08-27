[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$SourceRoot)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$source=Get-Content -LiteralPath (Join-Path $SourceRoot 'apps/compositor_spike/compositor_spike_controller.cpp') -Raw -Encoding utf8
$main=Get-Content -LiteralPath (Join-Path $SourceRoot 'apps/compositor_spike/main.cpp') -Raw -Encoding utf8
function Require([string]$Pattern,[string]$Message){if($source-notmatch$Pattern){throw $Message}}
# source layout（文字数）ではなく関数本体の構造で順序を検査する。
function Get-FunctionBody([string]$Text,[string]$Signature){
    $start=$Text.IndexOf($Signature)
    if($start-lt0){throw "関数本体を抽出できません: $Signature"}
    $terminator="`n}"
    $end=$Text.IndexOf($terminator,$start)
    if($end-lt0){throw "関数の終端を特定できません: $Signature"}
    return $Text.Substring($start,$end-$start)
}
$attachBody=Get-FunctionBody $source 'void CompositorSpikeController::attach(CompositorRhiItem* item) {'
$observerIndex=$attachBody.IndexOf('startVBlankObserverWithPreroll()')
$timerIndex=$attachBody.IndexOf('timer_.start()')
if($observerIndex-lt0-or$timerIndex-lt0-or$observerIndex-ge$timerIndex){
    throw 'startup Presentedより前のobserver開始が固定されていません'
}
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
