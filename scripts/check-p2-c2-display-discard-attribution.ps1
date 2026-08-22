[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OracleJson,
    [Parameter(Mandatory=$true)][string]$Output
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
function Equal($Actual,$Expected,[string]$Name){if($Actual-ne$Expected){Fail "$Name が一致しません (expected=$Expected actual=$Actual)"}}
$oracle=Get-Content -LiteralPath $OracleJson -Raw -Encoding utf8|ConvertFrom-Json
Equal ([string]$oracle.schema) 'mvm-p2-c0-native-etw-oracle-1' 'oracle schema'
Equal ([string]$oracle.oracle_status) 'ORACLE_VALID' 'oracle status'
Equal ([string]$oracle.display_completion_status) 'CLOSED' 'display completion status'
foreach($field in @('incomplete_unknown_count','lost_count','etw_events_lost','etw_buffers_lost','present_event_overflow_count')){
    Equal ([int64]$oracle.$field) 0 $field
}
$records=@($oracle.records)
Equal $records.Count ([int64]$oracle.native_present_count) '全Present台帳件数'
Equal ([int64]$oracle.composition_token_join_exact_count) $records.Count 'native/composition token exact join件数'
if($records.Count-lt2){Fail 'Present台帳が2件未満です'}

$ledger=@();$presented=@();$discardedCount=0L
for($index=0;$index-lt$records.Count;++$index){
    $record=$records[$index]
    Equal ([int64]$record.sequence_index) $index "sequence index[$index]"
    foreach($field in @('present_serial','composition_token_serial')){
        if([string]::IsNullOrWhiteSpace([string]$record.$field)){Fail "identityがありません: ${field}[$index]"}
    }
    $displayed=@($record.displayed_qpc);$vblanks=@($record.actual_opportunity_ordinals)
    $class=[string]$record.completion_class
    if($class-eq'PRESENTED'){
        if($displayed.Count-eq0-or$displayed.Count-ne$vblanks.Count){Fail "Presented display payloadが不正です: $index"}
        $presented+=$record
    }elseif($class-eq'DISCARDED'){
        if($displayed.Count-ne0-or$vblanks.Count-ne0){Fail "Discardedにdisplay payloadがあります: $index"}
        $discardedCount++
    }else{Fail "closure後の分類が不正です: $class"}
    $ledger+=[ordered]@{
        sequence_index=$index;present_serial=[string]$record.present_serial
        composition_token_serial=[string]$record.composition_token_serial
        source_frame=[int64]$record.output_frame;final_state=[string]$record.final_state
        completion_class=$class;present_mode=[string]$record.present_mode
        displayed_qpc=@($displayed|ForEach-Object{[int64]$_})
        physical_vblank_ordinals=@($vblanks|ForEach-Object{[int64]$_})
        present_start_qpc=[int64]$record.etw_present_start_qpc
        ready_qpc=$null;time_in_present_qpc=$null;queue_submit_sequence=$null
        seen_dxgk_present=[bool]$record.seen_dxgk_present
        seen_win32k_events=[bool]$record.seen_win32k_events
        seen_in_frame_event=[bool]$record.seen_in_frame_event
        win32k_present_count=$null;win32k_bind_id=$null
        dxgk_present_history_token=$null;dxgk_present_history_token_data=$null
    }
}
Equal $discardedCount ([int64]$oracle.discarded_count) 'Discarded件数'
Equal $presented.Count ([int64]$oracle.presented_count) 'Presented件数'

$timeline=@();$sourceVblankEqual=0L;$discardVblankEqual=0L;$discardSourceEqual=0L
for($index=1;$index-lt$presented.Count;++$index){
    $previous=$presented[$index-1];$current=$presented[$index]
    $previousVblank=[int64]@($previous.actual_opportunity_ordinals)[0]
    $currentVblank=[int64]@($current.actual_opportunity_ordinals)[0]
    $sourceDelta=[int64]$current.output_frame-[int64]$previous.output_frame
    $vblankDelta=$currentVblank-$previousVblank
    $intervening=[int64]$current.sequence_index-[int64]$previous.sequence_index-1
    if($vblankDelta-le0){Fail "Presented physical VBlankがstrict monotoneではありません: $index"}
    if($sourceDelta-eq$vblankDelta){$sourceVblankEqual++}
    if($intervening-eq$vblankDelta-1){$discardVblankEqual++}
    if($intervening-eq$sourceDelta-1){$discardSourceEqual++}
    $timeline+=[ordered]@{
        previous_present_serial=[string]$previous.present_serial
        current_present_serial=[string]$current.present_serial
        previous_source_frame=[int64]$previous.output_frame
        current_source_frame=[int64]$current.output_frame;source_frame_delta=$sourceDelta
        previous_physical_vblank=$previousVblank
        current_physical_vblank=$currentVblank;physical_vblank_delta=$vblankDelta
        intervening_discarded_count=$intervening
        source_delta_matches_physical_vblank_delta=($sourceDelta-eq$vblankDelta)
        discarded_count_matches_physical_hold_gap=($intervening-eq$vblankDelta-1)
    }
}
$pairCount=$timeline.Count
$result=[ordered]@{
    schema='mvm-p2-c2-display-discard-attribution-1';authority='diagnostic_offline_proof'
    proof_status='PASS';formal_counter_authority_changed=$false
    source_artifact_reacquired=$false;present_record_count=$records.Count
    presented_count=$presented.Count;discarded_count=$discardedCount
    presented_pair_count=$pairCount
    source_vblank_delta_equal_count=$sourceVblankEqual
    source_vblank_delta_mismatch_count=$pairCount-$sourceVblankEqual
    discarded_physical_hold_gap_equal_count=$discardVblankEqual
    discarded_physical_hold_gap_mismatch_count=$pairCount-$discardVblankEqual
    discarded_source_gap_equal_count=$discardSourceEqual
    physical_gap_accounting_exact=($discardVblankEqual-eq$pairCount)
    source_frame_is_exact_physical_timeline=($sourceVblankEqual-eq$pairCount)
    historical_field_availability=[ordered]@{
        native_serial=$true;composition_token=$true;source_frame=$true;final_state=$true
        present_mode=$true;displayed_qpc=$true;physical_vblank=$true;present_start=$true
        seen_dxgk=$true;seen_win32k=$true;seen_in_frame=$true
        ready_time=$false;time_in_present=$false;queue_submit_sequence=$false
        win32k_present_count=$false;win32k_bind_id=$false;present_history_tokens=$false
    }
    records=$ledger;presented_timeline=$timeline
}
$result|ConvertTo-Json -Depth 10|Set-Content -LiteralPath $Output -Encoding utf8
Write-Host "F3-C2 display/discard attribution: PASS records=$($records.Count) physical-gap=$discardVblankEqual/$pairCount source-vblank=$sourceVblankEqual/$pairCount"
