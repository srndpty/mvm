[CmdletBinding()]
param(
    # PreW2  : W2 wiring 前の baseline。legacy path が存在することを記録する。
    # PostW2 : W2 wiring 後。legacy path が v2 canonical から消えたことを固定する。
    # W2A1   : lower boundary preroll の順序不変量。
    [Parameter(Mandatory=$true)][ValidateSet('PreW2','W2A1','PostW2')][string]$Phase,
    [Parameter(Mandatory=$true)][string]$SourceRoot
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
function Read-Source([string]$Relative){
    $path=Join-Path $SourceRoot $Relative
    if(-not(Test-Path -LiteralPath $path)){Fail "契約対象sourceがありません: $path"}
    return Get-Content -LiteralPath $path -Raw -Encoding utf8
}
function Remove-Comments([string]$Text){
    $withoutBlock=[regex]::Replace($Text,'/\*.*?\*/','',[System.Text.RegularExpressions.RegexOptions]::Singleline)
    return [regex]::Replace($withoutBlock,'(?m)//.*$','')
}
if($Phase-eq'W2A1'){
    # W2-A.1。measurement窓を開く前に、observer startとその後の新規sample確認が
    # この順序で起きることを静的に固定する。順序が崩れると下側bracketは
    # race に戻る。
    $controller=Remove-Comments (Read-Source 'apps/compositor_spike/compositor_spike_controller.cpp')
    $index=$controller.IndexOf('void CompositorSpikeController::requestMeasurementStart()')
    if($index-lt0){Fail 'requestMeasurementStartが見つかりません'}
    $body=$controller.Substring($index)
    $baseline=$body.IndexOf('ring().publishSerial()')
    $start=$body.IndexOf('vblankObserver_.start(')
    $preroll=$body.IndexOf('prerollNewSample(')
    $arm=$body.IndexOf('measurementStartRequested.store(true')
    if($baseline-lt0){Fail 'W2A1違反: preroll baseline serialを取得していません'}
    if($start-lt0){Fail 'W2A1違反: VBlank observerをstartしていません'}
    if($preroll-lt0){Fail 'W2A1違反: prerollNewSampleを呼んでいません'}
    if($arm-lt0){Fail 'W2A1違反: measurementStartRequestedをarmしていません'}
    # baseline は start より前でなければ stale sample を受理しうる。
    if($baseline-gt$start){Fail 'W2A1違反: baseline serialがobserver startより後です'}
    if($start-gt$preroll){Fail 'W2A1違反: prerollがobserver startより前です'}
    if($preroll-gt$arm){Fail 'W2A1違反: measurement armがprerollより前です'}
    # timeout時にそのままarmしていないこと。preroll失敗経路がreturnで閉じている。
    $failurePath=$body.Substring($preroll,$arm-$preroll)
    if($failurePath-notmatch 'beginShutdown'){
        Fail 'W2A1違反: preroll失敗時にfail-closeしていません'
    }
    if($failurePath-notmatch 'return;'){
        Fail 'W2A1違反: preroll失敗時にmeasurementを開始しない経路がありません'
    }
    Write-Host 'P2-D5-2 formal architecture: PASS (W2A1 lower boundary preroll order)'
    exit 0
}
$item=Remove-Comments (Read-Source 'src/app/preview/compositor_rhi_item.cpp')
$scheduler=Remove-Comments (Read-Source 'src/media/gpu_preview/presentation_opportunity_scheduler.h')

# W0.5-A で legacy と証明した frameSwapped pairing の実体。
# recordFrameSwapped から formal scheduler の commitSwap を呼んでいるか。
$recordIndex=$item.IndexOf('void CompositorRhiItem::recordFrameSwapped()')
if($recordIndex-lt0){Fail 'recordFrameSwappedが見つかりません'}
$tail=$item.Substring($recordIndex)
$frameSwappedCommitsFormal=$tail-match 'formalOpportunityScheduler\.commitSwap'
# frameSwapped 由来の例外ハック。v2 では不要になるはず。
$ignoreNextSwapPresent=$item-match 'formalOpportunityIgnoreNextSwap'
# scheduler API に swap 概念が残っているか。
$schedulerHasCommitSwap=$scheduler-match 'bool\s+commitSwap\('

if($Phase-eq'PreW2'){
    # baseline を明示的に固定する。ここが変わったら W2 が始まったということ。
    if(-not$frameSwappedCommitsFormal){
        Fail 'PreW2 baselineが崩れています: recordFrameSwappedがcommitSwapを呼んでいません'
    }
    if(-not$ignoreNextSwapPresent){
        Fail 'PreW2 baselineが崩れています: formalOpportunityIgnoreNextSwapがありません'
    }
    if(-not$schedulerHasCommitSwap){
        Fail 'PreW2 baselineが崩れています: schedulerにcommitSwapがありません'
    }
    Write-Host 'P2-D5-2 formal architecture: PASS (PreW2 baseline recorded)'
    exit 0
}
# PostW2: v2 canonical path から legacy frameSwapped pairing が消えたことを要求する。
if($frameSwappedCommitsFormal){
    Fail 'PostW2違反: recordFrameSwappedがformal commitSwapを呼んでいます'
}
if($ignoreNextSwapPresent){
    Fail 'PostW2違反: formalOpportunityIgnoreNextSwapが残っています'
}
Write-Host 'P2-D5-2 formal architecture: PASS (PostW2 legacy pairing retired)'
