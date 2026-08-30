[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'Good','NegativePositionalIgnoreRetained','NegativeAdmissionCloseClosesScheduler',
        'NegativeSchedulerCloseBeforeDrain','NegativePendingOpportunityFinalizeSkipped',
        'NegativeQuiescencePredicateDropped','NegativeEpochCheckRemoved',
        'NegativeThreadCheckRemoved','NegativeEarlyQueueStart','NegativeEarlyMeasurementArm',
        'NegativeIssuanceBeforeArm','NegativeAdmissionGateRemoved','NegativeOneShotSnapshotRemoved',
        'NegativeCanonicalStartFromCallbackBegin','NegativeCanonicalStartBeforeCurrentQueueReady',
        'NegativeWaitChargedToWindow','NegativeTimeoutAsPerformanceDrop',
        'NegativeNearestQpcJoin','NegativeHistoricalMismatchReclassified')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Contract,
    [Parameter(Mandatory=$true)][string]$SourceRoot,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
# S2-h2: mutation match を physical line ending から独立させる。
# .cpp/.h は repo policy 上 LF、.ps1 は CRLF である (.gitattributes)。
# here-string の $From/$To は .ps1 の物理改行を持つため、そのままでは LF の
# source と一致しない。source と pattern の両方を LF domain へ射影してから
# match する。assertion semantics は変更しない。checkout 表現への依存を外すだけ。
function Normalize-Lf([string]$Text) {
    if ($null -eq $Text) { return $Text }
    return $Text -replace "`r`n", "`n"
}

$relatives=@(
    'src/media/gpu_preview/preroll_transition_handshake.h',
    'src/media/gpu_preview/preroll_transition_handshake.cpp',
    'src/app/preview/compositor_rhi_item.h',
    'src/app/preview/compositor_rhi_item.cpp',
    'apps/compositor_spike/compositor_spike_controller.cpp')
