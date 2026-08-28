[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'GoodPresented','GoodDiscarded','GoodDisplayedOutsideMeasurementDomain','GoodSuppressedPresented',
        'NegativeMissingPresentEvent','NegativeDuplicatePresentEvent','NegativeAmbiguousCandidate',
        'NegativeSequenceDiscontinuity','NegativeThreadMismatch','NegativeSyncIntervalMismatch',
        'NegativePresentFlagsMismatch','NegativePresentStartOutsideNativeInterval',
        'NegativeSwapchainMismatch','NegativeIntentJoinMutation','NegativeFinalStateUnknown',
        'NegativeTerminalMissing','NegativePresentedWithoutDisplayedQpc','NegativeEtwLost',
        'NegativeEtwBufferLoss','NegativeNativePresentCountMismatch','NegativeNativePresentDuplicate')]
    [string]$Case,
    [Parameter(Mandatory=$true)][string]$Checker,
    [Parameter(Mandatory=$true)][string]$SourceRoot,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if(-not(Test-Path -LiteralPath $Directory)){New-Item -ItemType Directory -Path $Directory|Out-Null}
$appPath=Join-Path $Directory 'app.json';$etwPath=Join-Path $Directory 'etw.json'
$output=Join-Path $Directory 'terminal-shadow.json';$candidate=Join-Path $Directory 'candidate.json'

$native=@(0..2|ForEach-Object{
    $index=$_
    [ordered]@{
        present_serial=[string](1+$index);swapchain_identity='4096';thread_id=77
        present_enter_qpc=110+100*$index;present_return_qpc=150+100*$index
        sync_interval=1;present_flags=0;hresult=0;token_present=$true
        intent_ordinal=[string](10+$index);intent_ordinal_valid=$true
        composition_token=[ordered]@{
            token_serial=[string](101+$index);intent_ordinal=[string](10+$index)
            intent_ordinal_valid=$true;output_frame=$index;source_count=0;sources=@()
        }
    }
})
$transportRecords=@(0..2|ForEach-Object{
    [ordered]@{
        native_present_embedded_token_serial=[string](101+$_)
        composition_token_intent_ordinal=[string](10+$_);composition_token_intent_valid=$true
        native_present_serial=[string](1+$_);native_present_intent_ordinal=[string](10+$_)
        native_present_intent_valid=$true
        formal_transport_eligible=$true;suppression_exact=$false
        transport_disposition='TRANSPORT'
    }
})
$transport=[ordered]@{
    schema='mvm-p2-d5-2-w2-b1-intent-identity-transport-2'
    abi_version=6;app_abi_version=6;qt_abi_version_observed=6
    layout_handshake_accepted=$true;layout_signature='123456789'
    composition_token_size=120;native_present_record_size=200
    shadow_only=$true;performance_accounting_connected=$false;formal_mode=$true
    record_count=3;transport_exact=$true;verdict='INTENT_IDENTITY_ABI_V4_TRANSPORT_EXACT'
    records=$transportRecords
}
$hook=[ordered]@{
    abi_version=6;composition_token_size=120;native_present_record_size=200
    layout_signature='123456789';qt_abi_version_observed=6;layout_handshake_accepted=$true
    available=$true;hook_enabled=$true;capture_started=$true;capture_stopped=$true
    overflow_count=0;missing_token_count=0;duplicate_token_count=0;stale_token_count=0
    token_set_failure_count=0;failed_present_count=0;authority_failure=$false
    record_count=3;records=$native;intent_identity_transport=$transport
}
$app=[ordered]@{
    process_id=1234
    presentation_opportunity=[ordered]@{measurement_start_qpc=100;measurement_end_qpc_exclusive=400}
    native_present_hook=$hook
}
$events=@(0..2|ForEach-Object{
    $index=$_
    [ordered]@{
        sequence_index=20+$index;present_start_qpc=120+100*$index
        process_id=1234;thread_id=77;swap_chain_address='0x1000'
        sync_interval=1;present_flags=0;final_state='Presented';is_completed=$true;is_lost=$false
        displayed=@([ordered]@{frame_type='Application';qpc=500+100*$index})
    }
})
$etw=[ordered]@{
    schema='mvm-p2-etw-present-history-1';raw_displayed_qpc=$true;target_process_id=1234
    etw_events_lost=0;etw_buffers_lost=0;present_event_overflow_count=0;events=$events
}

