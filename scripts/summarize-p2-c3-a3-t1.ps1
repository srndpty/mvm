[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$MatrixDirectory,
    [Parameter(Mandatory=$true)][string]$Output
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
function Classify($Proof){
    $dwmMax=[long]$Proof.dwm_wide_present_start_gap.max
    $dwmP95=[long]$Proof.dwm_wide_present_start_gap.p95
    $batchMax=[long]$Proof.dependency_batch_size.max
    $batchP95=[long]$Proof.dependency_batch_size.p95
    $parentCount=[long]$Proof.target_attached_parent_count
    if([long]$Proof.dwm_wide_present_start_count-le1-and$parentCount-le1){return 'LARGE_SUPPRESSION'}
    if($dwmMax-ge30-and($batchMax-ge30-or$parentCount-eq0)){return 'LARGE_SUPPRESSION'}
    if($dwmMax-lt30-and$batchMax-lt30-and$dwmP95-le2-and$batchP95-le2){return 'REGULAR'}
    return 'MIXED'
}
$indexPath=Join-Path $MatrixDirectory 'matrix-runs.json'
if(-not(Test-Path -LiteralPath $indexPath)){Fail "matrix indexがありません: $indexPath"}
$index=Get-Content -LiteralPath $indexPath -Raw -Encoding utf8|ConvertFrom-Json
if([string]$index.schema-ne'mvm-p2-c3-a3-t1-matrix-runs-1'-or[string]$index.status-ne'PASS'){Fail 'matrix indexがPASSではありません'}
$expectedModes=@('VISIBLE_UNOCCLUDED','FULLY_OCCLUDED','VISIBLE_UNOCCLUDED_FORCE_DIRTY')
$runs=@($index.runs)
if($runs.Count-ne9){Fail "matrix runが9件ではありません: $($runs.Count)"}
$rows=@()
foreach($run in $runs){
    if([long]$run.set-lt1-or[long]$run.set-gt3-or[long]$run.position-lt1-or[long]$run.position-gt3-or$expectedModes-notcontains[string]$run.mode){Fail 'matrix run identityが不正です'}
    $summaryPath=Join-Path (Join-Path $MatrixDirectory ([string]$run.directory)) 'summary.json'
    if(-not(Test-Path -LiteralPath $summaryPath)){Fail "condition summaryがありません: $summaryPath"}
    $summary=Get-Content -LiteralPath $summaryPath -Raw -Encoding utf8|ConvertFrom-Json
    if([string]$summary.schema-ne'mvm-p2-c3-a3-t1-condition-run-1'-or[string]$summary.status-ne'PASS'-or[string]$summary.mode-ne[string]$run.mode){Fail "condition summaryが不正です: $summaryPath"}
    if([bool]$summary.formal_counter_authority_changed-or[bool]$summary.formal_drop_threshold_changed-or[bool]$summary.production_scheduler_changed){Fail 'T1で禁止されたproduction/formal変更があります'}
    $proof=$summary.proof
    $rows+=[pscustomobject][ordered]@{
        set=[long]$run.set;position=[long]$run.position;mode=[string]$run.mode;classification=Classify $proof
        dwm_present_start_count=[long]$proof.dwm_wide_present_start_count
        dwm_gap_p50=[long]$proof.dwm_wide_present_start_gap.p50;dwm_gap_p95=[long]$proof.dwm_wide_present_start_gap.p95;dwm_gap_max=[long]$proof.dwm_wide_present_start_gap.max
        target_parent_count=[long]$proof.target_attached_parent_count
        target_parent_gap_p95=[long]$proof.target_parent_present_start_gap.p95;target_parent_gap_max=[long]$proof.target_parent_present_start_gap.max
        batch_p50=[long]$proof.dependency_batch_size.p50;batch_p95=[long]$proof.dependency_batch_size.p95;batch_max=[long]$proof.dependency_batch_size.max
        presented=[long]$proof.presented_count;discarded=[long]$proof.discarded_count
        dependent_superseded=[long]$proof.dependent_superseded_count;earlier_superseded=[long]$proof.earlier_superseded_count
    }
}
foreach($set in 1..3){
    $setRows=@($rows|Where-Object{$_.set-eq$set})
    if($setRows.Count-ne3-or@($setRows.position|Sort-Object -Unique).Count-ne3-or@($setRows.mode|Sort-Object -Unique).Count-ne3){Fail "set $set が3条件循環ではありません"}
}
foreach($mode in $expectedModes){
    $modeRows=@($rows|Where-Object{$_.mode-eq$mode})
    if($modeRows.Count-ne3-or@($modeRows.position|Sort-Object -Unique).Count-ne3){Fail "$mode が各positionに1回ずつ配置されていません"}
}
$visible=@($rows|Where-Object mode -eq 'VISIBLE_UNOCCLUDED')
$occluded=@($rows|Where-Object mode -eq 'FULLY_OCCLUDED')
$dirty=@($rows|Where-Object mode -eq 'VISIBLE_UNOCCLUDED_FORCE_DIRTY')
function All-Class($Values,[string]$Class){@($Values|Where-Object classification -ne $Class).Count-eq0}
$verdict=if((All-Class $visible 'REGULAR')-and(All-Class $occluded 'LARGE_SUPPRESSION')){
    'FORMAL_HARNESS_VISIBILITY_DEFECT'
}elseif((All-Class $visible 'LARGE_SUPPRESSION')-and(All-Class $dirty 'REGULAR')){
    'DIRTY_WAKE_SUPPRESSION'
}else{
    $allClasses=@($rows.classification|Sort-Object -Unique)
    if($allClasses.Count-eq1){'VISIBILITY_DIRTY_HYPOTHESIS_REJECTED'}else{'ORDER_TIME_CONFOUND_OR_MIXED'}
}
$byMode=[ordered]@{};foreach($mode in $expectedModes){$byMode[$mode]=@($rows|Where-Object mode -eq $mode|Sort-Object set)}
$byPosition=[ordered]@{};foreach($position in 1..3){$byPosition[[string]$position]=@($rows|Where-Object position -eq $position|Sort-Object set)}
[ordered]@{
    schema='mvm-p2-c3-a3-t1-matrix-proof-1';status='PASS';authority='diagnostic_only';verdict=$verdict
    formal_counter_authority_changed=$false;formal_drop_threshold_changed=$false;production_scheduler_changed=$false
    diagnostic_large_gap_threshold_vblanks=30
    primary_authority='DWM_WIDE_PRESENTSTART_PLUS_DEPENDENCY_BATCH';presented_role='DOWNSTREAM_CONSEQUENCE'
    by_mode=$byMode;by_position=$byPosition
}|ConvertTo-Json -Depth 10|Set-Content -LiteralPath $Output -Encoding utf8
Write-Host "F3-C3-A3-T1 summary: PASS verdict=$verdict"
