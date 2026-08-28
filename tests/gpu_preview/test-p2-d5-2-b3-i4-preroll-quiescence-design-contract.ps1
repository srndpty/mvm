[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'Good',
        'NegativeCurrentQueueStartBeforeClosure',
        'NegativeMeasurementWindowBeforeHandshake',
        'NegativeRetroactiveForeignOwner',
        'NegativeCurrentPresentBoundary',
        'NegativeTimeoutAsPerformanceDrop',
        'NegativeAdmissionCloseAsSchedulerClose',
        'NegativePendingOpportunityOmitted',
        'NegativePhaseBoolAuthority',
        'NegativeRequiredSetShrink',
        'NegativePositionalIgnoreRetained',
        'NegativeSerialInference',
        'NegativeHistoricalMismatchReclassified')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Contract,
    [Parameter(Mandatory=$true)][string]$RepoRoot
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

function Require([bool]$Condition,[string]$Message){if(-not$Condition){throw $Message}}
function Index-Of([object[]]$Values,[string]$Expected){
    for($index=0;$index-lt$Values.Count;$index++){if($Values[$index]-eq$Expected){return $index}}
    return -1
}
function Validate-Design([object]$Design){
    Require ($Design.schema-eq'mvm-p2-d5-2-b3-i4-preroll-transition-quiescence-design-1') 'schemaが不正です'
    Require ($Design.status-eq'DESIGN_ONLY') 'I4がdesign-onlyではありません'
    Require (-not$Design.production_behavior_changed-and-not$Design.queue_semantics_changed-and-not$Design.join_accept_reject_changed) 'production/I0/I1 semanticsを変更しています'
    Require (-not$Design.canonical_w3_allowed) 'I4でcanonical W3を許可しています'

    $inventoryNames=@($Design.inventory|ForEach-Object{$_.state})
    foreach($requiredState in @(
        'preroll_started','preroll_active','preroll_completed','required_queue_reservation',
        'scheduler_pending_render','scheduler_pending_qualified_evidence',
        'scheduler_render_completion','scheduler_pending_opportunity','join_state',
        'composition_token_pending','native_present_record','frame_swapped_receipt_pending',
        'qualified_swap_commit')){
        Require ($inventoryNames-contains$requiredState) "lifecycle inventoryが不足しています: $requiredState"
    }

    Require (-not$Design.quiescence.phase_bool_alone_is_authority) 'phase bool単独をquiescence authorityにしています'
    Require $Design.quiescence.same_epoch_snapshot_required '同一epoch snapshotを要求していません'
    foreach($predicate in @(
        'PREROLL_ADMISSION_CLOSED','SCHEDULER_PENDING_RENDER_FALSE',
        'SCHEDULER_PENDING_QUALIFIED_EVIDENCE_FALSE','QUEUE_ACTIVE_RESERVATION_COUNT_ZERO',
        'SCHEDULER_PENDING_OPPORTUNITY_FALSE_OR_EXACTLY_FINALIZED',
        'JOIN_ACTIVE_RESERVATION_FALSE','QT_PENDING_COMPOSITION_TOKEN_FALSE',
        'QT_PENDING_FRAME_SWAPPED_RECEIPT_FALSE',
        'ISSUED_EQUALS_RENDERED_EQUALS_QUALIFIED_COMMIT_EQUALS_DEQUEUED',
        'QUEUE_CONSERVATION_VALID','ISSUED_PREFIX_EXACT_IDENTITY_CLOSED',
        'PREROLL_SCOPE_LEDGER_TERMINAL_PARTITION_EXACT',
        'TRANSPORT_FAILURE_COUNTERS_ZERO')){
        Require (@($Design.quiescence.all_of)-contains$predicate) "quiescence predicateが不足しています: $predicate"
    }
    Require (-not$Design.quiescence.unissued_tail_participates_in_quiescence-and$Design.quiescence.unissued_tail_remains_immutable) 'unissued tailのimmutable contractが不正です'

    $steps=@($Design.transition.ordered_steps)
    $ack=Index-Of $steps 'PREROLL_QUIESCENCE_SNAPSHOT_AND_ACK'
    $queue=Index-Of $steps 'CURRENT_REQUIRED_QUEUE_START'
    $arm=Index-Of $steps 'CANONICAL_MEASUREMENT_ARM'
    $window=Index-Of $steps 'CURRENT_ISSUANCE_GATE_OPEN_AND_MEASUREMENT_WINDOW_START'
    Require ($ack-ge0-and$queue-gt$ack) 'preroll closureより前にcurrent queueをstartしています'
    Require ($arm-gt$ack-and$window-gt$arm) 'handshake完了前にmeasurement windowを開始しています'
    Require ($Design.transition.handshake_completed_before_measurement_arm) 'measurement arm前のhandshake完了を要求していません'
    Require (-not$Design.transition.admission_close_is_scheduler_close-and$Design.transition.scheduler_close_occurs_after_active_transaction_drain) 'admission closeとscheduler closeが未完了transactionのdrainを阻害します'
    Require (-not$Design.transition.waiting_charged_to_required_intent_window) 'handshake waitをrequired-intent windowへ混入しています'
    Require (-not$Design.transition.current_intent_issued_while_unresolved) 'unresolved preroll中のcurrent intent発行を許可しています'
    Require (-not$Design.transition.required_set_shrink_allowed) 'timeout時のrequired set縮小を許可しています'
    Require ($Design.transition.timeout_disposition-eq'PROTOCOL_FAIL_CLOSE_NOT_PERFORMANCE_DROP') 'timeoutをprotocol failure以外へ分類しています'

    Require $Design.boundary_ownership.positional_ignore_next_swap_removable 'positional ignore-next-swapを残しています'
    Require ($Design.boundary_ownership.selected_design-eq'QUIESCENCE_HANDSHAKE') 'closure handshakeがselected designではありません'
    Require $Design.boundary_ownership.candidate_may_be_created_only_for_currently_active_foreign_reservation 'completed FOREIGNへのowner後付けを禁止していません'
    Require (-not$Design.boundary_ownership.retroactive_owner_for_completed_foreign_present_allowed) 'completed FOREIGN Presentへのretroactive ownerを許可しています'
    Require (-not$Design.boundary_ownership.current_measurement_present_may_be_boundary) 'CURRENT Presentをboundaryとして許可しています'
    Require (-not$Design.boundary_ownership.candidate_is_quiescence_substitute) 'exact boundary reservationをquiescenceの代用にしています'

    foreach($authority in @('NEAREST_QPC','LATEST_PRESENT','CALLBACK_INDEX','PRESENT_SERIAL_INFERENCE','TOKEN_SERIAL_INFERENCE')){
        Require (@($Design.prohibited_identity_authorities)-contains$authority) "禁止identity authorityが不足しています: $authority"
    }
    $negativeNames=@($Design.negative_contracts|ForEach-Object{$_.name})
    foreach($negativeName in @(
        'CURRENT_QUEUE_START_BEFORE_PREROLL_CLOSURE',
        'MEASUREMENT_WINDOW_STARTS_BEFORE_HANDSHAKE_COMPLETION',
        'RETROACTIVE_OWNER_FOR_COMPLETED_FOREIGN_PRESENT',
        'CURRENT_PRESENT_CONSUMED_AS_BOUNDARY',
        'QUIESCENCE_TIMEOUT_TREATED_AS_PERFORMANCE_DROP')){
        Require ($negativeNames-contains$negativeName) "negative contractが不足しています: $negativeName"
        $entry=@($Design.negative_contracts|Where-Object{$_.name-eq$negativeName})
        Require ($entry.Count-eq1-and$entry[0].expected-eq'PROTOCOL_FATAL') "negative verdictが不正です: $negativeName"
    }
    Require ($Design.historical_composition_token_mismatch.status-eq'UNRESOLVED_HISTORICAL_RUNTIME_FAILURE') 'historical COMPOSITION_TOKEN_MISMATCHを未解決として保持していません'
    Require (-not$Design.historical_composition_token_mismatch.reclassified_as_i3_boundary_failure) 'historical mismatchをI3 failureへ再分類しています'
}

