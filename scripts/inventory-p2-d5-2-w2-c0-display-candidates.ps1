[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$B2LiveDirectory,
    [Parameter(Mandatory=$true)][string]$Output,
    [string]$JoinAuthority=(Join-Path $PSScriptRoot 'p2-native-present-event-exact-join.ps1'),
    [switch]$RequireCoverageComplete
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
function Need($Object,[string]$Name){
    if($null-eq$Object-or$Object.PSObject.Properties.Name-notcontains$Name){Fail "必須fieldがありません: $Name"}
    return $Object.$Name
}
function Display-Relation([int64]$Qpc,[int64]$OriginQpc,[int64]$LastQpc){
    if($Qpc-lt$OriginQpc){return 'BEFORE_ORIGIN'}
    if($Qpc-le$LastQpc){return 'WITHIN_OBSERVED_MEMBER_ENVELOPE'}
    return 'AFTER_LAST_SAMPLE_UNRESOLVED'
}
foreach($path in @($B2LiveDirectory,$JoinAuthority)){if(-not(Test-Path -LiteralPath $path)){Fail "W2-C0必須pathがありません: $path"}}
. $JoinAuthority
$summaryPath=Join-Path $B2LiveDirectory 'w2-b2-live-summary.json'
if(-not(Test-Path -LiteralPath $summaryPath)){Fail "W2-B2 live summaryがありません: $summaryPath"}
$b2Summary=Get-Content -LiteralPath $summaryPath -Raw -Encoding utf8|ConvertFrom-Json
if(-not[bool](Need $b2Summary 'matrix_pass')-or[string](Need $b2Summary 'verdict')-ne'NATIVE_PRESENT_TERMINAL_OUTCOME_EXACT'){
    Fail 'W2-B2 live authorityが不成立です'
}
$runCount=[int](Need $b2Summary 'runs');if($runCount-le0){Fail 'W2-B2 run数が不正です'}
$runResults=@();$globalBlockers=@{}
for($run=1;$run-le$runCount;++$run){
    $runDirectory=Join-Path $B2LiveDirectory "run-$run"
    $appPath=Join-Path $runDirectory 'traced-app.json'
    $etwPath=Join-Path $runDirectory 'present-history-raw.json'
    $ledgerPath=Join-Path $runDirectory 'terminal-shadow.json'
    foreach($path in @($appPath,$etwPath,$ledgerPath)){if(-not(Test-Path -LiteralPath $path)){Fail "run $run artifactがありません: $path"}}
    $app=Get-Content -LiteralPath $appPath -Raw -Encoding utf8|ConvertFrom-Json
    $etw=Get-Content -LiteralPath $etwPath -Raw -Encoding utf8|ConvertFrom-Json
    $b2Ledger=Get-Content -LiteralPath $ledgerPath -Raw -Encoding utf8|ConvertFrom-Json
    if([string](Need $b2Ledger 'schema')-ne'mvm-p2-d5-2-w2-b2-terminal-shadow-1'-or
       [string](Need $b2Ledger 'verdict')-ne'NATIVE_PRESENT_TERMINAL_OUTCOME_EXACT'-or
       [bool](Need $b2Ledger 'physical_mapping_connected')-or
       [bool](Need $b2Ledger 'performance_accounting_connected')){
        Fail "run $run W2-B2 ledger authorityが不正です"
    }
    foreach($field in @('etw_events_lost','etw_buffers_lost','present_event_overflow_count')){
        if([int64](Need $etw $field)-ne0){Fail "run $run $field が0ではありません"}
    }
    $join=Invoke-MvmNativePresentEventExactJoin -App $app -Etw $etw
    $opportunity=Need $app 'presentation_opportunity'
    $physical=Need $opportunity 'physical_vblank'
    $shadow=Need $opportunity 'physical_vblank_domain_shadow'
    $samples=@(Need $physical 'samples')
    $measurementStart=[int64](Need $opportunity 'measurement_start_qpc')
    $measurementEnd=[int64](Need $opportunity 'measurement_end_qpc_exclusive')
    $domainMembers=@($samples|Where-Object{[int64]$_.qpc-ge$measurementStart-and[int64]$_.qpc-lt$measurementEnd})
    if($domainMembers.Count-ne[int](Need $shadow 'physical_opportunity_count')){Fail "run $run physical domain member件数がshadowと一致しません"}
    $originQpc=[int64](Need $shadow 'origin_qpc');$lastQpc=[int64](Need $shadow 'last_qpc')
    $nativeAll=@(Need (Need $app 'native_present_hook') 'records')
    $intentAll=@($app.native_present_hook.intent_identity_transport.records)
    $intentByPresent=@{}
    foreach($intent in $intentAll){$serial=[string](Need $intent 'native_present_serial');if($intentByPresent.ContainsKey($serial)){Fail "run $run B1 serialが重複しています: $serial"};$intentByPresent[$serial]=$intent}
    $cohortSequence=@{}
    foreach($record in @($b2Ledger.records)){$cohortSequence[[string]$record.etw_sequence]=$true}

    $allTargetEvents=@($join.all_target_events)
    $presentedCandidates=@();$presentedEventSequences=@{};$multipleDisplayed=0;$observedMissingNative=0;$observedMissingIntent=0
    $observedForeignExact=0;$outsidePresentedCount=0
    $relationCounts=[ordered]@{
        BEFORE_ORIGIN=0
        WITHIN_OBSERVED_MEMBER_ENVELOPE=0
        AFTER_LAST_SAMPLE_UNRESOLVED=0
    }
    foreach($presentEvent in $allTargetEvents){
        $displayed=@(Need $presentEvent 'displayed')
        if([string](Need $presentEvent 'final_state')-ne'Presented'-or$displayed.Count-eq0){continue}
        $presentedEventSequences[[string]$presentEvent.sequence_index]=$true
        if($displayed.Count-ne1){++$multipleDisplayed}
        $startQpc=[int64](Need $presentEvent 'present_start_qpc')
        $nativeCandidates=@($nativeAll|Where-Object{
            $startQpc-ge[int64]$_.present_enter_qpc-and$startQpc-le[int64]$_.present_return_qpc-and
            [int64]$_.thread_id-eq[int64]$presentEvent.thread_id-and
            [int64]$_.sync_interval-eq[int64]$presentEvent.sync_interval-and
            [int64]$_.present_flags-eq[int64]$presentEvent.present_flags
        })
        $nativeExact=$nativeCandidates.Count-eq1
        $intentExact=$false;$intentOrdinal=$null;$nativeSerial=$null
        if($nativeExact){
            $nativeSerial=[string]$nativeCandidates[0].present_serial
            if($intentByPresent.ContainsKey($nativeSerial)){
                $intent=$intentByPresent[$nativeSerial]
                $tokenSerial=[string]$nativeCandidates[0].composition_token.token_serial
                $intentExact=$tokenSerial-eq[string]$intent.native_present_embedded_token_serial-and
                    [string]$nativeCandidates[0].intent_ordinal-eq[string]$intent.native_present_intent_ordinal-and
                    [bool]$nativeCandidates[0].intent_ordinal_valid-and[bool]$intent.native_present_intent_valid
                if($intentExact){$intentOrdinal=[string]$intent.native_present_intent_ordinal}
            }
        }
        $inLayer2=$cohortSequence.ContainsKey([string]$presentEvent.sequence_index)
        if(-not$inLayer2){++$outsidePresentedCount}
        foreach($display in $displayed){
            $displayQpc=[int64](Need $display 'qpc')
            $relation=Display-Relation $displayQpc $originQpc $lastQpc
            $relationCounts[$relation]=[int]$relationCounts[$relation]+1
            if($relation-eq'WITHIN_OBSERVED_MEMBER_ENVELOPE'){
                if(-not$nativeExact){++$observedMissingNative}
                elseif(-not$intentExact){++$observedMissingIntent}
                elseif(-not$inLayer2){++$observedForeignExact}
            }
            $presentedCandidates+=[ordered]@{
                etw_sequence=[int64]$presentEvent.sequence_index
                present_start_qpc=$startQpc;displayed_qpc=$displayQpc;display_relation=$relation
                layer2_cohort_member=$inLayer2;native_candidate_count=$nativeCandidates.Count
                native_exact=$nativeExact;native_present_serial=$nativeSerial
                intent_exact=$intentExact;intent_ordinal=$intentOrdinal
            }
        }
    }
    $blockers=@()
    if(-not[bool](Need $shadow 'shadow_authority_valid')){$blockers+='PHYSICAL_DOMAIN_AUTHORITY_INVALID'}
    if(-not[bool](Need $shadow 'successor_valid')){$blockers+='PHYSICAL_DOMAIN_SUCCESSOR_MISSING'}
    if($multipleDisplayed-ne0){$blockers+='PRESENTED_DISPLAYED_QPC_CARDINALITY_UNRESOLVED'}
    if($observedMissingNative-ne0){$blockers+='OBSERVED_DOMAIN_PRESENTED_NATIVE_JOIN_MISSING'}
    if($observedMissingIntent-ne0){$blockers+='OBSERVED_DOMAIN_PRESENTED_INTENT_JOIN_MISSING'}
    foreach($blocker in $blockers){$globalBlockers[$blocker]=$true}
    $runResults+=[ordered]@{
        run=$run;physical_shadow_valid=[bool]$shadow.shadow_authority_valid
        physical_shadow_reason=[string]$shadow.shadow_authority_canonical_reason
        predecessor_valid=[bool]$shadow.predecessor_valid;successor_valid=[bool]$shadow.successor_valid
        physical_domain_member_count=$domainMembers.Count;origin_qpc=$originQpc;last_qpc=$lastQpc
        all_target_present_event_count=$allTargetEvents.Count;available_native_present_count=$nativeAll.Count
        layer2_cohort_count=@($join.joined).Count;boundary_native_count=@($join.boundary_native).Count
        boundary_event_count=@($join.boundary_events).Count;boundary_exact_join_count=@($join.boundary_joined).Count
        boundary_ambiguous_or_missing_count=@($join.boundary_join_diagnostics|Where-Object{$_.candidate_count-ne1}).Count
        outside_native_cohort_event_count=@($join.outside_cohort_events).Count
        presented_event_count=$presentedEventSequences.Count
        presented_displayed_qpc_count=$presentedCandidates.Count;presented_display_relation_counts=$relationCounts
        outside_layer2_presented_event_count=$outsidePresentedCount
        observed_domain_missing_native_count=$observedMissingNative
        observed_domain_missing_intent_count=$observedMissingIntent
        observed_domain_foreign_intent_exact_count=$observedForeignExact
        coverage_complete=$blockers.Count-eq0;blockers=$blockers;candidates=$presentedCandidates
    }
}
$globalBlockerList=@($globalBlockers.Keys|Sort-Object)
$coverageComplete=$globalBlockerList.Count-eq0
$result=[ordered]@{
    schema='mvm-p2-d5-2-w2-c0-display-candidate-inventory-1';stage='P2-D5-2-W2-C0'
    source_b2_live_directory=(Resolve-Path -LiteralPath $B2LiveDirectory).Path
    layer2_join_semantics_unchanged=$true
    physical_mapping_connected=$false;intent_satisfaction_connected=$false
    performance_accounting_connected=$false
    run_count=$runCount;coverage_complete=$coverageComplete;blockers=$globalBlockerList
    verdict=$(if($coverageComplete){'DISPLAY_DOMAIN_CANDIDATE_COVERAGE_COMPLETE'}else{'DISPLAY_DOMAIN_CANDIDATE_COVERAGE_INCOMPLETE'})
    runs=$runResults
}
$result|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $Output -Encoding utf8
if($RequireCoverageComplete-and-not$coverageComplete){Fail "W2-C0 display candidate coverageが不足しています: $($globalBlockerList-join', ')"}
Write-Host "P2-D5-2-W2-C0 inventory: $($result.verdict) runs=$runCount"
