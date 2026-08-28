param([Parameter(Mandatory=$true)][string]$RepoRoot)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

function Read-Source([string]$RelativePath){Get-Content -Raw -LiteralPath (Join-Path $RepoRoot $RelativePath)}
function Require([string]$Text,[string]$Pattern,[string]$Message){if($Text-notmatch$Pattern){throw $Message}}
function Reject([string]$Text,[string]$Pattern,[string]$Message){if($Text-match$Pattern){throw $Message}}

$handshakeHeader=Read-Source 'src/media/gpu_preview/preroll_transition_handshake.h'
$handshake=Read-Source 'src/media/gpu_preview/preroll_transition_handshake.cpp'
$header=Read-Source 'src/app/preview/compositor_rhi_item.h'
$renderer=Read-Source 'src/app/preview/compositor_rhi_item.cpp'
$controller=Read-Source 'apps/compositor_spike/compositor_spike_controller.cpp'

# --- explicit state machine -------------------------------------------------
Require $handshakeHeader 'Open = 0,[\s\S]+DrainRequested,[\s\S]+Draining,[\s\S]+QuiescenceCheck,[\s\S]+Quiescent,[\s\S]+CurrentReady,[\s\S]+MeasurementArmed,[\s\S]+CurrentRunning,[\s\S]+ProtocolFatal' 'preroll transition stateの明示列挙と順序がありません'

# --- admission close は scheduler close ではない -----------------------------
Require $handshake 'requestAdmissionClose[\s\S]{0,600}foreignAdmissionClosed_ = true' 'admission closeが新規FOREIGN reservation gateを閉じていません'
Reject $handshake 'requestAdmissionClose[\s\S]{0,600}foreignSchedulerClosed_ = true' 'admission closeがscheduler closeを兼ねています'
Require $handshake 'foreignTransactionProgressAllowed\(\)\)\s*\r?\n\s*return fail\(PrerollTransitionError::ForeignProgressAfterQuiescence\)' 'existing FOREIGN transactionの前進許可がstate machineにありません'
Require $handshake 'noteForeignSchedulerClosed[\s\S]{0,600}activeForeignTransactionCount_ != 0\)\s*\r?\n\s*return fail\(PrerollTransitionError::SchedulerCloseBeforeDrain\)' 'scheduler closeがactive transaction drainを要求していません'
Require $handshake 'noteForeignSchedulerClosed[\s\S]{0,700}!pendingOpportunityFinalized_\)\s*\r?\n\s*return fail\(PrerollTransitionError::PendingOpportunityNotFinalized\)' 'scheduler closeがpending opportunity finalizeを要求していません'

# --- quiescence predicate 13件を個別fieldとして保存する ----------------------
foreach($predicate in @(
    'prerollAdmissionClosed','schedulerPendingRenderFalse','schedulerPendingQualifiedEvidenceFalse',
    'schedulerPendingOpportunityFalseOrExactlyFinalized','queueActiveReservationCountZero',
    'joinActiveReservationFalse','qtPendingCompositionTokenFalse','qtPendingFrameSwappedReceiptFalse',
    'issuedEqualsRenderedEqualsQualifiedCommitEqualsDequeued','queueConservationValid',
    'issuedPrefixExactIdentityClosed','prerollScopeLedgerTerminalPartitionExact',
    'transportFailureCountersZero')){
    Require $handshakeHeader ([regex]::Escape($predicate)) "quiescence predicate fieldが不足しています: $predicate"
    Require $handshake ('verdict\.'+[regex]::Escape($predicate)+'\s*(&&|;)') "quiescence predicateがconjunctionから外れています: $predicate"
}
Require $handshake 'verdict\.sameCaptureEpoch &&\s*\r?\n?\s*verdict\.sameRenderThread' 'quiescenceがsame epoch / same threadを要求していません'
Require $handshake 'observation\.captureEpoch == captureEpoch_' 'capture epochのexact一致検査がありません'
Require $handshake 'observation\.observerThreadId == renderThreadId_' 'render threadのexact一致検査がありません'
Require $handshake '!verdict\.sameCaptureEpoch\)[\s\S]{0,120}CaptureEpochMismatch' 'mixed epoch snapshotをfail-closeしていません'
Require $handshake '!verdict\.sameRenderThread\)[\s\S]{0,120}RenderThreadMismatch' 'wrong render thread snapshotをfail-closeしていません'

