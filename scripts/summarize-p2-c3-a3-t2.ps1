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
if(-not(Test-Path -LiteralPath $indexPath)){Fail "T2 matrix indexがありません: $indexPath"}
$index=Get-Content -LiteralPath $indexPath -Raw -Encoding utf8|ConvertFrom-Json
if([string]$index.schema-ne'mvm-p2-c3-a3-t2-matrix-runs-1'-or[string]$index.status-ne'PASS'){Fail 'T2 matrix indexがPASSではありません'}
$conditions=@('CONTROL','TARGET_RHIITEM_PIXEL_TOGGLE','EXTERNAL_DIRTY')
$runs=@($index.runs)
if($runs.Count-ne9){Fail "T2 matrix runが9件ではありません: $($runs.Count)"}
$rows=@();$runtimeIdentities=@()
foreach($run in $runs){
    if([long]$run.set-lt1-or[long]$run.set-gt3-or[long]$run.position-lt1-or[long]$run.position-gt3-or$conditions-notcontains[string]$run.condition){Fail 'T2 matrix run identityが不正です'}
    $directory=Join-Path $MatrixDirectory ([string]$run.directory)
    $summaryPath=Join-Path $directory 't2-summary.json'
    $canonicalPath=Join-Path $directory 'canonical\summary.json'
    foreach($path in @($summaryPath,$canonicalPath)){if(-not(Test-Path -LiteralPath $path)){Fail "T2 condition artifactがありません: $path"}}
    $summary=Get-Content -LiteralPath $summaryPath -Raw -Encoding utf8|ConvertFrom-Json
    $canonical=Get-Content -LiteralPath $canonicalPath -Raw -Encoding utf8|ConvertFrom-Json
    if([string]$summary.schema-ne'mvm-p2-c3-a3-t2-condition-run-1'-or[string]$summary.status-ne'PASS'-or[string]$summary.condition-ne[string]$run.condition){Fail "T2 condition summaryが不正です: $summaryPath"}
    if([bool]$summary.formal_counter_authority_changed-or[bool]$summary.formal_drop_threshold_changed-or[bool]$summary.production_scheduler_changed){Fail 'T2で禁止されたproduction/formal変更があります'}
    $chain=$summary.update_chain;$dwm=$summary.dwm_condition
    if([string]$dwm.schema-ne'mvm-p2-c3-a3-t1-condition-proof-1'-or[string]$dwm.status-ne'PASS'){Fail "T1 condition proofが不正です: $summaryPath"}
    if([string]$chain.status-ne'PASS'-or[string]$chain.verdict-ne'UPDATE_CHAIN_EXACT'){Fail 'update-chain exact closureが成立していません'}
    $expectedPixel=[string]$run.condition-eq'TARGET_RHIITEM_PIXEL_TOGGLE'
    if([bool]$chain.target_pixel_toggle-ne$expectedPixel){Fail 'TARGET pixel marker exact closureが条件と一致しません'}
    $dirtyTicks=[long]$dwm.window_state.dirty_tick_delta
    if([string]$run.condition-eq'EXTERNAL_DIRTY'){
        if($dirtyTicks-lt[Math]::Floor([double]$dwm.measurement_seconds*40)){Fail 'external companion cadenceが不足しています'}
    }elseif($dirtyTicks-ne0){Fail '非EXTERNAL条件でexternal companionが動作しています'}
    if([string]$canonical.acquisition_mode-ne'CanonicalPresentMonLive'-or[string]$canonical.submission_mode-ne'CONTROL'-or[string]$canonical.c0_r2_status-ne'PASS'-or[string]$canonical.oracle_status-ne'ORACLE_VALID'-or[string]$canonical.display_completion_status-ne'CLOSED'){Fail 'native/token/ETW exact authorityが閉じていません'}
    if([long]$canonical.incomplete_unknown_count-ne0-or[long]$canonical.lost_count-ne0){Fail 'Unknown/Lostが0ではありません'}
    $identity=$canonical.identities
    $runtimeIdentities+="$( [string]$identity.executable_sha256)|$( [string]$identity.decoder_sha256)|$( [string]$identity.qt_upstream_commit)|$( [string]$identity.qt_gui_dll_sha256)|$( [string]$identity.qt_core_dll_sha256)|$( [string]$identity.t2_qtbase_patch_sha256)|$( [string]$identity.t2_qtdeclarative_patch_sha256)|$( [string]$identity.t2_qt_quick_dll_sha256)|$( [string]$identity.qtdeclarative_upstream_commit)"
    $rows+=[pscustomobject][ordered]@{
        set=[long]$run.set;position=[long]$run.position;condition=[string]$run.condition;classification=Classify $dwm
        exact_closed_count=[long]$chain.exact_closed_count;pixel_marker_exact=$expectedPixel;external_dirty_tick_delta=$dirtyTicks
        dwm_present_start_count=[long]$dwm.dwm_wide_present_start_count
        dwm_gap_p95=[long]$dwm.dwm_wide_present_start_gap.p95;dwm_gap_max=[long]$dwm.dwm_wide_present_start_gap.max
        target_parent_count=[long]$dwm.target_attached_parent_count
        batch_p95=[long]$dwm.dependency_batch_size.p95;batch_max=[long]$dwm.dependency_batch_size.max
        presented=[long]$dwm.presented_count;discarded=[long]$dwm.discarded_count
    }
}
if(@($runtimeIdentities|Sort-Object -Unique).Count-ne1){Fail '9 runのruntime/provenanceが一致しません'}
foreach($set in 1..3){$setRows=@($rows|Where-Object set -eq $set);if($setRows.Count-ne3-or@($setRows.position|Sort-Object -Unique).Count-ne3-or@($setRows.condition|Sort-Object -Unique).Count-ne3){Fail "set $set が3条件循環ではありません"}}
foreach($condition in $conditions){$conditionRows=@($rows|Where-Object condition -eq $condition);if($conditionRows.Count-ne3-or@($conditionRows.position|Sort-Object -Unique).Count-ne3){Fail "$condition が各positionに1回ずつ配置されていません"}}
$control=@($rows|Where-Object condition -eq 'CONTROL')
$target=@($rows|Where-Object condition -eq 'TARGET_RHIITEM_PIXEL_TOGGLE')
$external=@($rows|Where-Object condition -eq 'EXTERNAL_DIRTY')
$nextAction='RETURN_TO_DWM_LATENT_STATE'
$verdict=if((All-Class $control 'LARGE_SUPPRESSION')-and(All-Class $target 'LARGE_SUPPRESSION')-and(All-Class $external 'REGULAR')){
    $nextAction='T2_C_TARGET_HWND_DAMAGE_NOTIFICATION_PROBE';'TARGET_PIXEL_DOES_NOT_WAKE_DWM_EXTERNAL_DIRTY_REGULAR'
}elseif((All-Class $control 'LARGE_SUPPRESSION')-and(All-Class $target 'REGULAR')-and(All-Class $external 'REGULAR')){
    $nextAction='TARGET_GPU_NO_VISIBLE_CHANGE_NEGATIVE_CONTROL';'TARGET_PIXEL_AND_EXTERNAL_REGULAR_NEGATIVE_CONTROL_REQUIRED'
}elseif((All-Class $control 'LARGE_SUPPRESSION')-and(All-Class $target 'LARGE_SUPPRESSION')-and(All-Class $external 'LARGE_SUPPRESSION')){
    $nextAction='COMPARE_T2_RUNTIME_WITH_T1';'T2_B_ACQUISITION_REGIME_INVALID'
}else{'ORDER_TIME_CONFOUND_OR_MIXED'}
$byCondition=[ordered]@{};foreach($condition in $conditions){$byCondition[$condition]=@($rows|Where-Object condition -eq $condition|Sort-Object set)}
$byPosition=[ordered]@{};foreach($position in 1..3){$byPosition[[string]$position]=@($rows|Where-Object position -eq $position|Sort-Object set)}
[ordered]@{
    schema='mvm-p2-c3-a3-t2-matrix-proof-1';status='PASS';authority='diagnostic_only';verdict=$verdict;next_action=$nextAction
    formal_counter_authority_changed=$false;formal_drop_threshold_changed=$false;production_scheduler_changed=$false
    diagnostic_large_gap_threshold_vblanks=30;primary_authority='DWM_WIDE_PRESENTSTART_PLUS_DEPENDENCY_BATCH';presented_role='DOWNSTREAM_EVIDENCE'
    runtime_provenance_exact=$true;by_condition=$byCondition;by_position=$byPosition
}|ConvertTo-Json -Depth 10|Set-Content -LiteralPath $Output -Encoding utf8
Write-Host "F3-C3-A3-T2-B summary: PASS verdict=$verdict"
