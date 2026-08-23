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
function All-Class($Rows,[string]$Class){@($Rows|Where-Object classification -ne $Class).Count-eq0}
$indexPath=Join-Path $MatrixDirectory 'matrix-runs.json'
if(-not(Test-Path -LiteralPath $indexPath)){Fail "T2-C matrix indexがありません: $indexPath"}
$index=Get-Content -LiteralPath $indexPath -Raw -Encoding utf8|ConvertFrom-Json
if([string]$index.schema-ne'mvm-p2-c3-a3-t2-c-matrix-runs-1'-or[string]$index.status-ne'PASS'){Fail 'T2-C matrix indexがPASSではありません'}
$stage=[string]$index.stage
if($stage-notin@('C1','C2')){Fail "T2-C stageが不正です: $stage"}
$conditions=if($stage-eq'C1'){@('CONTROL','TARGET_HWND_INVALIDATE','EXTERNAL_DIRTY')}
            else{@('TARGET_HWND_INVALIDATE','TARGET_HWND_REDRAW_NOW','EXTERNAL_DIRTY')}
$damageConditions=@('TARGET_HWND_INVALIDATE','TARGET_HWND_REDRAW_NOW')
$runs=@($index.runs)
if($runs.Count-ne9){Fail "T2-C matrix runが9件ではありません: $($runs.Count)"}
$rows=@();$runtimeIdentities=@()
foreach($run in $runs){
    if([long]$run.set-lt1-or[long]$run.set-gt3-or[long]$run.position-lt1-or[long]$run.position-gt3-or$conditions-notcontains[string]$run.condition){Fail 'T2-C matrix run identityが不正です'}
    $directory=Join-Path $MatrixDirectory ([string]$run.directory)
    $summaryPath=Join-Path $directory 't2-summary.json'
    $canonicalPath=Join-Path $directory 'canonical\summary.json'
    foreach($path in @($summaryPath,$canonicalPath)){if(-not(Test-Path -LiteralPath $path)){Fail "T2-C condition artifactがありません: $path"}}
    $summary=Get-Content -LiteralPath $summaryPath -Raw -Encoding utf8|ConvertFrom-Json
    $canonical=Get-Content -LiteralPath $canonicalPath -Raw -Encoding utf8|ConvertFrom-Json
    if([string]$summary.schema-ne'mvm-p2-c3-a3-t2-condition-run-1'-or[string]$summary.status-ne'PASS'-or[string]$summary.condition-ne[string]$run.condition){Fail "T2-C condition summaryが不正です: $summaryPath"}
    if([bool]$summary.formal_counter_authority_changed-or[bool]$summary.formal_drop_threshold_changed-or[bool]$summary.production_scheduler_changed){Fail 'T2-Cで禁止されたproduction/formal変更があります'}
    $chain=$summary.update_chain;$dwm=$summary.dwm_condition
    if([string]$dwm.schema-ne'mvm-p2-c3-a3-t1-condition-proof-1'-or[string]$dwm.status-ne'PASS'){Fail "T1 condition proofが不正です: $summaryPath"}
    # T2-Cはmvm render pathへ何も足さない。全条件でT2-A closureが維持されること。
    if([string]$chain.status-ne'PASS'-or[string]$chain.verdict-ne'UPDATE_CHAIN_EXACT'){Fail 'update-chain exact closureが成立していません'}
    if([bool]$chain.target_pixel_toggle){Fail 'T2-Cにtarget pixel markerが混入しています'}
    $state=$dwm.window_state
    $dirtyTicks=[long]$state.dirty_tick_delta
    $damageDelta=[long]$state.target_damage_delta
    if([string]$run.condition-eq'EXTERNAL_DIRTY'){
        if($dirtyTicks-lt[Math]::Floor([double]$dwm.measurement_seconds*40)){Fail 'external companion cadenceが不足しています'}
        if($damageDelta-ne0){Fail 'EXTERNAL_DIRTYにtarget HWND damageが混入しています'}
    }elseif($damageConditions-contains[string]$run.condition){
        if($dirtyTicks-ne0){Fail 'target damage条件にexternal companionが混入しています'}
        if($damageDelta-lt[Math]::Floor([double]$dwm.measurement_seconds*40)){Fail "target HWND damage cadenceが不足しています: $damageDelta"}
        if([long]$state.target_damage_failure_count-ne0){Fail 'target HWND damage注入が失敗しています'}
    }elseif($dirtyTicks-ne0-or$damageDelta-ne0){Fail 'CONTROLにdamage注入が混入しています'}
    if([bool]$state.user_input_detected){Fail 'PROTOCOL_INVALID: measurement中にユーザー入力があります'}
    if([string]$canonical.acquisition_mode-ne'CanonicalPresentMonLive'-or[string]$canonical.submission_mode-ne'CONTROL'-or[string]$canonical.c0_r2_status-ne'PASS'-or[string]$canonical.oracle_status-ne'ORACLE_VALID'-or[string]$canonical.display_completion_status-ne'CLOSED'){Fail 'native/token/ETW exact authorityが閉じていません'}
    if([long]$canonical.incomplete_unknown_count-ne0-or[long]$canonical.lost_count-ne0){Fail 'Unknown/Lostが0ではありません'}
    $identity=$canonical.identities
    $runtimeIdentities+="$([string]$identity.executable_sha256)|$([string]$identity.decoder_sha256)|$([string]$identity.qt_upstream_commit)|$([string]$identity.qt_gui_dll_sha256)|$([string]$identity.qt_core_dll_sha256)|$([string]$identity.t2_qtbase_patch_sha256)|$([string]$identity.t2_qtdeclarative_patch_sha256)|$([string]$identity.t2_qt_quick_dll_sha256)|$([string]$identity.qtdeclarative_upstream_commit)"
    $rows+=[pscustomobject][ordered]@{
        set=[long]$run.set;position=[long]$run.position;condition=[string]$run.condition;classification=Classify $dwm
        exact_closed_count=[long]$chain.exact_closed_count;native_present_count=[long]$chain.native_present_count
        target_damage_delta=$damageDelta;external_dirty_tick_delta=$dirtyTicks
        target_update_region_fraction=[double]$state.target_update_region_fraction
        dwm_present_start_count=[long]$dwm.dwm_wide_present_start_count
        dwm_gap_p95=[long]$dwm.dwm_wide_present_start_gap.p95;dwm_gap_max=[long]$dwm.dwm_wide_present_start_gap.max
        target_parent_count=[long]$dwm.target_attached_parent_count
        dependency_batch_evaluable=[long]$dwm.target_attached_parent_count-gt0
        batch_p95=[long]$dwm.dependency_batch_size.p95;batch_max=[long]$dwm.dependency_batch_size.max
        presented=[long]$dwm.presented_count;discarded=[long]$dwm.discarded_count
    }
}
if(@($runtimeIdentities|Sort-Object -Unique).Count-ne1){Fail '9 runのruntime/provenanceが一致しません'}
foreach($set in 1..3){$setRows=@($rows|Where-Object set -eq $set);if($setRows.Count-ne3-or@($setRows.position|Sort-Object -Unique).Count-ne3-or@($setRows.condition|Sort-Object -Unique).Count-ne3){Fail "set $set が3条件循環ではありません"}}
foreach($condition in $conditions){$conditionRows=@($rows|Where-Object condition -eq $condition);if($conditionRows.Count-ne3-or@($conditionRows.position|Sort-Object -Unique).Count-ne3){Fail "$condition が各positionに1回ずつ配置されていません"}}
# RedrawWindow(UPDATENOW)はQt側のevent processingを刺激しうる。update-chainの
# 「量」が条件間でずれていれば、HWND damageだけを注入した比較ではなくなる。
$presentCounts=@($rows|ForEach-Object{[long]$_.native_present_count})
$presentMin=($presentCounts|Measure-Object -Minimum).Minimum
$presentMax=($presentCounts|Measure-Object -Maximum).Maximum
$presentSpread=if($presentMax-gt0){[double]($presentMax-$presentMin)/[double]$presentMax}else{1.0}
$updateChainVolumeComparable=$presentSpread-le0.02
$byConditionRows=@{};foreach($condition in $conditions){$byConditionRows[$condition]=@($rows|Where-Object condition -eq $condition)}
$nextAction='RETURN_TO_DWM_LATENT_STATE'
$verdict=if(-not$updateChainVolumeComparable){
    $nextAction='LIMIT_INTERPRETATION_UPDATE_CHAIN_VOLUME_DIVERGENT';'UPDATE_CHAIN_VOLUME_DIVERGENT'
}elseif($stage-eq'C1'){
    $control=$byConditionRows['CONTROL'];$invalidate=$byConditionRows['TARGET_HWND_INVALIDATE'];$external=$byConditionRows['EXTERNAL_DIRTY']
    if((All-Class $control 'LARGE_SUPPRESSION')-and(All-Class $invalidate 'REGULAR')-and(All-Class $external 'REGULAR')){
        $nextAction='INVESTIGATE_QT_D3D_MISSING_HWND_INVALIDATION';'TARGET_HWND_INVALIDATION_WAKES_DWM'
    }elseif((All-Class $control 'LARGE_SUPPRESSION')-and(All-Class $invalidate 'LARGE_SUPPRESSION')-and(All-Class $external 'REGULAR')){
        # InvalidateRectはupdate regionを設定するだけである。ここでredirection pathを
        # 疑うのは早い。forced redrawのpositive controlを必ず経由する。
        $nextAction='T2_C2_FORCED_REDRAW_POSITIVE_CONTROL';'TARGET_INVALIDATION_INSUFFICIENT_FORCED_REDRAW_REQUIRED'
    }elseif((All-Class $control 'LARGE_SUPPRESSION')-and(All-Class $invalidate 'LARGE_SUPPRESSION')-and(All-Class $external 'LARGE_SUPPRESSION')){
        $nextAction='COMPARE_T2_C_RUNTIME_WITH_T2_B';'T2_C_ACQUISITION_REGIME_INVALID'
    }else{'ORDER_TIME_CONFOUND_OR_MIXED'}
}else{
    $invalidate=$byConditionRows['TARGET_HWND_INVALIDATE'];$redraw=$byConditionRows['TARGET_HWND_REDRAW_NOW'];$external=$byConditionRows['EXTERNAL_DIRTY']
    if((All-Class $invalidate 'LARGE_SUPPRESSION')-and(All-Class $redraw 'REGULAR')-and(All-Class $external 'REGULAR')){
        $nextAction='INVESTIGATE_WIN32_QPA_TO_DWM_DAMAGE_PROCESSING_BOUNDARY';'TARGET_PAINT_PROCESSING_WAKES_DWM'
    }elseif((All-Class $invalidate 'LARGE_SUPPRESSION')-and(All-Class $redraw 'LARGE_SUPPRESSION')-and(All-Class $external 'REGULAR')){
        $nextAction='T2_D_REDIRECTION_PATH_PROBE';'TARGET_REDIRECTION_PATH_SUSPECT'
    }elseif((All-Class $invalidate 'LARGE_SUPPRESSION')-and(All-Class $redraw 'LARGE_SUPPRESSION')-and(All-Class $external 'LARGE_SUPPRESSION')){
        $nextAction='COMPARE_T2_C_RUNTIME_WITH_T2_B';'T2_C_ACQUISITION_REGIME_INVALID'
    }elseif(All-Class $invalidate 'REGULAR'){
        $nextAction='RECONCILE_WITH_T2_C1';'C1_INCONSISTENT_INVALIDATION_REGULAR'
    }else{'ORDER_TIME_CONFOUND_OR_MIXED'}
}
$byCondition=[ordered]@{};foreach($condition in $conditions){$byCondition[$condition]=@($rows|Where-Object condition -eq $condition|Sort-Object set)}
$byPosition=[ordered]@{};foreach($position in 1..3){$byPosition[[string]$position]=@($rows|Where-Object position -eq $position|Sort-Object set)}
[ordered]@{
    schema='mvm-p2-c3-a3-t2-c-matrix-proof-1';status='PASS';stage=$stage;authority='diagnostic_only';verdict=$verdict;next_action=$nextAction
    formal_counter_authority_changed=$false;formal_drop_threshold_changed=$false;production_scheduler_changed=$false
    diagnostic_large_gap_threshold_vblanks=30
    primary_authority='DWM_WIDE_PRESENTSTART'
    secondary_authority='DEPENDENCY_BATCH_WHERE_OBSERVABLE'
    presented_role='DOWNSTREAM_EVIDENCE'
    dependency_batch_evaluable_run_count=@($rows|Where-Object dependency_batch_evaluable).Count
    update_chain_volume_comparable=$updateChainVolumeComparable
    native_present_count_spread=$presentSpread
    runtime_provenance_exact=$true;by_condition=$byCondition;by_position=$byPosition
}|ConvertTo-Json -Depth 10|Set-Content -LiteralPath $Output -Encoding utf8
Write-Host "F3-C3-A3-T2-C $stage summary: PASS verdict=$verdict"
