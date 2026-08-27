[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'Good','NegativeBaselineAfterStart','NegativeHelperDoesNotFailClose',
        'NegativeDuplicatePrerollCall','NegativeArmBypassesPrerollPath',
        'NegativeEnvelopeOpensBeforePreroll','NegativeDuplicateArm',
        'NegativeArmBeforePreroll','NegativeNoShutdownOnFailure')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Contract,
    [Parameter(Mandatory=$true)][string]$SourceRoot,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

# W2-A.1 guard の mutation test。
#
# architecture guard は実装構造の変更で静かに死ぬ (実際に helper extraction 後、
# requestMeasurementStart 本文だけを走査する版が空振りしていた)。guard 自身が
# 不変量の破れを検出できることを、実 source の変異コピーで固定する。
# product code は変更しない。検査対象は guard 側である。

$relative='apps/compositor_spike/compositor_spike_controller.cpp'
$original=Get-Content -LiteralPath (Join-Path $SourceRoot $relative) -Raw -Encoding utf8
$root=Join-Path $Directory "process-$PID"
if(Test-Path -LiteralPath $root){Remove-Item -LiteralPath $root -Recurse -Force}
New-Item -ItemType Directory -Path (Join-Path $root 'apps/compositor_spike') -Force|Out-Null

# 変異は ASCII だけを足がかりにする。source の日本語文字列に依存しない。
function Get-Region([string]$Text,[string]$Signature){
    $start=$Text.IndexOf($Signature)
    if($start-lt0){throw "変異対象の関数がありません: $Signature"}
    $end=$Text.IndexOf("`n}",$start)
    if($end-lt0){throw "変異対象の関数末尾を特定できません: $Signature"}
    return @{Start=$start;End=$end;Body=$Text.Substring($start,$end-$start)}
}
function Set-Region([string]$Text,$Region,[string]$Body){
    return $Text.Substring(0,$Region.Start)+$Body+$Text.Substring($Region.End)
}
$helperSignature='bool CompositorSpikeController::startVBlankObserverWithPreroll()'
$armSignature='void CompositorSpikeController::armMeasurementAfterCaptureEnvelopeOpen()'
$requestSignature='void CompositorSpikeController::requestMeasurementStart()'
$armStatement='state_->measurementStartRequested.store(true, std::memory_order_release);'

$mutated=$original
switch($Case){
    'NegativeBaselineAfterStart'{
        # baseline serial を observer start より後に取ると stale sample を受理しうる。
        $region=Get-Region $mutated $helperSignature
        $body=$region.Body -replace `
            '(?s)(    const unsigned long long prerollBaseline = vblankObserver_\.ring\(\)\.publishSerial\(\);\r?\n)(    if \(!vblankObserver_\.start\(vblankHwnd, vblankObserverError_\)\) \{\r?\n        return false;\r?\n    \}\r?\n)','$2$1'
        if($body-eq$region.Body){throw '変異を適用できませんでした: NegativeBaselineAfterStart'}
        $mutated=Set-Region $mutated $region $body
    }
    'NegativeHelperDoesNotFailClose'{
        # preroll に失敗しても helper が成功を返す。
        $region=Get-Region $mutated $helperSignature
        $prerollIndex=$region.Body.IndexOf('prerollNewSample(')
        $tail=$region.Body.Substring($prerollIndex) -replace 'return false;','return true;'
        $body=$region.Body.Substring(0,$prerollIndex)+$tail
        if($body-eq$region.Body){throw '変異を適用できませんでした: NegativeHelperDoesNotFailClose'}
        $mutated=Set-Region $mutated $region $body
    }
    'NegativeDuplicatePrerollCall'{
        # helper の外に2つ目の preroll 経路を作る。
        $region=Get-Region $mutated $armSignature
        $body=$region.Body -replace [regex]::Escape('    if (!startVBlankObserverWithPreroll()) {'),`
            "    vblankObserver_.prerollNewSample(0, 1, vblankPreroll_);`n    if (!startVBlankObserverWithPreroll()) {"
        if($body-eq$region.Body){throw '変異を適用できませんでした: NegativeDuplicatePrerollCall'}
        $mutated=Set-Region $mutated $region $body
    }
    'NegativeArmBypassesPrerollPath'{
        # preroll helper を通らない arm 経路を作る。
        $armRegion=Get-Region $mutated $armSignature
        $armBody=$armRegion.Body -replace [regex]::Escape($armStatement),''
        $mutated=Set-Region $mutated $armRegion $armBody
        $requestRegion=Get-Region $mutated $requestSignature
        $requestBody=$requestRegion.Body -replace [regex]::Escape('    measurementStartCaptured_ = false;'),`
            "    measurementStartCaptured_ = false;`n    $armStatement"
        if($requestBody-eq$requestRegion.Body){throw '変異を適用できませんでした: NegativeArmBypassesPrerollPath'}
        $mutated=Set-Region $mutated $requestRegion $requestBody
    }
    'NegativeEnvelopeOpensBeforePreroll'{
        # producer を開いてから preroll する。lower support が QPC heuristic 依存へ戻る。
        $region=Get-Region $mutated $requestSignature
        $body=$region.Body -replace [regex]::Escape('    if (state_->nativePresentCaptureEnvelopeEnabled.load(std::memory_order_acquire)) {'),`
            "    if (state_->nativePresentCaptureEnvelopeEnabled.load(std::memory_order_acquire)) {`n        state_->nativePresentEnvelopeStartRequested.store(true, std::memory_order_release);"
        if($body-eq$region.Body){throw '変異を適用できませんでした: NegativeEnvelopeOpensBeforePreroll'}
        $mutated=Set-Region $mutated $region $body
    }
    'NegativeDuplicateArm'{
        # arm が2箇所になると、どちらが preroll 後かを静的に決められない。
        $region=Get-Region $mutated $armSignature
        $body=$region.Body+"`n    $armStatement`n"
        $mutated=Set-Region $mutated $region $body
    }
    'NegativeArmBeforePreroll'{
        # arm を preroll helper 呼び出しより前へ移す。
        $region=Get-Region $mutated $armSignature
        $body=$region.Body -replace [regex]::Escape($armStatement),''
        $body=$body -replace [regex]::Escape('    if (!startVBlankObserverWithPreroll()) {'),`
            "    $armStatement`n    if (!startVBlankObserverWithPreroll()) {"
        if($body-eq$region.Body){throw '変異を適用できませんでした: NegativeArmBeforePreroll'}
        $mutated=Set-Region $mutated $region $body
    }
    'NegativeNoShutdownOnFailure'{
        # preroll 失敗を fail-close しない。
        $region=Get-Region $mutated $armSignature
        $body=$region.Body -replace 'beginShutdown\(','noopShutdown('
        if($body-eq$region.Body){throw '変異を適用できませんでした: NegativeNoShutdownOnFailure'}
        $mutated=Set-Region $mutated $region $body
    }
}
if($Case-ne'Good'-and$mutated-eq$original){throw "$Case の変異が source に適用されていません"}
Set-Content -LiteralPath (Join-Path $root $relative) -Value $mutated -Encoding utf8

$failed=$false
try{& $Contract -Phase W2A1 -SourceRoot $root *> $null}catch{$failed=$true}
if($Case-eq'Good'){
    if($failed){throw '未変異のsourceをW2A1 guardが拒否しました'}
    Write-Output "W2-A.1 preroll guard $Case`: PASS";exit 0
}
if(-not$failed){throw "$Case をW2A1 guardが検出できませんでした (guardが空振りしています)"}
Write-Output "W2-A.1 preroll guard $Case`: PASS"
