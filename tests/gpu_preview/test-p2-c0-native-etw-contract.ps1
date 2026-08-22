param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'Good','GoodDiscarded','GoodBoundaryStraddle','NegativeUnknown','NegativeLost','NegativeOrder',
        'NegativeOutsideInterval','NegativeCount')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Fixture,
    [Parameter(Mandatory=$true)][string]$NativeChecker,
    [Parameter(Mandatory=$true)][string]$EtwChecker,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
if(-not(Test-Path -LiteralPath $Directory)){New-Item -ItemType Directory -Path $Directory|Out-Null}
$appJson=Join-Path $Directory 'app.json';$etwJson=Join-Path $Directory 'etw.json';$oracle=Join-Path $Directory 'oracle.json'
& pwsh -NoProfile -File $Fixture -Case Good -Checker $NativeChecker -Output $appJson *> $null
if($LASTEXITCODE-ne0){throw 'C0 native app fixture生成に失敗しました'}
$app=Get-Content -LiteralPath $appJson -Raw -Encoding utf8|ConvertFrom-Json
$records=@($app.native_present_hook.records);$samples=@($app.presentation_opportunity.physical_vblank.samples)
$events=@(0..($records.Count-1)|ForEach-Object{
    [ordered]@{
        sequence_index=100+$_;present_start_qpc=[int64]$records[$_].present_enter_qpc+25
        process_id=1234;thread_id=[int64]$records[$_].thread_id;swap_chain_address='0x1000'
        window_handle='0x2000';sync_interval=1;present_flags=0;final_state='Presented'
        completion_class='PRESENTED';is_completed=$true;is_lost=$false
        present_mode='Composed_Flip';seen_dxgk_present=$true;seen_win32k_events=$true
        seen_in_frame_event=$true;wait_for_flip_event=$false;wait_for_mpo_flip_event=$false
        displayed=@([ordered]@{frame_type='NotSet';qpc=[int64]$samples[$_+1].qpc});present_ids=@()}})
switch($Case){
    'GoodDiscarded'{$events[10].displayed=@();$events[10].final_state='Discarded';$events[10].completion_class='DISCARDED'}
    'GoodBoundaryStraddle'{
        $records[-1].present_return_qpc=[int64]$app.presentation_opportunity.measurement_end_qpc_exclusive
        $events=@($events|Select-Object -SkipLast 1)
        $app|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $appJson -Encoding utf8
    }
    'NegativeUnknown'{$events[10].displayed=@();$events[10].final_state='Unknown';$events[10].completion_class='INCOMPLETE_UNKNOWN'}
    'NegativeLost'{$events[10].displayed=@();$events[10].final_state='Unknown';$events[10].is_lost=$true;$events[10].completion_class='LOST'}
    'NegativeOrder'{$events[10].sequence_index=999}
    'NegativeOutsideInterval'{$events[10].present_start_qpc=[int64]$records[10].present_return_qpc+1}
    'NegativeCount'{$events=@($events|Select-Object -SkipLast 1)}
}
$etw=[ordered]@{
    schema='mvm-p2-etw-present-history-1';acquisition_mode='CANONICAL_PRESENTMON_LIVE'
    provider_configuration='PINNED_PRESENTMON_PMTRACESESSION_ENABLEPROVIDERS';event_id_filtering=$true
    raw_displayed_qpc=$true;qpc_frequency=10000000
    target_process_id=1234;etw_events_lost=0;etw_buffers_lost=0;present_event_overflow_count=0
    events=$events}
$etw|ConvertTo-Json -Depth 10|Set-Content -LiteralPath $etwJson -Encoding utf8
& pwsh -NoProfile -File $EtwChecker -AppJson $appJson -EtwJson $etwJson -Output $oracle `
    -NativeChecker $NativeChecker -ProcessExitCode 0 *> $null
$actual=$LASTEXITCODE;$expected=if($Case-in@('Good','GoodDiscarded','GoodBoundaryStraddle')){0}else{1}
if($actual-ne$expected){throw "$Case C0 native ETW contract exitが不正です: expected=$expected actual=$actual"}
if(-not(Test-Path -LiteralPath $oracle)-and$Case-in@('Good','GoodDiscarded','GoodBoundaryStraddle','NegativeUnknown','NegativeLost')){
    throw "$Case がoracleを残しませんでした"
}
if($Case-in@('Good','GoodDiscarded','GoodBoundaryStraddle','NegativeUnknown','NegativeLost')){
    $result=Get-Content -LiteralPath $oracle -Raw -Encoding utf8|ConvertFrom-Json
    if($result.oracle_status-ne'ORACLE_VALID'){
        throw "$Case oracle acquisition statusが不正です: $($result.oracle_status)"
    }
    $expectedCompletion=if($Case-in@('Good','GoodDiscarded','GoodBoundaryStraddle')){'CLOSED'}else{'INCOMPLETE'}
    $expectedReason=switch($Case){'NegativeUnknown'{'DISPLAY_COMPLETION_INCOMPLETE'}'NegativeLost'{'PRESENTMON_LOST'}default{'NONE'}}
    if($result.display_completion_status-ne$expectedCompletion){throw "$Case completion statusが不正です: $($result.display_completion_status)"}
    if($result.exit_reason-ne$expectedReason){throw "$Case exit reasonが不正です: $($result.exit_reason)"}
    if($Case-eq'GoodDiscarded'-and$result.native_present_alone_status-ne'REJECTED'){
        throw '明示的Discardedがnative Present単独authorityを棄却しませんでした'
    }
    if($Case-eq'GoodBoundaryStraddle'-and$result.boundary_straddling_native_count-ne1){
        throw 'measurement終了をまたぐnative Presentが境界recordとして分離されませんでした'
    }
}
Write-Host "C0 native ETW $Case test: PASS"
