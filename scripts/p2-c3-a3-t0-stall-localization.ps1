[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$CanonicalDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
function Required-Int64($Object,[string]$Name,[string]$Context){
    if($Object.PSObject.Properties.Name-notcontains$Name){Fail "$Context に $Name がありません"}
    return [int64]$Object.$Name
}
$appPath=Join-Path $CanonicalDirectory 'traced-app.json'
$rawPath=Join-Path $CanonicalDirectory 'present-history-raw.json'
$oraclePath=Join-Path $CanonicalDirectory 'oracle.json'
$canonicalSummaryPath=Join-Path $CanonicalDirectory 'summary.json'
$canonicalManifestPath=Join-Path $CanonicalDirectory 'manifest.sha256'
foreach($path in @($appPath,$rawPath,$oraclePath,$canonicalSummaryPath,$canonicalManifestPath)){
    if(-not(Test-Path -LiteralPath $path)){Fail "F3-C3-A3-T0必須pathがありません: $path"}
}
if(Test-Path -LiteralPath $OutputDirectory){Fail "既存F3-C3-A3-T0 artifactを上書きしません: $OutputDirectory"}
$app=Get-Content -LiteralPath $appPath -Raw -Encoding utf8|ConvertFrom-Json
$raw=Get-Content -LiteralPath $rawPath -Raw -Encoding utf8|ConvertFrom-Json
$oracle=Get-Content -LiteralPath $oraclePath -Raw -Encoding utf8|ConvertFrom-Json
$canonicalSummary=Get-Content -LiteralPath $canonicalSummaryPath -Raw -Encoding utf8|ConvertFrom-Json
if([string]$canonicalSummary.c0_r2_status-ne'PASS'-or[string]$canonicalSummary.submission_mode-ne'CONTROL'){Fail 'CONTROL canonical runがPASSではありません'}
if([string]$raw.schema-ne'mvm-p2-etw-present-history-1'-or-not[bool]$raw.dependency_lifecycle_diagnostic){Fail 'lifecycle付きPresentMon rawではありません'}
if([string]$oracle.oracle_status-ne'ORACLE_VALID'-or[string]$oracle.display_completion_status-ne'CLOSED'){Fail 'oracleが閉じていません'}
foreach($field in @('incomplete_unknown_count','lost_count','etw_events_lost','etw_buffers_lost','present_event_overflow_count')){
    if([int64]$oracle.$field-ne0){Fail "$field が0ではありません"}
}
$measurementStart=[int64]$app.presentation_opportunity.measurement_start_qpc
$measurementEnd=[int64]$app.presentation_opportunity.measurement_end_qpc_exclusive
$samples=@($app.presentation_opportunity.physical_vblank.samples)
if($measurementStart-le0-or$measurementEnd-le$measurementStart-or$samples.Count-lt120){Fail 'physical VBlank measurement windowが不正です'}
$qpcFrequency=[int64]$app.presentation_opportunity.qpc_frequency
$outputIdentity=$app.presentation_opportunity.physical_vblank.window_output_start
$refreshNumerator=[int64]$outputIdentity.refresh_numerator;$refreshDenominator=[int64]$outputIdentity.refresh_denominator
$periodScaled=[decimal]$qpcFrequency*[decimal]$refreshDenominator
$phases=@(0..119|ForEach-Object{([decimal][int64]$samples[$_].qpc*[decimal]$refreshNumerator)-([decimal][int64]$samples[$_].ordinal*$periodScaled)}|Sort-Object)
$originScaled=[decimal]$phases[59];$outsideSampleCount=0L
function Map-Qpc([int64]$Qpc){
    $relative=([decimal]$Qpc*[decimal]$refreshNumerator)-$originScaled
    $floor=[decimal]::Floor($relative/$periodScaled);$remainder=$relative-$floor*$periodScaled
    if($remainder*2-eq$periodScaled){Fail "QPCがVBlank境界で曖昧です: $Qpc"}
    $ordinal=if($remainder*2-lt$periodScaled){$floor}else{$floor+1}
    if($ordinal-lt[decimal][int64]$samples[0].ordinal-or$ordinal-gt[decimal][int64]$samples[-1].ordinal){$script:outsideSampleCount++}
    return [int64]$ordinal
}
$records=@($oracle.records)
if($records.Count-lt2){Fail 'target lifecycle recordが不足しています'}
$parentIds=@($records|ForEach-Object{Required-Int64 $_ 'attached_dwm_parent_present_start_qpc' 'oracle record'}|Sort-Object -Unique)
if($parentIds.Count-lt2-or@($parentIds|Where-Object{$_-le0}).Count-ne0){Fail 'actual DWM parent identityが不足しています'}
$rawEvents=@($raw.events)
$rawByStart=@{}
foreach($presentEvent in $rawEvents){
    $start=Required-Int64 $presentEvent 'present_start_qpc' 'raw event';$key=[string]$start
    if($rawByStart.ContainsKey($key)){Fail "raw PresentStart identityが重複しています: $start"}
    $rawByStart[$key]=$presentEvent
}
$targetPid=[int64]$raw.target_process_id
$matchedParentEvents=@()
foreach($parentId in $parentIds){
    $key=[string]$parentId
    if(-not$rawByStart.ContainsKey($key)){Fail "actual DWM parentがrawにありません: $parentId"}
    $presentEvent=$rawByStart[$key]
    if([int64]$presentEvent.process_id-eq$targetPid){Fail "actual parentがtarget processとして記録されています: $parentId"}
    $matchedParentEvents+=$presentEvent
}
$dwmPids=@($matchedParentEvents|Select-Object -ExpandProperty process_id -Unique)
if($dwmPids.Count-ne1){Fail "actual parentを生成したprocessが一意ではありません: $($dwmPids-join',')"}
$dwmPid=[int64]$dwmPids[0]
$dwmEventsAll=@($rawEvents|Where-Object{[int64]$_.process_id-eq$dwmPid}|Sort-Object {[int64]$_.present_start_qpc})
$dwmEventsInWindow=@($dwmEventsAll|Where-Object{[int64]$_.present_start_qpc-ge$measurementStart-and[int64]$_.present_start_qpc-lt$measurementEnd})
if($dwmEventsInWindow.Count-lt2){Fail 'measurement内のDWM Presentが不足しています'}
$dwmLost=@($dwmEventsInWindow|Where-Object{[bool]$_.is_lost-or[string]$_.completion_class-eq'LOST'})
if($dwmLost.Count-ne0){Fail "measurement内のDWM PresentにLostがあります: $($dwmLost.Count)"}
$parentIdSet=@{};$parentIds|ForEach-Object{$parentIdSet[[string]$_]=$true}
$dwmTimeline=@()
foreach($presentEvent in $dwmEventsInWindow){
    $start=[int64]$presentEvent.present_start_qpc;$ready=Required-Int64 $presentEvent 'ready_qpc' "DWM event $start"
    $displayed=@($presentEvent.displayed);$displayQpc=if($displayed.Count-gt0){[int64]$displayed[0].qpc}else{0L}
    $dwmTimeline+=[ordered]@{
        present_start_qpc=$start;present_start_ordinal=Map-Qpc $start
        ready_qpc=$ready;ready_ordinal=$(if($ready-gt0){Map-Qpc $ready}else{$null})
        displayed_qpc=$displayQpc;displayed_ordinal=$(if($displayQpc-gt0){Map-Qpc $displayQpc}else{$null})
        completion_class=[string]$presentEvent.completion_class;present_mode=[string]$presentEvent.present_mode
        attached_to_target=$parentIdSet.ContainsKey([string]$start)
    }
}
$parentRows=@()
foreach($group in @($records|Group-Object attached_dwm_parent_present_start_qpc|Sort-Object {[int64]$_.Name})){
    $members=@($group.Group|Sort-Object sequence_index);$parentId=[int64]$group.Name
    $parentEvent=$rawByStart[[string]$parentId]
    $waitingValues=@($members|ForEach-Object{Required-Int64 $_ 'waiting_for_dwm_qpc' "parent $parentId"})
    $attachValues=@($members|ForEach-Object{Required-Int64 $_ 'attached_to_dwm_parent_qpc' "parent $parentId"}|Sort-Object -Unique)
    $displayValues=@($members|ForEach-Object{Required-Int64 $_ 'dwm_parent_displayed_qpc' "parent $parentId"}|Where-Object{$_-gt0}|Sort-Object -Unique)
    $completionValues=@($members|ForEach-Object{Required-Int64 $_ 'dwm_parent_completion_qpc' "parent $parentId"}|Where-Object{$_-gt0}|Sort-Object -Unique)
    if($attachValues.Count-ne1-or[long]$attachValues[0]-ne$parentId){Fail "parent attach identityが一致しません: $parentId"}
    if($displayValues.Count-gt1-or$completionValues.Count-gt1){Fail "parent display/completion identityが一意ではありません: $parentId"}
    $rawDisplayed=@($parentEvent.displayed);$rawDisplayQpc=if($rawDisplayed.Count-gt0){[int64]$rawDisplayed[0].qpc}else{0L}
    $displayQpc=if($displayValues.Count-eq1){[int64]$displayValues[0]}else{0L}
    $completionQpc=if($completionValues.Count-eq1){[int64]$completionValues[0]}else{0L}
    if($displayQpc-gt0-and$rawDisplayQpc-ne$displayQpc){Fail "child/parent DisplayedQPCが一致しません: $parentId"}
    $readyQpc=Required-Int64 $parentEvent 'ready_qpc' "parent $parentId"
    if($displayQpc-gt0-and($readyQpc-lt$parentId-or$displayQpc-lt$readyQpc-or$completionQpc-lt$displayQpc)){Fail "parent Start→Ready→Display→Completion順序が不正です: $parentId"}
    $parentRows+=[pscustomobject]@{
        parent_present_start_qpc=$parentId;parent_present_start_ordinal=Map-Qpc $parentId
        parent_ready_qpc=$readyQpc;parent_ready_ordinal=$(if($readyQpc-gt0){Map-Qpc $readyQpc}else{$null})
        parent_displayed_qpc=$displayQpc;parent_displayed_ordinal=$(if($displayQpc-gt0){Map-Qpc $displayQpc}else{$null})
        parent_completion_qpc=$completionQpc;parent_completion_ordinal=$(if($completionQpc-gt0){Map-Qpc $completionQpc}else{$null})
        start_to_ready_qpc=$(if($readyQpc-gt0){$readyQpc-$parentId}else{$null})
        start_to_display_qpc=$(if($displayQpc-gt0){$displayQpc-$parentId}else{$null})
        start_to_completion_qpc=$(if($completionQpc-gt0){$completionQpc-$parentId}else{$null})
        start_to_display_vblanks=$(if($displayQpc-gt0){(Map-Qpc $displayQpc)-(Map-Qpc $parentId)}else{$null})
        first_waiting_qpc=[int64]($waitingValues|Measure-Object -Minimum).Minimum
        last_waiting_qpc=[int64]($waitingValues|Measure-Object -Maximum).Maximum
        attach_qpc=[int64]$attachValues[0];dependent_count=$members.Count
        presented_count=@($members|Where-Object{[string]$_.completion_class-eq'PRESENTED'}).Count
        discarded_count=@($members|Where-Object{[string]$_.completion_class-eq'DISCARDED'}).Count
    }
}
$stageRows=@()
for($index=1;$index-lt$parentRows.Count;++$index){
    $previous=$parentRows[$index-1];$current=$parentRows[$index]
    $startGap=[int64]$current.parent_present_start_ordinal-[int64]$previous.parent_present_start_ordinal
    $displayGap=if($null-ne$current.parent_displayed_ordinal-and$null-ne$previous.parent_displayed_ordinal){[int64]$current.parent_displayed_ordinal-[int64]$previous.parent_displayed_ordinal}else{$null}
    $dwmBetween=@($dwmEventsAll|Where-Object{[int64]$_.present_start_qpc-gt[long]$previous.parent_present_start_qpc-and[int64]$_.present_start_qpc-le[long]$current.parent_present_start_qpc})
    $classification=if($current.dependent_count-ge60){
        if($startGap-ge60-and$dwmBetween.Count-eq1-and$null-ne$displayGap-and[Math]::Abs($startGap-$displayGap)-le1-and[long]$current.start_to_display_vblanks-le2){'DWM_WIDE_PARENT_PRESENTSTART_GAP'}
        elseif($dwmBetween.Count-ge[Math]::Max(2,$startGap-2)-and$startGap-ge60){'TARGET_ATTACH_SUPPRESSION'}
        elseif($startGap-le2-and$null-ne$displayGap-and$displayGap-ge60-and[long]$current.start_to_display_vblanks-ge60){'PARENT_SUBMIT_TO_SCANOUT_STALL'}
        else{'NOT_LOCALIZED'}
    }else{'NOT_LARGE_BATCH'}
    $stageRows+=[pscustomobject][ordered]@{
        previous_parent_present_start_qpc=[long]$previous.parent_present_start_qpc
        parent_present_start_qpc=[long]$current.parent_present_start_qpc;dependent_count=[long]$current.dependent_count
        parent_present_start_gap_qpc=[long]$current.parent_present_start_qpc-[long]$previous.parent_present_start_qpc
        parent_present_start_gap_vblanks=$startGap;parent_display_gap_vblanks=$displayGap
        parent_start_to_ready_qpc=$current.start_to_ready_qpc;parent_start_to_display_qpc=$current.start_to_display_qpc
        parent_start_to_completion_qpc=$current.start_to_completion_qpc;parent_start_to_display_vblanks=$current.start_to_display_vblanks
        first_waiting_qpc=[long]$current.first_waiting_qpc;last_waiting_qpc=[long]$current.last_waiting_qpc;attach_qpc=[long]$current.attach_qpc
        all_dwm_present_start_count_between=$dwmBetween.Count
        unattached_dwm_present_start_count_between=@($dwmBetween|Where-Object{-not$parentIdSet.ContainsKey([string][int64]$_.present_start_qpc)}).Count
        classification=$classification
    }
}
$largeStages=@($stageRows|Where-Object{$_.dependent_count-ge60})
$allLargeParents=@($parentRows|Where-Object{$_.dependent_count-ge60})
if($largeStages.Count-lt2){Fail 'stage localization対象のlarge batchが不足しています'}
if(@($largeStages|Where-Object{$_.classification-eq'NOT_LOCALIZED'}).Count-ne0){Fail 'large batchのstageをlocalizeできません'}
$classes=@($largeStages|Select-Object -ExpandProperty classification -Unique)
$verdict=if($classes.Count-eq1){[string]$classes[0]}else{'MIXED_STALL_STAGES'}
$next=if($verdict-eq'DWM_WIDE_PARENT_PRESENTSTART_GAP'){'F3-C3-A3-T1_VISIBILITY_OCCLUSION_DIRTY_STATE_CAUSAL_PROOF'}elseif($verdict-eq'PARENT_SUBMIT_TO_SCANOUT_STALL'){'F3-C3-A3-T1_DWM_PARENT_SUBMIT_TO_SCANOUT_ATTRIBUTION'}else{'F3-C3-A3-T1_TARGET_ATTACH_ELIGIBILITY_ATTRIBUTION'}
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
[ordered]@{
    schema='mvm-p2-c3-a3-t0-timeline-1';dwm_process_id=$dwmPid
    qpc_frequency=$qpcFrequency
    measurement_start_qpc=$measurementStart;measurement_end_qpc_exclusive=$measurementEnd
    parent_rows=$parentRows;stage_rows=$stageRows;dwm_present_timeline=$dwmTimeline
}|ConvertTo-Json -Depth 10|Set-Content -LiteralPath (Join-Path $OutputDirectory 'timeline.json') -Encoding utf8
[ordered]@{
    schema='mvm-p2-c3-a3-t0-summary-1';status='PASS';authority='diagnostic_offline'
    formal_counter_authority_changed=$false;formal_drop_threshold_changed=$false;production_scheduler_changed=$false
    raw_sufficiency='FULL_FOR_T0';dwm_process_identification='UNIQUE_PID_BY_ACTUAL_PARENT_IDENTITY';dwm_process_id=$dwmPid
    target_present_count=$records.Count;actual_target_parent_count=$parentRows.Count
    attached_target_parent_count_in_measurement=@($parentRows|Where-Object{[long]$_.parent_present_start_qpc-ge$measurementStart-and[long]$_.parent_present_start_qpc-lt$measurementEnd}).Count
    dwm_present_count_in_measurement=$dwmEventsInWindow.Count
    dwm_presented_count_in_measurement=@($dwmEventsInWindow|Where-Object{[string]$_.completion_class-eq'PRESENTED'}).Count
    dwm_discarded_count_in_measurement=@($dwmEventsInWindow|Where-Object{[string]$_.completion_class-eq'DISCARDED'}).Count
    dwm_lost_count_in_measurement=$dwmLost.Count
    unattached_dwm_present_count_in_measurement=@($dwmEventsInWindow|Where-Object{-not$parentIdSet.ContainsKey([string][int64]$_.present_start_qpc)}).Count
    large_batch_count=$allLargeParents.Count;localized_large_batch_count=$largeStages.Count
    boundary_large_batch_without_previous_target_parent_count=$allLargeParents.Count-$largeStages.Count
    large_batch_localization=$largeStages;verdict=$verdict;next=$next
    verdict_scope='OBSERVED_DWM_PRESENT_STREAM_GAP; CPU_OR_GPU_HANG_NOT_CLAIMED'
    lifecycle_ordinal_mapping='OBSERVED_WINDOW_WITH_CALIBRATED_TAIL_EXTRAPOLATION'
    lifecycle_qpc_outside_observed_sample_count=$outsideSampleCount
    identities=[ordered]@{
        traced_app_sha256=Hash $appPath;present_history_raw_sha256=Hash $rawPath
        oracle_sha256=Hash $oraclePath;canonical_summary_sha256=Hash $canonicalSummaryPath
        canonical_manifest_sha256=Hash $canonicalManifestPath;analyzer_sha256=Hash $PSCommandPath
    }
}|ConvertTo-Json -Depth 10|Set-Content -LiteralPath (Join-Path $OutputDirectory 'summary.json') -Encoding utf8
$manifest=Join-Path $OutputDirectory 'manifest.sha256'
Get-ChildItem -LiteralPath $OutputDirectory -File|Where-Object{$_.FullName-ne$manifest}|Sort-Object Name|ForEach-Object{"$(Hash $_.FullName)  $($_.Name)"}|Set-Content -LiteralPath $manifest -Encoding ascii
Write-Host "F3-C3-A3-T0: PASS verdict=$verdict large_batches=$($largeStages.Count)"
