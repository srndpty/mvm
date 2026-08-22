param(
    [Parameter(Mandatory=$true)][ValidateSet('Good','NegativeUnknown','NegativeIdentity','NegativePayload','NegativeOrder')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Checker,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
if(-not(Test-Path -LiteralPath $Directory)){New-Item -ItemType Directory -Path $Directory|Out-Null}
$oraclePath=Join-Path $Directory 'oracle.json';$output=Join-Path $Directory 'proof.json'
$records=@(
    [ordered]@{sequence_index=0;present_serial='1';composition_token_serial='11';output_frame=0;etw_present_start_qpc=100;completion_class='PRESENTED';final_state='Presented';present_mode='Composed_Flip';seen_dxgk_present=$true;seen_win32k_events=$true;seen_in_frame_event=$true;displayed_qpc=@(150);actual_opportunity_ordinals=@(1)},
    [ordered]@{sequence_index=1;present_serial='2';composition_token_serial='12';output_frame=1;etw_present_start_qpc=200;completion_class='DISCARDED';final_state='Discarded';present_mode='Composed_Flip';seen_dxgk_present=$true;seen_win32k_events=$true;seen_in_frame_event=$true;displayed_qpc=@();actual_opportunity_ordinals=@()},
    [ordered]@{sequence_index=2;present_serial='3';composition_token_serial='13';output_frame=2;etw_present_start_qpc=300;completion_class='PRESENTED';final_state='Presented';present_mode='Composed_Flip';seen_dxgk_present=$true;seen_win32k_events=$true;seen_in_frame_event=$true;displayed_qpc=@(350);actual_opportunity_ordinals=@(3)}
)
$oracle=[ordered]@{schema='mvm-p2-c0-native-etw-oracle-1';oracle_status='ORACLE_VALID';display_completion_status='CLOSED';native_present_count=3;presented_count=2;discarded_count=1;composition_token_join_exact_count=3;incomplete_unknown_count=0;lost_count=0;etw_events_lost=0;etw_buffers_lost=0;present_event_overflow_count=0;records=$records}
switch($Case){
    'NegativeUnknown'{$oracle.incomplete_unknown_count=1;$oracle.display_completion_status='INCOMPLETE'}
    'NegativeIdentity'{$records[1].composition_token_serial=''}
    'NegativePayload'{$records[1].displayed_qpc=@(250);$records[1].actual_opportunity_ordinals=@(2)}
    'NegativeOrder'{$records[2].actual_opportunity_ordinals=@(1)}
}
$oracle|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $oraclePath -Encoding utf8
& pwsh -NoProfile -File $Checker -OracleJson $oraclePath -Output $output *> $null
$actual=$LASTEXITCODE;$expected=if($Case-eq'Good'){0}else{1}
if($actual-ne$expected){throw "$Case F3-C2 contract exitが不正です: expected=$expected actual=$actual"}
if($Case-eq'Good'){
    $proof=Get-Content -LiteralPath $output -Raw -Encoding utf8|ConvertFrom-Json
    if(-not$proof.physical_gap_accounting_exact-or$proof.present_record_count-ne3-or$proof.presented_timeline.Count-ne1){throw 'F3-C2 good proofが不正です'}
}
Write-Host "F3-C2 display/discard attribution $Case test: PASS"
