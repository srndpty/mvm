[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateRange(1,10)][int]$Runs=1,
    [ValidateRange(1,60)][int]$WarmupSeconds=2,
    [ValidateRange(1,60)][int]$MeasureSeconds=20,
    [ValidateRange(30,600)][int]$TimeoutSeconds=180
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

# P2-D5-2 B3-I6B-V deterministic fatal publication runtime vector。
#
# **diagnostic-only。canonical W3とは別のworkloadであり、canonical metricを生成しない。**
#
# legacy mappingは target(i) = floor(i * sourceFps * refreshDen / (sourceFpsDen * refreshNum)) で
# required set [0, 60*T) を消費する。fatal ordinal は概ね T*refresh であり、
# planned window end より前に到達するかどうかは「issuance rate > display refresh rate」だけで決まる。
# 本vectorはvsync待ちを外してissuance rateをrefresh rateより高くすることでこれを決定的にする。
#
# physical VBlank / QPC / DWM refresh authorityは一切偽造しない。presentation authorityは
# 通常runと同じくDWM composition clockから取得する。変更するのはswap cadence (workload) だけである。
# canonical fixture / canonical required set / threshold / denominator / production semanticsは変更しない。

$repo=Split-Path -Parent $PSScriptRoot
$executable=Join-Path $repo 'build\ucrt64-release\bin\mvm_compositor_spike.exe'
$patchedQtBin=Join-Path $repo 'build\qtbase-c0\bin'
$patchedQtQuickBin=Join-Path $repo 'build\qtquick-t2-runtime'
$sourceA=Join-Path $repo 'tests\assets\benchmark\v1080p60_h264.mp4'
$sourceB=Join-Path $repo 'tests\assets\benchmark\v1080p60_hevc.mp4'
foreach($path in @($executable,$patchedQtBin,$patchedQtQuickBin,$sourceA,$sourceB)){
    if(-not(Test-Path -LiteralPath $path)){throw "I6B-V必須pathがありません: $path"}
}
if($Runs-ne1){throw 'I6B-V vectorはexactly 1 runである'}
if(-not(Test-Path -LiteralPath $OutputDirectory)){New-Item -ItemType Directory -Path $OutputDirectory|Out-Null}
$runDirectory=Join-Path $OutputDirectory 'run-1'
if(-not(Test-Path -LiteralPath $runDirectory)){New-Item -ItemType Directory -Path $runDirectory|Out-Null}
$metrics=Join-Path $runDirectory 'traced-app.json'

$env:MVM_P2_C3_SUBMISSION_MODE='CONTROL'
$env:QT_D3D_MAX_FRAME_LATENCY='2'
# vsync待ちを外し、issuance rateをdisplay refresh rateより高くする。
# これはworkload cadenceの変更であり、authorityの偽造ではない。
$env:QSG_NO_VSYNC='1'
$env:PATH="$patchedQtQuickBin;$patchedQtBin;C:\msys64\ucrt64\bin;$env:PATH"
$env:QT_PLUGIN_PATH=(Join-Path (Split-Path -Parent $patchedQtBin) 'plugins')
$env:QML_IMPORT_PATH="$((Join-Path (Split-Path -Parent $patchedQtQuickBin) 'qml'));C:\msys64\ucrt64\share\qt6\qml"

$arguments=@('--source-a',$sourceA,'--source-b',$sourceB,'--metrics',$metrics,
    '--warmup-seconds',[string]$WarmupSeconds,'--measure-seconds',[string]$MeasureSeconds,
    '--seed','20260808','--seek-count','1000','--display-timeout-ms','2000',
    '--gpu-completion','fence','--mode','playback','--vblank-observer',
    '--presentation-opportunity-ring','--native-present-hook','on','--formal-preflight')
$startInfo=[Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName=$executable
$startInfo.UseShellExecute=$false
foreach($argument in $arguments){$startInfo.ArgumentList.Add($argument)}
$startInfo.RedirectStandardOutput=$true
$startInfo.RedirectStandardError=$true
$process=[Diagnostics.Process]::Start($startInfo)
if($null-eq$process){throw 'I6B-V compositor processを起動できませんでした'}
$stdout=$process.StandardOutput.ReadToEndAsync()
$stderr=$process.StandardError.ReadToEndAsync()
if(-not$process.WaitForExit($TimeoutSeconds*1000)){
    $process.Kill($true)
    throw "I6B-V vectorがtimeoutしました: ${TimeoutSeconds}s"
}
Set-Content -LiteralPath (Join-Path $runDirectory 'app-stdout.txt') -Value $stdout.Result -Encoding utf8
Set-Content -LiteralPath (Join-Path $runDirectory 'app-stderr.txt') -Value $stderr.Result -Encoding utf8
$exitCode=$process.ExitCode
Write-Host ("P2-D5-2 B3-I6B-V vector: exit={0} metrics={1}" -f $exitCode,$metrics)
if($exitCode-ne0){
    throw "I6B-V vectorがPROTOCOL_FATALで終了しました (exit=$exitCode)"
}
Write-Host 'P2-D5-2 B3-I6B-V vector: 期待したPROTOCOL_FATALが発生しませんでした'