$design=Get-Content -Raw -LiteralPath $Contract -Encoding utf8|ConvertFrom-Json
switch($Case){
    'Good'{}
    'NegativeCurrentQueueStartBeforeClosure'{$design.transition.ordered_steps=@('CURRENT_REQUIRED_QUEUE_START')+$design.transition.ordered_steps}
    'NegativeMeasurementWindowBeforeHandshake'{$design.transition.ordered_steps=@('CURRENT_ISSUANCE_GATE_OPEN_AND_MEASUREMENT_WINDOW_START')+$design.transition.ordered_steps}
    'NegativeRetroactiveForeignOwner'{$design.boundary_ownership.retroactive_owner_for_completed_foreign_present_allowed=$true}
    'NegativeCurrentPresentBoundary'{$design.boundary_ownership.current_measurement_present_may_be_boundary=$true}
    'NegativeTimeoutAsPerformanceDrop'{$design.transition.timeout_disposition='PERFORMANCE_DROP'}
    'NegativeAdmissionCloseAsSchedulerClose'{$design.transition.admission_close_is_scheduler_close=$true}
    'NegativePendingOpportunityOmitted'{$design.quiescence.all_of=@($design.quiescence.all_of|Where-Object{$_-ne'SCHEDULER_PENDING_OPPORTUNITY_FALSE_OR_EXACTLY_FINALIZED'})}
    'NegativePhaseBoolAuthority'{$design.quiescence.phase_bool_alone_is_authority=$true}
    'NegativeRequiredSetShrink'{$design.transition.required_set_shrink_allowed=$true}
    'NegativePositionalIgnoreRetained'{$design.boundary_ownership.positional_ignore_next_swap_removable=$false}
    'NegativeSerialInference'{$design.prohibited_identity_authorities=@($design.prohibited_identity_authorities|Where-Object{$_-ne'PRESENT_SERIAL_INFERENCE'})}
    'NegativeHistoricalMismatchReclassified'{$design.historical_composition_token_mismatch.reclassified_as_i3_boundary_failure=$true}
}