# --- ordered steps ----------------------------------------------------------
Require $handshake '!verdict_\.evaluated \|\| !verdict_\.quiescent\)\s*\r?\n\s*return fail\(PrerollTransitionError::QuiescencePredicateFailed\)' 'predicate failureでackを拒否していません'
Require $handshake 'startCurrentRequiredQueue[\s\S]{0,400}state_ != PrerollTransitionState::Quiescent\)\s*\r?\n\s*return fail\(PrerollTransitionError::CurrentQueueStartBeforeQuiescence\)' 'quiescence ack前のcurrent queue startを拒否していません'
Require $handshake 'startCurrentRequiredQueue[\s\S]{0,500}currentIssuanceOpen_ = false' 'current queue初期化がissuanceを閉じたままにしていません'
Require $handshake 'armMeasurement[\s\S]{0,400}state_ != PrerollTransitionState::CurrentReady\)\s*\r?\n\s*return fail\(PrerollTransitionError::MeasurementArmBeforeCurrentQueue\)' 'current queue start前のmeasurement armを拒否していません'
Require $handshake 'armMeasurement[\s\S]{0,900}canonicalWindowFrozen_ = true' 'canonical start/end authorityをfreezeしていません'
Require $handshake 'openCurrentIssuanceGate[\s\S]{0,400}state_ != PrerollTransitionState::MeasurementArmed\)\s*\r?\n\s*return fail\(PrerollTransitionError::IssuanceBeforeMeasurementArm\)' 'measurement arm前のissuance gate openを拒否していません'

# --- handshake waitはcanonical measurement windowの外 -----------------------
Require $handshake 'canonicalStartQpc < quiescenceAckQpc_\)\s*\r?\n\s*return fail\(PrerollTransitionError::CanonicalWindowMutated\)' 'handshake wait区間をmeasurement windowから除外していません'
Require $handshake 'snapshot\.waitChargedToMeasurementWindow = false' 'handshake waitをmeasurement windowへ算入しています'
Require $handshake 'HandshakeTimeout' 'timeoutのfail-closeがありません'

# --- positional ignore-next-swapの削除 --------------------------------------
Reject $header 'formalOpportunityIgnoreNextSwap' 'positional ignore-next-swapがheaderに残っています'
Reject $renderer 'formalOpportunityIgnoreNextSwap' 'positional ignore-next-swapがrendererに残っています'
Reject $controller 'ignore_publication_serial' 'positional ignore-next-swap artifactがcontrollerに残っています'

# --- renderer wiring --------------------------------------------------------
Require $renderer 'requestPrerollAdmissionClose[\s\S]+advancePrerollTransition[\s\S]+startCurrentRequiredQueue[\s\S]+armMeasurement[\s\S]+openCurrentIssuanceGate' 'transition stepのsource順序がordered stepsと一致しません'
Require $renderer 'output < 0 && formalOpportunityActive && foreignAdmissionOpen' '新規FOREIGN reservationのadmission gateがありません'
Require $renderer 'foreignPreMeasurement &&\s*\r?\n?\s*!state_->formalPrerollTransition\.noteForeignReservationAdmitted' 'FOREIGN admissionをtransition state machineへ記録していません'
Require $renderer 'readOneShotSnapshot\(hook->captureEpoch\(\)' 'quiescence checkがABI v6 one-shot snapshotを使っていません'
Require $renderer 'oneShot\.pendingTokenValid[\s\S]{0,200}oneShot\.pendingReceiptValid' 'Qt one-shot pending stateをquiescenceへ取り込んでいません'
Require $renderer 'computePrerollIdentityClosure' 'issued prefixのexact identity closureを評価していません'
Require $renderer 'transportFailureCounterTotal' 'transport failure counterをquiescenceへ取り込んでいません'
Require $renderer 'finalizePendingOpportunityExact\(\)[\s\S]{0,400}notePendingOpportunityFinalized[\s\S]{0,600}closeWithoutNormalCompletion\(\)[\s\S]{0,200}noteForeignSchedulerClosed' 'scheduler closeがdrain / pending opportunity finalizeより前に来ています'
Require $renderer 'startCurrentRequiredQueue\(\)[\s\S]+startFormalOpportunityScheduler\(\)[\s\S]+const long long measurementArmQpc = gpu::qpcTicks\(\);[\s\S]{0,3000}armMeasurement\(' 'canonical start authorityがcurrent queue / current scheduler準備完了後にsampleされていません'
Reject $renderer 'const long long measurementArmQpc = gpu::qpcTicks\(\);[\s\S]{0,900}startCurrentRequiredQueue' 'canonical startをcurrent queue start前にsampleしています'
Require $renderer 'const long long measurementArmQpc = gpu::qpcTicks\(\);\s*
?
\s*scheduler_\.start\(measurementArmQpc' 'canonical schedulerが同一sampleでstartしていません'
Require $renderer 'armMeasurement\(\s*
?
?\s*measurementArmQpc, measurementArmQpc \+ duration\)' 'canonical windowのfreeze値がarm時点のsampleではありません'
Reject $renderer 'armMeasurement\(callbackBegin' 'handshake評価を含むcallback begin QPCをcanonical window startにしています'
Require $renderer 'openCurrentIssuanceGate\(\)[\s\S]{0,600}formalOpportunityCaptureActive\.store\(true' 'issuance gateがmeasurement arm後のopen siteと結びついていません'
Require $renderer 'PrerollTransitionProgress::Waiting[\s\S]{0,200}update\(\);\s*\r?\n\s*return true' 'quiescence未成立時に待機せずarmしています'
Require $renderer 'record\.token\.tokenSerial != scopeRecord\.tokenSerial' 'identity closureがtoken serialのexact一致で結合していません'
Reject $renderer '(?i)(nearestBoundary|closestBoundary|latestPresentJoin)' 'rendererがnearest/latestからboundary identityを推定しています'

