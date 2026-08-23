[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)]
    [ValidateSet('Good','NegativeFlagGatedContext','NegativeMissingNullGuard',
                 'NegativeVisibleAtLoad','NegativeShowBeforeAttach')][string]$Case,
    [Parameter(Mandatory=$true)][string]$SourceRoot,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

$renderer='src/app/preview/compositor_rhi_item.cpp'
$qml='apps/compositor_spike/Main.qml'
$entry='apps/compositor_spike/main.cpp'

function Read-Source([string]$Root,[string]$Relative){
    $path=Join-Path $Root $Relative
    if(-not(Test-Path -LiteralPath $path)){throw "契約対象sourceがありません: $path"}
    Get-Content -LiteralPath $path -Raw -Encoding utf8
}
# 契約判定はコード本体に対して行う。コメント本文に識別子が現れても違反にしない。
function Remove-Comments([string]$Text){
    $withoutBlock=[regex]::Replace($Text,'/\*.*?\*/','',[System.Text.RegularExpressions.RegexOptions]::Singleline)
    return [regex]::Replace($withoutBlock,'(?m)//.*$','')
}
function Get-FunctionBody([string]$Text,[string]$Signature){
    $start=$Text.IndexOf($Signature)
    if($start-lt0){throw "関数が見つかりません: $Signature"}
    $open=$Text.IndexOf('{',$start)
    if($open-lt0){throw "関数本体の開き括弧がありません: $Signature"}
    $depth=0
    for($i=$open;$i-lt$Text.Length;++$i){
        if($Text[$i]-eq'{'){$depth++}
        elseif($Text[$i]-eq'}'){$depth--;if($depth-eq0){return $Text.Substring($open,$i-$open+1)}}
    }
    throw "関数本体を閉じられません: $Signature"
}
# F3-C3-A3-T2-B startup configuration raceのregression契約。
# render threadのinitialize()がGUI threadのattach()を追い越しても
# TARGET_RHIITEM_PIXEL_TOGGLEがnull derefにならないことをsource levelで固定する。
function Assert-StartupOrder([string]$Root){
    $rendererText=Read-Source $Root $renderer
    $initialize=Get-FunctionBody $rendererText 'void initialize(QRhiCommandBuffer*) override'
    if($initialize-notmatch 'QueryInterface\(IID_PPV_ARGS\(&nativeContext1_\)\)'){
        throw 'initialize()がID3D11DeviceContext1 capabilityを取得していません'
    }
    if((Remove-Comments $initialize)-match 'diagnosticTargetPixelToggle'){
        throw 'initialize()が後から変更されるdiagnostic flagに依存しています'
    }
    $toggle=Get-FunctionBody $rendererText 'bool issueTargetPixelToggle('
    $guard=$toggle.IndexOf('if (!nativeContext1_)')
    $use=$toggle.IndexOf('nativeContext1_->')
    if($guard-lt0){throw 'issueTargetPixelToggle()にnativeContext1_のnull guardがありません'}
    if($use-lt0-or$use-lt$guard){throw 'issueTargetPixelToggle()がnull guardより前にcontextを使用しています'}

    $qmlText=Read-Source $Root $qml
    if($qmlText-notmatch '(?m)^\s*visible:\s*false\s*$'){
        throw 'Main.qmlがengine.load()時点でwindowを可視にしています'
    }

    $entryText=Read-Source $Root $entry
    $attach=$entryText.IndexOf('controller.attach(surface);')
    $show=$entryText.IndexOf('window->setVisible(true);')
    if($attach-lt0){throw 'main()にcontroller.attach(surface)がありません'}
    if($show-lt0){throw 'main()がwindowを明示的に可視化していません'}
    if($show-lt$attach){throw 'main()がattach()より前にwindowを可視化しています'}
}
function New-MutatedRoot([scriptblock]$Mutate){
    if(Test-Path -LiteralPath $Directory){Remove-Item -LiteralPath $Directory -Recurse -Force}
    foreach($relative in @($renderer,$qml,$entry)){
        $destination=Join-Path $Directory $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force|Out-Null
        Copy-Item -LiteralPath (Join-Path $SourceRoot $relative) -Destination $destination
    }
    & $Mutate
    return $Directory
}
function Edit-Copy([string]$Relative,[string]$Old,[string]$New){
    $path=Join-Path $Directory $Relative
    $text=Get-Content -LiteralPath $path -Raw -Encoding utf8
    if($text.IndexOf($Old)-lt0){throw "mutation対象が見つかりません: $Relative"}
    $text=$text.Replace($Old,$New)
    Set-Content -LiteralPath $path -Value $text -Encoding utf8 -NoNewline
}

if($Case-eq'Good'){
    Assert-StartupOrder $SourceRoot
    Write-Host 'F3-C3-A3-T2 startup order contract: PASS (Good)'
    exit 0
}
$root=switch($Case){
    'NegativeFlagGatedContext'{New-MutatedRoot {
        Edit-Copy $renderer @'
        const HRESULT contextResult = static_cast<ID3D11DeviceContext*>(h->context)
                                          ->QueryInterface(IID_PPV_ARGS(&nativeContext1_));
'@ @'
        HRESULT contextResult = S_OK;
        if (state_->diagnosticTargetPixelToggle.load(std::memory_order_acquire))
            contextResult = static_cast<ID3D11DeviceContext*>(h->context)
                                ->QueryInterface(IID_PPV_ARGS(&nativeContext1_));
'@
    }}
    'NegativeMissingNullGuard'{New-MutatedRoot {
        Edit-Copy $renderer @'
        if (!nativeContext1_) {
            err = "TARGET_RHIITEM_PIXEL_TOGGLEにD3D11.1 contextが必要です";
            return false;
        }
'@ ''
    }}
    'NegativeVisibleAtLoad'{New-MutatedRoot {Edit-Copy $qml '    visible: false' '    visible: true'}}
    'NegativeShowBeforeAttach'{New-MutatedRoot {
        Edit-Copy $entry @'
    controller.attach(surface);
'@ @'
    window->setVisible(true);
    controller.attach(surface);
'@
        Edit-Copy $entry @'
    window->setVisible(true);
    QObject::connect(&controller
'@ @'
    QObject::connect(&controller
'@
    }}
}
$failed=$false
try{Assert-StartupOrder $root}catch{$failed=$true;Write-Host "expected violation: $($_.Exception.Message)"}
if(-not$failed){throw "negative caseが契約違反を検出できませんでした: $Case"}
Write-Host "F3-C3-A3-T2 startup order contract: PASS ($Case)"
