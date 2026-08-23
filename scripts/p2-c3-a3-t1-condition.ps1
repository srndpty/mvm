[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [Parameter(Mandatory=$true)]
    [ValidateSet('VISIBLE_UNOCCLUDED','FULLY_OCCLUDED','VISIBLE_UNOCCLUDED_FORCE_DIRTY')]
    [string]$Mode,
    [ValidateRange(12,300)][int]$WarmupSeconds=12,
    [ValidateRange(1,300)][int]$MeasureSeconds=15,
    [ValidateRange(30,600)][int]$TimeoutSeconds=180,
    [string]$Controller=(Join-Path (Split-Path -Parent $PSScriptRoot) 'build\ucrt64-release\bin\mvm_p2_window_state_controller.exe')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$canonicalRunner=Join-Path $PSScriptRoot 'p2-c0-native-etw.ps1'
$finalizer=Join-Path $PSScriptRoot 'finalize-p2-c3-a3-t1-condition.ps1'
$identity=[Security.Principal.WindowsIdentity]::GetCurrent()
$principal=[Security.Principal.WindowsPrincipal]::new($identity)
if(-not$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'F3-C3-A3-T1採取には管理者権限が必要です'}
foreach($path in @($canonicalRunner,$finalizer,$Controller)){
    if(-not(Test-Path -LiteralPath $path)){throw "F3-C3-A3-T1必須pathがありません: $path"}
}
if(Test-Path -LiteralPath $OutputDirectory){throw "既存F3-C3-A3-T1 condition artifactを上書きしません: $OutputDirectory"}
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
$canonical=Join-Path $OutputDirectory 'canonical'
$runnerArguments=@('-NoProfile','-File',$canonicalRunner,'-OutputDirectory',$canonical,
    '-AcquisitionMode','CanonicalPresentMonLive','-SubmissionMode','CONTROL',
    '-WarmupSeconds',[string]$WarmupSeconds,'-MeasureSeconds',[string]$MeasureSeconds,
    '-TimeoutSeconds',[string]$TimeoutSeconds)
$runnerProcess=Start-Process -FilePath 'pwsh' -ArgumentList $runnerArguments -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput (Join-Path $OutputDirectory 'canonical-runner-stdout.txt') `
    -RedirectStandardError (Join-Path $OutputDirectory 'canonical-runner-stderr.txt')
$controllerProcess=$null;$controllerExit=$null
$pidFile=Join-Path $canonical 'target.pid';$readyFile=Join-Path $OutputDirectory 'window-state.ready'
$stopFile=Join-Path $OutputDirectory 'window-state.stop';$stateJson=Join-Path $OutputDirectory 'window-state-raw.json'
try{
    $pidDeadline=[DateTime]::UtcNow.AddSeconds(3*($WarmupSeconds+$MeasureSeconds)+60)
    while(-not(Test-Path -LiteralPath $pidFile)){
        if($runnerProcess.HasExited){throw "canonical runnerがtarget PID取得前に終了しました: $($runnerProcess.ExitCode)"}
        if([DateTime]::UtcNow-ge$pidDeadline){throw 'target PID取得がtimeoutしました'}
        Start-Sleep -Milliseconds 20
    }
    $targetPid=[long](Get-Content -LiteralPath $pidFile -Raw -Encoding ascii).Trim()
    if($targetPid-le0){throw "target PIDが不正です: $targetPid"}
    $env:PATH="C:\msys64\ucrt64\bin;$env:PATH"
    $controllerArguments=@('--process-id',[string]$targetPid,'--mode',$Mode,'--output',$stateJson,
        '--ready-file',$readyFile,'--stop-file',$stopFile)
    $controllerProcess=Start-Process -FilePath $Controller -ArgumentList $controllerArguments -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $OutputDirectory 'window-state-stdout.txt') `
        -RedirectStandardError (Join-Path $OutputDirectory 'window-state-stderr.txt')
    $readyDeadline=[DateTime]::UtcNow.AddSeconds($WarmupSeconds+3)
    while(-not(Test-Path -LiteralPath $readyFile)){
        if($controllerProcess.HasExited){throw "window-state controllerがready前に終了しました: $($controllerProcess.ExitCode)"}
        if([DateTime]::UtcNow-ge$readyDeadline){throw 'window-state controller readyがwarmup内に成立しません'}
        Start-Sleep -Milliseconds 10
    }
    $totalWaitSeconds=3*$TimeoutSeconds+120
    if(-not$runnerProcess.WaitForExit($totalWaitSeconds*1000)){
        $runnerProcess.Kill($true);$runnerProcess.WaitForExit();throw 'canonical runner全体がtimeoutしました'
    }
}finally{
    'STOP'|Set-Content -LiteralPath $stopFile -Encoding ascii
    if($null-ne$controllerProcess){
        if(-not$controllerProcess.WaitForExit(15000)){$controllerProcess.Kill($true);$controllerProcess.WaitForExit()}
        $controllerExit=$controllerProcess.ExitCode
    }
    if(-not$runnerProcess.HasExited){$runnerProcess.Kill($true);$runnerProcess.WaitForExit()}
}
if($runnerProcess.ExitCode-ne0){throw "canonical runnerが失敗しました: $($runnerProcess.ExitCode)"}
if($controllerExit-ne0-or-not(Test-Path -LiteralPath $stateJson)){throw "window-state controllerが失敗しました: $controllerExit"}
& pwsh -NoProfile -File $finalizer -OutputDirectory $OutputDirectory -Mode $Mode `
    -WarmupSeconds $WarmupSeconds -MeasureSeconds $MeasureSeconds -Controller $Controller
if($LASTEXITCODE-ne0){throw 'T1 condition finalizeが失敗しました'}
Write-Host "F3-C3-A3-T1 condition run: PASS mode=$Mode ($OutputDirectory)"
