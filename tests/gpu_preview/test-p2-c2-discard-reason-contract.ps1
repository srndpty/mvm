param(
    [Parameter(Mandatory=$true)][ValidateSet('Good','NegativeZeroDiscard','NegativeUnknownReason','NegativeMissingReason','NegativePresentedReason','NegativeMissingDiagnosticField')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Checker,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
if(-not(Test-Path -LiteralPath $Directory)){New-Item -ItemType Directory -Path $Directory|Out-Null}
$path=Join-Path $Directory 'oracle.json';$output=Join-Path $Directory 'proof.json'
function Record([int]$Index,[string]$Class,[string]$Reason){[ordered]@{sequence_index=$Index;completion_class=$Class;discard_reason=$Reason;dependency_batch_present_start_qpc=100;time_in_present_qpc=10;ready_qpc=20;queue_submit_sequence=30;composition_surface_luid='0x1';win32k_present_count=2;win32k_bind_id=3;dxgk_present_history_token='0x4';dxgk_present_history_token_data='0x5'}}
$records=@((Record 0 'PRESENTED' 'NONE'),(Record 1 'DISCARDED' 'EARLIER_SWAPCHAIN_PRESENT_SUPERSEDED'),(Record 2 'DISCARDED' 'WIN32K_TOKEN_NOT_IN_FRAME'))
$oracle=[ordered]@{schema='mvm-p2-c0-native-etw-oracle-1';oracle_status='ORACLE_VALID';display_completion_status='CLOSED';discard_reason_diagnostic=$true;native_present_count=3;composition_token_join_exact_count=3;presented_count=1;discarded_count=2;incomplete_unknown_count=0;lost_count=0;etw_events_lost=0;etw_buffers_lost=0;present_event_overflow_count=0;records=$records}
switch($Case){
    'NegativeZeroDiscard'{$records=@($records[0]);$oracle.records=$records;$oracle.native_present_count=1;$oracle.composition_token_join_exact_count=1;$oracle.discarded_count=0}
    'NegativeUnknownReason'{$records[1].discard_reason='UNKNOWN'}
    'NegativeMissingReason'{$records[1].discard_reason='NONE'}
    'NegativePresentedReason'{$records[0].discard_reason='BLIT_CANCEL'}
    'NegativeMissingDiagnosticField'{$records[1].Remove('ready_qpc')}
}
$oracle|ConvertTo-Json -Depth 7|Set-Content -LiteralPath $path -Encoding utf8
& pwsh -NoProfile -File $Checker -OracleJson $path -Output $output *> $null
$actual=$LASTEXITCODE;$expected=if($Case-eq'Good'){0}else{1}
if($actual-ne$expected){throw "$Case F3-C2 reason contract exitが不正です: expected=$expected actual=$actual"}
if($Case-eq'Good'){$proof=Get-Content -LiteralPath $output -Raw -Encoding utf8|ConvertFrom-Json;if($proof.discard_reason_count-ne2-or$proof.unknown_discard_reason_count-ne0){throw 'F3-C2 reason good proofが不正です'}}
Write-Host "F3-C2 discard reason $Case test: PASS"
