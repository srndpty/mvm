[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet('off','on')][string]$HookMode,
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string]$PatchedQtBin,
    [string]$PatchedQtQuickBin,
    [Parameter(Mandatory=$true)][string]$SourceA,
    [Parameter(Mandatory=$true)][string]$SourceB,
    [Parameter(Mandatory=$true)][string]$Metrics,
    [Parameter(Mandatory=$true)][int]$WarmupSeconds,
    [Parameter(Mandatory=$true)][int]$MeasureSeconds,
    [ValidateSet('CONTROL','DWM_FLUSH_AFTER_PRESENT','FRAME_LATENCY_1')]
    [string]$SubmissionMode='CONTROL',
    [ValidateSet('DISABLED','CONTROL','TARGET_RHIITEM_PIXEL_TOGGLE')]
    [string]$DirtyPropagationMode='DISABLED',
    [string]$PidFile,
    [switch]$FormalPreflight
)
$ErrorActionPreference='Stop'
foreach($path in @($Executable,$PatchedQtBin,$SourceA,$SourceB)){
    if(-not(Test-Path -LiteralPath $path)){throw "C0 run必須pathがありません: $path"}
}
if(-not[string]::IsNullOrWhiteSpace($PatchedQtQuickBin)-and-not(Test-Path -LiteralPath $PatchedQtQuickBin)){
    throw "T2 patched QtQuick binがありません: $PatchedQtQuickBin"
}
Remove-Item Env:QSG_NO_VSYNC -ErrorAction SilentlyContinue
$env:MVM_P2_C3_SUBMISSION_MODE=$SubmissionMode
$env:QT_D3D_MAX_FRAME_LATENCY=$(if($SubmissionMode-eq'FRAME_LATENCY_1'){'1'}else{'2'})
$env:PATH="$(if($PatchedQtQuickBin){$PatchedQtQuickBin+';'}else{''})$PatchedQtBin;C:\msys64\ucrt64\bin;$env:PATH"
$env:QT_PLUGIN_PATH=(Join-Path (Split-Path -Parent $PatchedQtBin) 'plugins')
$env:QML_IMPORT_PATH="$(if($PatchedQtQuickBin){(Join-Path (Split-Path -Parent $PatchedQtQuickBin) 'qml')+';'}else{''})C:\msys64\ucrt64\share\qt6\qml"
$arguments=@('--source-a',$SourceA,'--source-b',$SourceB,'--metrics',$Metrics,
    '--warmup-seconds',[string]$WarmupSeconds,'--measure-seconds',[string]$MeasureSeconds,
    '--seed','20260808','--seek-count','1000','--display-timeout-ms','2000',
    '--gpu-completion','fence','--mode','playback','--vblank-observer',
    '--presentation-opportunity-ring','--native-present-hook',$HookMode)
if($FormalPreflight){$arguments+='--formal-preflight'}
if($DirtyPropagationMode-eq'TARGET_RHIITEM_PIXEL_TOGGLE'){
    $arguments+='--target-rhiitem-pixel-toggle'
}
$startInfo=[Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName=$Executable
$startInfo.UseShellExecute=$false
foreach($argument in $arguments){$startInfo.ArgumentList.Add($argument)}
$process=[Diagnostics.Process]::Start($startInfo)
if($null-eq$process){throw 'C0 compositor processを起動できませんでした'}
if(-not[string]::IsNullOrWhiteSpace($PidFile)){
    [string]$process.Id|Set-Content -LiteralPath $PidFile -Encoding ascii
}
$process.WaitForExit()
exit $process.ExitCode
