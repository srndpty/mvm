[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OracleJson,
    [Parameter(Mandatory=$true)][string]$AppJson,
    [Parameter(Mandatory=$true)][ValidateSet('CONTROL','DWM_FLUSH_AFTER_PRESENT','FRAME_LATENCY_1')]
    [string]$SubmissionMode,
    [Parameter(Mandatory=$true)][string]$Output
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
function Equal($Actual,$Expected,[string]$Name){if($Actual-ne$Expected){Fail "$Name が一致しません (expected=$Expected actual=$Actual)"}}
function Percentile([long[]]$Values,[double]$P){
    if($Values.Count-eq0){return 0L}
    $sorted=@($Values|Sort-Object);$index=[Math]::Ceiling($P*$sorted.Count)-1
    return [int64]$sorted[[Math]::Max(0,$index)]
}
$oracle=Get-Content -LiteralPath $OracleJson -Raw -Encoding utf8|ConvertFrom-Json
$app=Get-Content -LiteralPath $AppJson -Raw -Encoding utf8|ConvertFrom-Json
Equal ([string]$oracle.schema) 'mvm-p2-c0-native-etw-oracle-1' 'oracle schema'
Equal ([string]$oracle.oracle_status) 'ORACLE_VALID' 'oracle status'
Equal ([string]$oracle.display_completion_status) 'CLOSED' 'display completion status'
Equal ([bool]$oracle.formal_counter_authority_changed) $false 'formal authority'
foreach($field in @('incomplete_unknown_count','lost_count','etw_events_lost','etw_buffers_lost','present_event_overflow_count')){
    Equal ([int64]$oracle.$field) 0 $field
}
$records=@($oracle.records)
Equal $records.Count ([int64]$oracle.native_present_count) 'native Present count'
Equal $records.Count ([int64]$oracle.etw_present_count) 'ETW Present count'
Equal $records.Count ([int64]$oracle.composition_token_join_exact_count) 'composition token exact join count'
if($records.Count-lt2){Fail 'C3-A Present recordが不足しています'}
$hook=$app.native_present_hook
$expectedMode=@{CONTROL=0;DWM_FLUSH_AFTER_PRESENT=1;FRAME_LATENCY_1=2}[$SubmissionMode]
Equal ([int]$hook.submission_mode) $expectedMode 'submission mode'
Equal ([bool]$hook.frame_latency_waitable_object_available) $true 'frame latency waitable object'
$expectedLatency=if($SubmissionMode-eq'FRAME_LATENCY_1'){1}else{2}
Equal ([int]$hook.configured_maximum_frame_latency) $expectedLatency 'configured maximum frame latency'
Equal ([int]$hook.swapchain_maximum_frame_latency) $expectedLatency 'swapchain maximum frame latency'
Equal ([int64]$hook.dwm_flush_failure_count) 0 'DwmFlush failure count'
$expectedFlush=if($SubmissionMode-eq'DWM_FLUSH_AFTER_PRESENT'){$records.Count}else{0}
Equal ([int64]$hook.dwm_flush_call_count) $expectedFlush 'DwmFlush call count'
$allowed=@('BACK_TO_BACK_FLIP_SUPERSEDED','WIN32K_TOKEN_NOT_IN_FRAME','DEPENDENT_PRESENT_SUPERSEDED','DO_NOT_SEQUENCE','NOT_VISIBLE','BLIT_CANCEL','OTHER_EXPLICIT_DISCARD','EARLIER_SWAPCHAIN_PRESENT_SUPERSEDED')
$reasons=[ordered]@{};foreach($reason in $allowed){$reasons[$reason]=0L}
$batches=@{};$presentedFrames=New-Object System.Collections.Generic.List[long]
$discarded=0L;$unattributedSupersede=0L
foreach($record in $records){
    $class=[string]$record.completion_class;$reason=[string]$record.discard_reason
    if($class-eq'PRESENTED'){
        Equal $reason 'NONE' "Presented discard reason[$($record.sequence_index)]"
        $presentedFrames.Add([int64]$record.output_frame)
    }elseif($class-eq'DISCARDED'){
        $discarded++
        if($reason-notin$allowed){Fail "unknown discard reasonです: $reason"}
        $reasons[$reason]=[int64]$reasons[$reason]+1
    }else{Fail "completion classが閉じていません: $class"}
    $batch=[int64]$record.dependency_batch_present_start_qpc
    if($batch-gt0){$key=[string]$batch;$batches[$key]=if($batches.ContainsKey($key)){[int64]$batches[$key]+1}else{1L}}
    elseif($reason-in@('DEPENDENT_PRESENT_SUPERSEDED','EARLIER_SWAPCHAIN_PRESENT_SUPERSEDED')){$unattributedSupersede++}
}
Equal $discarded ([int64]$oracle.discarded_count) 'discarded count'
Equal $unattributedSupersede 0 'supersede batch attribution missing count'
$batchSizes=[long[]]@($batches.Values|ForEach-Object{[int64]$_})
$uniqueFrames=@($presentedFrames|Sort-Object -Unique)
$sourceDomainSize=[int64]$app.required_measurement_frame_count
if($sourceDomainSize-le0){Fail 'source frame domain sizeが不正です'}
$sourceGapDrops=0L;$nextFrame=0L
foreach($frame in $uniqueFrames){
    $frame=[int64]$frame
    if($frame-lt0-or$frame-ge$sourceDomainSize){Fail "displayed source frameがdomain外です: $frame"}
    if($frame-ge$nextFrame){$sourceGapDrops+=$frame-$nextFrame;$nextFrame=$frame+1}
}
$tailDrops=[Math]::Max(0L,$sourceDomainSize-$nextFrame)
$formalSourceDrops=$sourceGapDrops+$tailDrops
Equal ($uniqueFrames.Count+$formalSourceDrops) $sourceDomainSize 'source frame domain accounting'
$opportunity=$app.presentation_opportunity
$elapsed=([double][int64]$opportunity.measurement_end_qpc_exclusive-[double][int64]$opportunity.measurement_start_qpc)/[double][int64]$opportunity.qpc_frequency
if($elapsed-le0){Fail 'measurement elapsedが不正です'}
$physical=@($opportunity.physical_vblank.samples)
if($physical.Count-lt2){Fail 'physical VBlank sampleが不足しています'}
$physicalElapsed=([double][int64]$physical[-1].qpc-[double][int64]$physical[0].qpc)/[double][int64]$opportunity.qpc_frequency
if($physicalElapsed-le0){Fail 'physical VBlank elapsedが不正です'}
[ordered]@{
    schema='mvm-p2-c3-submission-backpressure-proof-2';proof_status='PASS'
    authority='diagnostic_only';formal_counter_authority_changed=$false
    submission_mode=$SubmissionMode;native_present_count=$records.Count
    presented_count=[int64]$oracle.presented_count;discarded_count=$discarded
    discard_reason_histogram=$reasons;unknown_lost_overflow_count=0
    source_domain_size=$sourceDomainSize;displayed_unique_source_frame_count=$uniqueFrames.Count
    source_frame_gap_drops=$sourceGapDrops;tail_source_frame_drops=$tailDrops
    formal_source_frame_drops=$formalSourceDrops;source_frame_accounting_exact=$true
    present_cadence_per_second=$records.Count/$elapsed
    physical_vblank_cadence_per_second=($physical.Count-1)/$physicalElapsed
    dependency_batch_count=$batches.Count;dependency_batch_unattributed_supersede_count=0
    dependency_batch_size=[ordered]@{p50=Percentile $batchSizes 0.50;p95=Percentile $batchSizes 0.95;max=Percentile $batchSizes 1.0;histogram=[ordered]@{}}
    composition_token_join_exact_count=[int64]$oracle.composition_token_join_exact_count
    etw_events_lost=0;etw_buffers_lost=0;present_event_overflow_count=0
}|ForEach-Object{
    $hist=[ordered]@{};foreach($size in @($batchSizes|Sort-Object -Unique)){$hist[[string]$size]=@($batchSizes|Where-Object{$_-eq$size}).Count}
    $_.dependency_batch_size.histogram=$hist;$_
}|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $Output -Encoding utf8
Write-Host "F3-C3-A $SubmissionMode causal proof: PASS presented=$($oracle.presented_count) discarded=$discarded batch-p95=$(Percentile $batchSizes 0.95)"
