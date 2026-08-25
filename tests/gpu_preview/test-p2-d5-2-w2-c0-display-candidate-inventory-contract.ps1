[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'GoodForeignBoundary','GoodLowerForeignOrdinalCollision','GoodUpperCurrentStraddlesEnd',
        'NegativeMissingNative','NegativeMissingIntent','NegativePhysicalAuthority',
        'NegativeIntentScopeMissing','NegativeIntentScopeAmbiguous',
        'NegativeIntentScopeMutation')][string]$Case,
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

function ScopeJoin([string]$Token,[string]$Ordinal,[string]$Scope){
    [ordered]@{
        token_serial=$Token;intent_ordinal=$Ordinal;intent_scope=$Scope;match_count=1;exact=$true
        formal_transport_eligible=$true;transport_disposition='TRANSPORT'
    }
}
function NativeRecord([string]$Present,[int64]$Enter,[int64]$Return,[string]$Token,[string]$Ordinal,[string]$Scope){
    [ordered]@{
        present_serial=$Present;swapchain_identity='4096';thread_id=77
        present_enter_qpc=$Enter;present_return_qpc=$Return;sync_interval=1;present_flags=0
        token_present=$true;intent_ordinal=$Ordinal;intent_ordinal_valid=$true
        composition_token=[ordered]@{token_serial=$Token;intent_ordinal=$Ordinal;intent_ordinal_valid=$true}
        intent_scope_provenance=(ScopeJoin $Token $Ordinal $Scope)
    }
}
$native=@(
    (NativeRecord '1' 80 95 '101' '0' 'FOREIGN_PRE_MEASUREMENT'),
    (NativeRecord '2' 120 150 '102' '0' 'CURRENT_MEASUREMENT'),
    # current intentだがnative returnだけがmeasurement endをまたぐ。
    (NativeRecord '3' 180 220 '103' '1' 'CURRENT_MEASUREMENT'))
$scope=@(
    [ordered]@{token_serial='101';intent_ordinal='0';intent_scope='FOREIGN_PRE_MEASUREMENT'},
    [ordered]@{token_serial='102';intent_ordinal='0';intent_scope='CURRENT_MEASUREMENT'},
    [ordered]@{token_serial='103';intent_ordinal='1';intent_scope='CURRENT_MEASUREMENT'})
$intent=@([ordered]@{
    native_present_serial='2';native_present_embedded_token_serial='102'
    native_present_intent_ordinal='0';native_present_intent_valid=$true
})
$shadow=[ordered]@{
    shadow_authority_valid=$true;shadow_authority_canonical_reason='NONE'
    physical_opportunity_count=2;origin_qpc=100;last_qpc=150
    predecessor_valid=$true;predecessor_qpc=90;successor_valid=$true;successor_qpc=240
}
$scopeAuthority=[ordered]@{
    schema='mvm-p2-d5-2-w2-c011-intent-scope-provenance-1';shadow_only=$true
    abi_version_unchanged=$true;join_key='composition_token.token_serial'
    scope_derived_from_present_qpc=$false;scope_derived_from_source_frame=$false
    scope_derived_from_layer2_membership=$false;record_count=3
    missing_scope_count=0;ambiguous_scope_count=0;mutation_count=0;unmatched_scope_count=0
    authority_pass=$true;records=$scope
}
$app=[ordered]@{
    process_id=1234
    presentation_opportunity=[ordered]@{
        measurement_start_qpc=100;measurement_end_qpc_exclusive=200
        physical_vblank=[ordered]@{samples=@(
            [ordered]@{ordinal=0;qpc=90},[ordered]@{ordinal=1;qpc=100},
            [ordered]@{ordinal=2;qpc=150},[ordered]@{ordinal=3;qpc=240})}
        physical_vblank_domain_shadow=$shadow
    }
    native_present_hook=[ordered]@{
        records=@($native[1]);capture_envelope_records=$native
        capture_envelope=[ordered]@{authority_pass=$true;overflow_count=0}
        intent_identity_transport=[ordered]@{records=$intent}
        intent_scope_provenance=$scopeAuthority
    }
}
$events=@(
    [ordered]@{sequence_index=10;present_start_qpc=90;process_id=1234;thread_id=77;swap_chain_address='0x1000';sync_interval=1;present_flags=0;final_state='Presented';displayed=@([ordered]@{qpc=110})},
    [ordered]@{sequence_index=11;present_start_qpc=130;process_id=1234;thread_id=77;swap_chain_address='0x1000';sync_interval=1;present_flags=0;final_state='Presented';displayed=@([ordered]@{qpc=150})},
    [ordered]@{sequence_index=12;present_start_qpc=190;process_id=1234;thread_id=77;swap_chain_address='0x1000';sync_interval=1;present_flags=0;final_state='Presented';displayed=@([ordered]@{qpc=190})})
