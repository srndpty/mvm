[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$Executable=(Join-Path (Split-Path -Parent $PSScriptRoot) 'build\ucrt64-release\bin\mvm_compositor_spike.exe'),
    [string]$Decoder=(Join-Path (Split-Path -Parent $PSScriptRoot) 'build\p2-etw-decoder\mvm_present_history_decoder.exe'),
    [string]$PatchedQtBin=(Join-Path (Split-Path -Parent $PSScriptRoot) 'build\qtbase-c0\bin'),
    [ValidateSet('CanonicalPresentMonLive','TargetedLive','Wpr')]
    [string]$AcquisitionMode='CanonicalPresentMonLive',
    [ValidateRange(1,300)][int]$WarmupSeconds=5,
    [ValidateRange(1,300)][int]$MeasureSeconds=15,
    [ValidateRange(30,600)][int]$TimeoutSeconds=180
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
$repo=Split-Path -Parent $PSScriptRoot
$sourceA=Join-Path $repo 'tests\assets\benchmark\v1080p60_h264.mp4'
$sourceB=Join-Path $repo 'tests\assets\benchmark\v1080p60_hevc.mp4'
$nativeChecker=Join-Path $PSScriptRoot 'check-p2-c0-native-hook.ps1'
$etwChecker=Join-Path $PSScriptRoot 'check-p2-c0-native-etw.ps1'
$runWrapper=Join-Path $PSScriptRoot 'invoke-p2-c0-native-run.ps1'
$provenance=Join-Path $repo 'build\qtbase-c0\mvm-c0-provenance.json'
$patch=Join-Path $repo 'qt-patches\qtbase-6.11.1\0001-mvm-native-present-hook.patch'
$qtGui=Join-Path $PatchedQtBin 'Qt6Gui.dll';$qtCore=Join-Path $PatchedQtBin 'Qt6Core.dll'
$qtPluginPath=Join-Path (Split-Path -Parent $PatchedQtBin) 'plugins'
$identity=[Security.Principal.WindowsIdentity]::GetCurrent()
$principal=[Security.Principal.WindowsPrincipal]::new($identity)
if(-not$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){
    throw 'F3-C0 ETW採取には管理者権限が必要です。昇格したPowerShellから実行してください'
}
foreach($path in @($Executable,$Decoder,$PatchedQtBin,$sourceA,$sourceB,$nativeChecker,$etwChecker,$runWrapper,
                    $provenance,$patch,$qtGui,$qtCore,$qtPluginPath)){
    if(-not(Test-Path -LiteralPath $path)){throw "F3-C0必須pathがありません: $path"}
}
if(Test-Path -LiteralPath $OutputDirectory){throw "既存C0 artifactを上書きしません: $OutputDirectory"}
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
$Executable=(Resolve-Path -LiteralPath $Executable).Path;$Decoder=(Resolve-Path -LiteralPath $Decoder).Path
$PatchedQtBin=(Resolve-Path -LiteralPath $PatchedQtBin).Path
Remove-Item Env:QSG_NO_VSYNC -ErrorAction SilentlyContinue
function Start-Compositor([string]$Mode,[string]$Metrics,[string]$Stdout,[string]$Stderr,
                          [int]$RunWarmup,[int]$RunMeasure,[string]$PidFile=''){
    # Start-Process -Environment は、削除した QSG_NO_VSYNC を空文字で子へ再構成する場合がある。
    # 実行用ラッパー内で環境を確定し、実アプリの PID は metrics から取得する。
    $arguments=@('-NoProfile','-File',$runWrapper,'-HookMode',$Mode,
        '-Executable',$Executable,'-PatchedQtBin',$PatchedQtBin,
        '-SourceA',$sourceA,'-SourceB',$sourceB,'-Metrics',$Metrics,
        '-WarmupSeconds',[string]$RunWarmup,'-MeasureSeconds',[string]$RunMeasure)
    if(-not[string]::IsNullOrWhiteSpace($PidFile)){$arguments+=@('-PidFile',$PidFile)}
    return Start-Process -FilePath 'pwsh' -ArgumentList $arguments `
        -PassThru -WindowStyle Hidden -RedirectStandardOutput $Stdout -RedirectStandardError $Stderr
}
function Wait-Compositor($Process,[int]$RunTimeout){
    $process=$Process
    if(-not$process.WaitForExit($RunTimeout*1000)){
        $process.Kill($true);$process.WaitForExit()
        return [ordered]@{process=$process;exit_code=124}
    }
    return [ordered]@{process=$process;exit_code=$process.ExitCode}
}
function Invoke-Compositor([string]$Mode,[string]$Metrics,[string]$Stdout,[string]$Stderr,
                           [int]$RunWarmup,[int]$RunMeasure,[int]$RunTimeout){
    $process=Start-Compositor $Mode $Metrics $Stdout $Stderr $RunWarmup $RunMeasure
    return Wait-Compositor $process $RunTimeout
}
function Wait-ForFile([string]$Path,$Process,[int]$Seconds,[string]$Description){
    $deadline=[DateTime]::UtcNow.AddSeconds($Seconds)
    while(-not(Test-Path -LiteralPath $Path)){
        if($Process.HasExited){throw "$Description より前にprocessが終了しました: $($Process.ExitCode)"}
        if([DateTime]::UtcNow-ge$deadline){throw "$Description がtimeoutしました"}
        Start-Sleep -Milliseconds 20
    }
}
function Rate([string]$Json){
    $raw=Get-Content -LiteralPath $Json -Raw -Encoding utf8|ConvertFrom-Json
    $elapsed=([double][int64]$raw.presentation_opportunity.measurement_end_qpc_exclusive-
              [double][int64]$raw.presentation_opportunity.measurement_start_qpc)/
             [double][int64]$raw.presentation_opportunity.qpc_frequency
    if($elapsed-le0){throw "measurement elapsedが不正です: $Json"}
    return [double][int64]$raw.presentation_opportunity.swap_record_count/$elapsed
}
$offJson=Join-Path $OutputDirectory 'hook-off-app.json'
$off=Invoke-Compositor 'off' $offJson (Join-Path $OutputDirectory 'hook-off-stdout.txt') `
    (Join-Path $OutputDirectory 'hook-off-stderr.txt') $WarmupSeconds $MeasureSeconds $TimeoutSeconds
if($off.exit_code-ne0-or-not(Test-Path -LiteralPath $offJson)){throw "hook OFF controlが失敗しました: $($off.exit_code)"}
& pwsh -NoProfile -File $nativeChecker -Json $offJson -HookMode off -ProcessExitCode $off.exit_code
if($LASTEXITCODE-ne0){throw 'hook OFF control contractが不成立です'}
$onJson=Join-Path $OutputDirectory 'hook-on-app.json'
$on=Invoke-Compositor 'on' $onJson (Join-Path $OutputDirectory 'hook-on-stdout.txt') `
    (Join-Path $OutputDirectory 'hook-on-stderr.txt') $WarmupSeconds $MeasureSeconds $TimeoutSeconds
if($on.exit_code-ne0-or-not(Test-Path -LiteralPath $onJson)){throw "hook ON controlが失敗しました: $($on.exit_code)"}
& pwsh -NoProfile -File $nativeChecker -Json $onJson -HookMode on -ProcessExitCode $on.exit_code
if($LASTEXITCODE-ne0){throw 'hook ON control contractが不成立です'}
$offRate=Rate $offJson;$onRate=Rate $onJson;$cadenceRatio=$onRate/$offRate
if($cadenceRatio-lt0.5-or$cadenceRatio-gt1.5){throw "hook ON/OFFでcadenceが大きく変化しました: $cadenceRatio"}
$traceJson=Join-Path $OutputDirectory 'traced-app.json';$etl=Join-Path $OutputDirectory 'trace.etl'
$etwJson=Join-Path $OutputDirectory 'present-history-raw.json';$oracle=Join-Path $OutputDirectory 'oracle.json'
$traceRun=$null;$decoderExit=$null;$targetProcessId=0
if($AcquisitionMode-eq'Wpr'){
    $traceStarted=$false
    try{
        & wpr.exe -start GeneralProfile -start GPU -start DesktopComposition -filemode
        if($LASTEXITCODE-ne0){throw 'WPR startに失敗しました'}
        $traceStarted=$true
        & wpr.exe -marker 'F3-C0 native-present-start'|Out-Null
        $traceRun=Invoke-Compositor 'on' $traceJson (Join-Path $OutputDirectory 'traced-stdout.txt') `
            (Join-Path $OutputDirectory 'traced-stderr.txt') $WarmupSeconds $MeasureSeconds $TimeoutSeconds
    }finally{
        if($traceStarted){
            & wpr.exe -marker 'F3-C0 native-present-end'|Out-Null
            & wpr.exe -stop $etl 'F3-C0 Native D3D11 Present Serial Authority Proof'
            if($LASTEXITCODE-ne0){throw 'WPR stopに失敗しました'}
        }
    }
}else{
    $pidFile=Join-Path $OutputDirectory 'target.pid'
    $readyFile=Join-Path $OutputDirectory 'live-etw.ready'
    $stopFile=Join-Path $OutputDirectory 'live-etw.stop'
    $traceProcess=Start-Compositor 'on' $traceJson (Join-Path $OutputDirectory 'traced-stdout.txt') `
        (Join-Path $OutputDirectory 'traced-stderr.txt') $WarmupSeconds $MeasureSeconds $pidFile
    Wait-ForFile $pidFile $traceProcess 10 'target PID取得'
    $targetProcessId=[int64](Get-Content -LiteralPath $pidFile -Raw -Encoding ascii).Trim()
    if($targetProcessId-le0){throw "target PIDが不正です: $targetProcessId"}
    $decoderArguments=@('--live','--process-id',[string]$targetProcessId,'--ready-file',$readyFile,
        '--stop-file',$stopFile,'--output',$etwJson)
    $liveDecoder=Start-Process -FilePath $Decoder -ArgumentList $decoderArguments -PassThru `
        -WindowStyle Hidden -RedirectStandardOutput (Join-Path $OutputDirectory 'live-etw-stdout.txt') `
        -RedirectStandardError (Join-Path $OutputDirectory 'live-etw-stderr.txt')
    try{
        Wait-ForFile $readyFile $liveDecoder 10 'targeted live ETW ready'
        $traceRun=Wait-Compositor $traceProcess $TimeoutSeconds
        Start-Sleep -Milliseconds 2500
    }finally{
        'STOP'|Set-Content -LiteralPath $stopFile -Encoding ascii
        if(-not$liveDecoder.WaitForExit(30000)){$liveDecoder.Kill($true);$liveDecoder.WaitForExit()}
        $decoderExit=$liveDecoder.ExitCode
        if(-not$traceProcess.HasExited){$traceProcess.Kill($true);$traceProcess.WaitForExit()}
    }
}
if($null-eq$traceRun-or$traceRun.exit_code-ne0-or-not(Test-Path -LiteralPath $traceJson)){
    throw "ETW中のnative Present runが失敗しました: $(if($traceRun){$traceRun.exit_code}else{'未起動'})"
}
$traceRaw=Get-Content -LiteralPath $traceJson -Raw -Encoding utf8|ConvertFrom-Json
if($targetProcessId-eq0){$targetProcessId=[int64]$traceRaw.process_id}
if($targetProcessId-le0){throw "metricsのprocess_idが不正です: $targetProcessId"}
if([int64]$traceRaw.process_id-ne$targetProcessId){throw 'PID fileとmetricsのprocess_idが一致しません'}
if($AcquisitionMode-eq'Wpr'){
    & $Decoder --etl $etl --process-id $targetProcessId --output $etwJson
    $decoderExit=$LASTEXITCODE
}
if($decoderExit-ne0-and$decoderExit-ne5){throw "ETW decodeに失敗しました: $decoderExit"}
if(-not(Test-Path -LiteralPath $etwJson)){throw 'ETW raw JSONがありません'}
$etw=Get-Content -LiteralPath $etwJson -Raw -Encoding utf8|ConvertFrom-Json
$etw.cadence_diagnostic.traced_swaps_per_second=Rate $traceJson
$etw.cadence_diagnostic.baseline_swaps_per_second=$onRate
$etw.cadence_diagnostic.ratio=$etw.cadence_diagnostic.traced_swaps_per_second/$onRate
$etw.cadence_diagnostic.extreme_change=$etw.cadence_diagnostic.ratio-lt0.5-or$etw.cadence_diagnostic.ratio-gt1.5
$etw|ConvertTo-Json -Depth 20|Set-Content -LiteralPath $etwJson -Encoding utf8
& pwsh -NoProfile -File $etwChecker -AppJson $traceJson -EtwJson $etwJson -Output $oracle -ProcessExitCode $traceRun.exit_code
$checkerExit=$LASTEXITCODE
$oracleRaw=if(Test-Path -LiteralPath $oracle){Get-Content -LiteralPath $oracle -Raw -Encoding utf8|ConvertFrom-Json}else{$null}
$qtProvenance=Get-Content -LiteralPath $provenance -Raw -Encoding utf8|ConvertFrom-Json
[ordered]@{
    schema='mvm-p2-c0-native-etw-run-1';authority='diagnostic_only'
    acquisition_mode=$AcquisitionMode
    c0_r2_status=$(if($checkerExit-eq0){'PASS'}else{'FAIL'})
    oracle_status=$(if($oracleRaw){$oracleRaw.oracle_status}else{'INVALID'})
    display_completion_status=$(if($oracleRaw){$oracleRaw.display_completion_status}else{'NOT_EVALUABLE'})
    native_present_alone_status=$(if($oracleRaw){$oracleRaw.native_present_alone_status}else{'NOT_EVALUABLE'})
    exit_reason=$(if($oracleRaw){$oracleRaw.exit_reason}else{'ORACLE_INVALID'})
    formal_counter_authority_changed=$false
    hook_off_rate=$offRate;hook_on_rate=$onRate;hook_on_off_ratio=$cadenceRatio
    traced_rate=$etw.cadence_diagnostic.traced_swaps_per_second
    trace_on_ratio=$etw.cadence_diagnostic.ratio
    target_process_id=$targetProcessId
    decoder_exit_code=$decoderExit;checker_exit_code=$checkerExit
    presented_count=$(if($oracleRaw){$oracleRaw.presented_count}else{$null})
    discarded_count=$(if($oracleRaw){$oracleRaw.discarded_count}else{$null})
    incomplete_unknown_count=$(if($oracleRaw){$oracleRaw.incomplete_unknown_count}else{$null})
    lost_count=$(if($oracleRaw){$oracleRaw.lost_count}else{$null})
    native_present_count=$(if($oracleRaw){$oracleRaw.native_present_count}else{$null})
    identities=[ordered]@{
        executable_sha256=Hash $Executable;decoder_sha256=Hash $Decoder
        source_a_sha256=Hash $sourceA;source_b_sha256=Hash $sourceB
        qt_upstream_commit=$qtProvenance.qt_upstream_commit
        qt_patch_sha256=Hash $patch;qt_gui_dll_sha256=Hash $qtGui;qt_core_dll_sha256=Hash $qtCore
        etl_sha256=$(if(Test-Path -LiteralPath $etl){Hash $etl}else{$null})
        raw_etw_json_sha256=Hash $etwJson
    }
}|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $OutputDirectory 'summary.json') -Encoding utf8
$manifestPath=Join-Path $OutputDirectory 'manifest.sha256'
Get-ChildItem -LiteralPath $OutputDirectory -File|Where-Object{$_.FullName-ne$manifestPath}|Sort-Object Name|ForEach-Object{"$(Hash $_.FullName)  $($_.Name)"}|Set-Content -LiteralPath $manifestPath -Encoding ascii
if($checkerExit-ne0){throw 'F3-C0 native/ETW oracleはFAILです。formalへ昇格しません'}
Write-Host "F3-C0 native Present ETW oracle: PASS ($OutputDirectory)"
