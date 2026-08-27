[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateRange(1,10)][int]$Runs=3,
    [ValidateRange(1,60)][int]$WarmupSeconds=2,
    [ValidateRange(1,60)][int]$MeasureSeconds=5,
    [ValidateRange(30,600)][int]$TimeoutSeconds=120,
    [string]$Executable=(Join-Path (Split-Path -Parent $PSScriptRoot) 'build\ucrt64-release\bin\mvm_compositor_spike.exe'),
    [string]$Decoder=(Join-Path (Split-Path -Parent $PSScriptRoot) 'build\p2-etw-decoder\mvm_present_history_decoder.exe'),
    [string]$PatchedQtBin=(Join-Path (Split-Path -Parent $PSScriptRoot) 'build\qtbase-c0\bin'),
    [string]$PatchedQtQuickBin=(Join-Path (Split-Path -Parent $PSScriptRoot) 'build\qtquick-t2-runtime')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
function Wait-ForFile([string]$Path,$Process,[int]$Seconds,[string]$Description){
    $deadline=[DateTime]::UtcNow.AddSeconds($Seconds)
    while(-not(Test-Path -LiteralPath $Path)){
        if($Process.HasExited){throw "$Description より前にprocessが終了しました: $($Process.ExitCode)"}
        if([DateTime]::UtcNow-ge$deadline){throw "$Description がtimeoutしました"}
        Start-Sleep -Milliseconds 20
    }
}
$repo=Split-Path -Parent $PSScriptRoot
$sourceA=Join-Path $repo 'tests\assets\benchmark\v1080p60_h264.mp4'
$sourceB=Join-Path $repo 'tests\assets\benchmark\v1080p60_hevc.mp4'
$wrapper=Join-Path $PSScriptRoot 'invoke-p2-c0-native-run.ps1'
$checker=Join-Path $PSScriptRoot 'check-p2-d5-2-w2-b2-terminal-shadow.ps1'
$qtGui=Join-Path $PatchedQtBin 'Qt6Gui.dll';$qtQuick=Join-Path $PatchedQtQuickBin 'Qt6Quick.dll'
$identity=[Security.Principal.WindowsIdentity]::GetCurrent()
$principal=[Security.Principal.WindowsPrincipal]::new($identity)
if(-not$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){
    throw 'W2-B2 CanonicalPresentMonLiveには管理者権限が必要です。昇格したPowerShellから実行してください'
}
foreach($path in @($Executable,$Decoder,$PatchedQtBin,$PatchedQtQuickBin,$sourceA,$sourceB,$wrapper,$checker,$qtGui,$qtQuick)){
    if(-not(Test-Path -LiteralPath $path)){throw "W2-B2 live必須pathがありません: $path"}
}
if(Test-Path -LiteralPath $OutputDirectory){throw "既存W2-B2 artifactを上書きしません: $OutputDirectory"}
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
$results=@()
for($run=1;$run-le$Runs;++$run){
    $runDirectory=Join-Path $OutputDirectory "run-$run"
    New-Item -ItemType Directory -Path $runDirectory|Out-Null
    $appJson=Join-Path $runDirectory 'traced-app.json';$etwJson=Join-Path $runDirectory 'present-history-raw.json'
    $ledger=Join-Path $runDirectory 'terminal-shadow.json';$pidFile=Join-Path $runDirectory 'target.pid'
    $readyFile=Join-Path $runDirectory 'live-etw.ready';$stopFile=Join-Path $runDirectory 'live-etw.stop'
    $appArguments=@('-NoProfile','-File',$wrapper,'-HookMode','on','-Executable',$Executable,
        '-PatchedQtBin',$PatchedQtBin,'-PatchedQtQuickBin',$PatchedQtQuickBin,
        '-SourceA',$sourceA,'-SourceB',$sourceB,'-Metrics',$appJson,
        '-WarmupSeconds',[string]$WarmupSeconds,'-MeasureSeconds',[string]$MeasureSeconds,
        '-SubmissionMode','CONTROL','-DirtyPropagationMode','DISABLED','-PidFile',$pidFile,
        '-FormalPreflight')
    $appProcess=Start-Process -FilePath 'pwsh' -ArgumentList $appArguments -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $runDirectory 'app-stdout.txt') `
        -RedirectStandardError (Join-Path $runDirectory 'app-stderr.txt')
    Wait-ForFile $pidFile $appProcess 10 "run $run target PID取得"
    $targetPid=[int64](Get-Content -LiteralPath $pidFile -Raw -Encoding ascii).Trim()
    if($targetPid-le0){throw "run $run target PIDが不正です: $targetPid"}
    $decoderArguments=@('--live','--process-id',[string]$targetPid,'--ready-file',$readyFile,
        '--stop-file',$stopFile,'--output',$etwJson)
    $decoderProcess=Start-Process -FilePath $Decoder -ArgumentList $decoderArguments -PassThru `
        -WindowStyle Hidden -RedirectStandardOutput (Join-Path $runDirectory 'decoder-stdout.txt') `
        -RedirectStandardError (Join-Path $runDirectory 'decoder-stderr.txt')
    try{
        Wait-ForFile $readyFile $decoderProcess 10 "run $run PresentMon ready"
        if(-not$appProcess.WaitForExit($TimeoutSeconds*1000)){
            $appProcess.Kill($true);$appProcess.WaitForExit();throw "W2-B2 live run $run がtimeoutしました"
        }
        Start-Sleep -Milliseconds 2500
    }finally{
        'STOP'|Set-Content -LiteralPath $stopFile -Encoding ascii
        if(-not$decoderProcess.WaitForExit(30000)){$decoderProcess.Kill($true);$decoderProcess.WaitForExit()}
        if(-not$appProcess.HasExited){$appProcess.Kill($true);$appProcess.WaitForExit()}
    }
    if($appProcess.ExitCode-ne0-or-not(Test-Path -LiteralPath $appJson)){
        throw "W2-B2 live run $run appが失敗しました: $($appProcess.ExitCode)"
    }
    if($decoderProcess.ExitCode-ne0-or-not(Test-Path -LiteralPath $etwJson)){
        throw "W2-B2 live run $run PresentMon acquisitionが失敗しました: $($decoderProcess.ExitCode)"
    }
    & pwsh -NoProfile -File $checker -AppJson $appJson -EtwJson $etwJson -Output $ledger -SourceRoot $repo
    if($LASTEXITCODE-ne0){throw "W2-B2 live run $run terminal shadow checkerが失敗しました"}
    $proof=Get-Content -LiteralPath $ledger -Raw -Encoding utf8|ConvertFrom-Json
    $results+=[ordered]@{
        run=$run;native_successful_present_count=[int]$proof.successful_native_present_count
        present_event_count=[int]$proof.present_event_count;exact_join_count=[int]$proof.exact_join_count
        presented_event_count=[int]$proof.presented_event_count;discarded_event_count=[int]$proof.discarded_event_count
        unknown_event_count=[int]$proof.unknown_event_count;terminal_closure_exact=[bool]$proof.terminal_closure_exact
        app_sha256=Hash $appJson;presentmon_raw_sha256=Hash $etwJson;ledger_sha256=Hash $ledger
    }
}
$summary=[ordered]@{
    schema='mvm-p2-d5-2-w2-b2-live-1';stage='P2-D5-2-W2-B2'
    acquisition_mode='CanonicalPresentMonLive';shadow_only=$true
    physical_mapping_connected=$false;performance_accounting_connected=$false
    runs=$Runs;warmup_seconds=$WarmupSeconds;measure_seconds=$MeasureSeconds
    executable_sha256=Hash $Executable;decoder_sha256=Hash $Decoder
    qt_gui_sha256=Hash $qtGui;qt_quick_sha256=Hash $qtQuick
    matrix_pass=$true;verdict='NATIVE_PRESENT_TERMINAL_OUTCOME_EXACT';results=$results
}
$summary|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $OutputDirectory 'w2-b2-live-summary.json') -Encoding utf8
Write-Host "P2-D5-2 W2-B2 live: PASS ($Runs/$Runs) NATIVE_PRESENT_TERMINAL_OUTCOME_EXACT"
