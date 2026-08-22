param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'GoodControl','GoodDwmFlush','GoodFrameLatency1','NegativeMissingBatch',
        'NegativeWrongLatency','NegativeFlushCount')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Checker,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
if(-not(Test-Path -LiteralPath $Directory)){New-Item -ItemType Directory -Path $Directory|Out-Null}
$mode=switch($Case){'GoodDwmFlush'{'DWM_FLUSH_AFTER_PRESENT'}'NegativeFlushCount'{'DWM_FLUSH_AFTER_PRESENT'}'GoodFrameLatency1'{'FRAME_LATENCY_1'}default{'CONTROL'}}
$modeNumber=@{CONTROL=0;DWM_FLUSH_AFTER_PRESENT=1;FRAME_LATENCY_1=2}[$mode]
$latency=if($mode-eq'FRAME_LATENCY_1'){1}else{2}
$records=@(
    [ordered]@{sequence_index=0;completion_class='DISCARDED';discard_reason='DEPENDENT_PRESENT_SUPERSEDED';dependency_batch_present_start_qpc=1000;output_frame=0},
    [ordered]@{sequence_index=1;completion_class='DISCARDED';discard_reason='EARLIER_SWAPCHAIN_PRESENT_SUPERSEDED';dependency_batch_present_start_qpc=1000;output_frame=1},
    [ordered]@{sequence_index=2;completion_class='PRESENTED';discard_reason='NONE';dependency_batch_present_start_qpc=1000;output_frame=2})
$oracle=[ordered]@{
    schema='mvm-p2-c0-native-etw-oracle-1';oracle_status='ORACLE_VALID';display_completion_status='CLOSED'
    formal_counter_authority_changed=$false;native_present_count=3;etw_present_count=3
    composition_token_join_exact_count=3;presented_count=1;discarded_count=2
    incomplete_unknown_count=0;lost_count=0;etw_events_lost=0;etw_buffers_lost=0
    present_event_overflow_count=0;records=$records}
$samples=@(0..3|ForEach-Object{[ordered]@{ordinal=$_;qpc=1000000+166667*$_}})
$app=[ordered]@{
    required_measurement_frame_count=3
    native_present_hook=[ordered]@{
        submission_mode=$modeNumber;configured_maximum_frame_latency=$latency
        swapchain_maximum_frame_latency=$latency;frame_latency_waitable_object_available=$true
        dwm_flush_call_count=$(if($mode-eq'DWM_FLUSH_AFTER_PRESENT'){3}else{0});dwm_flush_failure_count=0}
    presentation_opportunity=[ordered]@{
        measurement_start_qpc=1000000;measurement_end_qpc_exclusive=1500001;qpc_frequency=10000000
        physical_vblank=[ordered]@{samples=$samples}}}
switch($Case){
    'NegativeMissingBatch'{$records[0].dependency_batch_present_start_qpc=0}
    'NegativeWrongLatency'{$app.native_present_hook.swapchain_maximum_frame_latency=1}
    'NegativeFlushCount'{$app.native_present_hook.dwm_flush_call_count=2}
}
$oraclePath=Join-Path $Directory 'oracle.json';$appPath=Join-Path $Directory 'app.json';$output=Join-Path $Directory 'proof.json'
$oracle|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $oraclePath -Encoding utf8
$app|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $appPath -Encoding utf8
& pwsh -NoProfile -File $Checker -OracleJson $oraclePath -AppJson $appPath -SubmissionMode $mode -Output $output *> $null
$actual=$LASTEXITCODE;$expected=if($Case-like'Good*'){0}else{1}
if($actual-ne$expected){throw "$Case F3-C3-A contract exitが不正です: expected=$expected actual=$actual"}
if($Case-like'Good*'){
    $proof=Get-Content -LiteralPath $output -Raw -Encoding utf8|ConvertFrom-Json
    if($proof.dependency_batch_size.p95-ne3-or$proof.discarded_count-ne2){throw 'F3-C3-A good proofが不正です'}
}
Write-Host "F3-C3-A $Case contract test: PASS"