# S2-h: PID だけでは isolation key にならない。Windows は PID を再利用するため、
# 過去 run の process-<PID> directory と衝突して「既存artifactを上書きしません」で
# 失敗する。S2-f2 と同じく invocation ごとに一意な suffix を付ける。
$mutationRoot=Join-Path $Directory ("process-$PID-" + [guid]::NewGuid().ToString('N').Substring(0,12))
$sources=@{}
foreach($relativePath in $relatives){
    $sources[$relativePath]=Normalize-Lf (Get-Content -LiteralPath (Join-Path $SourceRoot $relativePath) -Raw -Encoding utf8)
}
function Edit-Source([string]$RelativePath,[string]$From,[string]$To){
    $From=Normalize-Lf $From
    $To=Normalize-Lf $To
    $sourceText=$sources[$RelativePath]
    if($sourceText-notmatch[regex]::Escape($From)){throw "変異対象がありません: $RelativePath / $From"}
    $sources[$RelativePath]=$sourceText.Replace($From,$To)
}
$handshake='src/media/gpu_preview/preroll_transition_handshake.cpp'
$renderer='src/app/preview/compositor_rhi_item.cpp'
$controller='apps/compositor_spike/compositor_spike_controller.cpp'
switch($Case){
    'Good'{}
    'NegativePositionalIgnoreRetained'{
        Edit-Source $renderer 'bool measurementStartPending_ = false;' "bool measurementStartPending_ = false;`n    bool formalOpportunityIgnoreNextSwap = false;"}
    'NegativeAdmissionCloseClosesScheduler'{
        Edit-Source $handshake "    foreignAdmissionClosed_ = true;`n    admissionCloseQpc_ = qpc;" "    foreignAdmissionClosed_ = true;`n    foreignSchedulerClosed_ = true;`n    admissionCloseQpc_ = qpc;"}
    'NegativeSchedulerCloseBeforeDrain'{
        Edit-Source $handshake "    if (activeForeignTransactionCount_ != 0)`n        return fail(PrerollTransitionError::SchedulerCloseBeforeDrain);" '    (void)activeForeignTransactionCount_;'}
    'NegativePendingOpportunityFinalizeSkipped'{
        Edit-Source $renderer 'if (!scheduler.finalizePendingOpportunityExact() ||' 'if (false ||'}
    'NegativeQuiescencePredicateDropped'{
        Edit-Source $handshake 'verdict.qtPendingCompositionTokenFalse && verdict.qtPendingFrameSwappedReceiptFalse &&' 'verdict.qtPendingCompositionTokenFalse &&'}
    'NegativeEpochCheckRemoved'{
        Edit-Source $handshake '        observation.captureEpoch != 0 && observation.captureEpoch == captureEpoch_;' '        true;'}
    'NegativeThreadCheckRemoved'{
        Edit-Source $handshake '        observation.observerThreadId != 0 && observation.observerThreadId == renderThreadId_;' '        true;'}
    'NegativeEarlyQueueStart'{
        Edit-Source $handshake "    if (state_ != PrerollTransitionState::Quiescent)`n        return fail(PrerollTransitionError::CurrentQueueStartBeforeQuiescence);" '    // quiescence ackを待たずにcurrent queueを開始する'}
    'NegativeEarlyMeasurementArm'{
        Edit-Source $handshake "    if (state_ != PrerollTransitionState::CurrentReady)`n        return fail(PrerollTransitionError::MeasurementArmBeforeCurrentQueue);" '    // current queue startを待たずにmeasurementをarmする'}
    'NegativeIssuanceBeforeArm'{
        Edit-Source $handshake "    if (state_ != PrerollTransitionState::MeasurementArmed)`n        return fail(PrerollTransitionError::IssuanceBeforeMeasurementArm);" '    // measurement armを待たずにissuance gateを開く'}
    'NegativeAdmissionGateRemoved'{
        Edit-Source $renderer 'output < 0 && formalOpportunityActive && foreignAdmissionOpen' 'output < 0 && formalOpportunityActive'}
    'NegativeOneShotSnapshotRemoved'{
        Edit-Source $renderer 'readOneShotSnapshot(hook->captureEpoch()' 'readOneShotSnapshot(0'}
    'NegativeCanonicalStartBeforeCurrentQueueReady'{
        Edit-Source $renderer "            const long long duration =`n                state_->measurementDurationQpc.load(std::memory_order_acquire);" "            const long long measurementArmQpc = gpu::qpcTicks();`n            const long long duration =`n                state_->measurementDurationQpc.load(std::memory_order_acquire);"}
    'NegativeCanonicalStartFromCallbackBegin'{
        Edit-Source $renderer 'measurementArmQpc, measurementArmQpc + duration)) {' 'callbackBegin, callbackBegin + duration)) {'}
    'NegativeWaitChargedToWindow'{
        Edit-Source $handshake 'snapshot.waitChargedToMeasurementWindow = false;' 'snapshot.waitChargedToMeasurementWindow = true;'}
    'NegativeTimeoutAsPerformanceDrop'{
        Edit-Source $controller '{"timeout_disposition", "PROTOCOL_FAIL_CLOSE_NOT_PERFORMANCE_DROP"},' '{"timeout_disposition", "PERFORMANCE_DROP"},'}
    'NegativeNearestQpcJoin'{
        Edit-Source $renderer '        const auto hookSnapshot = hook->snapshot();' "        const auto nearestBoundaryQpc = gpu::qpcTicks();`n        (void)nearestBoundaryQpc;`n        const auto hookSnapshot = hook->snapshot();"}
    'NegativeHistoricalMismatchReclassified'{
        Edit-Source $controller '{"reclassified_as_i5b_transition_failure", false}' '{"reclassified_as_i5b_transition_failure", true}'}
}
foreach($relativePath in $relatives){
    $targetPath=Join-Path $mutationRoot $relativePath
    New-Item -ItemType Directory -Path (Split-Path -Parent $targetPath) -Force|Out-Null
    Set-Content -LiteralPath $targetPath -Value $sources[$relativePath] -Encoding utf8 -NoNewline
}
$guardFailed=$false
try{& $Contract -RepoRoot $mutationRoot *> $null}catch{$guardFailed=$true}
if($Case-eq'Good'){
    if($guardFailed){throw '未変異sourceをB3-I5B guardが拒否しました'}
    Write-Output "P2-D5-2 B3-I5B handshake guard $Case`: PASS";exit 0
}
if(-not$guardFailed){throw "$Case をB3-I5B guardが検出できませんでした"}
Write-Output "P2-D5-2 B3-I5B handshake guard $Case`: PASS"