# --- controller artifact ----------------------------------------------------
foreach($fieldName in @(
    'preroll_transition_handshake','positional_ignore_next_swap_removed',
    'admission_close_is_scheduler_close','scheduler_closed_after_active_transaction_drain',
    'handshake_step_order_exact','canonical_start_order_exact',
    'canonical_start_after_current_queue_ready','quiescence_ack_qpc',
    'current_queue_start_event_qpc','measurement_armed_event_qpc',
    'current_issuance_open_event_qpc','handshake_wait_qpc','wait_charged_to_measurement_window',
    'canonical_measurement_start_qpc','canonical_window_frozen','current_issuance_open',
    'boundary_owner_bound','retroactive_owner_for_completed_foreign_present',
    'current_present_consumed_as_boundary','PREROLL_TRANSACTION_FULLY_QUIESCENT',
    'same_capture_epoch','same_render_thread','issued_prefix_exact_identity_closed',
    'preroll_scope_ledger_terminal_partition_exact','transport_failure_counters_zero')){
    Require $controller ([regex]::Escape($fieldName)) "B3-I5B artifact fieldが不足しています: $fieldName"
}
Require $controller 'quiescenceAckQpc <= currentQueueStartEventQpc[\s\S]{0,400}currentQueueStartEventQpc <= prerollTransition\.canonicalMeasurementStartQpc[\s\S]{0,400}canonicalMeasurementStartQpc <= measurementArmedEventQpc[\s\S]{0,200}measurementArmedEventQpc <= issuanceOpenEventQpc' 'ack <= queue start <= canonical start <= arm <= issuance openのruntime closureがありません'
Require $controller 'canonicalStartOrderExact && quiescenceVerdict\.quiescent' 'canonical start orderingがhandshake verdictへ組み込まれていません'
Require $controller '"timeout_disposition", "PROTOCOL_FAIL_CLOSE_NOT_PERFORMANCE_DROP"' 'timeoutをperformance dropへ流していないことを固定していません'
Require $controller '"admission_close_is_scheduler_close", false' 'admission closeとscheduler closeの分離を固定していません'
Require $controller '"nearest_qpc_used", false[\s\S]{0,200}"callback_index_used_as_identity", false[\s\S]{0,200}"event_serial_is_identity_authority", false' 'QPC/callback/event serialをidentity authorityから排除していません'
Require $controller '"status", "UNRESOLVED_HISTORICAL_RUNTIME_FAILURE"[\s\S]{0,300}"reclassified_as_i5b_transition_failure", false' 'historical COMPOSITION_TOKEN_MISMATCHを未解決のまま保持していません'
Require $controller '"queue_semantics_changed", false[\s\S]{0,200}"join_accept_reject_changed", false' 'queue / join semanticsの不変を固定していません'

Write-Output 'P2-D5-2 B3-I5B admission-close / drain handshake architecture: PASS'
