#include "media/gpu_preview/presentation_opportunity_scheduler.h"

#include <limits>

namespace mvm::gpu {
namespace {

bool checkedMultiply(long long left, long long right, long long& result) {
    if (left < 0 || right < 0 ||
        (left != 0 && right > std::numeric_limits<long long>::max() / left))
        return false;
    result = left * right;
    return true;
}

} // namespace

bool PresentationOpportunityScheduler::start(const PresentationOpportunityConfig& config) {
    *this = {};
    if (config.requiredFrameCount <= 0 || config.sourceFrameOffset < 0 ||
        config.sourceFrameOffset >= config.requiredFrameCount || config.sourceFpsNumerator <= 0 ||
        config.sourceFpsDenominator <= 0 || config.refreshNumerator <= 0 ||
        config.refreshDenominator <= 0 || config.qpcFrequency <= 0)
        return fail(PresentationOpportunityError::InvalidConfiguration);
    if (config.requiredFrameCount >
        static_cast<long long>(std::numeric_limits<std::size_t>::max() / 2))
        return fail(PresentationOpportunityError::ArithmeticOverflow);
    config_ = config;
    records_.reserve(static_cast<std::size_t>(config.requiredFrameCount * 2));
    if (config.invocationLedgerEnabled)
        invocationRecords_.reserve(static_cast<std::size_t>(config.requiredFrameCount * 2));
    if (!requiredIntentQueue_.start(config.requiredFrameCount))
        return fail(PresentationOpportunityError::RequiredQueueFailure);
    started_ = true;
    return true;
}

PresentationOpportunityDecision PresentationOpportunityScheduler::selectForRender(
    long long callbackQpc, const PresentationAuthoritySample& preRenderAuthority,
    long long renderOrdinal) {
    const auto pre = invocationState();
    if (!started_ || closed_ || error_ != PresentationOpportunityError::None || callbackQpc <= 0) {
        fail(PresentationOpportunityError::InvalidConfiguration);
        return finishInvocation(pre, callbackQpc, preRenderAuthority,
                                PresentationSchedulerInvocationResult::InvalidFatal,
                                PresentationSchedulerInvocationReason::InvalidConfiguration, {});
    }
    if (pendingRender_) {
        auto duplicate = pendingDecision_;
        duplicate.duplicateCallback = true;
        return finishInvocation(pre, callbackQpc, preRenderAuthority,
                                PresentationSchedulerInvocationResult::DuplicateDecision,
                                PresentationSchedulerInvocationReason::PendingRender, duplicate);
    }
    if (!presentationAuthorityUsable(preRenderAuthority, config_.refreshNumerator,
                                     config_.refreshDenominator) ||
        !presentationAuthorityMonotonic(lastAuthority_, preRenderAuthority)) {
        captureFirstEvent(PresentationOpportunityClassification::AuthorityDiscontinuity, -1, -1, 0,
                          preRenderAuthority, -1, false);
        fail(PresentationOpportunityError::AuthorityDiscontinuity);
        return finishInvocation(pre, callbackQpc, preRenderAuthority,
                                PresentationSchedulerInvocationResult::InvalidFatal,
                                PresentationSchedulerInvocationReason::AuthorityUnusable, {});
    }
    // QPCはordinal authorityではない。continuityのcross-checkにだけ使う。
    if (callbackQpc < lastSwapQpc_) {
        captureFirstEvent(PresentationOpportunityClassification::Regression, -1, -1, 0,
                          preRenderAuthority, -1, false);
        fail(PresentationOpportunityError::OpportunityRegression);
        return finishInvocation(pre, callbackQpc, preRenderAuthority,
                                PresentationSchedulerInvocationResult::InvalidFatal,
                                PresentationSchedulerInvocationReason::CallbackQpcRegression, {});
    }

    // B3-I1。actual issuance identityはDWM counterやprevious ordinalではなく、
    // immutable required setのqueue headを先にreserveして確定する。
    const auto queueDecision = requiredIntentQueue_.reserveHead();
    if (queueDecision.result == RequiredIntentReserveResult::Rejected) {
        fail(PresentationOpportunityError::RequiredQueueFailure);
        return finishInvocation(pre, callbackQpc, preRenderAuthority,
                                PresentationSchedulerInvocationResult::InvalidFatal,
                                PresentationSchedulerInvocationReason::InvalidConfiguration, {});
    }
    if (queueDecision.result == RequiredIntentReserveResult::Exhausted) {
        PresentationOpportunityDecision exhausted;
        exhausted.valid = true;
        exhausted.requiredIntentMembershipExact = true;
        exhausted.renderBeginQpc = callbackQpc;
        exhausted.renderOrdinal = renderOrdinal;
        exhausted.preRenderAuthority = preRenderAuthority;
        return finishInvocation(
            pre, callbackQpc, preRenderAuthority,
            PresentationSchedulerInvocationResult::RequiredQueueExhaustedDecision,
            PresentationSchedulerInvocationReason::RequiredQueueExhausted, exhausted);
    }
    const long long ordinal = queueDecision.reservation.intentOrdinal;

    long long target = -1;
    if (!targetFor(ordinal, target)) {
        fail(PresentationOpportunityError::ArithmeticOverflow);
        return finishInvocation(pre, callbackQpc, preRenderAuthority,
                                PresentationSchedulerInvocationResult::InvalidFatal,
                                PresentationSchedulerInvocationReason::TargetArithmeticOverflow,
                                {});
    }
    PresentationOpportunityDecision decision;
    decision.valid = true;
    decision.opportunityOrdinal = ordinal;
    decision.requiredIntentMembership = ordinal >= 0 && ordinal < config_.requiredFrameCount;
    decision.requiredIntentMembershipExact = true;
    decision.targetFrame = target;
    decision.lastFinalizedOpportunityOrdinal = lastFinalizedOrdinal_;
    decision.renderBeginQpc = callbackQpc;
    decision.renderOrdinal = renderOrdinal;
    decision.reservationId = queueDecision.reservation.reservationId;
    decision.preRenderAuthority = preRenderAuthority;
    if (target >= config_.requiredFrameCount) {
        decision.pastSourceDomain = true;
        pastSourceDomain_ = true;
        fail(PresentationOpportunityError::SourceCoverageInsufficient);
        decision.valid = false;
        return finishInvocation(pre, callbackQpc, preRenderAuthority,
                                PresentationSchedulerInvocationResult::InvalidFatal,
                                PresentationSchedulerInvocationReason::PastSourceDomain, decision);
    }
    decision.repeat = target == lastUniqueFrame_;
    lastAuthority_ = preRenderAuthority;
    pendingDecision_ = decision;
    pendingRender_ = true;
    pendingRenderCompleted_ = false;
    pendingQualifiedEvidence_ = false;
    pendingRenderEndQpc_ = 0;
    pendingRenderedSourceFrame_ = -1;
    return finishInvocation(pre, callbackQpc, preRenderAuthority,
                            PresentationSchedulerInvocationResult::PrimaryDecision,
                            PresentationSchedulerInvocationReason::Primary, decision);
}

bool PresentationOpportunityScheduler::markRenderComplete(long long renderEndQpc,
                                                          long long renderedSourceFrame,
                                                          long long renderOrdinal) {
    if (!started_ || closed_ || error_ != PresentationOpportunityError::None || !pendingRender_) {
        captureFirstEvent(PresentationOpportunityClassification::PairingDefect, -1, -1, 0, {}, -1,
                          false);
        return fail(PresentationOpportunityError::RenderWithoutSwap);
    }
    if (pendingRenderCompleted_ || renderEndQpc < pendingDecision_.renderBeginQpc ||
        (pendingDecision_.renderOrdinal >= 0 && renderOrdinal != pendingDecision_.renderOrdinal)) {
        captureFirstEvent(PresentationOpportunityClassification::PairingDefect, -1, -1, 0, {}, -1,
                          false);
        return fail(PresentationOpportunityError::RenderOrdinalMismatch);
    }
    if (renderedSourceFrame != pendingDecision_.targetFrame) {
        pendingRenderedSourceFrame_ = renderedSourceFrame;
        captureFirstEvent(PresentationOpportunityClassification::PairingDefect, -1, -1, 0, {}, -1,
                          false);
        return fail(PresentationOpportunityError::PresentedFrameMismatch);
    }
    pendingRenderCompleted_ = true;
    pendingRenderEndQpc_ = renderEndQpc;
    pendingRenderedSourceFrame_ = renderedSourceFrame;
    if (!requiredIntentQueue_.markRenderComplete(pendingDecision_.reservationId,
                                                 pendingDecision_.opportunityOrdinal))
        return fail(PresentationOpportunityError::RequiredQueueFailure);
    return true;
}

bool PresentationOpportunityScheduler::commitQualifiedPresent(unsigned long long reservationId,
                                                              long long intentOrdinal) {
    if (!started_ || closed_ || error_ != PresentationOpportunityError::None || !pendingRender_ ||
        !pendingRenderCompleted_ || pendingQualifiedEvidence_)
        return fail(PresentationOpportunityError::QualifiedCommitMissing);
    if (reservationId != pendingDecision_.reservationId ||
        intentOrdinal != pendingDecision_.opportunityOrdinal)
        return fail(PresentationOpportunityError::RequiredQueueFailure);
    // I0 QUALIFIED_COMMITのpending evidenceだけを確定する。dequeueはまだ行わない。
    pendingQualifiedEvidence_ = true;
    return true;
}

bool PresentationOpportunityScheduler::commitSwap(
    long long swapQpc, const PresentationAuthoritySample& postSwapAuthority,
    long long swapOrdinal) {
    if (!started_ || closed_ || error_ != PresentationOpportunityError::None)
        return fail(PresentationOpportunityError::InvalidConfiguration);
    if (!pendingRender_) {
        captureFirstEvent(PresentationOpportunityClassification::PairingDefect, -1, -1, swapQpc,
                          postSwapAuthority, swapOrdinal, false);
        return fail(PresentationOpportunityError::SwapWithoutRender);
    }
    if (!pendingRenderCompleted_) {
        captureFirstEvent(PresentationOpportunityClassification::PairingDefect, -1, -1, swapQpc,
                          postSwapAuthority, swapOrdinal, false);
        return fail(PresentationOpportunityError::RenderNotCompleted);
    }
    if (!pendingQualifiedEvidence_)
        return fail(PresentationOpportunityError::QualifiedCommitMissing);
    // QPCはcontinuityのcross-checkであり、opportunity序数の根拠にはしない。
    if (swapQpc <= 0 || swapQpc < pendingRenderEndQpc_ ||
        (lastSwapQpc_ > 0 && swapQpc <= lastSwapQpc_)) {
        captureFirstEvent(PresentationOpportunityClassification::Regression, -1, -1, swapQpc,
                          postSwapAuthority, swapOrdinal, false);
        return fail(PresentationOpportunityError::OpportunityRegression);
    }
    if (pendingDecision_.renderOrdinal >= 0 &&
        pendingDecision_.renderOrdinal != lastRenderOrdinal_ + 1) {
        captureFirstEvent(PresentationOpportunityClassification::PairingDefect, -1, -1, swapQpc,
                          postSwapAuthority, swapOrdinal, false);
        return fail(PresentationOpportunityError::RenderOrdinalMismatch);
    }
    if (swapOrdinal >= 0 && swapOrdinal != lastSwapOrdinal_ + 1) {
        captureFirstEvent(PresentationOpportunityClassification::PairingDefect, -1, -1, swapQpc,
                          postSwapAuthority, swapOrdinal, false);
        return fail(PresentationOpportunityError::SwapOrdinalMismatch);
    }
    if (!presentationAuthorityUsable(postSwapAuthority, config_.refreshNumerator,
                                     config_.refreshDenominator) ||
        !presentationAuthorityMonotonic(pendingDecision_.preRenderAuthority, postSwapAuthority) ||
        !presentationAuthorityMonotonic(lastAuthority_, postSwapAuthority)) {
        captureFirstEvent(PresentationOpportunityClassification::AuthorityDiscontinuity, -1, -1,
                          swapQpc, postSwapAuthority, swapOrdinal, false);
        return fail(PresentationOpportunityError::AuthorityDiscontinuity);
    }

    // opportunity序数はrefresh count authorityだけから決める。最初のswapの
    // post-swap refresh countをopportunity 0のoriginとして固定する。
    const bool establishesAnchor = !anchored_;
    const unsigned long long proposedOriginRefreshCount =
        establishesAnchor ? postSwapAuthority.refreshCount : originRefreshCount_;
    long long actualOrdinal = 0;
    if (!presentationOpportunityOrdinal(proposedOriginRefreshCount, postSwapAuthority,
                                        actualOrdinal)) {
        captureFirstEvent(PresentationOpportunityClassification::AuthorityDiscontinuity, -1, -1,
                          swapQpc, postSwapAuthority, swapOrdinal, false);
        return fail(PresentationOpportunityError::AuthorityDiscontinuity);
    }

    Candidate candidate;
    candidate.predictedOpportunityOrdinal = pendingDecision_.opportunityOrdinal;
    candidate.predictedSourceFrame = pendingDecision_.targetFrame;
    candidate.presentedSourceFrame = pendingRenderedSourceFrame_;
    candidate.renderBeginQpc = pendingDecision_.renderBeginQpc;
    candidate.renderEndQpc = pendingRenderEndQpc_;
    candidate.swapQpc = swapQpc;
    candidate.renderOrdinal = pendingDecision_.renderOrdinal;
    candidate.swapOrdinal = swapOrdinal;
    candidate.preRenderAuthority = pendingDecision_.preRenderAuthority;
    candidate.postSwapAuthority = postSwapAuthority;

    enum class PendingAction { Install, Advance, Supersede };
    PendingAction pendingAction = PendingAction::Install;
    PendingOpportunityFinalization preparedFinalization;
    if (pendingOpportunity_ && actualOrdinal > pendingOpportunityOrdinal_) {
        pendingAction = PendingAction::Advance;
        // dequeueより前に、直前pendingの全failure条件も副作用なしで検証する。
        if (!preparePendingOpportunityFinalization(preparedFinalization))
            return false;
    } else if (pendingOpportunity_ && actualOrdinal == pendingOpportunityOrdinal_) {
        pendingAction = PendingAction::Supersede;
    } else if (pendingOpportunity_) {
        captureFirstEvent(PresentationOpportunityClassification::Regression, actualOrdinal, -1,
                          swapQpc, postSwapAuthority, swapOrdinal, true);
        return fail(PresentationOpportunityError::OpportunityRegression);
    }

    // failure-free logical commit point。ここより前はqueue/scheduler stateを変更しない。
    // rollbackで救済せず、I0 evidenceとswap validationが揃った1件だけをdequeueする。
    if (!requiredIntentQueue_.commitQualified(pendingDecision_.reservationId,
                                              pendingDecision_.opportunityOrdinal))
        return fail(PresentationOpportunityError::RequiredQueueFailure);

    if (establishesAnchor) {
        originRefreshCount_ = proposedOriginRefreshCount;
        anchored_ = true;
    }
    if (pendingAction == PendingAction::Advance) {
        applyPendingOpportunityFinalization(preparedFinalization);
        pendingOpportunity_ = true;
        pendingOpportunityOrdinal_ = actualOrdinal;
        pendingSupersededCount_ = 0;
        pendingCandidate_ = candidate;
    } else if (pendingAction == PendingAction::Supersede) {
        ++pendingSupersededCount_;
        ++supersededCandidateCount_;
        pendingCandidate_ = candidate;
    } else {
        pendingOpportunity_ = true;
        pendingOpportunityOrdinal_ = actualOrdinal;
        pendingSupersededCount_ = 0;
        pendingCandidate_ = candidate;
    }

    ++swappedCompositionCount_;
    lastSwapQpc_ = swapQpc;
    lastRenderOrdinal_ = pendingDecision_.renderOrdinal;
    lastSwapOrdinal_ = swapOrdinal;
    lastAuthority_ = postSwapAuthority;
    pendingRender_ = false;
    pendingDecision_ = {};
    pendingRenderCompleted_ = false;
    pendingQualifiedEvidence_ = false;
    pendingRenderEndQpc_ = 0;
    pendingRenderedSourceFrame_ = -1;
    return true;
}

bool PresentationOpportunityScheduler::finalizePendingOpportunity() {
    PendingOpportunityFinalization prepared;
    if (!preparePendingOpportunityFinalization(prepared))
        return false;
    applyPendingOpportunityFinalization(prepared);
    return true;
}

bool PresentationOpportunityScheduler::preparePendingOpportunityFinalization(
    PendingOpportunityFinalization& prepared) {
    const Candidate candidate = pendingCandidate_;
    const long long ordinal = pendingOpportunityOrdinal_;
    long long expectedTarget = -1;
    if (!targetFor(ordinal, expectedTarget))
        return fail(PresentationOpportunityError::ArithmeticOverflow);
    if (candidate.presentedSourceFrame < 0 ||
        candidate.presentedSourceFrame >= config_.requiredFrameCount) {
        captureFirstEvent(PresentationOpportunityClassification::PairingDefect, ordinal,
                          expectedTarget, candidate.swapQpc, candidate.postSwapAuthority,
                          candidate.swapOrdinal, true);
        return fail(PresentationOpportunityError::PresentedFrameMismatch);
    }
    const bool repeat = candidate.presentedSourceFrame == lastUniqueFrame_;
    const long long trueDropBefore =
        repeat ? 0 : candidate.presentedSourceFrame - lastUniqueFrame_ - 1;
    if (trueDropBefore < 0)
        return fail(PresentationOpportunityError::OpportunityRegression);
    const long long lostOpportunities =
        lastFinalizedOrdinal_ >= 0 ? ordinal - lastFinalizedOrdinal_ - 1 : ordinal;
    if (lostOpportunities < 0)
        return fail(PresentationOpportunityError::OpportunityRegression);
    const bool forward = candidate.predictedOpportunityOrdinal >= 0 &&
                         ordinal > candidate.predictedOpportunityOrdinal;
    const auto classification = lostOpportunities > 0
                                    ? PresentationOpportunityClassification::ForwardOpportunityLoss
                                    : PresentationOpportunityClassification::Exact;
    if (classification == PresentationOpportunityClassification::ForwardOpportunityLoss &&
        !firstEvent_.captured) {
        prepared.captureFirstEvent = true;
        prepared.firstEvent = {true,
                               classification,
                               lastFinalizedOrdinal_,
                               candidate.predictedOpportunityOrdinal,
                               ordinal,
                               candidate.renderBeginQpc,
                               candidate.renderEndQpc,
                               candidate.swapQpc,
                               candidate.preRenderAuthority,
                               candidate.postSwapAuthority,
                               candidate.predictedSourceFrame,
                               expectedTarget,
                               candidate.presentedSourceFrame,
                               candidate.renderOrdinal,
                               candidate.swapOrdinal,
                               true};
    }

    prepared.record = {lastFinalizedOrdinal_,
                       candidate.predictedOpportunityOrdinal,
                       ordinal,
                       candidate.renderBeginQpc,
                       candidate.renderEndQpc,
                       candidate.swapQpc,
                       candidate.renderOrdinal,
                       candidate.swapOrdinal,
                       config_.refreshNumerator,
                       config_.refreshDenominator,
                       candidate.preRenderAuthority,
                       candidate.postSwapAuthority,
                       true,
                       candidate.predictedSourceFrame,
                       expectedTarget,
                       candidate.presentedSourceFrame,
                       repeat,
                       trueDropBefore,
                       lostOpportunities,
                       pendingSupersededCount_,
                       forward,
                       classification};
    prepared.repeat = repeat;
    prepared.forward = forward;
    prepared.trueDropBefore = trueDropBefore;
    prepared.lostOpportunities = lostOpportunities;
    return true;
}

void PresentationOpportunityScheduler::applyPendingOpportunityFinalization(
    const PendingOpportunityFinalization& prepared) {
    if (prepared.captureFirstEvent)
        firstEvent_ = prepared.firstEvent;
    records_.push_back(prepared.record);
    if (prepared.repeat) {
        ++repeated_;
    } else {
        ++displayedUnique_;
        gapTrueDrop_ += prepared.trueDropBefore;
        lastUniqueFrame_ = prepared.record.presentedSourceFrame;
    }
    if (prepared.forward)
        ++forwardReconciliationCount_;
    lostOpportunityCount_ += prepared.lostOpportunities;
    lastFinalizedOrdinal_ = prepared.record.actualOpportunityOrdinal;
    pendingOpportunity_ = false;
    pendingOpportunityOrdinal_ = -1;
    pendingSupersededCount_ = 0;
    pendingCandidate_ = {};
}

bool PresentationOpportunityScheduler::finalizePendingOpportunityExact() {
    if (!started_ || closed_ || error_ != PresentationOpportunityError::None)
        return fail(PresentationOpportunityError::InvalidConfiguration);
    // in-flight transactionが残っている間はfinalizeしない。drain完了後だけ呼ぶ。
    if (pendingRender_)
        return fail(PresentationOpportunityError::RenderWithoutSwap);
    if (pendingOpportunity_ && !finalizePendingOpportunity())
        return false;
    pendingOpportunityExactlyFinalized_ = true;
    return true;
}

bool PresentationOpportunityScheduler::closePlannedWindow() {
    return close(true);
}

bool PresentationOpportunityScheduler::closeWithoutNormalCompletion() {
    return close(false);
}

bool PresentationOpportunityScheduler::close(bool plannedWindowEnd) {
    if (!started_ || closed_)
        return fail(PresentationOpportunityError::InvalidConfiguration);
    if (error_ != PresentationOpportunityError::None) {
        // fatal原因は保持したまま、non-normal cleanupだけを許可する。
        // source-coverage fatalのactive reservation/unissued tailをconsumeしない。
        if (plannedWindowEnd)
            return false;
        const bool queueClosed = requiredIntentQueue_.closeWithoutNormalCompletion();
        closed_ = true;
        return queueClosed;
    }
    const bool queueClosed = plannedWindowEnd ? requiredIntentQueue_.closePlannedWindow()
                                              : requiredIntentQueue_.closeWithoutNormalCompletion();
    if (!queueClosed)
        return fail(PresentationOpportunityError::RequiredQueueFailure);
    // measurement endではpending opportunityをfinalizeしてからtailを数える。
    if (pendingOpportunity_ && !finalizePendingOpportunity())
        return false;
    tailTrueDrop_ = config_.requiredFrameCount - lastUniqueFrame_ - 1;
    if (tailTrueDrop_ < 0)
        return fail(PresentationOpportunityError::OpportunityRegression);
    closed_ = true;
    return true;
}

PresentationOpportunitySnapshot PresentationOpportunityScheduler::snapshot() const {
    return {started_ && error_ == PresentationOpportunityError::None,
            closed_,
            error_,
            anchored_,
            originRefreshCount_,
            displayedUnique_,
            repeated_,
            gapTrueDrop_,
            tailTrueDrop_,
            gapTrueDrop_ + tailTrueDrop_,
            lastFinalizedOrdinal_,
            lastUniqueFrame_,
            forwardReconciliationCount_,
            lostOpportunityCount_,
            supersededCandidateCount_,
            swappedCompositionCount_,
            firstEvent_,
            records_,
            started_ && error_ == PresentationOpportunityError::None,
            requiredIntentQueue_.snapshot().requiredIntentOrdinals,
            config_.invocationLedgerEnabled,
            invocationRecords_,
            requiredIntentQueue_.snapshot(),
            config_};
}

bool PresentationOpportunityScheduler::noteInvocationTransportDisposition(
    unsigned long long invocationSerial, FormalIntentTransportDisposition disposition) {
    if (!config_.invocationLedgerEnabled)
        return true;
    if (invocationSerial == 0 || invocationRecords_.empty() ||
        invocationRecords_.back().invocationSerial != invocationSerial ||
        invocationRecords_.back().transportDispositionExact)
        return false;
    invocationRecords_.back().transportDisposition = disposition;
    invocationRecords_.back().transportDispositionExact = true;
    return true;
}

PresentationSchedulerInvocationState PresentationOpportunityScheduler::invocationState() const {
    return {started_,       closed_,          anchored_, originRefreshCount_, lastFinalizedOrdinal_,
            pendingRender_, pastSourceDomain_};
}

PresentationOpportunityDecision PresentationOpportunityScheduler::finishInvocation(
    const PresentationSchedulerInvocationState& pre, long long invocationQpc,
    const PresentationAuthoritySample& inputAuthority, PresentationSchedulerInvocationResult result,
    PresentationSchedulerInvocationReason reason, PresentationOpportunityDecision decision) {
    if (!config_.invocationLedgerEnabled)
        return decision;
    decision.invocationSerial = ++invocationSerial_;
    invocationRecords_.push_back({decision.invocationSerial, invocationQpc, inputAuthority, pre,
                                  result, reason, decision,
                                  FormalIntentTransportDisposition::InvalidMembershipProvenance,
                                  false, invocationState(), true});
    return decision;
}

bool PresentationOpportunityScheduler::fail(PresentationOpportunityError error) {
    if (error_ == PresentationOpportunityError::None)
        error_ = error;
    return false;
}

bool PresentationOpportunityScheduler::targetFor(long long ordinal, long long& target) const {
    long long numerator = 0;
    long long denominator = 0;
    if (!checkedMultiply(ordinal, config_.sourceFpsNumerator, numerator) ||
        !checkedMultiply(numerator, config_.refreshDenominator, numerator) ||
        !checkedMultiply(config_.sourceFpsDenominator, config_.refreshNumerator, denominator) ||
        denominator <= 0)
        return false;
    const long long relativeTarget = numerator / denominator;
    if (relativeTarget > std::numeric_limits<long long>::max() - config_.sourceFrameOffset)
        return false;
    target = config_.sourceFrameOffset + relativeTarget;
    return true;
}

void PresentationOpportunityScheduler::captureFirstEvent(
    PresentationOpportunityClassification classification, long long actualOrdinal,
    long long actualTarget, long long swapQpc, const PresentationAuthoritySample& post,
    long long swapOrdinal, bool continuous) {
    if (firstEvent_.captured)
        return;
    firstEvent_ = {true,
                   classification,
                   pendingDecision_.valid ? pendingDecision_.lastFinalizedOpportunityOrdinal
                                          : lastFinalizedOrdinal_,
                   pendingDecision_.opportunityOrdinal,
                   actualOrdinal,
                   pendingDecision_.renderBeginQpc,
                   pendingRenderEndQpc_,
                   swapQpc,
                   pendingDecision_.preRenderAuthority,
                   post,
                   pendingDecision_.targetFrame,
                   actualTarget,
                   pendingRenderedSourceFrame_,
                   pendingDecision_.renderOrdinal,
                   swapOrdinal,
                   continuous};
}

const char* presentationOpportunityErrorName(PresentationOpportunityError error) {
    switch (error) {
    case PresentationOpportunityError::None:
        return "NONE";
    case PresentationOpportunityError::InvalidConfiguration:
        return "INVALID_CONFIGURATION";
    case PresentationOpportunityError::ArithmeticOverflow:
        return "ARITHMETIC_OVERFLOW";
    case PresentationOpportunityError::OpportunityRegression:
        return "OPPORTUNITY_REGRESSION";
    case PresentationOpportunityError::AuthorityDiscontinuity:
        return "AUTHORITY_DISCONTINUITY";
    case PresentationOpportunityError::RenderWithoutSwap:
        return "RENDER_WITHOUT_SWAP";
    case PresentationOpportunityError::SwapWithoutRender:
        return "SWAP_WITHOUT_RENDER";
    case PresentationOpportunityError::RenderNotCompleted:
        return "RENDER_NOT_COMPLETED";
    case PresentationOpportunityError::RenderOrdinalMismatch:
        return "RENDER_ORDINAL_MISMATCH";
    case PresentationOpportunityError::SwapOrdinalMismatch:
        return "SWAP_ORDINAL_MISMATCH";
    case PresentationOpportunityError::PresentedFrameMismatch:
        return "PRESENTED_FRAME_MISMATCH";
    case PresentationOpportunityError::RequiredQueueFailure:
        return "REQUIRED_QUEUE_FAILURE";
    case PresentationOpportunityError::SourceCoverageInsufficient:
        return "SOURCE_COVERAGE_INSUFFICIENT";
    case PresentationOpportunityError::QualifiedCommitMissing:
        return "QUALIFIED_COMMIT_MISSING";
    }
    return "UNKNOWN";
}

const char*
presentationOpportunityClassificationName(PresentationOpportunityClassification classification) {
    switch (classification) {
    case PresentationOpportunityClassification::None:
        return "NONE";
    case PresentationOpportunityClassification::Exact:
        return "EXACT";
    case PresentationOpportunityClassification::ForwardOpportunityLoss:
        return "FORWARD_OPPORTUNITY_LOSS";
    case PresentationOpportunityClassification::Regression:
        return "REGRESSION";
    case PresentationOpportunityClassification::AuthorityDiscontinuity:
        return "AUTHORITY_DISCONTINUITY";
    case PresentationOpportunityClassification::PairingDefect:
        return "PAIRING_DEFECT";
    }
    return "UNKNOWN";
}

const char*
presentationSchedulerInvocationResultName(PresentationSchedulerInvocationResult result) {
    switch (result) {
    case PresentationSchedulerInvocationResult::PrimaryDecision:
        return "PRIMARY_DECISION";
    case PresentationSchedulerInvocationResult::DuplicateDecision:
        return "DUPLICATE_DECISION";
    case PresentationSchedulerInvocationResult::OutsideSourceDomainDecision:
        return "OUTSIDE_SOURCE_DOMAIN_DECISION";
    case PresentationSchedulerInvocationResult::RequiredQueueExhaustedDecision:
        return "REQUIRED_QUEUE_EXHAUSTED_DECISION";
    case PresentationSchedulerInvocationResult::InvalidFatal:
        return "INVALID_FATAL";
    }
    return "UNKNOWN";
}

const char*
presentationSchedulerInvocationReasonName(PresentationSchedulerInvocationReason reason) {
    switch (reason) {
    case PresentationSchedulerInvocationReason::Primary:
        return "PRIMARY";
    case PresentationSchedulerInvocationReason::PendingRender:
        return "PENDING_RENDER";
    case PresentationSchedulerInvocationReason::PastSourceDomain:
        return "PAST_SOURCE_DOMAIN";
    case PresentationSchedulerInvocationReason::RequiredQueueExhausted:
        return "REQUIRED_QUEUE_EXHAUSTED";
    case PresentationSchedulerInvocationReason::InvalidConfiguration:
        return "INVALID_CONFIGURATION";
    case PresentationSchedulerInvocationReason::AuthorityUnusable:
        return "AUTHORITY_UNUSABLE";
    case PresentationSchedulerInvocationReason::CallbackQpcRegression:
        return "CALLBACK_QPC_REGRESSION";
    case PresentationSchedulerInvocationReason::CompletedOrdinalUnavailable:
        return "COMPLETED_ORDINAL_UNAVAILABLE";
    case PresentationSchedulerInvocationReason::CompletedOrdinalOverflow:
        return "COMPLETED_ORDINAL_OVERFLOW";
    case PresentationSchedulerInvocationReason::TargetArithmeticOverflow:
        return "TARGET_ARITHMETIC_OVERFLOW";
    }
    return "UNKNOWN";
}

} // namespace mvm::gpu