if($Case-eq'NegativeMissingNative'){
    $events+=,[ordered]@{sequence_index=13;present_start_qpc=225;process_id=1234;thread_id=77;swap_chain_address='0x1000';sync_interval=1;present_flags=0;final_state='Presented';displayed=@([ordered]@{qpc=230})}
}
if($Case-eq'NegativeMissingIntent'){$native[1].intent_ordinal_valid=$false}
if($Case-eq'NegativePhysicalAuthority'){
    $shadow.shadow_authority_valid=$false;$shadow.shadow_authority_canonical_reason='PHYSICAL_VBLANK_BOUNDARY_NOT_BRACKETED'
    $shadow.successor_valid=$false
}
if($Case-eq'NegativeIntentScopeMissing'){
    $scopeAuthority.records=@($scope|Where-Object{$_.token_serial-ne'102'})
    $scopeAuthority.record_count=2;$scopeAuthority.missing_scope_count=1;$scopeAuthority.authority_pass=$false
}
if($Case-eq'NegativeIntentScopeAmbiguous'){
    $scopeAuthority.records=@($scope)+@([ordered]@{token_serial='102';intent_ordinal='0';intent_scope='FOREIGN_PRE_MEASUREMENT'})
    $scopeAuthority.record_count=4;$scopeAuthority.ambiguous_scope_count=1;$scopeAuthority.authority_pass=$false
}
if($Case-eq'NegativeIntentScopeMutation'){
    $native[1].intent_scope_provenance.intent_scope='FOREIGN_PRE_MEASUREMENT'
}
$etw=[ordered]@{
    target_process_id=1234;etw_events_lost=0;etw_buffers_lost=0;present_event_overflow_count=0
    events=$events
}
# B2 cohortはreturn < endのrecordだけ。upper straddleをscope分類へ流用しない。
$ledger=[ordered]@{
    schema='mvm-p2-d5-2-w2-b2-terminal-shadow-1';verdict='NATIVE_PRESENT_TERMINAL_OUTCOME_EXACT'
    physical_mapping_connected=$false;performance_accounting_connected=$false
    records=@([ordered]@{etw_sequence=11})
}
$summary=[ordered]@{matrix_pass=$true;verdict='NATIVE_PRESENT_TERMINAL_OUTCOME_EXACT';runs=1}
$app|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $appPath -Encoding utf8
$etw|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $etwPath -Encoding utf8
$ledger|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $ledgerPath -Encoding utf8
$summary|ConvertTo-Json -Depth 4|Set-Content -LiteralPath $summaryPath -Encoding utf8
& pwsh -NoProfile -File $Inventory -B2LiveDirectory $Directory -Output $output -RequireCoverageComplete *> $null
$actual=$LASTEXITCODE;$positive=$Case-like'Good*';$expected=if($positive){0}else{1}
if($actual-ne$expected){throw "$Case W2-C0.1.1 contract exitが不正です: expected=$expected actual=$actual"}
$result=Get-Content -LiteralPath $output -Raw -Encoding utf8|ConvertFrom-Json
if($positive){
    $run=$result.runs[0]
    if(-not[bool]$result.coverage_complete-or-not[bool]$result.intent_scope_exact-or
       $run.observed_domain_foreign_intent_exact_count-ne1-or
       $run.observed_domain_current_intent_exact_count-ne2){
        throw "$Case intent scope exact coverageが成立しません"
    }
    if($Case-eq'GoodLowerForeignOrdinalCollision'){
        $zeroScopes=@($run.candidates|Where-Object{$_.intent_ordinal-eq'0'}|Select-Object -ExpandProperty intent_scope -Unique)
        if($zeroScopes.Count-ne2){throw '同じordinalのforeign/currentをscopeで区別できません'}
    }
    if($Case-eq'GoodUpperCurrentStraddlesEnd'){
        $upper=$run.candidates|Where-Object{$_.etw_sequence-eq12}
        if($upper.layer2_cohort_member-or$upper.intent_scope-ne'CURRENT_MEASUREMENT'){
            throw 'upper straddle current intentがLayer2外という理由でforeign化されました'
        }
    }
}elseif([bool]$result.coverage_complete){throw "$Case がcoverage completeとして受理されました"}
Write-Output "W2-C0.1.1 $Case contract: PASS"
