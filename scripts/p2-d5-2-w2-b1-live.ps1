[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateRange(1,10)][int]$Runs=3,
    [ValidateRange(0,60)][int]$WarmupSeconds=1,
    [ValidateRange(1,60)][int]$MeasureSeconds=5,
    [ValidateRange(10,600)][int]$TimeoutSeconds=120,
    [string]$Executable=(Join-Path (Split-Path -Parent $PSScriptRoot) 'build\ucrt64-release\bin\mvm_compositor_spike.exe'),
    [string]$PatchedQtBin=(Join-Path (Split-Path -Parent $PSScriptRoot) 'build\qtbase-c0\bin'),
    [string]$PatchedQtQuickBin=(Join-Path (Split-Path -Parent $PSScriptRoot) 'build\qtquick-t2-runtime')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
$repo=Split-Path -Parent $PSScriptRoot
$sourceA=Join-Path $repo 'tests\assets\benchmark\v1080p60_h264.mp4'
$sourceB=Join-Path $repo 'tests\assets\benchmark\v1080p60_hevc.mp4'
$checker=Join-Path $PSScriptRoot 'check-p2-d5-2-w2-b1-intent-transport.ps1'
$qtGui=Join-Path $PatchedQtBin 'Qt6Gui.dll'
$qtQuick=Join-Path $PatchedQtQuickBin 'Qt6Quick.dll'
foreach($path in @($Executable,$sourceA,$sourceB,$checker,$qtGui,$qtQuick)){
    if(-not(Test-Path -LiteralPath $path)){throw "W2-B1 live必須fileがありません: $path"}
}
if(Test-Path -LiteralPath $OutputDirectory){throw "既存W2-B1 artifactを上書きしません: $OutputDirectory"}
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
$Executable=(Resolve-Path -LiteralPath $Executable).Path
Remove-Item Env:QSG_NO_VSYNC -ErrorAction SilentlyContinue
$env:QT_D3D_MAX_FRAME_LATENCY='2'
$env:MVM_P2_C3_SUBMISSION_MODE='CONTROL'
$env:PATH="$PatchedQtQuickBin;$PatchedQtBin;C:\msys64\ucrt64\bin;$env:PATH"
$env:QT_PLUGIN_PATH=(Join-Path (Split-Path -Parent $PatchedQtBin) 'plugins')
$env:QML_IMPORT_PATH='C:\msys64\ucrt64\share\qt6\qml'
$results=@()
for($run=1;$run-le$Runs;++$run){
    $metrics=Join-Path $OutputDirectory "run-$run.json"
    $stdout=Join-Path $OutputDirectory "run-$run-stdout.txt"
    $stderr=Join-Path $OutputDirectory "run-$run-stderr.txt"
    $arguments=@('--source-a',$sourceA,'--source-b',$sourceB,'--metrics',$metrics,
        '--warmup-seconds',[string]$WarmupSeconds,'--measure-seconds',[string]$MeasureSeconds,
        '--seed','20260824','--seek-count','1000','--display-timeout-ms','2000',
        '--gpu-completion','fence','--mode','playback','--formal-preflight',
        '--vblank-observer','--presentation-opportunity-ring','--native-present-hook','on')
    $process=Start-Process -FilePath $Executable -ArgumentList $arguments -PassThru `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    if(-not$process.WaitForExit($TimeoutSeconds*1000)){
        $process.Kill($true);$process.WaitForExit();throw "W2-B1 live run $run がtimeoutしました"
    }
    if($process.ExitCode-ne0-or-not(Test-Path -LiteralPath $metrics)){
        throw "W2-B1 live run $run が失敗しました (exit=$($process.ExitCode))"
    }
    & pwsh -NoProfile -File $checker -Json $metrics -SourceRoot $repo -RequireFormalMode
    if($LASTEXITCODE-ne0){throw "W2-B1 live run $run のtransport checkerが失敗しました"}
    $raw=Get-Content -LiteralPath $metrics -Raw -Encoding utf8|ConvertFrom-Json
    $transport=$raw.native_present_hook.intent_identity_transport
    $results+=[ordered]@{
        run=$run;metrics="run-$run.json";metrics_sha256=Hash $metrics
        process_exit_code=$process.ExitCode;abi_version=[int]$transport.abi_version
        qt_abi_version_observed=[int]$transport.qt_abi_version_observed
        layout_handshake_accepted=[bool]$transport.layout_handshake_accepted
        layout_signature=[string]$transport.layout_signature
        record_count=[int]$transport.record_count;formal_mode=[bool]$transport.formal_mode
        transport_exact=[bool]$transport.transport_exact;verdict=[string]$transport.verdict
    }
}
$summary=[ordered]@{
    schema='mvm-p2-d5-2-w2-b1-live-1';stage='P2-D5-2-W2-B1'
    shadow_only=$true;performance_evaluated=$false
    present_event_connected=$false;final_state_connected=$false;displayed_qpc_connected=$false
    runs=$Runs;warmup_seconds=$WarmupSeconds;measure_seconds=$MeasureSeconds
    executable_sha256=Hash $Executable;qt_gui_sha256=Hash $qtGui;qt_quick_sha256=Hash $qtQuick
    matrix_pass=$true;verdict='INTENT_IDENTITY_ABI_V4_TRANSPORT_EXACT';results=$results
}
$summary|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $OutputDirectory 'w2-b1-live-summary.json') -Encoding utf8
Write-Host "P2-D5-2 W2-B1 live: PASS ($Runs/$Runs) INTENT_IDENTITY_ABI_V4_TRANSPORT_EXACT"
