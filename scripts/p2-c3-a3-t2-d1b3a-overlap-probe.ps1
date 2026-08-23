[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateRange(1,4)][int]$RepeatsPerCondition=1,
    [ValidateRange(12,300)][int]$WarmupSeconds=26,
    [ValidateRange(200,60000)][int]$PreCleanMs=2000,
    [ValidateRange(200,60000)][int]$OverlapMs=3000,
    [ValidateRange(1,300)][int]$MeasureSeconds=5,
    [ValidateRange(30,600)][int]$TimeoutSeconds=180
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
$runner=Join-Path $PSScriptRoot 'p2-c3-a3-t1-condition.ps1'
$preflightChecker=Join-Path $PSScriptRoot 'check-p2-c3-a3-t2-d1b0-preflight.ps1'
$summarizer=Join-Path $PSScriptRoot 'summarize-p2-c3-a3-t2-d1b3a.ps1'
$identity=[Security.Principal.WindowsIdentity]::GetCurrent();$principal=[Security.Principal.WindowsPrincipal]::new($identity)
if(-not$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'F3-C3-A3-T2-D1-B3a probe採取には管理者権限が必要です'}
foreach($path in @($runner,$preflightChecker,$summarizer)){
    if(-not(Test-Path -LiteralPath $path)){throw "D1-B3a必須scriptがありません: $path"}
}
# controllerはsettleに約9秒使ってからreadyになる。phaseはその後に始まるため、
# PRE_CLEAN+OVERLAPがapp warmup内で完結することを事前に要求する。
$controllerSettleSeconds=9
$phaseSeconds=($PreCleanMs+$OverlapMs)/1000.0
$requiredWarmup=$controllerSettleSeconds+$phaseSeconds+4
if($WarmupSeconds-lt$requiredWarmup){
    throw ("OVERLAP phaseがmeasurementへ食い込みます。WarmupSecondsを{0}以上にしてください (現在 {1})" -f [math]::Ceiling($requiredWarmup),$WarmupSeconds)
}
if(Test-Path -LiteralPath $OutputDirectory){throw "既存D1-B3a artifactを上書きしません: $OutputDirectory"}
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
# discovery probe。CLEAN_STATICはbaseline、他2条件はrunner-controlled intervention。
# 人間のAlt+Tabやwindow操作はinterventionに使わない。
$conditions=@(
    @{name='CLEAN_STATIC';mode='VISIBLE_UNOCCLUDED'},
    @{name='FOREIGN_WINDOW_OVERLAP';mode='FOREIGN_WINDOW_OVERLAP'},
    @{name='OVERLAP_THEN_REMOVE';mode='OVERLAP_THEN_REMOVE'}
)
$sequence=@()
for($repeat=1;$repeat-le$RepeatsPerCondition;++$repeat){
    # repeatごとに順序を回して時間依存を薄める。
    for($offset=0;$offset-lt$conditions.Count;++$offset){
        $sequence+=$conditions[($offset+$repeat-1)%$conditions.Count]
    }
}
$estimatedMinutes=[math]::Ceiling($sequence.Count*($WarmupSeconds+$MeasureSeconds+63)/60)
Write-Host ''
Write-Host ("【操作停止必須：約{0}分】" -f $estimatedMinutes)
Write-Host 'presentation regime が測定対象です。実行中は Alt+Tab、window の移動・リサイズ、'
Write-Host '他 window 表示、通知操作、動画再生、ビルド等を行わないでください。'
Write-Host 'measurement 中の入力は PROTOCOL_INVALID とします。'
Write-Host ''
$runs=@()
for($index=0;$index-lt$sequence.Count;++$index){
    $condition=$sequence[$index].name;$mode=$sequence[$index].mode
    $name=('probe-{0:d2}-{1}' -f ($index+1),$condition.ToLowerInvariant())
    $directory=Join-Path $OutputDirectory $name
    Write-Host "F3-C3-A3-T2-D1-B3a probe: $($index+1)/$($sequence.Count) condition=$condition"
    & pwsh -NoProfile -File $runner -OutputDirectory $directory -Mode $mode `
        -DirtyPropagationMode 'DISABLED' -WarmupSeconds $WarmupSeconds `
        -MeasureSeconds $MeasureSeconds -TimeoutSeconds $TimeoutSeconds `
        -PreCleanMs $PreCleanMs -OverlapMs $OverlapMs
    if($LASTEXITCODE-ne0){throw "D1-B3a probe runが失敗しました: $name"}
    $app=Join-Path $directory 'canonical\traced-app.json'
    $preflightProof=Join-Path $directory 'preflight-proof.json'
    & pwsh -NoProfile -File $preflightChecker -AppJson $app -Output $preflightProof
    if($LASTEXITCODE-ne0){throw "D1-B3a preflightが失敗しました: $name"}
    $runs+=[ordered]@{index=$index+1;condition=$condition;window_mode=$mode;directory=$name
        condition_proof_sha256=Hash (Join-Path $directory 'condition-proof.json')
        preflight_proof_sha256=Hash $preflightProof}
}
$indexPath=Join-Path $OutputDirectory 'probe-runs.json'
[ordered]@{schema='mvm-p2-c3-a3-t2-d1b3a-probe-runs-1';status='PASS';authority='diagnostic_only'
    formal_counter_authority_changed=$false;formal_drop_threshold_changed=$false
    production_scheduler_changed=$false;warmup_seconds=$WarmupSeconds;measure_seconds=$MeasureSeconds
    repeats_per_condition=$RepeatsPerCondition;runs=$runs}|
    ConvertTo-Json -Depth 8|Set-Content -LiteralPath $indexPath -Encoding utf8
$proofPath=Join-Path $OutputDirectory 'probe-proof.json'
& pwsh -NoProfile -File $summarizer -ProbeDirectory $OutputDirectory -Output $proofPath
if($LASTEXITCODE-ne0){throw 'D1-B3a summarizerが失敗しました'}
$manifest=Join-Path $OutputDirectory 'manifest.sha256'
Get-ChildItem -LiteralPath $OutputDirectory -File|Where-Object{$_.FullName-ne$manifest}|Sort-Object Name|
    ForEach-Object{"$(Hash $_.FullName)  $($_.Name)"}|Set-Content -LiteralPath $manifest -Encoding ascii
Write-Host "F3-C3-A3-T2-D1-B3a probe: PASS ($proofPath)"
