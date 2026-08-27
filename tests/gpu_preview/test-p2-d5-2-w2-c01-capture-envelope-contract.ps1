[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'Good','NegativeStartAfterArm','NegativeEndMutation','NegativeSuccessorBeforeEnd',
        'NegativeCloseBeforeSuccessor','NegativeSuccessorTimeout','NegativeWindowExtended')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Checker,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if(-not(Test-Path -LiteralPath $Directory)){New-Item -ItemType Directory -Path $Directory|Out-Null}
$inputPath=Join-Path $Directory 'app.json'
$record=[ordered]@{present_serial='2';thread_id=7;present_enter_qpc=210;present_return_qpc=250;sync_interval=1;present_flags=0}
$app=[ordered]@{
    presentation_opportunity=[ordered]@{
        measurement_end_qpc_exclusive=600
        physical_vblank_domain_shadow=[ordered]@{
            shadow_authority_valid=$true;predecessor_valid=$true;successor_valid=$true;successor_qpc=610
        }
    }
    native_present_hook=[ordered]@{
        records=@($record)
        capture_envelope_records=@(
            [ordered]@{present_serial='1';thread_id=7;present_enter_qpc=150;present_return_qpc=180;sync_interval=1;present_flags=0},
            $record,
            [ordered]@{present_serial='3';thread_id=7;present_enter_qpc=605;present_return_qpc=615;sync_interval=1;present_flags=0})
        capture_envelope=[ordered]@{
            schema='mvm-p2-d5-2-w2-c01-capture-envelope-1';enabled=$true
            lower_intent_producer='formal opportunity scheduler preroll'
            lower_intent_producer_started=$true
            lower_intent_producer_completed_before_measurement=$true
            begin_qpc=100;measurement_arm_qpc=180;measurement_start_qpc=200
            frozen_measurement_end_qpc=600;serialized_measurement_end_qpc=600
            successor_wait_completed=$true;successor_wait_timeout=$false;successor_sample_qpc=610
            close_qpc=620;lower_closed_before_measurement_arm=$true
            measurement_window_extended=$false;measurement_window_unchanged=$true
            upper_closed_after_successor=$true;record_count=3
            overflow_count=0;missing_token_count=0;duplicate_token_count=0;stale_token_count=0
            authority_pass=$true
        }
    }
}
$envelope=$app.native_present_hook.capture_envelope
switch($Case){
    'NegativeStartAfterArm'{$envelope.begin_qpc=190}
    'NegativeEndMutation'{$envelope.serialized_measurement_end_qpc=601}
    'NegativeSuccessorBeforeEnd'{$envelope.successor_sample_qpc=599;$app.presentation_opportunity.physical_vblank_domain_shadow.successor_qpc=599}
    'NegativeCloseBeforeSuccessor'{$envelope.close_qpc=609}
    'NegativeSuccessorTimeout'{$envelope.successor_wait_completed=$false;$envelope.successor_wait_timeout=$true}
    'NegativeWindowExtended'{$envelope.measurement_window_extended=$true}
}
$app|ConvertTo-Json -Depth 10|Set-Content -LiteralPath $inputPath -Encoding utf8
& pwsh -NoProfile -File $Checker -InputJson $inputPath *> $null
$actual=$LASTEXITCODE;$expected=if($Case-eq'Good'){0}else{1}
if($actual-ne$expected){throw "$Case C0.1 contract exitが不正です: expected=$expected actual=$actual"}
Write-Output "W2-C0.1 $Case contract: PASS"
