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
Equal ([bool]$oracle.discard_reason_diagnostic) $true 'discard reason diagnostic'
foreach($field in @('incomplete_unknown_count','lost_count','etw_events_lost','etw_buffers_lost','present_event_overflow_count')){Equal ([int64]$oracle.$field) 0 $field}
$records=@($oracle.records);Equal $records.Count ([int64]$oracle.native_present_count) 'Present record count'
Equal ([int64]$oracle.composition_token_join_exact_count) $records.Count 'native/token exact join count'
$allowed=@('BACK_TO_BACK_FLIP_SUPERSEDED','WIN32K_TOKEN_NOT_IN_FRAME','DEPENDENT_PRESENT_SUPERSEDED','DO_NOT_SEQUENCE','NOT_VISIBLE','BLIT_CANCEL','OTHER_EXPLICIT_DISCARD','EARLIER_SWAPCHAIN_PRESENT_SUPERSEDED')
$requiredFields=@('time_in_present_qpc','ready_qpc','queue_submit_sequence','composition_surface_luid','win32k_present_count','win32k_bind_id','dxgk_present_history_token','dxgk_present_history_token_data')
$histogram=[ordered]@{};foreach($reason in $allowed){$histogram[$reason]=0L}
$discarded=0L;$unknown=0L
foreach($record in $records){
    foreach($field in $requiredFields){
        if($record.PSObject.Properties.Name-notcontains$field-or$null-eq$record.$field){Fail "C2 diagnostic fieldがありません: $field sequence=$($record.sequence_index)"}
    }
    $class=[string]$record.completion_class;$reason=[string]$record.discard_reason
    if($class-eq'DISCARDED'){
        $discarded++
        if($reason-notin$allowed){$unknown++;continue}
        $histogram[$reason]=[int64]$histogram[$reason]+1
    }elseif($class-eq'PRESENTED'){
        Equal $reason 'NONE' "Presented discard reason[$($record.sequence_index)]"
    }else{Fail "closure後のcompletion classが不正です: $class"}
}
Equal $discarded ([int64]$oracle.discarded_count) 'discard count'
if($discarded-le0){Fail 'Discarded reason attribution対象が0件です（evidence empty）'}
$reasonCount=0L;foreach($reason in $allowed){$reasonCount+=[int64]$histogram[$reason]}
Equal $reasonCount $discarded 'discard/reason accounting'
Equal $unknown 0 'unknown discard reason count'
$result=[ordered]@{
    schema='mvm-p2-c2-discard-reason-proof-1';proof_status='PASS';authority='diagnostic_only'
    formal_counter_authority_changed=$false;present_record_count=$records.Count
    presented_count=[int64]$oracle.presented_count;discarded_count=$discarded
    discard_reason_count=$reasonCount;unknown_discard_reason_count=$unknown
    native_token_join_exact_count=[int64]$oracle.composition_token_join_exact_count
    etw_events_lost=0;etw_buffers_lost=0;present_event_overflow_count=0
    reason_histogram=$histogram
}
$result|ConvertTo-Json -Depth 6|Set-Content -LiteralPath $Output -Encoding utf8
Write-Host "F3-C2 discard reason closure: PASS discarded=$discarded reasons=$reasonCount unknown=0"
