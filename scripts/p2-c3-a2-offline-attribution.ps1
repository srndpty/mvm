[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string[]]$SourceDirectories,
    [Parameter(Mandatory=$true)][string]$OutputDirectory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
function Percentile([long[]]$Values,[double]$P){
    if($Values.Count-eq0){return 0L}
    $sorted=@($Values|Sort-Object);$index=[Math]::Ceiling($P*$sorted.Count)-1
    return [int64]$sorted[[Math]::Max(0,$index)]
}
if($SourceDirectories.Count-ne3){Fail 'F3-C3-A2 offline解析にはcounterbalanced source runが3件必要です'}
if(Test-Path -LiteralPath $OutputDirectory){Fail "既存F3-C3-A2 artifactを上書きしません: $OutputDirectory"}
$modes=@('CONTROL','FRAME_LATENCY_1','DWM_FLUSH_AFTER_PRESENT')
$pathByMode=@{CONTROL='control';FRAME_LATENCY_1='frame_latency_1';DWM_FLUSH_AFTER_PRESENT='dwm_flush_after_present'}
$expectedOrders=@(
    'CONTROL,FRAME_LATENCY_1,DWM_FLUSH_AFTER_PRESENT',
    'FRAME_LATENCY_1,DWM_FLUSH_AFTER_PRESENT,CONTROL',
    'DWM_FLUSH_AFTER_PRESENT,CONTROL,FRAME_LATENCY_1')
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
$runResults=@();$sourceIdentities=@()
for($sourceIndex=0;$sourceIndex-lt3;++$sourceIndex){
    $source=(Resolve-Path -LiteralPath $SourceDirectories[$sourceIndex]).Path
    $sourceSummaryPath=Join-Path $source 'summary.json'
    $sourceSummary=Get-Content -LiteralPath $sourceSummaryPath -Raw -Encoding utf8|ConvertFrom-Json
    $order=@($sourceSummary.execution_order)-join','
    if($order-ne$expectedOrders[$sourceIndex]){Fail "counterbalanced orderが不正です: run=$($sourceIndex+1) actual=$order"}
    $sourceIdentities+=[ordered]@{run=$sourceIndex+1;directory=$source;summary_sha256=Hash $sourceSummaryPath;execution_order=@($sourceSummary.execution_order)}
    foreach($mode in $modes){
        $canonical=Join-Path $source $pathByMode[$mode]
        $oraclePath=Join-Path $canonical 'oracle.json';$appPath=Join-Path $canonical 'traced-app.json'
        $oracle=Get-Content -LiteralPath $oraclePath -Raw -Encoding utf8|ConvertFrom-Json
        $app=Get-Content -LiteralPath $appPath -Raw -Encoding utf8|ConvertFrom-Json
        if([string]$oracle.oracle_status-ne'ORACLE_VALID'-or[string]$oracle.display_completion_status-ne'CLOSED'){Fail "oracleが閉じていません: run=$($sourceIndex+1) mode=$mode"}
        foreach($field in @('incomplete_unknown_count','lost_count','etw_events_lost','etw_buffers_lost','present_event_overflow_count')){
            if([int64]$oracle.$field-ne0){Fail "$field が0ではありません: run=$($sourceIndex+1) mode=$mode"}
        }
        $records=@($oracle.records);if($records.Count-lt2){Fail 'Present recordが不足しています'}
        $samples=@($app.presentation_opportunity.physical_vblank.samples);if($samples.Count-lt2){Fail 'physical VBlank sampleが不足しています'}
        $starts=@{};$previousSequence=-1L
        foreach($record in $records){
            $sequence=[int64]$record.sequence_index
            if($sequence-ne$previousSequence+1){Fail "sequenceがstrictではありません: $sequence"};$previousSequence=$sequence
            if([string]::IsNullOrWhiteSpace([string]$record.present_mode)){Fail "PresentModeがありません: $sequence"}
            $batch=[int64]$record.dependency_batch_present_start_qpc
            if($batch-le0){Fail "dependency batch identityがありません: $sequence"}
            $starts[[string][int64]$record.etw_present_start_qpc]=$record
        }
        function Bracket([int64]$Qpc){
            for($i=0;$i+1-lt$samples.Count;++$i){if([int64]$samples[$i].qpc-le$Qpc-and$Qpc-lt[int64]$samples[$i+1].qpc){return [int64]$samples[$i].ordinal}}
            return $null
        }
        $modeHistogram=[ordered]@{};$classByMode=[ordered]@{};$transitions=@()
        for($i=0;$i-lt$records.Count;++$i){
            $presentMode=[string]$records[$i].present_mode
            if(-not$modeHistogram.Contains($presentMode)){$modeHistogram[$presentMode]=0L;$classByMode[$presentMode]=[ordered]@{presented=0L;discarded=0L}}
            $modeHistogram[$presentMode]=[int64]$modeHistogram[$presentMode]+1
            $class=if([string]$records[$i].completion_class-eq'PRESENTED'){'presented'}else{'discarded'}
            $classByMode[$presentMode][$class]=[int64]$classByMode[$presentMode][$class]+1
            if($i-gt0-and$presentMode-ne[string]$records[$i-1].present_mode){$transitions+=[ordered]@{sequence=$i;present_serial=[string]$records[$i].present_serial;qpc=[int64]$records[$i].etw_present_start_qpc;from=[string]$records[$i-1].present_mode;to=$presentMode;vblank_ordinal=Bracket ([int64]$records[$i].etw_present_start_qpc)}}
        }
        $batchRows=@()
        foreach($group in @($records|Group-Object dependency_batch_present_start_qpc)){
            $members=@($group.Group|Sort-Object {[int64]$_.sequence_index});$batchQpc=[int64]$group.Name
            $presented=@($members|Where-Object{[string]$_.completion_class-eq'PRESENTED'})
            $parentRecord=if($starts.ContainsKey([string]$batchQpc)){$starts[[string]$batchQpc]}else{$null}
            $displaySource=if($presented.Count-gt0){$presented[0]}elseif($null-ne$parentRecord-and[string]$parentRecord.completion_class-eq'PRESENTED'){$parentRecord}else{$null}
            $displayedQpc=if($null-ne$displaySource-and@($displaySource.displayed_qpc).Count-gt0){[int64]@($displaySource.displayed_qpc)[0]}else{$null}
            $displayedOrdinal=if($null-ne$displaySource-and@($displaySource.actual_opportunity_ordinals).Count-gt0){[int64]@($displaySource.actual_opportunity_ordinals)[0]}else{$null}
            $first=$members[0];$last=$members[-1]
            $batchRows+=[ordered]@{
                batch_identity_qpc=$batchQpc
                identity_kind=if($null-ne$parentRecord){'MATCHES_APP_PRESENT'}else{'NON_APP_PARENT_CANDIDATE'}
                first_app_present_serial=[string]$first.present_serial;first_app_present_qpc=[int64]$first.etw_present_start_qpc
                last_app_present_serial=[string]$last.present_serial;last_app_present_qpc=[int64]$last.etw_present_start_qpc
                dependent_count=$members.Count;presented_count=$presented.Count;discarded_count=$members.Count-$presented.Count
                present_modes=@($members.present_mode|Sort-Object -Unique)
                parent_present_start_qpc=$batchQpc;parent_present_start_vblank_ordinal=Bracket $batchQpc
                parent_displayed_qpc=$displayedQpc;parent_displayed_vblank_ordinal=$displayedOrdinal
                parent_display_vblank_delta=$null
                parent_completion_qpc=$null;parent_completion_qpc_available=$false
                first_app_to_parent_display_vblanks=if($null-ne$displayedOrdinal-and$null-ne$first.enter_bracket_ordinal){$displayedOrdinal-[int64]$first.enter_bracket_ordinal}else{$null}
                last_app_to_parent_display_vblanks=if($null-ne$displayedOrdinal-and$null-ne$last.enter_bracket_ordinal){$displayedOrdinal-[int64]$last.enter_bracket_ordinal}else{$null}
            }
        }
        $displayedBatches=@($batchRows|Where-Object{$null-ne$_.parent_displayed_vblank_ordinal}|Sort-Object {[int64]$_.parent_displayed_vblank_ordinal})
        for($i=0;$i-lt$displayedBatches.Count;++$i){
            $displayedBatches[$i]['parent_display_vblank_delta']=if($i-eq0){$null}else{[int64]$displayedBatches[$i].parent_displayed_vblank_ordinal-[int64]$displayedBatches[$i-1].parent_displayed_vblank_ordinal}
        }
        $displayGroups=@();$groupedByDisplay=@($displayedBatches|Group-Object {[int64]$_.parent_displayed_vblank_ordinal}|Sort-Object {[int64]$_.Name})
        $largeDisplayGroups=0L;$largeDisplayGapComparable=0L
        for($i=0;$i-lt$groupedByDisplay.Count;++$i){
            $ordinal=[int64]$groupedByDisplay[$i].Name
            $dependent=[int64](($groupedByDisplay[$i].Group|ForEach-Object{[int64]$_.dependent_count}|Measure-Object -Sum).Sum)
            $delta=if($i-eq0){$null}else{$ordinal-[int64]$groupedByDisplay[$i-1].Name}
            $comparable=$null
            if($dependent-gt1-and$null-ne$delta){
                $largeDisplayGroups++
                $comparable=[Math]::Abs([int64]$delta-$dependent)-le1
                if($comparable){$largeDisplayGapComparable++}
            }
            $displayGroups+=[ordered]@{displayed_vblank_ordinal=$ordinal;batch_identity_count=$groupedByDisplay[$i].Count;dependent_count=$dependent;previous_display_vblank_delta=$delta;dependent_count_matches_display_gap_within_one=$comparable}
        }
        $presentedTimeline=@($records|Where-Object{[string]$_.completion_class-eq'PRESENTED'}|Sort-Object {[int64]@($_.actual_opportunity_ordinals)[0]})
        $gapPairs=0L;$gapExact=0L;$gapMismatch=0L;$gapMismatches=@()
        for($i=1;$i-lt$presentedTimeline.Count;++$i){
            $sourceGap=[int64]$presentedTimeline[$i].output_frame-[int64]$presentedTimeline[$i-1].output_frame-1
            $physicalGap=[int64]@($presentedTimeline[$i].actual_opportunity_ordinals)[0]-[int64]@($presentedTimeline[$i-1].actual_opportunity_ordinals)[0]-1
            $gapPairs++;if($sourceGap-eq$physicalGap){$gapExact++}else{$gapMismatch++;$gapMismatches+=[ordered]@{previous_output_frame=[int64]$presentedTimeline[$i-1].output_frame;output_frame=[int64]$presentedTimeline[$i].output_frame;source_gap=$sourceGap;previous_physical_ordinal=[int64]@($presentedTimeline[$i-1].actual_opportunity_ordinals)[0];physical_ordinal=[int64]@($presentedTimeline[$i].actual_opportunity_ordinals)[0];physical_gap=$physicalGap}}
        }
        $sourceSpan=if($presentedTimeline.Count-ge2){[int64]$presentedTimeline[-1].output_frame-[int64]$presentedTimeline[0].output_frame}else{0L}
        $physicalSpan=if($presentedTimeline.Count-ge2){[int64]@($presentedTimeline[-1].actual_opportunity_ordinals)[0]-[int64]@($presentedTimeline[0].actual_opportunity_ordinals)[0]}else{0L}
        $batchSizes=[long[]]@($batchRows.dependent_count|ForEach-Object{[int64]$_})
        $result=[ordered]@{
            schema='mvm-p2-c3-a2-offline-run-1';authority='diagnostic_offline';run=$sourceIndex+1
            position=[Array]::IndexOf(@($sourceSummary.execution_order),$mode)+1;submission_mode=$mode
            present_count=$records.Count;presented_count=[int64]$oracle.presented_count;discarded_count=[int64]$oracle.discarded_count
            present_mode_histogram=$modeHistogram;present_mode_completion=$classByMode
            present_mode_transition_count=$transitions.Count;present_mode_transitions=$transitions
            dependency_batch_count=$batchRows.Count;dependency_batch_size=[ordered]@{p50=Percentile $batchSizes .5;p95=Percentile $batchSizes .95;max=Percentile $batchSizes 1.0}
            app_identity_batch_count=@($batchRows|Where-Object{$_.identity_kind-eq'MATCHES_APP_PRESENT'}).Count
            non_app_parent_candidate_batch_count=@($batchRows|Where-Object{$_.identity_kind-eq'NON_APP_PARENT_CANDIDATE'}).Count
            parent_display_available_batch_count=$displayedBatches.Count
            parent_display_group_count=$displayGroups.Count
            duplicate_parent_display_batch_identity_count=$displayedBatches.Count-$displayGroups.Count
            large_parent_display_group_count=$largeDisplayGroups
            large_parent_display_gap_comparable_count=$largeDisplayGapComparable
            large_parent_display_gap_mismatch_count=$largeDisplayGroups-$largeDisplayGapComparable
            parent_completion_qpc_available_batch_count=0
            physical_source_gap_pair_count=$gapPairs;physical_source_gap_exact_count=$gapExact;physical_source_gap_mismatch_count=$gapMismatch
            physical_source_gap_mismatches=$gapMismatches
            displayed_source_span=$sourceSpan;displayed_physical_vblank_span=$physicalSpan
            displayed_source_physical_span_difference=$sourceSpan-$physicalSpan
            lifecycle_field_availability=[ordered]@{app_present_qpc=$true;waiting_for_dwm_qpc=$false;attached_to_parent_qpc=$false;parent_identity=$true;parent_displayed_qpc=$true;parent_completion_qpc=$false;dependent_finalized_qpc=$false}
            batches=$batchRows
            parent_display_groups=$displayGroups
            identities=[ordered]@{oracle_sha256=Hash $oraclePath;app_sha256=Hash $appPath}
        }
        $runPath=Join-Path $OutputDirectory ("run{0}-{1}.json" -f ($sourceIndex+1),$pathByMode[$mode])
        $result|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $runPath -Encoding utf8
        $runResults+=$result
    }
}
$allModes=@($runResults|ForEach-Object{@($_.present_mode_histogram.Keys)}|Sort-Object -Unique)
$allTransitionCount=[int64](($runResults|ForEach-Object{[int64]$_.present_mode_transition_count}|Measure-Object -Sum).Sum)
$allGapPairs=[int64](($runResults|ForEach-Object{[int64]$_.physical_source_gap_pair_count}|Measure-Object -Sum).Sum)
$allGapExact=[int64](($runResults|ForEach-Object{[int64]$_.physical_source_gap_exact_count}|Measure-Object -Sum).Sum)
$allLargeGroups=[int64](($runResults|ForEach-Object{[int64]$_.large_parent_display_group_count}|Measure-Object -Sum).Sum)
$allLargeComparable=[int64](($runResults|ForEach-Object{[int64]$_.large_parent_display_gap_comparable_count}|Measure-Object -Sum).Sum)
$spanComparableRuns=@($runResults|Where-Object{$_.presented_count-ge2-and[Math]::Abs([int64]$_.displayed_source_physical_span_difference)-le1}).Count
[ordered]@{
    schema='mvm-p2-c3-a2-offline-summary-1';status='PASS';authority='diagnostic_offline'
    formal_counter_authority_changed=$false;formal_drop_threshold_changed=$false;production_scheduler_changed=$false
    source_runs=$sourceIdentities;run_count=$runResults.Count
    observed_present_modes=$allModes;present_mode_transition_count=$allTransitionCount
    all_runs_single_present_mode=($allModes.Count-eq1-and$allTransitionCount-eq0)
    physical_source_gap_pair_count=$allGapPairs;physical_source_gap_exact_count=$allGapExact
    physical_source_gap_mismatch_count=$allGapPairs-$allGapExact
    large_parent_display_group_count=$allLargeGroups
    large_parent_display_gap_comparable_count=$allLargeComparable
    large_parent_display_gap_mismatch_count=$allLargeGroups-$allLargeComparable
    displayed_source_physical_span_comparable_run_count=$spanComparableRuns
    full_dependency_lifecycle_reconstructable=$false
    missing_lifecycle_fields=@('waiting_for_dwm_qpc','attached_to_parent_qpc','parent_completion_qpc','dependent_finalized_qpc')
    offline_exit='LIFECYCLE_INSTRUMENTATION_REQUIRED'
    preliminary_branch='B_DWM_PARENT_DISPLAY_GAP_WITHIN_SINGLE_COMPOSED_FLIP_REGIME'
}|ConvertTo-Json -Depth 10|Set-Content -LiteralPath (Join-Path $OutputDirectory 'summary.json') -Encoding utf8
$manifest=Join-Path $OutputDirectory 'manifest.sha256'
Get-ChildItem -LiteralPath $OutputDirectory -File|Where-Object{$_.FullName-ne$manifest}|Sort-Object Name|ForEach-Object{"$(Hash $_.FullName)  $($_.Name)"}|Set-Content -LiteralPath $manifest -Encoding ascii
Write-Host "F3-C3-A2 offline attribution: PASS modes=$($allModes-join',') transitions=$allTransitionCount lifecycle=INCOMPLETE"