switch($Case){
    'GoodSuppressedPresented'{
        $native[1].intent_ordinal='0';$native[1].intent_ordinal_valid=$false
        $native[1].composition_token.intent_ordinal='0'
        $native[1].composition_token.intent_ordinal_valid=$false
        $transportRecords[1].composition_token_intent_ordinal='0'
        $transportRecords[1].native_present_intent_ordinal='0'
        $transportRecords[1].composition_token_intent_valid=$false
        $transportRecords[1].native_present_intent_valid=$false
        $transportRecords[1].formal_transport_eligible=$false
        $transportRecords[1].suppression_exact=$true
        $transportRecords[1].transport_disposition='SUPPRESS_DUPLICATE_CALLBACK'
    }
    'GoodDiscarded'{$events[1].final_state='Discarded';$events[1].displayed=@()}
    'GoodDisplayedOutsideMeasurementDomain'{$events[1].displayed[0].qpc=999999}
    'NegativeMissingPresentEvent'{$etw.events=@($events|Select-Object -SkipLast 1)}
    'NegativeDuplicatePresentEvent'{
        $duplicate=($events[1]|ConvertTo-Json -Depth 8|ConvertFrom-Json);$duplicate.sequence_index=23
        $etw.events=@($events)+@($duplicate)
    }
    'NegativeAmbiguousCandidate'{$events[1].present_start_qpc=130}
    'NegativeSequenceDiscontinuity'{$events[1].sequence_index=99}
    'NegativeThreadMismatch'{$events[1].thread_id=88}
    'NegativeSyncIntervalMismatch'{$events[1].sync_interval=0}
    'NegativePresentFlagsMismatch'{$events[1].present_flags=2}
    'NegativePresentStartOutsideNativeInterval'{$events[1].present_start_qpc=180}
    'NegativeSwapchainMismatch'{$events[1].swap_chain_address='0x2000'}
    'NegativeFinalStateUnknown'{$events[1].final_state='Unknown';$events[1].displayed=@()}
    'NegativeTerminalMissing'{$events[1].Remove('final_state')}
    'NegativePresentedWithoutDisplayedQpc'{$events[1].displayed=@()}
    'NegativeEtwLost'{$etw.etw_events_lost=1}
    'NegativeEtwBufferLoss'{$etw.etw_buffers_lost=1}
    'NegativeNativePresentCountMismatch'{$hook.records=@($native|Select-Object -SkipLast 1);$hook.record_count=2}
    'NegativeNativePresentDuplicate'{$native[1].present_serial=$native[0].present_serial}
}
$app|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $appPath -Encoding utf8
$etw|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $etwPath -Encoding utf8

if($Case-eq'NegativeIntentJoinMutation'){
    & pwsh -NoProfile -File $Checker -AppJson $appPath -EtwJson $etwPath -Output $candidate `
        -SourceRoot $SourceRoot *> $null
    if($LASTEXITCODE-ne0){throw 'intent mutation用の対照ledger生成に失敗しました'}
    $ledger=Get-Content -LiteralPath $candidate -Raw -Encoding utf8|ConvertFrom-Json
    $ledger.records[1].intent_ordinal=$ledger.records[0].intent_ordinal
    $ledger|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $candidate -Encoding utf8
    & pwsh -NoProfile -File $Checker -AppJson $appPath -EtwJson $etwPath -Output $output `
        -SourceRoot $SourceRoot -CandidateLedger $candidate *> $null
}else{
    & pwsh -NoProfile -File $Checker -AppJson $appPath -EtwJson $etwPath -Output $output `
        -SourceRoot $SourceRoot *> $null
}
$actual=$LASTEXITCODE
$good=$Case-in@('GoodPresented','GoodDiscarded','GoodDisplayedOutsideMeasurementDomain','GoodSuppressedPresented')
$expected=if($good){0}else{1}
if($actual-ne$expected){throw "$Case W2-B2 contract exitが不正です: expected=$expected actual=$actual"}
if($good){
    $result=Get-Content -LiteralPath $output -Raw -Encoding utf8|ConvertFrom-Json
    if($result.verdict-ne'NATIVE_PRESENT_TERMINAL_OUTCOME_EXACT'-or
       [bool]$result.physical_mapping_connected-or[bool]$result.performance_accounting_connected){
        throw "$Case W2-B2 shadow artifactが不正です"
    }
    if($Case-eq'GoodSuppressedPresented'-and
       ([int]$result.presented_event_count-ne3-or[int]$result.formal_presented_event_count-ne2)){
        throw 'suppressed Presentをformal Presentedへ算入しています'
    }
}
Write-Output "W2-B2 $Case contract: PASS"
