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
$indexPath=Join-Path $ProbeDirectory 'probe-runs.json'
if(-not(Test-Path -LiteralPath $indexPath)){Fail "probe indexがありません: $indexPath"}
$index=Get-Content -LiteralPath $indexPath -Raw -Encoding utf8|ConvertFrom-Json
if([string]$index.schema-ne'mvm-p2-c3-a3-t2-d1b1-probe-runs-1'-or[string]$index.status-ne'PASS'){Fail 'probe indexがPASSではありません'}
$runs=@($index.runs)
if($runs.Count-lt4){Fail "probe runが不足しています: $($runs.Count)"}
$rows=@();$preflightKeys=@()
foreach($run in $runs){
    $directory=Join-Path $ProbeDirectory ([string]$run.directory)
    $canonical=Join-Path $directory 'canonical'
    $rawPath=Join-Path $canonical 'present-history-raw.json'
    $appPath=Join-Path $canonical 'traced-app.json'
    $conditionPath=Join-Path $directory 'condition-proof.json'
    $preflightPath=Join-Path $directory 'preflight-proof.json'
    foreach($path in @($rawPath,$appPath,$conditionPath,$preflightPath)){
        if(-not(Test-Path -LiteralPath $path)){Fail "D1-B1 artifactがありません: $path"}
    }
    $raw=Get-Content -LiteralPath $rawPath -Raw -Encoding utf8|ConvertFrom-Json
    $app=Get-Content -LiteralPath $appPath -Raw -Encoding utf8|ConvertFrom-Json
    $condition=Get-Content -LiteralPath $conditionPath -Raw -Encoding utf8|ConvertFrom-Json
    $preflight=Get-Content -LiteralPath $preflightPath -Raw -Encoding utf8|ConvertFrom-Json
    foreach($field in @('etw_events_lost','etw_buffers_lost','present_event_overflow_count')){
        if([long]$raw.$field-ne0){Fail "raw $field が0ではありません: $rawPath"}
    }
    if([string]$condition.status-ne'PASS'){Fail "condition proofがPASSではありません: $conditionPath"}
    if([bool]$condition.window_state.user_input_detected){Fail 'PROTOCOL_INVALID: measurement中にユーザー入力があります'}
    # static eligibility configurationがarm間で完全一致していることを要求する。
    $swapchain=$preflight.swapchain
    $preflightKeys+=("{0}|{1}|{2}|{3}|{4}|{5}|{6}|{7}|{8}|{9}|{10}" -f `
        $swapchain.width,$swapchain.height,$swapchain.format,$swapchain.buffer_count,
        $swapchain.scaling,$swapchain.swap_effect,$swapchain.alpha_mode,$swapchain.flags,
        $preflight.output.device_name,$preflight.capability.hardware_composition_support_flags,
        $preflight.capability.tearing_supported)
    $start=[long]$app.presentation_opportunity.measurement_start_qpc
    $end=[long]$app.presentation_opportunity.measurement_end_qpc_exclusive
    $targetPid=[long]$raw.target_process_id
    $events=@($raw.events|Where-Object{[long]$_.process_id-eq$targetPid-and
        [long]$_.present_start_qpc-ge$start-and[long]$_.present_start_qpc-lt$end})
    if($events.Count-eq0){Fail "measurement window内にtarget Presentがありません: $rawPath"}
    $modeCounts=@{};$displayed=0;$presented=0;$unknown=0;$independent=0;$composed=0
    foreach($event in $events){
        $mode=[string]$event.present_mode
        if(-not$modeCounts.ContainsKey($mode)){$modeCounts[$mode]=0}
        $modeCounts[$mode]++
        if($independentModes-contains$mode){$independent++}elseif($composedModes-contains$mode){$composed++}
        if([string]$event.final_state-eq'Presented'){
            $presented++
            $hit=@($event.displayed|Where-Object{$null-ne$_-and($_.PSObject.Properties.Name-contains'qpc')-and[long]$_.qpc-gt0})
            if($hit.Count-eq0){$unknown++}else{$displayed++}
        }
    }
    $total=$events.Count
    $displayedFraction=[double]$displayed/$total
    $independentFraction=[double]$independent/$total
    $composedFraction=[double]$composed/$total
    $regime=if($unknown-gt0){'UNRESOLVED'}
            elseif($displayedFraction-ge0.99){
                if($independentFraction-ge0.99){'GOOD_INDEPENDENT'}
                elseif($composedFraction-ge0.99){'GOOD_COMPOSED'}else{'UNRESOLVED'}}
            elseif($displayedFraction-le0.95){
                if($composedFraction-ge0.99){'BAD_SPARSE_COMPOSED'}else{'UNRESOLVED'}}
            else{'UNRESOLVED'}
    $row=[pscustomobject][ordered]@{
        index=[long]$run.index;arm=[string]$run.arm
        dirty_propagation_mode=[string]$run.dirty_propagation_mode;regime=$regime
        target_present_count=$total;presented_count=$presented;displayed_count=$displayed
        displayed_fraction=$displayedFraction
        independent_fraction=$independentFraction;composed_fraction=$composedFraction
        unknown_displayed_count=$unknown
        present_mode_histogram=[ordered]@{}
        dwm_wide_present_start_count=[long]$condition.dwm_wide_present_start_count
        target_attached_parent_count=[long]$condition.target_attached_parent_count
        target_parent_gap_max=[long]$condition.target_parent_present_start_gap.max
        dependency_batch_max=[long]$condition.dependency_batch_size.max
        discarded_count=[long]$condition.discarded_count
    }
    foreach($key in ($modeCounts.Keys|Sort-Object)){$row.present_mode_histogram[$key]=$modeCounts[$key]}
    $rows+=$row
}
$preflightIdentical=@($preflightKeys|Sort-Object -Unique).Count-eq1
$armA=@($rows|Where-Object arm -eq 'A');$armB=@($rows|Where-Object arm -eq 'B')
if($armA.Count-eq0-or$armB.Count-eq0){Fail '両armのrunが揃っていません'}
function AllRegime($Rows,[string]$Regime){@($Rows|Where-Object regime -ne $Regime).Count-eq0}
# arm内のばらつきが条件差より大きい場合はORDER_TIME_CONFOUNDとして止める。
$aRegimes=@($armA|Select-Object -ExpandProperty regime -Unique)
$bRegimes=@($armB|Select-Object -ExpandProperty regime -Unique)
$armsInternallyConsistent=$aRegimes.Count-eq1-and$bRegimes.Count-eq1
$nextAction='RETURN_TO_D1A_INVENTORY'
$verdict=if(-not$armsInternallyConsistent){
    $nextAction='STOP_DO_NOT_ADD_CONDITIONS';'ORDER_TIME_CONFOUND'
}elseif((AllRegime $armA 'BAD_SPARSE_COMPOSED')-and(AllRegime $armB 'GOOD_INDEPENDENT')){
    $nextAction='T2_D1_B2_PATH_DIVERGENCE_BOUNDARY';'DIRTY_PROPAGATION_MODE_REPRODUCES_BAD_REGIME'
}elseif((AllRegime $armA 'GOOD_INDEPENDENT')-and(AllRegime $armB 'GOOD_INDEPENDENT')){
    $nextAction='T2_D1_B3_RUNNER_CONTROLLED_OVERLAP_REPRODUCER';'BOTH_ARMS_GOOD_INDEPENDENT'
}elseif($aRegimes[0]-eq$bRegimes[0]){
    $nextAction='T2_D1_B3_RUNNER_CONTROLLED_OVERLAP_REPRODUCER';'BOTH_ARMS_SAME_REGIME'
}elseif(@($rows|Where-Object{$_.composed_fraction-lt0.99}).Count-eq0){
    # 両armともcomposedだがdisplayが違うなら、composed自体ではなくparent schedulingへ絞れる。
    $nextAction='T2_D1_B2_COMPOSED_PATH_WAKE_SCHEDULING';'COMPOSED_BOTH_ARMS_PARENT_CADENCE_DIFFERS'
}else{$nextAction='T2_D1_B2_PATH_DIVERGENCE_BOUNDARY';'ARMS_DIFFER_MIXED'}
if(-not$preflightIdentical-and$verdict-ne'ORDER_TIME_CONFOUND'){
    # static configで説明できるかどうかは結論を変える。差があれば明示する。
    $verdict='STATIC_ELIGIBILITY_CONFIG_DIFFERS'
    $nextAction='ATTRIBUTE_REGIME_TO_STATIC_ELIGIBILITY_DIFFERENCE'
}
[ordered]@{
    schema='mvm-p2-c3-a3-t2-d1b1-probe-proof-1';status='PASS';authority='diagnostic_only'
    verdict=$verdict;next_action=$nextAction
    formal_counter_authority_changed=$false;formal_drop_threshold_changed=$false
    production_scheduler_changed=$false
    primary_authority='PRESENT_MODE_PLUS_FINALSTATE_DISPLAYEDQPC'
    static_preflight_identical_across_arms=$preflightIdentical
    arms_internally_consistent=$armsInternallyConsistent
    arm_a_regimes=$aRegimes;arm_b_regimes=$bRegimes
    runs=$rows
}|ConvertTo-Json -Depth 10|Set-Content -LiteralPath $Output -Encoding utf8
Write-Host "F3-C3-A3-T2-D1-B1 summary: PASS verdict=$verdict"
