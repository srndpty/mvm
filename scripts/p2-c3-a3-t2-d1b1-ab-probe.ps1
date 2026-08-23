[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [switch]$Resume,
    [ValidateRange(12,300)][int]$WarmupSeconds=12,
    [ValidateRange(1,300)][int]$MeasureSeconds=5,
    [ValidateRange(30,600)][int]$TimeoutSeconds=180
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
$runner=Join-Path $PSScriptRoot 'p2-c3-a3-t1-condition.ps1'
$chainChecker=Join-Path $PSScriptRoot 'check-p2-c3-a3-t2-update-chain.ps1'
$preflightChecker=Join-Path $PSScriptRoot 'check-p2-c3-a3-t2-d1b0-preflight.ps1'
$summarizer=Join-Path $PSScriptRoot 'summarize-p2-c3-a3-t2-d1b1.ps1'
$identity=[Security.Principal.WindowsIdentity]::GetCurrent();$principal=[Security.Principal.WindowsPrincipal]::new($identity)
if(-not$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'F3-C3-A3-T2-D1-B1 probe採取には管理者権限が必要です'}
foreach($path in @($runner,$chainChecker,$preflightChecker,$summarizer)){
    if(-not(Test-Path -LiteralPath $path)){throw "D1-B1必須scriptがありません: $path"}
}
if(Test-Path -LiteralPath $OutputDirectory){
    if(-not$Resume){throw "既存D1-B1 artifactを上書きしません: $OutputDirectory"}
    foreach($terminal in @('probe-runs.json','probe-proof.json','manifest.sha256')){
        if(Test-Path -LiteralPath (Join-Path $OutputDirectory $terminal)){throw "完了済みD1-B1はresumeできません: $OutputDirectory"}
    }
}else{New-Item -ItemType Directory -Path $OutputDirectory|Out-Null}
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
# A = DISABLED (T1相当)、B = CONTROL (T2相当)。geometryとwindow protocolは同一。
# 時間依存を排すため A B B A / B A A B の counterbalanced order とする。
$sequence=@(
    @{arm='A';mode='DISABLED'},@{arm='B';mode='CONTROL'},@{arm='B';mode='CONTROL'},@{arm='A';mode='DISABLED'},
    @{arm='B';mode='CONTROL'},@{arm='A';mode='DISABLED'},@{arm='A';mode='DISABLED'},@{arm='B';mode='CONTROL'}
)
$estimatedMinutes=[math]::Ceiling($sequence.Count*($WarmupSeconds+$MeasureSeconds+63)/60)
Write-Host ''
Write-Host ("【操作停止必須：約{0}分】" -f $estimatedMinutes)
Write-Host 'presentation regime が測定対象です。実行中は Alt+Tab、window の移動・リサイズ、'
Write-Host '他 window 表示、通知操作、動画再生、ビルド等を行わないでください。'
Write-Host 'measurement 中の入力は PROTOCOL_INVALID とします。'
Write-Host ''
$runs=@()
for($index=0;$index-lt$sequence.Count;++$index){
    $arm=$sequence[$index].arm;$mode=$sequence[$index].mode
    $name=('probe-{0:d2}-{1}-{2}' -f ($index+1),$arm,$mode.ToLowerInvariant())
    $directory=Join-Path $OutputDirectory $name
    Write-Host "F3-C3-A3-T2-D1-B1 probe: $($index+1)/$($sequence.Count) arm=$arm DirtyPropagationMode=$mode"
    $conditionProof=Join-Path $directory 'condition-proof.json'
    if(-not(Test-Path -LiteralPath $conditionProof)){
        if(Test-Path -LiteralPath $directory){throw "未完了runは条件artifactを保護するため自動再開しません: $directory"}
        & pwsh -NoProfile -File $runner -OutputDirectory $directory -Mode 'VISIBLE_UNOCCLUDED' `
            -DirtyPropagationMode $mode -WarmupSeconds $WarmupSeconds `
            -MeasureSeconds $MeasureSeconds -TimeoutSeconds $TimeoutSeconds
        if($LASTEXITCODE-ne0){throw "D1-B1 probe runが失敗しました: $name"}
    }
    $app=Join-Path $directory 'canonical\traced-app.json'
    $preflightProof=Join-Path $directory 'preflight-proof.json'
    & pwsh -NoProfile -File $preflightChecker -AppJson $app -Output $preflightProof
    if($LASTEXITCODE-ne0){throw "D1-B1 preflightが失敗しました: $name"}
    # update chainはCONTROL条件のみ閉じる。DISABLEDはpatched QtQuickを読まない。
    if($mode-eq'CONTROL'){
        & pwsh -NoProfile -File $chainChecker -AppJson $app -ExpectedMode 'CONTROL' `
            -Output (Join-Path $directory 'update-chain-proof.json')
        if($LASTEXITCODE-ne0){throw "D1-B1 update chainが失敗しました: $name"}
    }
    $runs+=[ordered]@{index=$index+1;arm=$arm;dirty_propagation_mode=$mode;directory=$name
        condition_proof_sha256=Hash $conditionProof;preflight_proof_sha256=Hash $preflightProof}
}
$indexPath=Join-Path $OutputDirectory 'probe-runs.json'
[ordered]@{schema='mvm-p2-c3-a3-t2-d1b1-probe-runs-1';status='PASS';authority='diagnostic_only'
    formal_counter_authority_changed=$false;formal_drop_threshold_changed=$false
    production_scheduler_changed=$false;warmup_seconds=$WarmupSeconds;measure_seconds=$MeasureSeconds
    counterbalanced_sequence=@($sequence|ForEach-Object{$_.arm});runs=$runs}|
    ConvertTo-Json -Depth 8|Set-Content -LiteralPath $indexPath -Encoding utf8
$proofPath=Join-Path $OutputDirectory 'probe-proof.json'
& pwsh -NoProfile -File $summarizer -ProbeDirectory $OutputDirectory -Output $proofPath
if($LASTEXITCODE-ne0){throw 'D1-B1 summarizerが失敗しました'}
$manifest=Join-Path $OutputDirectory 'manifest.sha256'
Get-ChildItem -LiteralPath $OutputDirectory -File|Where-Object{$_.FullName-ne$manifest}|Sort-Object Name|
    ForEach-Object{"$(Hash $_.FullName)  $($_.Name)"}|Set-Content -LiteralPath $manifest -Encoding ascii
Write-Host "F3-C3-A3-T2-D1-B1 probe: PASS ($proofPath)"
