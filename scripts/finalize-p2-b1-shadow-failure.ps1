param(
    [Parameter(Mandatory=$true)][string]$Directory,
    [ValidateRange(1,10)][int]$RequestedRuns=3,
    [ValidateRange(1,300)][int]$WarmupSeconds=5,
    [ValidateRange(1,300)][int]$MeasureSeconds=60
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
$Directory=(Resolve-Path -LiteralPath $Directory).Path
$summaryPath=Join-Path $Directory 'summary.json';$manifestPath=Join-Path $Directory 'manifest.sha256'
if((Test-Path -LiteralPath $summaryPath)-or(Test-Path -LiteralPath $manifestPath)){throw '既存summary/manifestを上書きしません'}
$metricsFiles=@(Get-ChildItem -LiteralPath $Directory -Filter 'run-*.json'|Sort-Object Name)
if($metricsFiles.Count-eq0){throw 'finalizeするrun JSONがありません'}
$checker=Join-Path $PSScriptRoot 'check-p2-b1-shadow-failure.ps1';$runs=@()
foreach($metrics in $metricsFiles){
    $raw=Get-Content -LiteralPath $metrics.FullName -Raw -Encoding utf8|ConvertFrom-Json
    & pwsh -NoProfile -File $checker -Json $metrics.FullName -ProcessExitCode ([int]$raw.process_exit_code)
    if($LASTEXITCODE-ne0){throw "期待FAIL検査が不成立です: $($metrics.Name)"}
    $mapper=$raw.presentation_opportunity.incremental_mapper_shadow
    $runs+=[ordered]@{
        run=$runs.Count+1;metrics=$metrics.Name;metrics_sha256=Hash $metrics.FullName
        process_exit_code=[int]$raw.process_exit_code;mapper_pass=[bool]$mapper.mapper_pass
        mapper_error=[string]$mapper.mapper_error;final_solution_class=[string]$mapper.final_solution_class
        observed_swap_count=[int64]$mapper.observed_swap_count
        closed_record_count=[int64]$mapper.closed_record_count
        commit_watermark=[int64]$mapper.commit_watermark
    }
}
[ordered]@{
    schema='mvm-p2-b1-shadow-matrix-1';b1_shadow_status='FAIL';authority='diagnostic_only'
    requested_run_count=$RequestedRuns;completed_run_count=$runs.Count;stop_on_first_failure=$true
    failure='NO_SOLUTION_INJECTIVE_CAPACITY_EXHAUSTED';warmup_seconds=$WarmupSeconds
    measure_seconds=$MeasureSeconds;formal_counter_authority_changed=$false;runs=$runs
}|ConvertTo-Json -Depth 10|Set-Content -LiteralPath $summaryPath -Encoding utf8
$manifest=Get-ChildItem -LiteralPath $Directory -File |
    Where-Object { $_.FullName -ne $manifestPath } |
    Sort-Object Name |
    ForEach-Object { "$(Hash $_.FullName)  $($_.Name)" }
$manifest|Set-Content -LiteralPath $manifestPath -Encoding ascii
Write-Host "B1 shadow FAIL artifactを固定しました: $Directory"
