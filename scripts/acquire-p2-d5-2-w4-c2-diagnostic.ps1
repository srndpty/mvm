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
$repo=Split-Path -Parent $PSScriptRoot
$checker=Join-Path $PSScriptRoot 'check-p2-d5-2-w4-c2-invocation-ledger.ps1'
$sourceA=Join-Path $repo 'tests\assets\benchmark\v1080p60_h264.mp4'
$sourceB=Join-Path $repo 'tests\assets\benchmark\v1080p60_hevc.mp4'
$qtGui=Join-Path $PatchedQtBin 'Qt6Gui.dll';$qtQuick=Join-Path $PatchedQtQuickBin 'Qt6Quick.dll'
foreach($path in @($Executable,$checker,$sourceA,$sourceB,$qtGui,$qtQuick)){
    if(-not(Test-Path -LiteralPath $path)){throw "W4-C2 capture必須pathがありません: $path"}
}
if(Test-Path -LiteralPath $OutputDirectory){throw "既存W4-C2 captureを上書きしません: $OutputDirectory"}
$headSha=(& git -C $repo rev-parse HEAD).Trim()
$status=& git -C $repo status --porcelain
if(-not[string]::IsNullOrWhiteSpace(($status|Out-String))){
    throw 'W4-C2 diagnostic captureはclean worktreeから取得してください'
}
$binaryHash=(Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash.ToLowerInvariant()
$qtGuiHash=(Get-FileHash -LiteralPath $qtGui -Algorithm SHA256).Hash.ToLowerInvariant()
$qtQuickHash=(Get-FileHash -LiteralPath $qtQuick -Algorithm SHA256).Hash.ToLowerInvariant()
$sourceHashes=[ordered]@{}
foreach($path in @('src/media/gpu_preview/presentation_opportunity_scheduler.h',
        'src/media/gpu_preview/presentation_opportunity_scheduler.cpp',
        'src/app/preview/compositor_rhi_item.cpp')){
    $sourceHashes[$path]=(Get-FileHash -LiteralPath (Join-Path $repo $path) -Algorithm SHA256).Hash.ToLowerInvariant()
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
$results=@()
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
        $process.Kill($true);$process.WaitForExit();throw "W4-C2 run $run がtimeoutしました"
    }
    if($process.ExitCode-ne0-or-not(Test-Path -LiteralPath $json)){
        throw "W4-C2 run $run が失敗しました: exit=$($process.ExitCode)"
    }
    $proof=Join-Path $runDirectory 'invocation-ledger-check.json'
    & pwsh -NoProfile -File $checker -Json $json -Output $proof *> $null
    if($LASTEXITCODE-ne0){throw "W4-C2 run $run invocation checkerが失敗しました"}
    $checked=Get-Content -LiteralPath $proof -Raw -Encoding utf8|ConvertFrom-Json
    $results+=,[ordered]@{run=$run;app_sha256=(Get-FileHash $json -Algorithm SHA256).Hash.ToLowerInvariant()
        proof_sha256=(Get-FileHash $proof -Algorithm SHA256).Hash.ToLowerInvariant()
        invocation_count=[int64]$checked.invocation_count
        terminal_invocation_serial=[string]$checked.terminal_invocation_serial
        verdict=[string]$checked.verdict}
}
if((& git -C $repo rev-parse HEAD).Trim()-ne$headSha){throw 'W4-C2 capture中にHEADが変化しました'}
$postStatus=& git -C $repo status --porcelain
if(-not[string]::IsNullOrWhiteSpace(($postStatus|Out-String))){
    throw 'W4-C2 capture中にworktreeが変化しました'
}
if((Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash.ToLowerInvariant()-ne$binaryHash){
    throw 'W4-C2 capture中にbinaryが変化しました'
}
foreach($path in $sourceHashes.Keys){
    $postSourceHash=(Get-FileHash -LiteralPath (Join-Path $repo $path) -Algorithm SHA256).Hash.ToLowerInvariant()
    if($postSourceHash-ne$sourceHashes[$path]){
        throw "W4-C2 capture中にsourceが変化しました: $path"
    }
}
if((Get-FileHash -LiteralPath $qtGui -Algorithm SHA256).Hash.ToLowerInvariant()-ne$qtGuiHash){
    throw 'W4-C2 capture中にQt6Gui.dllが変化しました'
}
if((Get-FileHash -LiteralPath $qtQuick -Algorithm SHA256).Hash.ToLowerInvariant()-ne$qtQuickHash){
    throw 'W4-C2 capture中にQt6Quick.dllが変化しました'
}
$summary=[ordered]@{schema='mvm-p2-d5-2-w4-c2-diagnostic-acquisition-1';stage='P2-D5-2-W4-C2'
    diagnostic_root_cause_capture=$true;canonical_performance_authority=$false
    historical_w3_verdict_rewritten=$false;historical_w4a_rewritten=$false
    historical_w4b_rewritten=$false;checkpoint_sha=$headSha;worktree_clean=$true
    instrumentation_schema_version=1;executable_sha256=$binaryHash
    interactive_protocol='OPERATION_STOP_REQUIRED'
    qt_gui_sha256=$qtGuiHash
    qt_quick_sha256=$qtQuickHash
    source_sha256=$sourceHashes;runs=$Runs;warmup_seconds=$WarmupSeconds
    measure_seconds=$MeasureSeconds;results=$results}
$summary|ConvertTo-Json -Depth 12|Set-Content -LiteralPath (Join-Path $OutputDirectory 'w4-c2-acquisition.json') -Encoding utf8
Write-Host "P2-D5-2 W4-C2 diagnostic acquisition: PASS ($Runs/$Runs)"