$failed=$false
try{Validate-Design $design}catch{$failed=$true}
if($Case-eq'Good'){
    if($failed){Validate-Design $design}
    $header=Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src/app/preview/compositor_rhi_item.h')
    $renderer=Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src/app/preview/compositor_rhi_item.cpp')
    $queueSource=Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src/media/gpu_preview/required_intent_queue.cpp')
    $qtPatch=Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'qt-patches/qtbase-6.11.1/0001-mvm-native-present-hook.patch')
    foreach($field in @('formalOpportunityEnvelopePrerollStarted','formalOpportunityEnvelopePrerollActive','formalOpportunityEnvelopePrerollCompleted')){Require ($header-match[regex]::Escape($field)) "preroll phase fieldが見つかりません: $field"}
    Require ($renderer-match'closeWithoutNormalCompletion\(\)[\s\S]+formalOpportunityEnvelopePrerollCompleted\.store') '現行Completed writerとnon-normal closeの関係をinventoryできません'
    Require ($queueSource-match'active reservationとunissued tailは意図的に保持') 'non-normal closeがactive/tailを保持するsource contractがありません'
    Require ($renderer-match'formalQualifiedCommitJoin\.reserve[\s\S]+markFormalRenderComplete') 'reservation/render completion lifecycleをinventoryできません'
    Require ($renderer-match'takeFrameSwappedReceipt[\s\S]+bindNativePresent[\s\S]+commitFrameSwapped[\s\S]+commitQualifiedPresent[\s\S]+commitSwap') 'receiptからswap commitまでのlifecycleをinventoryできません'
    Require ($qtPatch-match'mvmPendingTokenValid = true[\s\S]+mvmPendingTokenValid = false') 'Qt one-shot token lifecycleをinventoryできません'
    Require ($qtPatch-match'mvmFrameSwappedReceiptValid = true[\s\S]+mvmFrameSwappedReceiptValid = false') 'Qt one-shot receipt lifecycleをinventoryできません'
    Write-Output 'P2-D5-2 B3-I4 preroll quiescence design contract Good: PASS'
    exit 0
}
if(-not$failed){throw "$Case をB3-I4 design contractが検出できませんでした"}
Write-Output "P2-D5-2 B3-I4 preroll quiescence design contract $Case`: PASS"
