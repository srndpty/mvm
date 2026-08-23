[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$ProbeDirectory,
    [Parameter(Mandatory=$true)][string]$Output
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
$independentModes=@('Hardware_Independent_Flip','Hardware_Composed_Independent_Flip')
$composedModes=@('Composed_Flip','Composed_Copy_GPU_GDI','Composed_Copy_CPU_GDI')
function Classify($Stats){
    if($Stats.count-eq0){return 'NO_PRESENT'}
    if($Stats.unknown-gt0){return 'UNRESOLVED'}
    $displayedFraction=[double]$Stats.displayed/$Stats.count
    $independentFraction=[double]$Stats.independent/$Stats.count
    $composedFraction=[double]$Stats.composed/$Stats.count
    if($displayedFraction-ge0.99){
        if($independentFraction-ge0.99){return 'GOOD_INDEPENDENT'}
        if($composedFraction-ge0.99){return 'GOOD_COMPOSED'}
        return 'UNRESOLVED'
    }
    if($displayedFraction-le0.95-and$composedFraction-ge0.99){return 'BAD_SPARSE_COMPOSED'}
    return 'UNRESOLVED'
}
$indexPath=Join-Path $ProbeDirectory 'probe-runs.json'
if(-not(Test-Path -LiteralPath $indexPath)){Fail "probe indexがありません: $indexPath"}
$index=Get-Content -LiteralPath $indexPath -Raw -Encoding utf8|ConvertFrom-Json
if([string]$index.schema-ne'mvm-p2-c3-a3-t2-d1b3a-probe-runs-1'-or[string]$index.status-ne'PASS'){Fail 'probe indexがPASSではありません'}
$runs=@($index.runs)
if($runs.Count-lt3){Fail "probe runが不足しています: $($runs.Count)"}
$rows=@();$preflightKeys=@()
foreach($run in $runs){
    $directory=Join-Path $ProbeDirectory ([string]$run.directory)
    $canonical=Join-Path $directory 'canonical'
    $rawPath=Join-Path $canonical 'present-history-raw.json'
    $appPath=Join-Path $canonical 'traced-app.json'
    $statePath=Join-Path $directory 'window-state-raw.json'
    $conditionPath=Join-Path $directory 'condition-proof.json'
    $preflightPath=Join-Path $directory 'preflight-proof.json'
    foreach($path in @($rawPath,$appPath,$statePath,$conditionPath,$preflightPath)){
        if(-not(Test-Path -LiteralPath $path)){Fail "D1-B3a artifactがありません: $path"}
    }
    $raw=Get-Content -LiteralPath $rawPath -Raw -Encoding utf8|ConvertFrom-Json
    $app=Get-Content -LiteralPath $appPath -Raw -Encoding utf8|ConvertFrom-Json
    $state=Get-Content -LiteralPath $statePath -Raw -Encoding utf8|ConvertFrom-Json
    $condition=Get-Content -LiteralPath $conditionPath -Raw -Encoding utf8|ConvertFrom-Json
    $preflight=Get-Content -LiteralPath $preflightPath -Raw -Encoding utf8|ConvertFrom-Json
    foreach($field in @('etw_events_lost','etw_buffers_lost','present_event_overflow_count')){
        if([long]$raw.$field-ne0){Fail "raw $field が0ではありません: $rawPath"}
    }
    if([string]$condition.status-ne'PASS'){Fail "condition proofがPASSではありません: $conditionPath"}
    if([bool]$condition.window_state.user_input_detected){Fail 'PROTOCOL_INVALID: measurement中にユーザー入力があります'}
    $swapchain=$preflight.swapchain
    $preflightKeys+=("{0}|{1}|{2}|{3}|{4}|{5}|{6}" -f $swapchain.width,$swapchain.height,
        $swapchain.buffer_count,$swapchain.scaling,$swapchain.swap_effect,$swapchain.flags,
        $preflight.capability.hardware_composition_support_flags)
    $measureStart=[long]$app.presentation_opportunity.measurement_start_qpc
    $measureEnd=[long]$app.presentation_opportunity.measurement_end_qpc_exclusive
    # phase境界はcontrollerが記録したQPC。measurementはapp側の窓で上書きする。
    $boundaries=@($state.phase_boundaries|Sort-Object {[long]$_.qpc})
    if($boundaries.Count-eq0){Fail "phase boundaryがありません: $statePath"}
    $targetPid=[long]$raw.target_process_id
    # ETWはwarmupも含めて保持されているため、全phaseのPresentを分類できる。
    $events=@($raw.events|Where-Object{[long]$_.process_id-eq$targetPid}|Sort-Object {[long]$_.present_start_qpc})
    $phaseStats=[ordered]@{}
    foreach($event in $events){
        $qpc=[long]$event.present_start_qpc
        if($qpc-lt[long]$boundaries[0].qpc){continue}
        $phase='MEASURE'
        if($qpc-lt$measureStart-or$qpc-ge$measureEnd){
            $phase=[string]$boundaries[0].phase
            foreach($boundary in $boundaries){if($qpc-ge[long]$boundary.qpc){$phase=[string]$boundary.phase}}
            $phase=$phase+'_WARMUP'
        }
        if(-not$phaseStats.Contains($phase)){
            $phaseStats[$phase]=[ordered]@{count=0;displayed=0;discarded=0;unknown=0
                independent=0;composed=0;other=0;parent=0}
        }
        $stats=$phaseStats[$phase]
        $stats.count++
        $mode=[string]$event.present_mode
        if($independentModes-contains$mode){$stats.independent++}
        elseif($composedModes-contains$mode){$stats.composed++}else{$stats.other++}
        if($event.PSObject.Properties.Name-contains'attached_dwm_parent_present_start_qpc'-and
           [long]$event.attached_dwm_parent_present_start_qpc-gt0){$stats.parent++}
        if([string]$event.final_state-eq'Presented'){
            $hit=@($event.displayed|Where-Object{$null-ne$_-and($_.PSObject.Properties.Name-contains'qpc')-and[long]$_.qpc-gt0})
            if($hit.Count-eq0){$stats.unknown++}else{$stats.displayed++}
        }else{$stats.discarded++}
    }
    $phaseRegimes=[ordered]@{}
    foreach($key in $phaseStats.Keys){$phaseRegimes[$key]=Classify $phaseStats[$key]}
    $measureRegime=if($phaseRegimes.Contains('MEASURE')){$phaseRegimes['MEASURE']}else{'NO_PRESENT'}
    $rows+=[pscustomobject][ordered]@{
        index=[long]$run.index;condition=[string]$run.condition;window_mode=[string]$run.window_mode
        measure_regime=$measureRegime
        phase_sequence=[string]$condition.window_state.phase_sequence
        designated_expected_area=[long]$condition.window_state.designated_expected_area
        occluder_process_id=[long]$condition.window_state.occluder_process_id
        phase_regimes=$phaseRegimes;phase_stats=$phaseStats
        dwm_wide_present_start_count=[long]$condition.dwm_wide_present_start_count
        target_attached_parent_count=[long]$condition.target_attached_parent_count
        target_parent_gap_max=[long]$condition.target_parent_present_start_gap.max
        dependency_batch_max=[long]$condition.dependency_batch_size.max
    }
}
$preflightIdentical=@($preflightKeys|Sort-Object -Unique).Count-eq1
function RegimeOf($Rows,[string]$Condition,[string]$Phase){
    $row=@($Rows|Where-Object condition -eq $Condition)|Select-Object -First 1
    if($null-eq$row){return $null}
    if($Phase-eq'MEASURE'){return [string]$row.measure_regime}
    if($row.phase_regimes.Contains($Phase)){return [string]$row.phase_regimes[$Phase]}
    return $null
}
$cleanRegime=RegimeOf $rows 'CLEAN_STATIC' 'MEASURE'
$overlapRegime=RegimeOf $rows 'FOREIGN_WINDOW_OVERLAP' 'MEASURE'
$otrOverlapRegime=RegimeOf $rows 'OVERLAP_THEN_REMOVE' 'OVERLAP_WARMUP'
$otrMeasureRegime=RegimeOf $rows 'OVERLAP_THEN_REMOVE' 'MEASURE'
$overlapChangedPath=$null-ne$overlapRegime-and$null-ne$cleanRegime-and$overlapRegime-ne$cleanRegime
$nextAction='RETURN_TO_D1A_INVENTORY'
$verdict=if($null-eq$cleanRegime-or$null-eq$overlapRegime){'INSUFFICIENT_CONDITIONS'}
    elseif($overlapRegime-eq'BAD_SPARSE_COMPOSED'){
        $nextAction='T2_D1_B3B_COUNTERBALANCED_REPLICATION';'OVERLAP_REPRODUCES_BAD_SPARSE_COMPOSED'}
    elseif($overlapRegime-eq'GOOD_COMPOSED'){
        # independent flipを失っても表示が落ちないなら、失うこと自体は原因ではない。
        $nextAction='T2_D1_B3C_MOVE_RESIZE_AND_FOREGROUND_PROBE';'INDEPENDENT_LOSS_NOT_SUFFICIENT_FOR_FAILURE'}
    elseif(-not$overlapChangedPath){
        # overlap仮説のみ棄却する。move/resizeとforegroundは別候補として残す。
        $nextAction='T2_D1_B3C_MOVE_RESIZE_AND_FOREGROUND_PROBE';'OVERLAP_DOES_NOT_CHANGE_PRESENTATION_PATH'}
    else{$nextAction='T2_D1_B3C_MOVE_RESIZE_AND_FOREGROUND_PROBE';'OVERLAP_PATH_CHANGE_UNRESOLVED'}
# sticky判定はOVERLAP_THEN_REMOVEのphase遷移から直接読む。
$sticky=if($null-eq$otrOverlapRegime-or$null-eq$otrMeasureRegime){'UNAVAILABLE'}
        elseif($otrOverlapRegime-eq$cleanRegime){'NOT_LOST_DURING_OVERLAP'}
        elseif($otrMeasureRegime-eq$cleanRegime){'RECOVERED_AFTER_REMOVE'}
        else{'STICKY_AFTER_REMOVE'}
[ordered]@{
    schema='mvm-p2-c3-a3-t2-d1b3a-probe-proof-1';status='PASS';authority='diagnostic_only'
    verdict=$verdict;next_action=$nextAction
    formal_counter_authority_changed=$false;formal_drop_threshold_changed=$false
    production_scheduler_changed=$false
    primary_authority='PRESENT_MODE_PLUS_FINALSTATE_DISPLAYEDQPC'
    note='overlapでpathが変わらなくてもmove/resize/foreground仮説は棄却しない。'
    static_preflight_identical=$preflightIdentical
    clean_regime=$cleanRegime;overlap_regime=$overlapRegime
    overlap_then_remove_overlap_phase_regime=$otrOverlapRegime
    overlap_then_remove_measure_regime=$otrMeasureRegime
    eligibility_recovery=$sticky
    runs=$rows
}|ConvertTo-Json -Depth 10|Set-Content -LiteralPath $Output -Encoding utf8
Write-Host "F3-C3-A3-T2-D1-B3a summary: PASS verdict=$verdict recovery=$sticky"
