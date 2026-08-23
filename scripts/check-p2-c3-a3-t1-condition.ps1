[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$CanonicalDirectory,
    [Parameter(Mandatory=$true)][string]$WindowStateJson,
    [Parameter(Mandatory=$true)][string]$SubmissionProofJson,
    [Parameter(Mandatory=$true)]
    [ValidateSet('VISIBLE_UNOCCLUDED','FULLY_OCCLUDED','VISIBLE_UNOCCLUDED_FORCE_DIRTY')]
    [string]$ExpectedMode,
    [Parameter(Mandatory=$true)][string]$Output
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
function Percentile([long[]]$Values,[double]$P){
    if($Values.Count-eq0){return 0L}
    $sorted=@($Values|Sort-Object);$index=[Math]::Ceiling($P*$sorted.Count)-1
    return [long]$sorted[[Math]::Max(0,$index)]
}
$appPath=Join-Path $CanonicalDirectory 'traced-app.json'
$rawPath=Join-Path $CanonicalDirectory 'present-history-raw.json'
$oraclePath=Join-Path $CanonicalDirectory 'oracle.json'
foreach($path in @($appPath,$rawPath,$oraclePath,$WindowStateJson,$SubmissionProofJson)){
    if(-not(Test-Path -LiteralPath $path)){Fail "F3-C3-A3-T1必須pathがありません: $path"}
}
$app=Get-Content -LiteralPath $appPath -Raw -Encoding utf8|ConvertFrom-Json
$raw=Get-Content -LiteralPath $rawPath -Raw -Encoding utf8|ConvertFrom-Json
$oracle=Get-Content -LiteralPath $oraclePath -Raw -Encoding utf8|ConvertFrom-Json
$state=Get-Content -LiteralPath $WindowStateJson -Raw -Encoding utf8|ConvertFrom-Json
$submission=Get-Content -LiteralPath $SubmissionProofJson -Raw -Encoding utf8|ConvertFrom-Json
if([string]$raw.schema-ne'mvm-p2-etw-present-history-1'){Fail 'CanonicalPresentMonLive raw schemaが不正です'}
foreach($field in @('etw_events_lost','etw_buffers_lost','present_event_overflow_count')){
    if($raw.PSObject.Properties.Name-notcontains$field-or[long]$raw.$field-ne0){Fail "raw $field が0ではありません"}
}
if([string]$state.schema-ne'mvm-p2-c3-a3-t1-window-state-1'-or[string]$state.mode-ne$ExpectedMode){Fail 'window-state mode/schemaが一致しません'}
if([string]$submission.schema-ne'mvm-p2-c3-submission-backpressure-proof-2'-or[string]$submission.proof_status-ne'PASS'-or[string]$submission.submission_mode-ne'CONTROL'){Fail 'CONTROL submission proofが有効ではありません'}
if([string]$oracle.oracle_status-ne'ORACLE_VALID'-or[string]$oracle.display_completion_status-ne'CLOSED'){Fail 'canonical oracleが閉じていません'}
foreach($field in @('incomplete_unknown_count','lost_count','etw_events_lost','etw_buffers_lost','present_event_overflow_count')){
    if([long]$oracle.$field-ne0){Fail "$field が0ではありません"}
}
$start=[long]$app.presentation_opportunity.measurement_start_qpc
$end=[long]$app.presentation_opportunity.measurement_end_qpc_exclusive
$frequency=[long]$app.presentation_opportunity.qpc_frequency
if($start-le0-or$end-le$start-or[long]$state.qpc_frequency-ne$frequency){Fail 'window-state QPC domainが一致しません'}
$elapsed=([double]$end-[double]$start)/[double]$frequency
$samples=@($state.samples|Where-Object{[long]$_.qpc-ge$start-and[long]$_.qpc-lt$end}|Sort-Object {[long]$_.qpc})
if($samples.Count-lt[Math]::Floor($elapsed*8)){Fail "measurement内window-state sampleが不足しています: $($samples.Count)"}
$targetHwnd=[string]$state.target_hwnd
if([long]$state.target_process_id-ne[long]$raw.target_process_id-or$targetHwnd-eq'0x0'){Fail 'window-state target identityが不正です'}
foreach($sample in $samples){
    if([string]$sample.target_hwnd-ne$targetHwnd-or-not[bool]$sample.visible-or[bool]$sample.iconic-or-not[bool]$sample.topmost-or[long]$sample.cloaked-ne0){Fail 'visible/not-minimized/topmost/not-cloaked契約が不成立です'}
    if([long]$sample.client_area-le0-or[long]$sample.unexpected_intersection_area-ne0){Fail 'client areaまたはunexpected occlusionが不正です'}
    if($ExpectedMode-eq'FULLY_OCCLUDED'){
        if([string]$sample.occluder_hwnd-eq'0x0'-or[long]$sample.designated_intersection_area-ne[long]$sample.client_area){Fail 'FULLY_OCCLUDED被覆率が100%ではありません'}
    }elseif([string]$sample.occluder_hwnd-ne'0x0'-or[long]$sample.designated_intersection_area-ne0){Fail 'VISIBLE条件にdesignated occlusionがあります'}
}
$targetIds=@($samples|Select-Object -ExpandProperty target_hwnd -Unique)
$monitors=@($samples|Select-Object -ExpandProperty monitor -Unique)
$clientRects=@($samples|ForEach-Object{"$($_.client_rect.left),$($_.client_rect.top),$($_.client_rect.right),$($_.client_rect.bottom)"}|Select-Object -Unique)
$windowRects=@($samples|ForEach-Object{"$($_.window_rect.left),$($_.window_rect.top),$($_.window_rect.right),$($_.window_rect.bottom)"}|Select-Object -Unique)
if($targetIds.Count-ne1-or$monitors.Count-ne1-or$clientRects.Count-ne1-or$windowRects.Count-ne1){Fail 'HWND/monitor/window rectがmeasurement中に変化しました'}
$dirtyTicks=[long]$samples[-1].dirty_tick_count-[long]$samples[0].dirty_tick_count
$foregroundTargetCount=@($samples|Where-Object{[string]$_.foreground_hwnd-eq$targetHwnd}).Count
if($ExpectedMode-eq'VISIBLE_UNOCCLUDED_FORCE_DIRTY'){
    if([string]$state.dirty_companion_hwnd-eq'0x0'-or$dirtyTicks-lt[Math]::Floor($elapsed*40)){Fail "FORCE_DIRTY tickが不足しています: $dirtyTicks"}
}elseif([string]$state.dirty_companion_hwnd-ne'0x0'-or$dirtyTicks-ne0){Fail '非DIRTY条件でdirty companionが動作しています'}
$vblankSamples=@($app.presentation_opportunity.physical_vblank.samples)
$identity=$app.presentation_opportunity.physical_vblank.window_output_start
$numerator=[long]$identity.refresh_numerator;$denominator=[long]$identity.refresh_denominator
$periodScaled=[decimal]$frequency*[decimal]$denominator
$phases=@(0..119|ForEach-Object{([decimal][long]$vblankSamples[$_].qpc*[decimal]$numerator)-([decimal][long]$vblankSamples[$_].ordinal*$periodScaled)}|Sort-Object)
$originScaled=[decimal]$phases[59]
function Map-Qpc([long]$Qpc){
    $relative=([decimal]$Qpc*[decimal]$numerator)-$originScaled
    $floor=[decimal]::Floor($relative/$periodScaled);$remainder=$relative-$floor*$periodScaled
    if($remainder*2-eq$periodScaled){Fail "QPCがVBlank境界で曖昧です: $Qpc"}
    return [long]$(if($remainder*2-lt$periodScaled){$floor}else{$floor+1})
}
$records=@($oracle.records)
$parentIds=@($records|ForEach-Object{[long]$_.attached_dwm_parent_present_start_qpc}|Where-Object{$_-gt0}|Sort-Object -Unique)
$rawByStart=@{};foreach($presentEvent in @($raw.events)){$rawByStart[[string][long]$presentEvent.present_start_qpc]=$presentEvent}
$parentEvents=@();foreach($parentId in $parentIds){if(-not$rawByStart.ContainsKey([string]$parentId)){Fail "actual parentがrawにありません: $parentId"};$parentEvents+=$rawByStart[[string]$parentId]}
$dwmPids=@($parentEvents|Where-Object{[long]$_.process_id-ne[long]$raw.target_process_id}|Select-Object -ExpandProperty process_id -Unique)
$dwmIdentityMethod='ACTUAL_PARENT_EXACT'
if($dwmPids.Count-eq0){
    # FULLY_OCCLUDEDでtarget attachが0件でもDWM-wide cadenceを測る。rawが保持する
    # non-targetの一意PIDとDWM Hardware_Legacy_Flip signatureの両方を必須にする。
    $nonTargetPids=@($raw.events|Where-Object{[long]$_.process_id-ne[long]$raw.target_process_id}|Select-Object -ExpandProperty process_id -Unique)
    if($nonTargetPids.Count-ne1){Fail 'actual parentなしでDWM process identityを一意に決定できません'}
    $candidateEvents=@($raw.events|Where-Object{[long]$_.process_id-eq[long]$nonTargetPids[0]})
    if(@($candidateEvents|Where-Object{[string]$_.present_mode-eq'Hardware_Legacy_Flip'-and[string]$_.window_handle-eq'0x0'-and[string]$_.swap_chain_address-eq'0x0'}).Count-lt2){Fail 'non-target PIDにDWM Present signatureがありません'}
    $dwmPids=$nonTargetPids;$dwmIdentityMethod='UNIQUE_NON_TARGET_DWM_SIGNATURE'
}elseif($dwmPids.Count-ne1){Fail 'DWM process identityが一意ではありません'}
$dwmPid=[long]$dwmPids[0]
$dwmEvents=@($raw.events|Where-Object{[long]$_.process_id-eq$dwmPid-and[long]$_.present_start_qpc-ge$start-and[long]$_.present_start_qpc-lt$end}|Sort-Object {[long]$_.present_start_qpc})
# T1の一次量はPresentStartの発生cadenceである。DWM PID内の別Presentの
# completionが未解決でも、ETW loss/overflowが0ならStart QPC自体は観測済みである。
$dwmUnresolvedCompletionCount=@($dwmEvents|Where-Object{[bool]$_.is_lost-or[string]$_.completion_class-eq'LOST'}).Count
$dwmOrdinals=@($dwmEvents|ForEach-Object{Map-Qpc ([long]$_.present_start_qpc)})
$dwmGaps=@();for($index=1;$index-lt$dwmOrdinals.Count;++$index){$dwmGaps+=[long]$dwmOrdinals[$index]-[long]$dwmOrdinals[$index-1]}
$targetParents=@($parentIds|Where-Object{$_-ge$start-and$_-lt$end}|Sort-Object)
$targetOrdinals=@($targetParents|ForEach-Object{Map-Qpc $_})
$targetGaps=@();for($index=1;$index-lt$targetOrdinals.Count;++$index){$targetGaps+=[long]$targetOrdinals[$index]-[long]$targetOrdinals[$index-1]}
$result=[ordered]@{
    schema='mvm-p2-c3-a3-t1-condition-proof-1';status='PASS';authority='diagnostic_only';mode=$ExpectedMode
    formal_counter_authority_changed=$false;formal_drop_threshold_changed=$false;production_scheduler_changed=$false
    window_state=[ordered]@{
        target_hwnd=$targetHwnd;sample_count=$samples.Count;monitor=[string]$monitors[0]
        client_rect=[string]$clientRects[0];window_rect=[string]$windowRects[0]
        visible=$true;iconic=$false;topmost=$true;cloaked=0
        foreground_target_sample_count=$foregroundTargetCount;foreground_target_fraction=$foregroundTargetCount/$samples.Count
        designated_coverage=$(if($ExpectedMode-eq'FULLY_OCCLUDED'){1.0}else{0.0})
        unexpected_intersection_area_max=0;dirty_tick_delta=$dirtyTicks
    }
    dwm_process_id=$dwmPid;dwm_identity_method=$dwmIdentityMethod;measurement_seconds=$elapsed
    dwm_wide_present_start_count=$dwmEvents.Count;dwm_wide_present_starts_per_second=$dwmEvents.Count/$elapsed
    dwm_wide_cadence_observation=$(if($dwmEvents.Count-le1){'SPARSE_ZERO_OR_ONE'}else{'GAPS_OBSERVED'})
    dwm_wide_unresolved_completion_count=$dwmUnresolvedCompletionCount
    dwm_wide_present_start_gap=[ordered]@{p50=Percentile $dwmGaps 0.50;p95=Percentile $dwmGaps 0.95;max=[long]($dwmGaps|Measure-Object -Maximum).Maximum}
    target_attached_parent_count=$targetParents.Count;target_attached_parents_per_second=$targetParents.Count/$elapsed
    target_parent_present_start_gap=[ordered]@{p50=Percentile $targetGaps 0.50;p95=Percentile $targetGaps 0.95;max=[long]($targetGaps|Measure-Object -Maximum).Maximum}
    native_present_count=[long]$submission.native_present_count;presented_count=[long]$submission.presented_count;discarded_count=[long]$submission.discarded_count
    dependent_superseded_count=[long]$submission.discard_reason_histogram.DEPENDENT_PRESENT_SUPERSEDED
    earlier_superseded_count=[long]$submission.discard_reason_histogram.EARLIER_SWAPCHAIN_PRESENT_SUPERSEDED
    dependency_batch_count=[long]$submission.dependency_batch_count
    dependency_batch_size=$submission.dependency_batch_size
}
$result|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $Output -Encoding utf8
Write-Host "F3-C3-A3-T1 condition: PASS mode=$ExpectedMode DWM=$($dwmEvents.Count) parent_gap_max=$($result.target_parent_present_start_gap.max) batch_max=$($submission.dependency_batch_size.max)"
