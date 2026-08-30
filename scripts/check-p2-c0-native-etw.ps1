[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$AppJson,
    [Parameter(Mandatory=$true)][string]$EtwJson,
    [Parameter(Mandatory=$true)][string]$Output,
    [string]$NativeChecker=(Join-Path $PSScriptRoot 'check-p2-c0-native-hook.ps1'),
    [int]$ProcessExitCode=0,
    [string]$JoinAuthority=(Join-Path $PSScriptRoot 'p2-native-present-event-exact-join.ps1')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
function Equal($Actual,$Expected,[string]$Name){if($Actual-ne$Expected){Fail "$Name が一致しません (expected=$Expected actual=$Actual)"}}
function Field-OrNull($Object,[string]$Name){
    if($Object.PSObject.Properties.Name-contains$Name){return $Object.$Name}
    return $null
}
& pwsh -NoProfile -File $NativeChecker -Json $AppJson -HookMode on -ProcessExitCode $ProcessExitCode *> $null
if($LASTEXITCODE-ne0){Fail 'native Present hook authorityが不成立です'}
$app=Get-Content -LiteralPath $AppJson -Raw -Encoding utf8|ConvertFrom-Json
$etw=Get-Content -LiteralPath $EtwJson -Raw -Encoding utf8|ConvertFrom-Json
Equal ([string]$etw.schema) 'mvm-p2-etw-present-history-1' 'ETW schema'
Equal ([bool]$etw.raw_displayed_qpc) $true 'raw_displayed_qpc'
if([string]$etw.acquisition_mode-eq'CANONICAL_PRESENTMON_LIVE'){
    Equal ([string]$etw.provider_configuration) 'PINNED_PRESENTMON_PMTRACESESSION_ENABLEPROVIDERS' 'provider configuration'
    Equal ([bool]$etw.event_id_filtering) $true 'event_id_filtering'
}
foreach($field in @('etw_events_lost','etw_buffers_lost','present_event_overflow_count')){Equal ([int64]$etw.$field) 0 $field}
$opportunity=$app.presentation_opportunity
$measurementStart=[int64]$opportunity.measurement_start_qpc
$measurementEnd=[int64]$opportunity.measurement_end_qpc_exclusive
if($measurementStart-le0-or$measurementEnd-le$measurementStart){Fail 'measurement windowが不正です'}
Equal ([int64]$etw.qpc_frequency) ([int64]$opportunity.qpc_frequency) 'QPC frequency'
. $JoinAuthority
$exactJoin=Invoke-MvmNativePresentEventExactJoin -App $app -Etw $etw
$native=@($exactJoin.native);$presents=@($exactJoin.events)
$targetPid=[int64]$exactJoin.target_process_id;$nativeSwapchain=[uint64]$exactJoin.target_swapchain_identity
$boundaryNativeCount=[int]$exactJoin.boundary_native_count;$boundaryEtwCount=[int]$exactJoin.boundary_event_count
$samples=@($opportunity.physical_vblank.samples)
if($samples.Count-lt120){Fail 'physical VBlank sampleが120件未満です'}
$qpcFrequency=[int64]$opportunity.qpc_frequency
$identity=$opportunity.physical_vblank.window_output_start
$refreshNumerator=[int64]$identity.refresh_numerator;$refreshDenominator=[int64]$identity.refresh_denominator
$periodScaled=[decimal]$qpcFrequency*[decimal]$refreshDenominator
$phases=@(0..119|ForEach-Object{([decimal][int64]$samples[$_].qpc*[decimal]$refreshNumerator)-([decimal][int64]$samples[$_].ordinal*$periodScaled)}|Sort-Object)
$originScaled=[decimal]$phases[59]
function Map-Displayed([int64]$Qpc){
    $relative=([decimal]$Qpc*[decimal]$refreshNumerator)-$originScaled
    $floor=[decimal]::Floor($relative/$periodScaled);$remainder=$relative-$floor*$periodScaled
    if($remainder*2-eq$periodScaled){Fail "DisplayedQPCがVBlank境界で曖昧です: $Qpc"}
    $ordinal=if($remainder*2-lt$periodScaled){$floor}else{$floor+1}
    if($ordinal-lt[decimal][int64]$samples[0].ordinal-or$ordinal-gt[decimal][int64]$samples[-1].ordinal){Fail "DisplayedQPCがVBlank範囲外です: $Qpc"}
    return [int64]$ordinal
}
function Bracket([int64]$Qpc){
    for($sample=0;$sample+1-lt$samples.Count;++$sample){
        if([int64]$samples[$sample].qpc-le$Qpc-and$Qpc-lt[int64]$samples[$sample+1].qpc){return [int64]$samples[$sample].ordinal}
    }
    return $null
}
$records=@();$presentedCount=0;$discardedCount=0;$incompleteCount=0;$lostCount=0
for($index=0;$index-lt$native.Count;++$index){
    $present=$presents[$index];$record=$native[$index]
    $start=[int64]$present.present_start_qpc;$enter=[int64]$record.present_enter_qpc;$returned=[int64]$record.present_return_qpc
    foreach($field in @('completion_class','is_completed','is_lost','present_mode',
            'seen_dxgk_present','seen_win32k_events','seen_in_frame_event',
            'wait_for_flip_event','wait_for_mpo_flip_event')){
        if($present.PSObject.Properties.Name-notcontains$field){Fail "ETW completion fieldがありません: ${field}[$index]"}
    }
    $displayed=@($present.displayed);$mapped=@()
    $classification=if([bool]$present.is_lost){'LOST'}
        elseif(-not[bool]$present.is_completed){'INCOMPLETE_UNKNOWN'}
        elseif([string]$present.final_state-eq'Presented'-and$displayed.Count-gt0){'PRESENTED'}
        elseif([string]$present.final_state-eq'Discarded'-and$displayed.Count-eq0){'DISCARDED'}
        else{'INCOMPLETE_UNKNOWN'}
    Equal ([string]$present.completion_class) $classification "completion class[$index]"
    $discardReason=Field-OrNull $present 'discard_reason'
    if((Field-OrNull $etw 'discard_reason_diagnostic')-eq$true){
        if([string]::IsNullOrWhiteSpace([string]$discardReason)){Fail "discard reasonがありません: $index"}
        if($classification-eq'DISCARDED'-and$discardReason-in@('NONE','UNKNOWN')){Fail "Discarded reasonが未確定です: $index"}
        if($classification-eq'PRESENTED'-and$discardReason-ne'NONE'){Fail "Presentedにdiscard reasonがあります: $index"}
    }
    switch($classification){
        'PRESENTED'{$presentedCount++;foreach($display in $displayed){$mapped+=Map-Displayed ([int64]$display.qpc)}}
        'DISCARDED'{$discardedCount++}
        'INCOMPLETE_UNKNOWN'{$incompleteCount++}
        'LOST'{$lostCount++}
    }
    $records+=[ordered]@{
        sequence_index=$index;present_serial=[string]$record.present_serial
        composition_token_serial=[string]$record.composition_token.token_serial
        output_frame=[int64]$record.composition_token.output_frame
        native_enter_qpc=$enter;native_return_qpc=$returned;etw_present_start_qpc=$start
        enter_bracket_ordinal=Bracket $enter;return_bracket_ordinal=Bracket $returned
        completion_class=$classification;final_state=[string]$present.final_state
        discard_reason=$discardReason
        dependency_batch_present_start_qpc=Field-OrNull $present 'dependency_batch_present_start_qpc'
        waiting_for_dwm_qpc=Field-OrNull $present 'waiting_for_dwm_qpc'
        attached_to_dwm_parent_qpc=Field-OrNull $present 'attached_to_dwm_parent_qpc'
        attached_dwm_parent_present_start_qpc=Field-OrNull $present 'attached_dwm_parent_present_start_qpc'
        dwm_parent_displayed_qpc=Field-OrNull $present 'dwm_parent_displayed_qpc'
        dwm_parent_completion_qpc=Field-OrNull $present 'dwm_parent_completion_qpc'
        dependent_finalized_qpc=Field-OrNull $present 'dependent_finalized_qpc'
        earlier_superseded_by_present_start_qpc=Field-OrNull $present 'earlier_superseded_by_present_start_qpc'
        earlier_superseded_qpc=Field-OrNull $present 'earlier_superseded_qpc'
        is_completed=[bool]$present.is_completed;is_lost=[bool]$present.is_lost
        present_mode=[string]$present.present_mode
        seen_dxgk_present=[bool]$present.seen_dxgk_present
        seen_win32k_events=[bool]$present.seen_win32k_events
        seen_in_frame_event=[bool]$present.seen_in_frame_event
        wait_for_flip_event=[bool]$present.wait_for_flip_event
        wait_for_mpo_flip_event=[bool]$present.wait_for_mpo_flip_event
        time_in_present_qpc=Field-OrNull $present 'time_in_present_qpc'
        ready_qpc=Field-OrNull $present 'ready_qpc'
        queue_submit_sequence=Field-OrNull $present 'queue_submit_sequence'
        composition_surface_luid=Field-OrNull $present 'composition_surface_luid'
        win32k_present_count=Field-OrNull $present 'win32k_present_count'
        win32k_bind_id=Field-OrNull $present 'win32k_bind_id'
        dxgk_present_history_token=Field-OrNull $present 'dxgk_present_history_token'
        dxgk_present_history_token_data=Field-OrNull $present 'dxgk_present_history_token_data'
        displayed_qpc=@($displayed|ForEach-Object{[int64]$_.qpc})
        actual_opportunity_ordinals=$mapped
    }
}
$result=[ordered]@{
    schema='mvm-p2-c0-native-etw-oracle-1';authority='diagnostic_only'
    oracle_status='ORACLE_VALID';formal_counter_authority_changed=$false
    display_completion_status=$(if($incompleteCount-eq0-and$lostCount-eq0){'CLOSED'}else{'INCOMPLETE'})
    native_present_alone_status=$(if($discardedCount-eq0){'NOT_REJECTED_BY_THIS_RUN'}else{'REJECTED'})
    exit_reason=$(if($lostCount-ne0){'PRESENTMON_LOST'}elseif($incompleteCount-ne0){'DISPLAY_COMPLETION_INCOMPLETE'}else{'NONE'})
    target_process_id=$targetPid;target_swapchain_identity=[string]$nativeSwapchain
    native_present_count=$native.Count;etw_present_count=$presents.Count
    boundary_straddling_native_count=$boundaryNativeCount
    boundary_straddling_etw_count=$boundaryEtwCount
    measurement_domain='present_enter_qpc >= start && present_return_qpc < end'
    count_mismatch=0;order_mismatch=0;interval_mismatch=0
    composition_token_join_exact_count=$native.Count
    presented_count=$presentedCount;discarded_count=$discardedCount
    incomplete_unknown_count=$incompleteCount;lost_count=$lostCount
    etw_events_lost=0;etw_buffers_lost=0;present_event_overflow_count=0
    discard_reason_diagnostic=[bool]((Field-OrNull $etw 'discard_reason_diagnostic')-eq$true)
    dependency_lifecycle_diagnostic=[bool]((Field-OrNull $etw 'dependency_lifecycle_diagnostic')-eq$true)
    records=$records
}
$result|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $Output -Encoding utf8
if($lostCount-ne0){Fail "PresentMon LOSTが存在します: $lostCount"}
if($incompleteCount-ne0){Fail "display completionが未確定です: $incompleteCount"}
Write-Host "F3-C0 native/ETW closure: PASS records=$($native.Count) PRESENTED=$presentedCount DISCARDED=$discardedCount"
