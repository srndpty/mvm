[CmdletBinding()]
param(
    # PreW2  : W2 wiring 前の baseline。legacy path が存在することを記録する。
    # PostW2 : W2 wiring 後。legacy path が v2 canonical から消えたことを固定する。
    [Parameter(Mandatory=$true)][ValidateSet('PreW2','PostW2')][string]$Phase,
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
