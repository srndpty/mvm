[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateRange(1,10)][int]$Runs=3,
    [ValidateRange(1,300)][int]$WarmupSeconds=5,
    [ValidateRange(1,300)][int]$MeasureSeconds=60,
    [ValidateRange(10,600)][int]$TimeoutSeconds=180,
    [string]$Executable=(Join-Path (Split-Path -Parent $PSScriptRoot) 'build\ucrt64-release\bin\mvm_compositor_spike.exe')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
$repo=Split-Path -Parent $PSScriptRoot
$sourceA=Join-Path $repo 'tests\assets\benchmark\v1080p60_h264.mp4'
$sourceB=Join-Path $repo 'tests\assets\benchmark\v1080p60_hevc.mp4'
$checker=Join-Path $PSScriptRoot 'check-p2-b1-shadow.ps1'
$runWrapper=Join-Path $PSScriptRoot 'invoke-p2-b1-shadow-run.ps1'
foreach($path in @($Executable,$sourceA,$sourceB,$checker,$runWrapper)){if(-not(Test-Path -LiteralPath $path)){throw "B1 shadow必須fileがありません: $path"}}
if(Test-Path -LiteralPath $OutputDirectory){throw "既存B1 shadow artifactを上書きしません: $OutputDirectory"}
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
$Executable=(Resolve-Path -LiteralPath $Executable).Path
$env:PATH="C:\msys64\ucrt64\bin;$env:PATH"
$results=@()
$matrixPass=$true
$failure=''
for($run=1;$run-le$Runs;++$run){
    $metrics=Join-Path $OutputDirectory "run-$run.json"
    $stdout=Join-Path $OutputDirectory "run-$run-stdout.txt"
    $stderr=Join-Path $OutputDirectory "run-$run-stderr.txt"
    $arguments=@('-NoProfile','-File',$runWrapper,'-Executable',$Executable,
        '-SourceA',$sourceA,'-SourceB',$sourceB,'-Metrics',$metrics,
        '-WarmupSeconds',[string]$WarmupSeconds,'-MeasureSeconds',[string]$MeasureSeconds)
    $process=Start-Process -FilePath 'pwsh' -ArgumentList $arguments -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    if(-not$process.WaitForExit($TimeoutSeconds*1000)){$process.Kill($true);$process.WaitForExit();throw "B1 shadow run $run がtimeoutしました"}
    if(-not(Test-Path -LiteralPath $metrics)){throw "B1 shadow run $run がmetricsを生成しませんでした (exit=$($process.ExitCode))"}
    & pwsh -NoProfile -File $checker -Json $metrics -ProcessExitCode $process.ExitCode
    $checkerExit=$LASTEXITCODE
    $raw=Get-Content -LiteralPath $metrics -Raw -Encoding utf8|ConvertFrom-Json
    $mapper=$raw.presentation_opportunity.incremental_mapper_shadow
    $results+=[ordered]@{
        run=$run;metrics="run-$run.json";metrics_sha256=Hash $metrics
        process_exit_code=$process.ExitCode;checker_exit_code=$checkerExit
        mapper_pass=[bool]$mapper.mapper_pass;mapper_error=[string]$mapper.mapper_error
        final_solution_class=[string]$mapper.final_solution_class
        swap_count=[int64]$mapper.observed_swap_count
        lost_physical_opportunity_count=[int64]$mapper.lost_physical_opportunity_count
        displayed_unique_source_frames=[int64]$mapper.displayed_unique_source_frames
        source_frame_gap_drops=[int64]$mapper.source_frame_gap_drops
        tail_source_frame_drops=[int64]$mapper.tail_source_frame_drops
    }
    if($process.ExitCode-ne0-or$checkerExit-ne0-or$mapper.mapper_pass-ne$true){
        $matrixPass=$false
        $failure="run $run がfail-closedしました (process=$($process.ExitCode) checker=$checkerExit mapper=$($mapper.mapper_error))"
        break
    }
}
$summary=[ordered]@{
    schema='mvm-p2-b1-shadow-matrix-1'
    b1_shadow_status=$(if($matrixPass){'PASS'}else{'FAIL'})
    authority='diagnostic_only';requested_run_count=$Runs;completed_run_count=$results.Count
    stop_on_first_failure=$true;failure=$failure
    warmup_seconds=$WarmupSeconds;measure_seconds=$MeasureSeconds
    executable_sha256=Hash $Executable;source_a_sha256=Hash $sourceA;source_b_sha256=Hash $sourceB
    formal_counter_authority_changed=$false;runs=$results
}
$summary|ConvertTo-Json -Depth 10|Set-Content -LiteralPath (Join-Path $OutputDirectory 'summary.json') -Encoding utf8
$manifest=Get-ChildItem -LiteralPath $OutputDirectory -File|Sort-Object Name|ForEach-Object{"$(Hash $_.FullName)  $($_.Name)"}
$manifest|Set-Content -LiteralPath (Join-Path $OutputDirectory 'manifest.sha256') -Encoding ascii
if(-not$matrixPass){throw "B1 shadow matrixはFAILです: $failure"}
Write-Host "B1 shadow matrix: PASS ($Runs/$Runs)"
