[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'GoodForeignBoundary','NegativeMissingNative','NegativePhysicalAuthority')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Inventory,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if(-not(Test-Path -LiteralPath $Directory)){New-Item -ItemType Directory -Path $Directory|Out-Null}
$runDirectory=Join-Path $Directory 'run-1';if(-not(Test-Path -LiteralPath $runDirectory)){New-Item -ItemType Directory -Path $runDirectory|Out-Null}
$summaryPath=Join-Path $Directory 'w2-b2-live-summary.json';$appPath=Join-Path $runDirectory 'traced-app.json'
$etwPath=Join-Path $runDirectory 'present-history-raw.json';$ledgerPath=Join-Path $runDirectory 'terminal-shadow.json'
$output=Join-Path $Directory 'inventory.json'
$native=@(
    [ordered]@{present_serial='1';swapchain_identity='4096';thread_id=77;present_enter_qpc=80;present_return_qpc=95;sync_interval=1;present_flags=0;intent_ordinal='90';intent_ordinal_valid=$true;composition_token=[ordered]@{token_serial='101'}},
    [ordered]@{present_serial='2';swapchain_identity='4096';thread_id=77;present_enter_qpc=120;present_return_qpc=150;sync_interval=1;present_flags=0;intent_ordinal='100';intent_ordinal_valid=$true;composition_token=[ordered]@{token_serial='102'}},
    [ordered]@{present_serial='3';swapchain_identity='4096';thread_id=77;present_enter_qpc=220;present_return_qpc=250;sync_interval=1;present_flags=0;intent_ordinal='101';intent_ordinal_valid=$true;composition_token=[ordered]@{token_serial='103'}})
$intent=@(0..2|ForEach-Object{[ordered]@{
    native_present_serial=[string](1+$_);native_present_embedded_token_serial=[string](101+$_)
    native_present_intent_ordinal=[string](@(90,100,101)[$_]);native_present_intent_valid=$true
}})
$shadow=[ordered]@{
    shadow_authority_valid=$true;shadow_authority_canonical_reason='NONE'
    physical_opportunity_count=3;origin_qpc=100;last_qpc=300
    predecessor_valid=$true;successor_valid=$true
}
$app=[ordered]@{
    process_id=1234
    presentation_opportunity=[ordered]@{
        measurement_start_qpc=100;measurement_end_qpc_exclusive=400
        physical_vblank=[ordered]@{samples=@(
            [ordered]@{ordinal=0;qpc=90},[ordered]@{ordinal=1;qpc=100},
            [ordered]@{ordinal=2;qpc=200},[ordered]@{ordinal=3;qpc=300},
            [ordered]@{ordinal=4;qpc=400})}
        physical_vblank_domain_shadow=$shadow
    }
    native_present_hook=[ordered]@{
        records=$native;intent_identity_transport=[ordered]@{records=$intent}
    }
}
$events=@(
    [ordered]@{sequence_index=10;present_start_qpc=90;process_id=1234;thread_id=77;swap_chain_address='0x1000';sync_interval=1;present_flags=0;final_state='Presented';displayed=@([ordered]@{qpc=110})},
    [ordered]@{sequence_index=11;present_start_qpc=130;process_id=1234;thread_id=77;swap_chain_address='0x1000';sync_interval=1;present_flags=0;final_state='Presented';displayed=@([ordered]@{qpc=210})},
    [ordered]@{sequence_index=12;present_start_qpc=230;process_id=1234;thread_id=77;swap_chain_address='0x1000';sync_interval=1;present_flags=0;final_state='Presented';displayed=@([ordered]@{qpc=250})})
if($Case-eq'NegativeMissingNative'){
    $events+=,[ordered]@{sequence_index=13;present_start_qpc=270;process_id=1234;thread_id=77;swap_chain_address='0x1000';sync_interval=1;present_flags=0;final_state='Presented';displayed=@([ordered]@{qpc=280})}
}
if($Case-eq'NegativePhysicalAuthority'){
    $shadow.shadow_authority_valid=$false;$shadow.shadow_authority_canonical_reason='PHYSICAL_VBLANK_BOUNDARY_NOT_BRACKETED'
    $shadow.successor_valid=$false
}
$etw=[ordered]@{
    target_process_id=1234;etw_events_lost=0;etw_buffers_lost=0;present_event_overflow_count=0
    events=$events
}
$ledger=[ordered]@{
    schema='mvm-p2-d5-2-w2-b2-terminal-shadow-1';verdict='NATIVE_PRESENT_TERMINAL_OUTCOME_EXACT'
    physical_mapping_connected=$false;performance_accounting_connected=$false
    records=@([ordered]@{etw_sequence=11},[ordered]@{etw_sequence=12})
}
$summary=[ordered]@{matrix_pass=$true;verdict='NATIVE_PRESENT_TERMINAL_OUTCOME_EXACT';runs=1}
$app|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $appPath -Encoding utf8
$etw|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $etwPath -Encoding utf8
$ledger|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $ledgerPath -Encoding utf8
$summary|ConvertTo-Json -Depth 4|Set-Content -LiteralPath $summaryPath -Encoding utf8
& pwsh -NoProfile -File $Inventory -B2LiveDirectory $Directory -Output $output -RequireCoverageComplete *> $null
$actual=$LASTEXITCODE;$expected=if($Case-eq'GoodForeignBoundary'){0}else{1}
if($actual-ne$expected){throw "$Case W2-C0 contract exitが不正です: expected=$expected actual=$actual"}
$result=Get-Content -LiteralPath $output -Raw -Encoding utf8|ConvertFrom-Json
if($Case-eq'GoodForeignBoundary'){
    if(-not[bool]$result.coverage_complete-or$result.runs[0].observed_domain_foreign_intent_exact_count-ne1){
        throw 'boundary foreign intentのexact coverageが成立しません'
    }
}elseif([bool]$result.coverage_complete){throw "$Case がcoverage completeとして受理されました"}
Write-Output "W2-C0 $Case contract: PASS"
