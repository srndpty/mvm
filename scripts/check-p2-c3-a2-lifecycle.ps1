[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$AppJson,
    [Parameter(Mandatory=$true)][string]$OracleJson,
    [Parameter(Mandatory=$true)][string]$Output
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
function Field($Object,[string]$Name,[int]$Index){
    if($Object.PSObject.Properties.Name-notcontains$Name){Fail "lifecycle fieldがありません: ${Name}[$Index]"}
    return [int64]$Object.$Name
}
$app=Get-Content -LiteralPath $AppJson -Raw -Encoding utf8|ConvertFrom-Json
$oracle=Get-Content -LiteralPath $OracleJson -Raw -Encoding utf8|ConvertFrom-Json
if([string]$oracle.schema-ne'mvm-p2-c0-native-etw-oracle-1'-or[string]$oracle.oracle_status-ne'ORACLE_VALID'){Fail 'canonical oracleが有効ではありません'}
if(-not[bool]$oracle.dependency_lifecycle_diagnostic){Fail 'dependency lifecycle診断decoderではありません'}
foreach($field in @('incomplete_unknown_count','lost_count','etw_events_lost','etw_buffers_lost','present_event_overflow_count')){
    if([int64]$oracle.$field-ne0){Fail "$field が0ではありません"}
}
$records=@($oracle.records);if($records.Count-lt2){Fail 'lifecycle recordが不足しています'}
$samples=@($app.presentation_opportunity.physical_vblank.samples);if($samples.Count-lt120){Fail 'physical VBlank sampleが不足しています'}
$qpcFrequency=[int64]$app.presentation_opportunity.qpc_frequency
$identity=$app.presentation_opportunity.physical_vblank.window_output_start
$refreshNumerator=[int64]$identity.refresh_numerator;$refreshDenominator=[int64]$identity.refresh_denominator
$periodScaled=[decimal]$qpcFrequency*[decimal]$refreshDenominator
$phases=@(0..119|ForEach-Object{([decimal][int64]$samples[$_].qpc*[decimal]$refreshNumerator)-([decimal][int64]$samples[$_].ordinal*$periodScaled)}|Sort-Object)
$originScaled=[decimal]$phases[59]
$outsideSampleQpcCount=0L
function Map-Qpc([int64]$Qpc){
    $relative=([decimal]$Qpc*[decimal]$refreshNumerator)-$originScaled
    $floor=[decimal]::Floor($relative/$periodScaled);$remainder=$relative-$floor*$periodScaled
    if($remainder*2-eq$periodScaled){Fail "lifecycle QPCがVBlank境界で曖昧です: $Qpc"}
    $ordinal=if($remainder*2-lt$periodScaled){$floor}else{$floor+1}
    if($ordinal-lt[decimal][int64]$samples[0].ordinal-or$ordinal-gt[decimal][int64]$samples[-1].ordinal){$script:outsideSampleQpcCount++}
    return [int64]$ordinal
}
$rows=@();$modeTransitions=@();$previousMode=$null;$earlierOnlyCount=0L;$identityOverwriteCount=0L
for($index=0;$index-lt$records.Count;++$index){
    $record=$records[$index];$mode=[string]$record.present_mode
    if([string]::IsNullOrWhiteSpace($mode)){Fail "PresentModeがありません: $index"}
    if($null-ne$previousMode-and$mode-ne$previousMode){$modeTransitions+=[ordered]@{sequence=$index;qpc=[int64]$record.etw_present_start_qpc;from=$previousMode;to=$mode}}
    $previousMode=$mode
    $start=[int64]$record.etw_present_start_qpc
    $waiting=Field $record 'waiting_for_dwm_qpc' $index
    $attached=Field $record 'attached_to_dwm_parent_qpc' $index
    $parent=Field $record 'attached_dwm_parent_present_start_qpc' $index
    $displayed=Field $record 'dwm_parent_displayed_qpc' $index
    $completion=Field $record 'dwm_parent_completion_qpc' $index
    $finalized=Field $record 'dependent_finalized_qpc' $index
    $earlierParent=Field $record 'earlier_superseded_by_present_start_qpc' $index
    $earlierQpc=Field $record 'earlier_superseded_qpc' $index
    $legacyBatch=[int64]$record.dependency_batch_present_start_qpc
    if($start-le0-or$waiting-lt$start-or$attached-lt$waiting-or$parent-ne$attached-or$finalized-lt$attached){Fail "APP→WAITING→ATTACHED→FINALIZED順序が不正です: $index"}
    if($legacyBatch-ne$parent){$identityOverwriteCount++}
    if($completion-gt0){
        if($displayed-lt$attached-or$completion-lt$displayed-or$finalized-ne$completion){Fail "DWM parent display/completion順序が不正です: $index"}
    }else{
        $earlierOnlyCount++
        if($displayed-ne0-or[string]$record.discard_reason-ne'EARLIER_SWAPCHAIN_PRESENT_SUPERSEDED'-or$earlierParent-le0-or$earlierQpc-lt$attached-or$finalized-ne$earlierQpc){Fail "earlier supersede lifecycleが閉じていません: $index"}
    }
    $rows+=[pscustomobject]@{
        sequence=$index;present_serial=[string]$record.present_serial;mode=$mode
        completion_class=[string]$record.completion_class;discard_reason=[string]$record.discard_reason
        start_qpc=$start;waiting_qpc=$waiting;attached_qpc=$attached;parent_qpc=$parent
        displayed_qpc=$displayed;completion_qpc=$completion;finalized_qpc=$finalized
        start_ordinal=Map-Qpc $start;waiting_ordinal=Map-Qpc $waiting;attached_ordinal=Map-Qpc $attached
        displayed_ordinal=$(if($displayed-gt0){Map-Qpc $displayed}else{$null})
        finalized_ordinal=Map-Qpc $finalized
        output_frame=[int64]$record.output_frame
        actual_display_ordinal=$(if(@($record.actual_opportunity_ordinals).Count-gt0){[int64]@($record.actual_opportunity_ordinals)[0]}else{$null})
    }
}
$batches=@()
foreach($group in @($rows|Group-Object parent_qpc|Sort-Object {[int64]$_.Name})){
    $members=@($group.Group|Sort-Object sequence);$first=$members[0];$last=$members[-1]
    $displayOrdinals=@($members|Where-Object{$null-ne$_.displayed_ordinal}|Select-Object -ExpandProperty displayed_ordinal -Unique)
    $completionQpcs=@($members|Where-Object{$_.completion_qpc-gt0}|Select-Object -ExpandProperty completion_qpc -Unique)
    if($displayOrdinals.Count-gt1-or$completionQpcs.Count-gt1){Fail "同一DWM parent内でcompletion identityが不一致です: $($group.Name)"}
    $displayOrdinal=if($displayOrdinals.Count-eq1){[int64]$displayOrdinals[0]}else{$null}
    $batches+=[pscustomobject]@{
        parent_present_start_qpc=[int64]$group.Name;first_app_sequence=$first.sequence;last_app_sequence=$last.sequence
        first_app_present_qpc=$first.start_qpc;last_app_present_qpc=$last.start_qpc
        dependent_count=$members.Count;presented_count=@($members|Where-Object{$_.completion_class-eq'PRESENTED'}).Count
        discarded_count=@($members|Where-Object{$_.completion_class-eq'DISCARDED'}).Count
        parent_attached_qpc=$first.attached_qpc;parent_displayed_qpc=$(if($displayOrdinal){$first.displayed_qpc}else{0})
        parent_completion_qpc=$(if($completionQpcs.Count-eq1){[int64]$completionQpcs[0]}else{0})
        first_app_ordinal=$first.start_ordinal;last_app_ordinal=$last.start_ordinal;parent_display_ordinal=$displayOrdinal
        first_app_to_parent_display_vblanks=$(if($null-ne$displayOrdinal){$displayOrdinal-$first.start_ordinal}else{$null})
        last_app_to_parent_display_vblanks=$(if($null-ne$displayOrdinal){$displayOrdinal-$last.start_ordinal}else{$null})
    }
}
$displayedBatches=@($batches|Where-Object{$null-ne$_.parent_display_ordinal}|Sort-Object parent_display_ordinal)
$comparisons=@();$parentGapProofCount=0L
for($index=1;$index-lt$displayedBatches.Count;++$index){
    $current=$displayedBatches[$index];$previous=$displayedBatches[$index-1]
    $gap=[int64]$current.parent_display_ordinal-[int64]$previous.parent_display_ordinal
    $difference=$gap-[int64]$current.dependent_count
    if($current.dependent_count-ge60-and[Math]::Abs($difference)-le1){$parentGapProofCount++}
    $comparisons+=[ordered]@{previous_parent_qpc=$previous.parent_present_start_qpc;current_parent_qpc=$current.parent_present_start_qpc;physical_vblank_delta=$gap;current_dependent_count=$current.dependent_count;delta_minus_dependent_count=$difference}
}
$presented=@($rows|Where-Object{$_.completion_class-eq'PRESENTED'}|Sort-Object sequence)
$sourcePhysicalPairs=0L;$sourcePhysicalExact=0L;$sourcePhysicalMaxAbsDifference=0L
for($index=1;$index-lt$presented.Count;++$index){
    $sourceGap=[int64]$presented[$index].output_frame-[int64]$presented[$index-1].output_frame-1
    $physicalGap=[int64]$presented[$index].actual_display_ordinal-[int64]$presented[$index-1].actual_display_ordinal-1
    $difference=[Math]::Abs($sourceGap-$physicalGap)
    $sourcePhysicalPairs++;if($sourceGap-eq$physicalGap){$sourcePhysicalExact++}
    if($difference-gt$sourcePhysicalMaxAbsDifference){$sourcePhysicalMaxAbsDifference=$difference}
}
$sourceSpan=if($presented.Count-ge2){[int64]$presented[-1].output_frame-[int64]$presented[0].output_frame}else{0L}
$physicalSpan=if($presented.Count-ge2){[int64]$presented[-1].actual_display_ordinal-[int64]$presented[0].actual_display_ordinal}else{0L}
$branch=if($modeTransitions.Count-gt0){'A_PRESENT_MODE_TRANSITION'}elseif($parentGapProofCount-gt0){'B_DWM_CONSUMPTION_STALL'}else{'C_DISPLAY_AUTHORITY_INVESTIGATION'}
$result=[ordered]@{
    schema='mvm-p2-c3-a2-lifecycle-proof-1';status='PASS';authority='diagnostic_only'
    formal_counter_authority_changed=$false;formal_drop_threshold_changed=$false;production_scheduler_changed=$false
    record_count=$rows.Count;presented_count=$presented.Count;discarded_count=@($rows|Where-Object{$_.completion_class-eq'DISCARDED'}).Count
    present_modes=@($rows|Group-Object mode|ForEach-Object{[ordered]@{mode=$_.Name;count=$_.Count}})
    present_mode_transitions=$modeTransitions;dependency_batch_count=$batches.Count
    max_dependency_batch_size=($batches|Measure-Object dependent_count -Maximum).Maximum
    earlier_only_count=$earlierOnlyCount;legacy_dependency_identity_overwrite_count=$identityOverwriteCount
    displayed_parent_count=$displayedBatches.Count;parent_display_pair_count=$comparisons.Count
    large_parent_gap_proof_count=$parentGapProofCount
    source_physical_gap_pair_count=$sourcePhysicalPairs;source_physical_gap_exact_count=$sourcePhysicalExact
    source_physical_gap_mismatch_count=$sourcePhysicalPairs-$sourcePhysicalExact
    source_physical_gap_max_abs_difference=$sourcePhysicalMaxAbsDifference
    displayed_source_span=$sourceSpan;displayed_physical_span=$physicalSpan
    displayed_source_minus_physical_span=$sourceSpan-$physicalSpan
    lifecycle_ordinal_mapping='OBSERVED_WINDOW_WITH_CALIBRATED_TAIL_EXTRAPOLATION'
    lifecycle_qpc_outside_observed_sample_count=$outsideSampleQpcCount
    branch=$branch;batches=$batches;parent_display_comparisons=$comparisons
}
$result|ConvertTo-Json -Depth 10|Set-Content -LiteralPath $Output -Encoding utf8
Write-Host "F3-C3-A2 lifecycle: PASS branch=$branch batches=$($batches.Count) max=$($result.max_dependency_batch_size)"
