[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateRange(1,200)][int]$Iterations=20,
    [ValidateSet('DISABLED','CONTROL','TARGET_RHIITEM_PIXEL_TOGGLE')]
    [string]$DirtyPropagationMode='TARGET_RHIITEM_PIXEL_TOGGLE',
    [ValidateRange(1,60)][int]$WarmupSeconds=2,
    [ValidateRange(1,60)][int]$MeasureSeconds=2,
    [ValidateRange(20,600)][int]$TimeoutSeconds=90,
    [string]$Executable=(Join-Path (Split-Path -Parent $PSScriptRoot) 'build\ucrt64-release\bin\mvm_compositor_spike.exe'),
    [string]$PatchedQtBin=(Join-Path (Split-Path -Parent $PSScriptRoot) 'build\qtbase-c0\bin'),
    [string]$PatchedQtQuickBin=(Join-Path (Split-Path -Parent $PSScriptRoot) 'build\qtquick-t2-runtime')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
# F3-C3-A3-T2-B startup configuration raceのpre-matrix gate。
# ETWを使わないためadmin不要。measurementは最短で、起動経路だけを反復検査する。
$repo=Split-Path -Parent $PSScriptRoot
$runWrapper=Join-Path $PSScriptRoot 'invoke-p2-c0-native-run.ps1'
$sourceA=Join-Path $repo 'tests\assets\benchmark\v1080p60_h264.mp4'
$sourceB=Join-Path $repo 'tests\assets\benchmark\v1080p60_hevc.mp4'
foreach($path in @($runWrapper,$sourceA,$sourceB,$Executable,$PatchedQtBin)){
    if(-not(Test-Path -LiteralPath $path)){throw "startup smoke必須pathがありません: $path"}
}
if($DirtyPropagationMode-ne'DISABLED'-and-not(Test-Path -LiteralPath $PatchedQtQuickBin)){
    throw "T2 patched QtQuick binがありません: $PatchedQtQuickBin"
}
if(Test-Path -LiteralPath $OutputDirectory){throw "既存smoke artifactを上書きしません: $OutputDirectory"}
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
$Executable=(Resolve-Path -LiteralPath $Executable).Path
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
$expectMarker=$DirtyPropagationMode-eq'TARGET_RHIITEM_PIXEL_TOGGLE'
$results=@()
$failures=0
for($iteration=1;$iteration-le$Iterations;++$iteration){
    $runDirectory=Join-Path $OutputDirectory ('iteration-{0:d3}' -f $iteration)
    New-Item -ItemType Directory -Path $runDirectory|Out-Null
    $metrics=Join-Path $runDirectory 'app.json'
    # markerの実発行はnative present hookのdirty propagation stageでしか観測できないため
    # hook ONで起動する。ETWは使わないのでadminは不要。
    $arguments=@('-NoProfile','-File',$runWrapper,'-HookMode','on','-Executable',$Executable,
        '-PatchedQtBin',$PatchedQtBin,'-SourceA',$sourceA,'-SourceB',$sourceB,'-Metrics',$metrics,
        '-WarmupSeconds',[string]$WarmupSeconds,'-MeasureSeconds',[string]$MeasureSeconds,
        '-SubmissionMode','CONTROL','-DirtyPropagationMode',$DirtyPropagationMode)
    if($DirtyPropagationMode-ne'DISABLED'){$arguments+=@('-PatchedQtQuickBin',$PatchedQtQuickBin)}
    $process=Start-Process -FilePath 'pwsh' -ArgumentList $arguments -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $runDirectory 'stdout.txt') `
        -RedirectStandardError (Join-Path $runDirectory 'stderr.txt')
    if(-not$process.WaitForExit($TimeoutSeconds*1000)){
        $process.Kill($true);$process.WaitForExit()
        $exitCode=124
    }else{$exitCode=$process.ExitCode}
    $markerCount=-1
    $crash=$exitCode-eq-1073741819
    $ringExact=$false
    if($exitCode-eq0-and(Test-Path -LiteralPath $metrics)){
        $raw=Get-Content -LiteralPath $metrics -Raw -Encoding utf8|ConvertFrom-Json
        $dirty=$raw.native_present_hook.dirty_propagation
        $markerCount=[int]$dirty.stage_counts.target_pixel_toggle
        $ringExact=[int64]$dirty.overflow_count-eq0-and[int64]$dirty.duplicate_stage_count-eq0
    }
    $ok=$exitCode-eq0-and$ringExact-and(-not$expectMarker-or$markerCount-gt0)
    if(-not$ok){$failures++}
    $results+=[ordered]@{iteration=$iteration;exit_code=$exitCode;access_violation=$crash;
        target_pixel_toggle=$markerCount;dirty_ring_exact=$ringExact;ok=$ok}
    Write-Host ("startup smoke {0}/{1}: exit={2} marker={3} ok={4}" -f $iteration,$Iterations,$exitCode,$markerCount,$ok)
}
$status=if($failures-eq0){'PASS'}else{'FAIL'}
$proof=[ordered]@{
    schema='mvm-p2-c3-a3-t2-startup-smoke-1';status=$status;authority='diagnostic_only'
    dirty_propagation_mode=$DirtyPropagationMode;iterations=$Iterations
    warmup_seconds=$WarmupSeconds;measure_seconds=$MeasureSeconds
    startup_failure_count=$failures
    access_violation_count=@($results|Where-Object{$_.access_violation}).Count
    executable_sha256=Hash $Executable;runs=$results
    formal_counter_authority_changed=$false;formal_drop_threshold_changed=$false
    production_scheduler_changed=$false
}
$proofPath=Join-Path $OutputDirectory 'startup-smoke-proof.json'
$proof|ConvertTo-Json -Depth 6|Set-Content -LiteralPath $proofPath -Encoding utf8
if($failures-ne0){throw "startup smokeが $failures 件失敗しました: $proofPath"}
Write-Host "F3-C3-A3-T2 startup smoke: PASS iterations=$Iterations ($proofPath)"
