[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [Parameter(Mandatory=$true)][ValidateSet('C1','C2')][string]$Stage,
    [switch]$Resume,
    [ValidateRange(12,300)][int]$WarmupSeconds=12,
    [ValidateRange(1,300)][int]$MeasureSeconds=15,
    [ValidateRange(30,600)][int]$TimeoutSeconds=180
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
$runner=Join-Path $PSScriptRoot 'p2-c3-a3-t2-condition.ps1'
$summarizer=Join-Path $PSScriptRoot 'summarize-p2-c3-a3-t2-c.ps1'
$identity=[Security.Principal.WindowsIdentity]::GetCurrent();$principal=[Security.Principal.WindowsPrincipal]::new($identity)
if(-not$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'F3-C3-A3-T2-C matrix採取には管理者権限が必要です'}
foreach($path in @($runner,$summarizer)){if(-not(Test-Path -LiteralPath $path)){throw "T2-C必須scriptがありません: $path"}}
if(Test-Path -LiteralPath $OutputDirectory){
    if(-not$Resume){throw "既存T2-C matrix artifactを上書きしません: $OutputDirectory"}
    foreach($terminal in @('matrix-runs.json','matrix-proof.json','manifest.sha256')){if(Test-Path -LiteralPath (Join-Path $OutputDirectory $terminal)){throw "完了済みT2-C matrixはresumeできません: $OutputDirectory"}}
}else{New-Item -ItemType Directory -Path $OutputDirectory|Out-Null}
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
# C1はInvalidateRectのみ、C2はforced RedrawWindowをpositive controlとして加える。
# InvalidateRectはupdate regionを設定するだけでpaint処理まで進むとは限らないため、
# C1で効かなくてもredirection pathへは進まず、必ずC2を経由する。
$triple=if($Stage-eq'C1'){@('CONTROL','TARGET_HWND_INVALIDATE','EXTERNAL_DIRTY')}
        else{@('TARGET_HWND_INVALIDATE','TARGET_HWND_REDRAW_NOW','EXTERNAL_DIRTY')}
$orders=@(
    @($triple[0],$triple[1],$triple[2]),
    @($triple[1],$triple[2],$triple[0]),
    @($triple[2],$triple[0],$triple[1])
)
$short=@{CONTROL='control';TARGET_HWND_INVALIDATE='target-invalidate';
         TARGET_HWND_REDRAW_NOW='target-redraw-now';EXTERNAL_DIRTY='external-dirty'}
$estimatedMinutes=[math]::Ceiling(9*($WarmupSeconds+$MeasureSeconds+63)/60)
Write-Host ''
Write-Host ("【操作停止必須：約{0}分】" -f $estimatedMinutes)
Write-Host 'F3-C3-A3-T2-C は DWM-wide dirty/wake が測定対象です。取得中は'
Write-Host '  - Alt+Tab / window の移動・リサイズ'
Write-Host '  - 他アプリの起動・更新・動画再生・ビルド'
Write-Host '  - notification の表示'
Write-Host 'を行わないでください。target window への重なり、および measurement 中の'
Write-Host 'ユーザー入力を検出した run は PROTOCOL_INVALID として fail-close します。'
Write-Host ''
$runs=@()
for($setIndex=0;$setIndex-lt3;++$setIndex){for($position=0;$position-lt3;++$position){
    $condition=$orders[$setIndex][$position]
    $name=('set-{0}-position-{1}-{2}'-f($setIndex+1),($position+1),$short[$condition])
    $directory=Join-Path $OutputDirectory $name
    Write-Host "F3-C3-A3-T2-C $Stage matrix: set=$($setIndex+1) position=$($position+1) condition=$condition"
    $summaryPath=Join-Path $directory 't2-summary.json'
    if(-not(Test-Path -LiteralPath $summaryPath)){
        if(Test-Path -LiteralPath $directory){throw "未完了runは条件artifactを保護するため自動再開しません: $directory"}
        & pwsh -NoProfile -File $runner -OutputDirectory $directory -Condition $condition -WarmupSeconds $WarmupSeconds -MeasureSeconds $MeasureSeconds -TimeoutSeconds $TimeoutSeconds
        if($LASTEXITCODE-ne0){throw "T2-C condition runが失敗しました: $name"}
    }
    $summary=Get-Content -LiteralPath $summaryPath -Raw -Encoding utf8|ConvertFrom-Json
    if([string]$summary.status-ne'PASS'-or[string]$summary.condition-ne$condition){throw "T2-C condition summaryが不正です: $name"}
    $runs+=[ordered]@{set=$setIndex+1;position=$position+1;condition=$condition;directory=$name;summary_sha256=Hash $summaryPath}
}}
$indexPath=Join-Path $OutputDirectory 'matrix-runs.json'
[ordered]@{schema='mvm-p2-c3-a3-t2-c-matrix-runs-1';status='PASS';stage=$Stage;authority='diagnostic_only';formal_counter_authority_changed=$false;formal_drop_threshold_changed=$false;production_scheduler_changed=$false;warmup_seconds=$WarmupSeconds;measure_seconds=$MeasureSeconds;cyclic_orders=$orders;runs=$runs}|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $indexPath -Encoding utf8
$proofPath=Join-Path $OutputDirectory 'matrix-proof.json'
& pwsh -NoProfile -File $summarizer -MatrixDirectory $OutputDirectory -Output $proofPath
if($LASTEXITCODE-ne0){throw 'T2-C matrix summarizerが失敗しました'}
$manifest=Join-Path $OutputDirectory 'manifest.sha256'
Get-ChildItem -LiteralPath $OutputDirectory -File|Where-Object{$_.FullName-ne$manifest}|Sort-Object Name|ForEach-Object{"$(Hash $_.FullName)  $($_.Name)"}|Set-Content -LiteralPath $manifest -Encoding ascii
Write-Host "F3-C3-A3-T2-C $Stage matrix: PASS ($proofPath)"
