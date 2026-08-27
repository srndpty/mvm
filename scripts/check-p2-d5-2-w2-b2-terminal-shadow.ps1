[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$AppJson,
    [Parameter(Mandatory=$true)][string]$EtwJson,
    [Parameter(Mandatory=$true)][string]$Output,
    [Parameter(Mandatory=$true)][string]$SourceRoot,
    [string]$CandidateLedger,
    [string]$B1Checker=(Join-Path $PSScriptRoot 'check-p2-d5-2-w2-b1-intent-transport.ps1'),
    [string]$JoinAuthority=(Join-Path $PSScriptRoot 'p2-native-present-event-exact-join.ps1')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
function Equal($Actual,$Expected,[string]$Name){if($Actual-ne$Expected){Fail "$Name が一致しません (expected=$Expected actual=$Actual)"}}
function Need($Object,[string]$Name){if($null-eq$Object-or$Object.PSObject.Properties.Name-notcontains$Name){Fail "必須fieldがありません: $Name"};return $Object.$Name}

foreach($path in @($AppJson,$EtwJson,$B1Checker,$JoinAuthority)){if(-not(Test-Path -LiteralPath $path)){Fail "B2必須fileがありません: $path"}}
& pwsh -NoProfile -File $B1Checker -Json $AppJson -SourceRoot $SourceRoot -RequireFormalMode *> $null
if($LASTEXITCODE-ne0){Fail 'W2-B1 formal transport authorityが不成立です'}
. $JoinAuthority
$app=Get-Content -LiteralPath $AppJson -Raw -Encoding utf8|ConvertFrom-Json
$etw=Get-Content -LiteralPath $EtwJson -Raw -Encoding utf8|ConvertFrom-Json
Equal ([string](Need $etw 'schema')) 'mvm-p2-etw-present-history-1' 'PresentMon schema'
Equal ([bool](Need $etw 'raw_displayed_qpc')) $true 'raw DisplayedQPC'
foreach($field in @('etw_events_lost','etw_buffers_lost','present_event_overflow_count')){Equal ([int64](Need $etw $field)) 0 $field}

$join=Invoke-MvmNativePresentEventExactJoin -App $app -Etw $etw
$transport=Need (Need $app.native_present_hook 'intent_identity_transport') 'records'
$intentRecords=@($transport)
$nativeAll=@(Need $app.native_present_hook 'records')
Equal $intentRecords.Count $nativeAll.Count 'B1/available native Present件数'
$intentByPresent=@{}
foreach($record in $intentRecords){
    $key=[string](Need $record 'native_present_serial')
    if($intentByPresent.ContainsKey($key)){Fail "B1 native Present serialが重複しています: $key"}
    $intentByPresent[$key]=$record
}
for($index=0;$index-lt$nativeAll.Count;++$index){
    $nativeRecord=$nativeAll[$index];$presentSerial=[string](Need $nativeRecord 'present_serial')
    if(-not$intentByPresent.ContainsKey($presentSerial)){Fail "available native Presentに対応するB1 intent recordがありません: $presentSerial"}
    $intentRecord=$intentByPresent[$presentSerial];$token=Need $nativeRecord 'composition_token'
    Equal ([string](Need $token 'token_serial')) ([string](Need $intentRecord 'native_present_embedded_token_serial')) "available token serial provenance[$index]"
    Equal ([uint64](Need $nativeRecord 'intent_ordinal')) ([uint64](Need $intentRecord 'native_present_intent_ordinal')) "available intent ordinal provenance[$index]"
    Equal ([bool](Need $nativeRecord 'intent_ordinal_valid')) ([bool](Need $intentRecord 'native_present_intent_valid')) "available intent validity provenance[$index]"
    $formalEligible=[bool](Need $intentRecord 'formal_transport_eligible')
    if($formalEligible){Equal ([bool]$nativeRecord.intent_ordinal_valid) $true "available formal intent validity[$index]"}
    else{
        Equal ([bool]$nativeRecord.intent_ordinal_valid) $false "suppressed native intent validity[$index]"
        Equal ([bool](Need $intentRecord 'suppression_exact')) $true "suppression witness[$index]"
    }
}

$entries=@();$presented=0;$discarded=0;$unknown=0;$formalPresented=0
for($index=0;$index-lt$join.joined.Count;++$index){
    $native=$join.joined[$index].native;$presentEvent=$join.joined[$index].present_event
    $presentSerial=[string](Need $native 'present_serial')
    if(-not$intentByPresent.ContainsKey($presentSerial)){Fail "native Presentに対応するB1 intent recordがありません: $presentSerial"}
    $intent=$intentByPresent[$presentSerial]
    $token=Need $native 'composition_token'
    Equal ([string](Need $token 'token_serial')) ([string](Need $intent 'native_present_embedded_token_serial')) "token serial provenance[$index]"
    Equal ([uint64](Need $native 'intent_ordinal')) ([uint64](Need $intent 'native_present_intent_ordinal')) "intent ordinal provenance[$index]"
    Equal ([bool](Need $native 'intent_ordinal_valid')) ([bool](Need $intent 'native_present_intent_valid')) "intent validity provenance[$index]"
    $formalEligible=[bool](Need $intent 'formal_transport_eligible')
    if($formalEligible){Equal ([bool]$native.intent_ordinal_valid) $true "formal intent validity[$index]"}
    else{
        Equal ([bool]$native.intent_ordinal_valid) $false "suppressed intent validity[$index]"
        Equal ([bool](Need $intent 'suppression_exact')) $true "suppression witness[$index]"
    }

    foreach($field in @('final_state','is_completed','is_lost','displayed')){[void](Need $presentEvent $field)}
    $displayed=@($presentEvent.displayed)
    $outcome=if([bool]$presentEvent.is_lost-or-not[bool]$presentEvent.is_completed){'UNKNOWN'}
        elseif([string]$presentEvent.final_state-eq'Presented'-and$displayed.Count-gt0){'PRESENTED'}
        elseif([string]$presentEvent.final_state-eq'Discarded'-and$displayed.Count-eq0){'DISCARDED'}
        else{'UNKNOWN'}
    switch($outcome){'PRESENTED'{++$presented}'DISCARDED'{++$discarded}default{++$unknown}}
    if($outcome-eq'PRESENTED'-and$formalEligible){++$formalPresented}
    $entries+=[ordered]@{
        native_present_serial=$presentSerial
        embedded_token_serial=[string](Need $token 'token_serial')
        intent_ordinal=[string](Need $native 'intent_ordinal')
        intent_ordinal_valid=[bool](Need $native 'intent_ordinal_valid')
        formal_transport_eligible=$formalEligible
        suppression_exact=[bool](Need $intent 'suppression_exact')
        transport_disposition=[string](Need $intent 'transport_disposition')
        native_enter_qpc=[int64](Need $native 'present_enter_qpc')
        native_return_qpc=[int64](Need $native 'present_return_qpc')
        native_thread_id=[int64](Need $native 'thread_id')
        native_sync_interval=[int64](Need $native 'sync_interval')
        native_present_flags=[int64](Need $native 'present_flags')
        native_swapchain=[string](Need $native 'swapchain_identity')
        etw_sequence=[int64](Need $presentEvent 'sequence_index')
        etw_present_start_qpc=[int64](Need $presentEvent 'present_start_qpc')
        etw_thread_id=[int64](Need $presentEvent 'thread_id')
        etw_sync_interval=[int64](Need $presentEvent 'sync_interval')
        etw_present_flags=[int64](Need $presentEvent 'present_flags')
        etw_swapchain=[string](Need $presentEvent 'swap_chain_address')
        final_state=[string](Need $presentEvent 'final_state')
        terminal_outcome=$outcome
        displayed_qpc=@($displayed|ForEach-Object{[int64](Need $_ 'qpc')})
    }
}
$eventCount=$entries.Count;$nativeCount=$join.native.Count
Equal $eventCount ($presented+$discarded+$unknown) 'E1 terminal accounting'
Equal $nativeCount $eventCount 'E2 native/PresentEvent accounting'
if($unknown-ne0){Fail "terminal outcomeが未確定です: $unknown"}

$result=[ordered]@{
    schema='mvm-p2-d5-2-w2-b2-terminal-shadow-1';stage='P2-D5-2-W2-B2'
    authority='shadow_only';shadow_only=$true
    cohort_inclusion_authority=$join.cohort_inclusion_authority
    cohort_membership_uses_displayed_qpc=$false
    exact_join_authority='PID_SWAPCHAIN_SEQUENCE_THREAD_SYNC_FLAGS_NATIVE_INTERVAL'
    nearest_qpc_matching=$false;tolerance_matching=$false
    physical_mapping_connected=$false;performance_accounting_connected=$false
    target_process_id=$join.target_process_id;target_swapchain_identity=$join.target_swapchain_identity
    successful_native_present_count=$nativeCount;present_event_count=$eventCount
    exact_join_count=$entries.Count;presented_event_count=$presented
    formal_presented_event_count=$formalPresented
    discarded_event_count=$discarded;unknown_event_count=$unknown
    etw_events_lost=0;etw_buffers_lost=0;present_event_overflow_count=0
    boundary_native_count=$join.boundary_native_count;boundary_event_count=$join.boundary_event_count
    outside_native_cohort_event_count=$join.outside_native_cohort_event_count
    terminal_closure_exact=$true;verdict='NATIVE_PRESENT_TERMINAL_OUTCOME_EXACT'
    records=$entries
}

if(-not[string]::IsNullOrWhiteSpace($CandidateLedger)){
    if(-not(Test-Path -LiteralPath $CandidateLedger)){Fail "candidate ledgerがありません: $CandidateLedger"}
    $candidate=Get-Content -LiteralPath $CandidateLedger -Raw -Encoding utf8|ConvertFrom-Json
    Equal ([string](Need $candidate 'schema')) $result.schema 'candidate schema'
    $candidateRecords=@(Need $candidate 'records');Equal $candidateRecords.Count $entries.Count 'candidate record count'
    $fields=@('native_present_serial','embedded_token_serial','intent_ordinal','intent_ordinal_valid',
        'formal_transport_eligible','suppression_exact','transport_disposition',
        'native_enter_qpc','native_return_qpc','native_thread_id','native_sync_interval','native_present_flags',
        'native_swapchain','etw_sequence','etw_present_start_qpc','etw_thread_id','etw_sync_interval',
        'etw_present_flags','etw_swapchain','final_state','terminal_outcome')
    for($index=0;$index-lt$entries.Count;++$index){
        foreach($field in $fields){Equal (Need $candidateRecords[$index] $field) $entries[$index][$field] "candidate $field[$index]"}
        Equal (@(Need $candidateRecords[$index] 'displayed_qpc')-join',') (@($entries[$index].displayed_qpc)-join',') "candidate displayed_qpc[$index]"
    }
}
$result|ConvertTo-Json -Depth 10|Set-Content -LiteralPath $Output -Encoding utf8
Write-Host "P2-D5-2-W2-B2 terminal shadow: PASS native=$nativeCount presented=$presented discarded=$discarded"
