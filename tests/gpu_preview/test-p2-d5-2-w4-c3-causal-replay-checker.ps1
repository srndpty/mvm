[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Checker,
    [Parameter(Mandatory=$true)][string]$Directory,
    [ValidateSet('Good','GoodLosingExplicitStopPublication','GoodExactInt64BoundaryDivision',
        'GoodObservedOrdinalGapPattern','NegativeRequiredMembershipReplayMutation',
        'NegativeRequiredMembershipExactMissing','NegativeC2LedgerPerformanceAuthorityPromotion',
        'NegativeC2LedgerRecordCountMismatch','NegativeC2LedgerPhysicalAuthorityPromotion',
        'NegativeTerminalMembershipFalse','NegativeTerminalPreStateMismatch',
        'NegativeStateContinuityBreak','NegativeMissingReplayField','NegativeMissingStopWitness',
        'NegativeDuplicateWitness','NegativeTerminalInvocationJoinMutation',
        'NegativeArbitrationWinnerMutation','NegativeArbitrationPreviousMutation',
        'NegativeMeasurementStartStateMutation','NegativeResetCountMutation',
        'NegativeCaptureGateExchangeMutation','NegativeGateCloseSnapshotMissing',
        'NegativeDomainTerminalClaimSourceMutation','NegativeTerminalWitnessFactMismatch',
        'NegativeTerminalTargetPredicateMutation','NegativeSchedulerConfigMutation',
        'NegativeCheckedOverflowBypass','NegativeNearestQpcJoin','NegativePostTerminalInvocation',
        'NegativeInvalidFatalPresent','NegativeRootCauseDeclaredWithoutExactReplay',
        'NegativePerformanceAuthorityPromotion')]
    [string]$Case='Good'
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

# synthetic artifactでchecker自身のfail-closed性を閉じる。実captureはここでは取らない。
# 60fps source / 30Hz refresh。terminalではtarget >= required_frame_count だが、
# ordinalはまだrequired-intent domain内（membership=true）という実workload形状にする。
$requiredFrameCount=10
$origin=1000
$schedulerConfig=[ordered]@{
    source_frame_offset=0;source_fps_numerator=60;source_fps_denominator=1
    refresh_numerator=30;refresh_denominator=1;required_frame_count=$requiredFrameCount}
$script:state=[ordered]@{started=$true;closed=$false;anchored=$true;origin_refresh_count=$origin
                         last_finalized_opportunity_ordinal=-1;pending_render=$false
                         past_source_domain=$false}
function Copy-State($State){
    $copy=[ordered]@{}
    foreach($key in $State.Keys){$copy[$key]=$State[$key]}
    return $copy
}
function New-Invocation([int]$Serial,[int64]$Ordinal,[string]$Result,[string]$Reason,
                        [int64]$Target,[bool]$Past,[int64]$Required){
    # C2 ledgerのstate規約: 1 invocation内でlast finalizedは変化しない。
    # primaryはpending renderを立て、terminalはpast_source_domainを立てる。
    $pre=Copy-State $script:state
    $post=Copy-State $script:state
    if($Result-eq'PRIMARY_DECISION'){$post.pending_render=$true}
    $post.past_source_domain=$Past
    $script:state=Copy-State $post
    return [ordered]@{
        scheduler_invocation_serial=$Serial
        invocation_qpc=(10000+$Serial*166)
        input_authority=[ordered]@{refresh_count=($origin+$Ordinal-1);qpc=(10000+$Serial*166)}
        pre=$pre
        result=$Result
        reason=$Reason
        decision_valid=$true
        duplicate_callback=$false
        intent_ordinal=$Ordinal
        target_frame=$Target
        repeat=$false
        past_source_domain=$Past
        required_intent_membership=($Ordinal-ge0-and$Ordinal-lt$Required)
        required_intent_membership_exact=$true
        last_finalized_opportunity_ordinal=$pre.last_finalized_opportunity_ordinal
        formal_transport_disposition=$(if($Ordinal-lt$Required){'TRANSPORT'}
                                       else{'SUPPRESS_OUTSIDE_REQUIRED_SET'})
        formal_transport_disposition_exact=$true
        post=$post
        state_transition_exact=$true
    }
}
function Build-Records([object]$Config,[int64[]]$Ordinals){
    $built=@()
    $required=[int64]$Config.required_frame_count
    $serial=0
    foreach($ordinal in $Ordinals){
        ++$serial
        $numerator=[int64]$ordinal*[int64]$Config.source_fps_numerator*
                   [int64]$Config.refresh_denominator
        $denominator=[int64]$Config.source_fps_denominator*[int64]$Config.refresh_numerator
        $remainder=[int64]0
        $target=[int64]$Config.source_frame_offset+
                [System.Math]::DivRem($numerator,$denominator,[ref]$remainder)
        $past=$target-ge$required
        $built+=,(New-Invocation $serial $ordinal `
            $(if($past){'OUTSIDE_SOURCE_DOMAIN_DECISION'}else{'PRIMARY_DECISION'}) `
            $(if($past){'PAST_SOURCE_DOMAIN'}else{'PRIMARY'}) $target $past $required)
    }
    return $built
}
if($Case-eq'GoodExactInt64BoundaryDivision'){
    # doubleに落ちると商が1ずれるconfig。exact int64除算でのみ再生できる。
    $schedulerConfig=[ordered]@{
        source_frame_offset=0;source_fps_numerator=[int64]9007199254740993
        source_fps_denominator=3;refresh_numerator=1;refresh_denominator=1
        required_frame_count=[int64]6004799503160662}
    $records=Build-Records $schedulerConfig @([int64]1,[int64]2)
}elseif($Case-eq'GoodObservedOrdinalGapPattern'){
    # 実観測形状: invocation serialは連続でも、completed refresh authorityから来る
    # intent ordinalは +2 / +3 で進む。ordinalのauthorityはrefresh countだけ。
    $schedulerConfig=[ordered]@{
        source_frame_offset=0;source_fps_numerator=60;source_fps_denominator=1
        refresh_numerator=30;refresh_denominator=1;required_frame_count=20}
    $records=Build-Records $schedulerConfig @([int64]1,[int64]3,[int64]5,[int64]8,[int64]10)
}else{
    $records=Build-Records $schedulerConfig @([int64]1,[int64]2,[int64]3,[int64]4,[int64]5)
}
$terminalRecord=$records[$records.Count-1]
$artifact=[ordered]@{
    formal_stop_witness=[ordered]@{
        schema='mvm-p2-d5-2-w4-c3-stop-witness-3'
        diagnostic_root_cause_capture=$true
        canonical_performance_authority=$false
        captured=$true
        witness_count=1
        duplicate_witness_count=0
        coalesced_stop_publication_count=0
        losing_stop_claim_count=0
        alternative_stop_fields_are_diagnostic_only=$true
        cause='DOMAIN_TERMINAL'
        render_callback_begin_qpc=10830
        scheduler_invocation_serial=[int64]$terminalRecord.scheduler_invocation_serial
        terminal_intent_ordinal=[int64]$terminalRecord.intent_ordinal
        terminal_target_frame=[int64]$terminalRecord.target_frame
        terminal_past_source_domain=[bool]$terminalRecord.past_source_domain
        terminal_required_intent_membership=[bool]$terminalRecord.required_intent_membership
        stop_arbitration=[ordered]@{
            previous='NONE';claimed='DOMAIN_TERMINAL';claim_succeeded=$true
            claim_recorded=$true;claim_source='THIS_CALL_SITE'
            measurement_start_state='NONE';reset_count_during_measurement=0}
        measurement_start=[ordered]@{explicit_stop_publish_serial=0;fatal_publish_serial=0}
        pre=[ordered]@{capture_gate_open=$true;explicit_stop_requested=$false
                       planned_window_end_reached=$false;fatal_latched=$false
                       explicit_stop_publish_serial=0;fatal_publish_serial=0}
        action=[ordered]@{formal_opportunity_domain_reached_published=$true
                          finish_measurement_entered=$true;capture_gate_exchange_closed=$true
                          measurement_stop_published=$true}
        at_gate_close=[ordered]@{snapshot_captured=$true;explicit_stop_publish_serial=0
                                 fatal_publish_serial=0}
        post=[ordered]@{capture_gate_open=$false;measurement_stop_qpc=10830}
    }
    formal_scheduler_invocation_ledger=[ordered]@{
        schema='mvm-p2-d5-2-w4-c2-scheduler-invocation-ledger-1'
        diagnostic_root_cause_capture=$true
        canonical_performance_authority=$false
        physical_vblank_successor_required=$false
        physical_mapping_support_authority=$false
        measurement_stop_qpc=10830
        native_envelope_close_qpc=10900
        record_count=$records.Count
        scheduler_config=$schedulerConfig
        records=$records
    }
}
$expectPass=$false
switch($Case){
    'Good' {$expectPass=$true}
    'GoodExactInt64BoundaryDivision' {$expectPass=$true}
    'GoodObservedOrdinalGapPattern' {$expectPass=$true}
    'NegativeRequiredMembershipReplayMutation' {
        # ordinalとconfigは正しいまま、membershipだけrecord/witness両方で捏造する。
        $terminal=$artifact.formal_scheduler_invocation_ledger.records[$records.Count-1]
        $second=$artifact.formal_scheduler_invocation_ledger.records[1]
        $second.required_intent_membership=(-not[bool]$second.required_intent_membership)}
    'NegativeRequiredMembershipExactMissing' {
        $artifact.formal_scheduler_invocation_ledger.records[1].required_intent_membership_exact=
            $false}
    'NegativeC2LedgerPerformanceAuthorityPromotion' {
        $artifact.formal_scheduler_invocation_ledger.canonical_performance_authority=$true}
    'NegativeC2LedgerRecordCountMismatch' {
        $artifact.formal_scheduler_invocation_ledger.record_count=($records.Count+1)}
    'NegativeC2LedgerPhysicalAuthorityPromotion' {
        $artifact.formal_scheduler_invocation_ledger.physical_mapping_support_authority=$true}
    'NegativeTerminalMembershipFalse' {
        # source domain外になった時点でrequired intent domainも尽きているcapture。
        # 壊れてはいないが、W4-C3 closureの対象workloadではない。
        $terminal=$artifact.formal_scheduler_invocation_ledger.records[$records.Count-1]
        $terminal.required_intent_membership=$false
        $terminal.formal_transport_disposition='SUPPRESS_OUTSIDE_REQUIRED_SET'
        $artifact.formal_stop_witness.terminal_required_intent_membership=$false}
    'NegativeTerminalPreStateMismatch' {
        # serial/ordinal/target/predicateは触らず、terminalのpre stateだけ壊す。
        $terminal=$artifact.formal_scheduler_invocation_ledger.records[$records.Count-1]
        $terminal.pre.last_finalized_opportunity_ordinal=
            ([int64]$terminal.pre.last_finalized_opportunity_ordinal-3)}
    'NegativeStateContinuityBreak' {
        $artifact.formal_scheduler_invocation_ledger.records[1].pre.origin_refresh_count=
            ($origin+5)}
    'NegativeMissingReplayField' {
        $artifact.formal_scheduler_invocation_ledger.records[0].pre.Remove('anchored')}
    'GoodLosingExplicitStopPublication' {
        # DOMAIN_TERMINALに負けた後着EXPLICIT_STOPがflag/serialを動かしてもPASSさせる。
        $expectPass=$true
        $artifact.formal_stop_witness.pre.explicit_stop_requested=$true
        $artifact.formal_stop_witness.pre.explicit_stop_publish_serial=1
        $artifact.formal_stop_witness.at_gate_close.explicit_stop_publish_serial=1
        $artifact.formal_stop_witness.losing_stop_claim_count=1}
    'NegativeMissingStopWitness' {$artifact.Remove('formal_stop_witness')}
    'NegativeDuplicateWitness' {$artifact.formal_stop_witness.duplicate_witness_count=1}
    'NegativeTerminalInvocationJoinMutation' {
        $artifact.formal_stop_witness.scheduler_invocation_serial=3}
    'NegativeArbitrationWinnerMutation' {
        $artifact.formal_stop_witness.stop_arbitration.claim_succeeded=$false}
    'NegativeArbitrationPreviousMutation' {
        $artifact.formal_stop_witness.stop_arbitration.previous='EXPLICIT_STOP'}
    'NegativeMeasurementStartStateMutation' {
        $artifact.formal_stop_witness.stop_arbitration.measurement_start_state='EXPLICIT_STOP'}
    'NegativeResetCountMutation' {
        $artifact.formal_stop_witness.stop_arbitration.reset_count_during_measurement=1}
    'NegativeCaptureGateExchangeMutation' {
        $artifact.formal_stop_witness.action.capture_gate_exchange_closed=$false}
    'NegativeGateCloseSnapshotMissing' {
        $artifact.formal_stop_witness.at_gate_close.snapshot_captured=$false}
    'NegativeDomainTerminalClaimSourceMutation' {
        $artifact.formal_stop_witness.stop_arbitration.claim_source='PUBLICATION_RECORD'}
    'NegativeTerminalWitnessFactMismatch' {
        # serial joinは正しいままterminal factだけ改変する。
        $artifact.formal_stop_witness.terminal_target_frame=4}
    'NegativeTerminalTargetPredicateMutation' {
        $terminal=$artifact.formal_scheduler_invocation_ledger.records[$records.Count-1]
        $terminal.target_frame=4
        $artifact.formal_stop_witness.terminal_target_frame=4}
    'NegativeSchedulerConfigMutation' {
        $artifact.formal_scheduler_invocation_ledger.scheduler_config.source_fps_numerator=30}
    'NegativeCheckedOverflowBypass' {
        # 中間積がint64をoverflowするconfig。最終値が範囲内でもPASSさせない。
        $artifact.formal_scheduler_invocation_ledger.scheduler_config.source_fps_numerator=
            [int64]4611686018427387904
        $artifact.formal_scheduler_invocation_ledger.scheduler_config.refresh_denominator=
            [int64]4}
    'NegativeNearestQpcJoin' {
        # serialは存在しない値、QPCだけterminalと同一。QPC近傍joinを禁止しているので失敗する。
        $artifact.formal_stop_witness.scheduler_invocation_serial=99}
    'NegativePostTerminalInvocation' {
        $extra=New-Invocation 6 6 'PRIMARY_DECISION' 'PRIMARY' 6 $false
        $artifact.formal_scheduler_invocation_ledger.records=@($records+,$extra)
        $artifact.formal_scheduler_invocation_ledger.record_count=6}
    'NegativeInvalidFatalPresent' {
        $fatal=New-Invocation 6 6 'INVALID_FATAL' 'TARGET_ARITHMETIC_OVERFLOW' -1 $false
        $fatal.decision_valid=$false
        $artifact.formal_scheduler_invocation_ledger.records=@($records+,$fatal)
        $artifact.formal_scheduler_invocation_ledger.record_count=6}
    'NegativeRootCauseDeclaredWithoutExactReplay' {
        # completed refresh -> ordinal の再生が壊れているのにterminalは成立している形。
        $artifact.formal_scheduler_invocation_ledger.records[2].input_authority.refresh_count=
            ($origin+7)}
    'NegativePerformanceAuthorityPromotion' {
        $artifact.formal_stop_witness.canonical_performance_authority=$true}
}
if(Test-Path -LiteralPath $Directory){Remove-Item -LiteralPath $Directory -Recurse -Force}
$null=New-Item -ItemType Directory -Path $Directory -Force
$jsonPath=Join-Path $Directory 'w4c3-artifact.json'
$artifact|ConvertTo-Json -Depth 20|Set-Content -LiteralPath $jsonPath -Encoding utf8
$outputPath=Join-Path $Directory 'w4c3-check.json'
$null=& pwsh -NoProfile -File $Checker -Json $jsonPath -Output $outputPath 2>&1
$checkerExit=$LASTEXITCODE
$summary=Get-Content -LiteralPath $outputPath -Raw -Encoding utf8|ConvertFrom-Json
if($expectPass){
    if($checkerExit-ne0){throw "$Case でcheckerが失敗しました: exit=$checkerExit"}
    if([string]$summary.verdict-ne'W4_C3_CAUSAL_REPLAY_EXACT'){
        throw "$Case のverdictがEXACTではありません: $($summary.verdict)"
    }
    if(-not[bool]$summary.root_cause_determined){throw "$Case でroot causeが確定していません"}
    if([bool]$summary.qpc_used_for_join-or[string]$summary.join_method-ne'scheduler_invocation_serial'){
        throw 'joinがscheduler serial以外に依存しています'
    }
    if([bool]$summary.alternative_stop_fields.used_as_authority){
        throw 'flag/serialがauthorityとして使われています'
    }
}else{
    if($checkerExit-eq0){throw "mutationが検出されませんでした: $Case"}
    if([string]$summary.verdict-notin@('W4_C3_PARTIAL','W4_C3_INCOMPATIBLE')){
        throw "$Case のverdict分類が不正です: $($summary.verdict)"
    }
    if([bool]$summary.root_cause_determined){
        throw "$Case でroot causeが昇格しています"
    }
}
Write-Output ("W4-C3 causal replay checker: PASS ({0} verdict={1})" -f $Case,$summary.verdict)
