param(
    [Parameter(Mandatory=$true)][string]$Json,
    [int]$ProcessExitCode=0,
    [string]$VBlankChecker=(Join-Path $PSScriptRoot 'check-p2-vblank-shadow.ps1')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
& pwsh -NoProfile -File $VBlankChecker -Json $Json -ProcessExitCode $ProcessExitCode
if($LASTEXITCODE-ne0){Fail '既存physical VBlank shadow contractが不成立です'}
$raw=Get-Content -LiteralPath $Json -Raw -Encoding utf8|ConvertFrom-Json
$opportunity=$raw.presentation_opportunity;$mapper=$opportunity.incremental_mapper_shadow
if($mapper.enabled-ne$true-or$mapper.shadow_only-ne$true-or$mapper.formal_counter_authority_changed-ne$false){
    Fail 'incremental mapperがshadow-onlyではありません'}
if($mapper.admissibility_relation-ne'VISIBLE_PREFIX: opportunity_start_qpc <= callback_qpc'){
    Fail 'B1 mapper admissibility relationがR4から変化しています'}
if([int]$mapper.sync_interval_precondition-ne1){Fail 'SyncInterval preconditionが1ではありません'}
if($mapper.qt_runtime_version-ne'6.11.1'-or$mapper.qt_source_tag-ne'v6.11.1'-or
   $mapper.qtbase_source_commit-ne'59c81a3c2247b821b9b84b4eb8d939b77e07e276'-or
   $mapper.qtdeclarative_source_commit-ne'a02bed441965ee1f18f856352c7d5ee5ba35d795'-or
   [int]$mapper.requested_swap_interval-ne1-or$mapper.qsg_no_vsync_environment_set-ne$false-or
   $mapper.d3d11_backend_forced-ne$true-or[int]$mapper.present_sync_interval-ne1-or
   [int]$mapper.present_flags-ne0-or$mapper.dxgi_present_restart_used-ne$false-or
   $mapper.tearing_path_used-ne$false){Fail 'Qt D3D11 Present source preconditionが不成立です'}
if($mapper.finalized-ne$true-or$mapper.mapper_pass-ne$true-or$mapper.mapper_error-ne'' -or
   $mapper.final_solution_class-ne'UNIQUE'){Fail 'incremental mapperが一意にfinalizeしていません'}
$samples=@($opportunity.physical_vblank.samples);$swaps=@($opportunity.swap_records)
$records=@($mapper.records);$transitions=@($mapper.transitions)
$measurementStart=[int64]$opportunity.measurement_start_qpc
$measurementEnd=[int64]$opportunity.measurement_end_qpc_exclusive
$origin=-1;$boundary=-1
for($index=0;$index-lt$samples.Count;++$index){
    if([int64]$samples[$index].qpc-le$measurementStart){$origin=$index}
    if($boundary-lt0-and[int64]$samples[$index].qpc-ge$measurementEnd){$boundary=$index}
}
if($origin-lt0-or$boundary-le$origin){Fail 'measurement VBlank domainを独立に閉じられません'}
$opportunities=@($samples[$origin..($boundary-1)])
if($records.Count-ne$swaps.Count-or$records.Count-ne[int]$mapper.observed_swap_count){
    Fail 'mapper/swaps record countが不一致です'}
if([int]$mapper.closed_record_count-ne$swaps.Count-or[int]$mapper.commit_watermark-ne$swaps.Count){
    Fail '全swapがclosed/committedではありません'}

# VISIBLE_PREFIX候補の上限をraw QPCから再計算する。prefix domainでは、
# earliest assignmentとlatest assignmentの一致がexact UNIQUE条件になる。
$upper=@();$cursor=0
for($record=0;$record-lt$swaps.Count;++$record){
    $swapQpc=[int64]$swaps[$record].swap_qpc
    while($cursor+1-lt$opportunities.Count-and[int64]$opportunities[$cursor+1].qpc-le$swapQpc){++$cursor}
    if([int64]$opportunities[$cursor].qpc-gt$swapQpc){Fail "candidateが0件です: record=$record"}
    $upper+=$cursor
    if([int64]$records[$record].candidate_first_opportunity_ordinal-ne[int64]$opportunities[0].ordinal-or
       [int64]$records[$record].candidate_last_opportunity_ordinal-ne[int64]$opportunities[$cursor].ordinal){
        Fail "candidate rangeのproducer集計が不一致です: record=$record"}
}
$earliest=@();$noSolution=$false
for($record=0;$record-lt$swaps.Count;++$record){
    if($upper[$record]-lt$record){$noSolution=$true;break};$earliest+=$record}
if($noSolution){Fail 'VISIBLE_PREFIX assignmentはNO_SOLUTIONです'}
$latest=New-Object 'long[]' $swaps.Count
$next=$opportunities.Count
for($record=$swaps.Count-1;$record-ge0;--$record){
    $next=[Math]::Min([int64]$upper[$record],$next-1)
    if($next-lt0){Fail 'latest assignmentを構築できません'}
    $latest[$record]=$next
}
$ambiguous=$false
for($record=0;$record-lt$swaps.Count;++$record){if($earliest[$record]-ne$latest[$record]){$ambiguous=$true;break}}
if($ambiguous){Fail 'measurement end後もVISIBLE_PREFIX assignmentがAMBIGUOUSです'}

$lostPhysical=0;$previousMapped=-1
for($record=0;$record-lt$records.Count;++$record){
    $expected=[int64]$opportunities[$earliest[$record]].ordinal
    $mapped=[int64]$records[$record].final_mapped_opportunity
    if($mapped-ne$expected-or$records[$record].committed-ne$true){Fail "final identity不一致: record=$record"}
    if([int64]$records[$record].swap_ordinal-ne$record-or[int64]$records[$record].swap_qpc-ne[int64]$swaps[$record].swap_qpc-or
       [int64]$records[$record].source_frame-ne[int64]$swaps[$record].presented_output_frame){Fail "swap identity不一致: record=$record"}
    if([int64]$records[$record].commit_qpc-le[int64]$records[$record].swap_qpc){Fail "upper boundary前にcommitしています: record=$record"}
    if($previousMapped-ge0-and$mapped-gt$previousMapped+1){$lostPhysical+=$mapped-$previousMapped-1}
    $previousMapped=$mapped
}
if([int64]$mapper.lost_physical_opportunity_count-ne$lostPhysical){Fail 'physical opportunity loss集計が不一致です'}

$watermark=0;$lastTransitionQpc=0
foreach($transition in $transitions){
    if([int64]$transition.qpc-lt$lastTransitionQpc){Fail 'mapper transition QPCが後退しました'}
    if([int]$transition.commit_watermark-lt$watermark){Fail 'commit watermarkが後退しました'}
    if($transition.mapper_error-ne'NONE'){Fail "途中mapper errorがあります: $($transition.mapper_error)"}
    $lastTransitionQpc=[int64]$transition.qpc;$watermark=[int]$transition.commit_watermark
}
if($watermark-ne$swaps.Count){Fail '最終transition watermarkがswap数と一致しません'}

$required=[int64]$raw.required_measurement_frame_count
$frames=@($swaps|ForEach-Object{[int64]$_.presented_output_frame}|Where-Object{$_-ge0}|Sort-Object -Unique)
$gapDrops=0;$nextFrame=0
foreach($frame in $frames){if($frame-ge$nextFrame){$gapDrops+=$frame-$nextFrame;$nextFrame=$frame+1}}
$tailDrops=[Math]::Max(0,$required-$nextFrame)
if([int64]$mapper.displayed_unique_source_frames-ne$frames.Count-or
   [int64]$mapper.source_frame_gap_drops-ne$gapDrops-or[int64]$mapper.tail_source_frame_drops-ne$tailDrops-or
   $frames.Count+$gapDrops+$tailDrops-ne$required-or$mapper.source_frame_accounting_exact-ne$true){
    Fail 'source-frame gap/tail accountingが不一致です'}
Write-Host ("B1 shadow mapper: PASS swaps=$($swaps.Count) opportunities=$($opportunities.Count) " +
    "lost_physical=$lostPhysical source_unique=$($frames.Count) gap=$gapDrops tail=$tailDrops")
