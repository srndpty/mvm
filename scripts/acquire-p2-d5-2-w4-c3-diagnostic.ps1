[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateRange(1,10)][int]$Runs=3,
    [ValidateRange(1,60)][int]$WarmupSeconds=2,
    [ValidateRange(1,60)][int]$MeasureSeconds=60,
    [ValidateRange(30,300)][int]$TimeoutSeconds=120,
    [string]$Executable=(Join-Path (Split-Path -Parent $PSScriptRoot) 'build\ucrt64-release\bin\mvm_compositor_spike.exe'),
    [string]$PatchedQtBin=(Join-Path (Split-Path -Parent $PSScriptRoot) 'build\qtbase-c0\bin'),
    [string]$PatchedQtQuickBin=(Join-Path (Split-Path -Parent $PSScriptRoot) 'build\qtquick-t2-runtime')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
# P2-D5-2 W4-C3 diagnostic-only fresh capture。canonical performance authorityへ昇格しない。
# workload/runtime条件はW4-C2 formal captureと同一。C3固有CLI flagは追加しない。
$repo=Split-Path -Parent $PSScriptRoot
$c3Checker=Join-Path $PSScriptRoot 'check-p2-d5-2-w4-c3-causal-replay.ps1'
$c2Checker=Join-Path $PSScriptRoot 'check-p2-d5-2-w4-c2-invocation-ledger.ps1'
$sourceA=Join-Path $repo 'tests\assets\benchmark\v1080p60_h264.mp4'
$sourceB=Join-Path $repo 'tests\assets\benchmark\v1080p60_hevc.mp4'
$qtGui=Join-Path $PatchedQtBin 'Qt6Gui.dll';$qtQuick=Join-Path $PatchedQtQuickBin 'Qt6Quick.dll'
foreach($path in @($Executable,$c3Checker,$c2Checker,$sourceA,$sourceB,$qtGui,$qtQuick)){
    if(-not(Test-Path -LiteralPath $path)){throw "W4-C3 capture必須pathがありません: $path"}
}
if(Test-Path -LiteralPath $OutputDirectory){throw "既存W4-C3 captureを上書きしません: $OutputDirectory"}
$headSha=(& git -C $repo rev-parse HEAD).Trim()
$status=& git -C $repo status --porcelain
if(-not[string]::IsNullOrWhiteSpace(($status|Out-String))){
    throw 'W4-C3 diagnostic captureはclean worktreeから取得してください'
}
function Get-Sha256([string]$Path){
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
$binaryHash=Get-Sha256 $Executable
$qtGuiHash=Get-Sha256 $qtGui
$qtQuickHash=Get-Sha256 $qtQuick
# C3 proofの意味はこの2 checkerに依存するので、hashをacquisitionへbindする。
$c3CheckerHash=Get-Sha256 $c3Checker
$c2CheckerHash=Get-Sha256 $c2Checker
$sourceAssetHashes=[ordered]@{}
foreach($asset in @($sourceA,$sourceB)){$sourceAssetHashes[(Split-Path -Leaf $asset)]=Get-Sha256 $asset}
$sourceHashes=[ordered]@{}
foreach($path in @('src/media/gpu_preview/presentation_opportunity_scheduler.h',
        'src/media/gpu_preview/presentation_opportunity_scheduler.cpp',
        'src/app/preview/compositor_rhi_item.h',
        'src/app/preview/compositor_rhi_item.cpp',
        'apps/compositor_spike/compositor_spike_controller.cpp')){
    $sourceHashes[$path]=Get-Sha256 (Join-Path $repo $path)
}
Write-Host ("【操作停止必須：約{0}分】" -f [Math]::Ceiling($Runs*($WarmupSeconds+$MeasureSeconds+10)/60.0))
Write-Host 'DWM refresh authorityを使用します。完了までAlt+Tab、window移動・resize、他アプリ操作、動画再生をしないでください。'
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
Remove-Item Env:QSG_NO_VSYNC -ErrorAction SilentlyContinue
$env:QT_D3D_MAX_FRAME_LATENCY='2'
$env:MVM_P2_C3_SUBMISSION_MODE='CONTROL'
$env:PATH="$PatchedQtQuickBin;$PatchedQtBin;C:\msys64\ucrt64\bin;$env:PATH"
$env:QT_PLUGIN_PATH=(Join-Path (Split-Path -Parent $PatchedQtBin) 'plugins')
$env:QML_IMPORT_PATH='C:\msys64\ucrt64\share\qt6\qml'
$results=@();$exactRuns=0
for($run=1;$run-le$Runs;++$run){
    $runDirectory=Join-Path $OutputDirectory "run-$run"
    New-Item -ItemType Directory -Path $runDirectory|Out-Null
    $json=Join-Path $runDirectory 'diagnostic-app.json'
    $arguments=@('--source-a',$sourceA,'--source-b',$sourceB,'--metrics',$json,
        '--warmup-seconds',[string]$WarmupSeconds,'--measure-seconds',[string]$MeasureSeconds,
        '--seed','20260827','--seek-count','1000','--display-timeout-ms','2000',
        '--gpu-completion','fence','--mode','playback','--formal-preflight','--vblank-observer',
        '--presentation-opportunity-ring','--native-present-hook','on',
        '--w4-c2-scheduler-invocation-ledger')
    $process=Start-Process -FilePath $Executable -ArgumentList $arguments -PassThru `
        -RedirectStandardOutput (Join-Path $runDirectory 'stdout.txt') `
        -RedirectStandardError (Join-Path $runDirectory 'stderr.txt')
    if(-not$process.WaitForExit($TimeoutSeconds*1000)){
        $process.Kill($true);$process.WaitForExit();throw "W4-C3 run $run がtimeoutしました"
    }
    if($process.ExitCode-ne0-or-not(Test-Path -LiteralPath $json)){
        throw "W4-C3 run $run が失敗しました: exit=$($process.ExitCode)"
    }
    $proof=Join-Path $runDirectory 'c3-causal-replay-check.json'
    & pwsh -NoProfile -File $c3Checker -Json $json -Output $proof *> $null
    $checkerExit=$LASTEXITCODE
    if(-not(Test-Path -LiteralPath $proof)){throw "W4-C3 run $run のproofが生成されませんでした"}
    $checked=Get-Content -LiteralPath $proof -Raw -Encoding utf8|ConvertFrom-Json
    # runごとに独立してclosure条件を判定する。多数決やaggregate昇格はしない。
    $runExact=($checkerExit-eq0-and
        [string]$checked.verdict-eq'W4_C3_CAUSAL_REPLAY_EXACT'-and
        [bool]$checked.root_cause_determined-and
        [bool]$checked.terminal_required_intent_membership-and
        -not[bool]$checked.canonical_performance_authority-and
        [string]$checked.join_method-eq'scheduler_invocation_serial'-and
        -not[bool]$checked.qpc_used_for_join)
    if($runExact){++$exactRuns}
    $entry=[ordered]@{run=$run
        app_sha256=(Get-Sha256 $json)
        c3_proof_sha256=(Get-Sha256 $proof)
        verdict=[string]$checked.verdict
        root_cause_determined=[bool]$checked.root_cause_determined
        run_closure_exact=$runExact}
    if($checkerExit-eq0){
        $entry.terminal_invocation_serial=[int64]$checked.terminal_invocation_serial
        $entry.terminal_intent_ordinal=[int64]$checked.terminal_intent_ordinal
        $entry.terminal_target_frame=[int64]$checked.terminal_target_frame
        $entry.terminal_required_intent_membership=[bool]$checked.terminal_required_intent_membership
        $entry.replayed_decision_count=[int64]$checked.replayed_decision_count
    }else{
        $entry.failure=[string]$checked.failure
    }
    $results+=,$entry
}
if((& git -C $repo rev-parse HEAD).Trim()-ne$headSha){throw 'W4-C3 capture中にHEADが変化しました'}
$postStatus=& git -C $repo status --porcelain
if(-not[string]::IsNullOrWhiteSpace(($postStatus|Out-String))){
    throw 'W4-C3 capture中にworktreeが変化しました'
}
if((Get-Sha256 $Executable)-ne$binaryHash){throw 'W4-C3 capture中にbinaryが変化しました'}
if((Get-Sha256 $c3Checker)-ne$c3CheckerHash){throw 'W4-C3 capture中にC3 checkerが変化しました'}
if((Get-Sha256 $c2Checker)-ne$c2CheckerHash){throw 'W4-C3 capture中にC2 checkerが変化しました'}
foreach($path in $sourceHashes.Keys){
    if((Get-Sha256 (Join-Path $repo $path))-ne$sourceHashes[$path]){
        throw "W4-C3 capture中にsourceが変化しました: $path"
    }
}
foreach($asset in @($sourceA,$sourceB)){
    if((Get-Sha256 $asset)-ne$sourceAssetHashes[(Split-Path -Leaf $asset)]){
        throw "W4-C3 capture中にsource assetが変化しました: $asset"
    }
}
if((Get-Sha256 $qtGui)-ne$qtGuiHash){throw 'W4-C3 capture中にQt6Gui.dllが変化しました'}
if((Get-Sha256 $qtQuick)-ne$qtQuickHash){throw 'W4-C3 capture中にQt6Quick.dllが変化しました'}
$closed=($exactRuns-eq$Runs)
$summary=[ordered]@{schema='mvm-p2-d5-2-w4-c3-diagnostic-acquisition-1';stage='P2-D5-2-W4-C3'
    diagnostic_root_cause_capture=$true;canonical_performance_authority=$false
    historical_w3_verdict_rewritten=$false;historical_w4a_rewritten=$false
    historical_w4b_rewritten=$false;checkpoint_sha=$headSha;worktree_clean=$true
    stop_witness_schema='mvm-p2-d5-2-w4-c3-stop-witness-3'
    executable_sha256=$binaryHash
    c3_checker_sha256=$c3CheckerHash
    c2_checker_sha256=$c2CheckerHash
    interactive_protocol='OPERATION_STOP_REQUIRED'
    qt_gui_sha256=$qtGuiHash
    qt_quick_sha256=$qtQuickHash
    source_sha256=$sourceHashes
    source_asset_sha256=$sourceAssetHashes
    runs=$Runs;warmup_seconds=$WarmupSeconds;measure_seconds=$MeasureSeconds
    exact_runs=$exactRuns
    aggregate_majority_used=$false
    results=$results
    root_cause_determined=$closed
    verdict=$(if($closed){'W4_C3_CAUSAL_REPLAY_EXACT'}else{'W4_C3_NOT_CLOSED'})}
$summary|ConvertTo-Json -Depth 12|Set-Content -LiteralPath (Join-Path $OutputDirectory 'w4-c3-acquisition.json') -Encoding utf8
if(-not$closed){
    Write-Host ("P2-D5-2 W4-C3 diagnostic acquisition: NOT CLOSED ({0}/{1} exact)" -f $exactRuns,$Runs)
    exit 4
}
Write-Host ("P2-D5-2 W4-C3 diagnostic acquisition: PASS ({0}/{1} exact)" -f $exactRuns,$Runs)
