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
    if (config.requiredFrameCount <= 0 || config.sourceFpsNumerator <= 0 ||
        config.sourceFpsDenominator <= 0 || config.refreshNumerator <= 0 ||
        config.refreshDenominator <= 0 || config.qpcFrequency <= 0)
        return fail(PresentationOpportunityError::InvalidConfiguration);
    if (config.requiredFrameCount >
        static_cast<long long>(std::numeric_limits<std::size_t>::max() / 2))
        return fail(PresentationOpportunityError::ArithmeticOverflow);
    config_ = config;
    records_.reserve(static_cast<std::size_t>(config.requiredFrameCount * 2));
    started_ = true;
    return true;
}

PresentationOpportunityDecision PresentationOpportunityScheduler::selectForRender(
    long long callbackQpc, const PresentationAuthoritySample& preRenderAuthority,
    long long renderOrdinal) {
    if (!started_ || closed_ || error_ != PresentationOpportunityError::None || callbackQpc <= 0) {
        fail(PresentationOpportunityError::InvalidConfiguration);
        return {};
    }
    if (pendingRender_) {
        auto duplicate = pendingDecision_;
        duplicate.duplicateCallback = true;
        return duplicate;
    }
    if (!presentationAuthorityUsable(preRenderAuthority, config_.refreshNumerator,
                                     config_.refreshDenominator) ||
        !presentationAuthorityMonotonic(lastAuthority_, preRenderAuthority)) {
        captureFirstEvent(PresentationOpportunityClassification::AuthorityDiscontinuity, -1, -1, 0,
                          preRenderAuthority, -1, false);
        fail(PresentationOpportunityError::AuthorityDiscontinuity);
        return {};
    }
    // QPCはordinal authorityではない。continuityのcross-checkにだけ使う。
    if (callbackQpc < lastSwapQpc_) {
        captureFirstEvent(PresentationOpportunityClassification::Regression, -1, -1, 0,
                          preRenderAuthority, -1, false);
        fail(PresentationOpportunityError::OpportunityRegression);
        return {};
    }

    // 最初のrenderはmeasurement先頭のopportunity 0を狙う。以降はpre-render
    // authorityが示す完了済みrefreshの「次」を狙う。丸めは一切入れない。
    long long ordinal = 0;
    if (anchored_) {
        long long completed = 0;
        if (!presentationOpportunityOrdinal(originRefreshCount_, preRenderAuthority, completed)) {
            captureFirstEvent(PresentationOpportunityClassification::AuthorityDiscontinuity, -1, -1,
                              0, preRenderAuthority, -1, false);
            fail(PresentationOpportunityError::AuthorityDiscontinuity);
            return {};
        }
        if (completed >= std::numeric_limits<long long>::max()) {
            fail(PresentationOpportunityError::ArithmeticOverflow);
            return {};
        }
        ordinal = completed + 1;
    }

    long long target = -1;
    if (!targetFor(ordinal, target)) {
        fail(PresentationOpportunityError::ArithmeticOverflow);
        return {};
    }
    PresentationOpportunityDecision decision;
    decision.valid = true;
    decision.opportunityOrdinal = ordinal;
    decision.targetFrame = target;
    decision.lastFinalizedOpportunityOrdinal = lastFinalizedOrdinal_;
    decision.renderBeginQpc = callbackQpc;
    decision.renderOrdinal = renderOrdinal;
    decision.preRenderAuthority = preRenderAuthority;
    if (target >= config_.requiredFrameCount) {
        decision.pastSourceDomain = true;
        pastSourceDomain_ = true;
        return decision;
    }
    decision.repeat = target == lastUniqueFrame_;
    lastAuthority_ = preRenderAuthority;
    pendingDecision_ = decision;
    pendingRender_ = true;
    pendingRenderCompleted_ = false;
    pendingRenderEndQpc_ = 0;
    pendingRenderedSourceFrame_ = -1;
    return decision;
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
    if (!anchored_) {
        originRefreshCount_ = postSwapAuthority.refreshCount;
        anchored_ = true;
    }
    long long actualOrdinal = 0;
    if (!presentationOpportunityOrdinal(originRefreshCount_, postSwapAuthority, actualOrdinal)) {
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

    if (!pendingOpportunity_) {
        pendingOpportunity_ = true;
        pendingOpportunityOrdinal_ = actualOrdinal;
        pendingSupersededCount_ = 0;
        pendingCandidate_ = candidate;
    } else if (actualOrdinal > pendingOpportunityOrdinal_) {
        // opportunityが前進した。直前pendingをlatest candidateでfinalizeし、
        // 間のopportunityはfinalize側でlossとしてaccountする。
        if (!finalizePendingOpportunity())
            return false;
        pendingOpportunity_ = true;
        pendingOpportunityOrdinal_ = actualOrdinal;
        pendingSupersededCount_ = 0;
        pendingCandidate_ = candidate;
    } else if (actualOrdinal == pendingOpportunityOrdinal_) {
        // 同一presentation opportunity内の追加swap。ambiguousではなく、
        // latest candidateが前candidateをsupersedeする。
        ++pendingSupersededCount_;
        ++supersededCandidateCount_;
        pendingCandidate_ = candidate;
    } else {
        captureFirstEvent(PresentationOpportunityClassification::Regression, actualOrdinal, -1,
                          swapQpc, postSwapAuthority, swapOrdinal, true);
        return fail(PresentationOpportunityError::OpportunityRegression);
    }

    ++swappedCompositionCount_;
    lastSwapQpc_ = swapQpc;
    lastRenderOrdinal_ = pendingDecision_.renderOrdinal;
    lastSwapOrdinal_ = swapOrdinal;
    lastAuthority_ = postSwapAuthority;
    pendingRender_ = false;
    pendingDecision_ = {};
    pendingRenderCompleted_ = false;
    pendingRenderEndQpc_ = 0;
    pendingRenderedSourceFrame_ = -1;
    return true;
}

bool PresentationOpportunityScheduler::finalizePendingOpportunity() {
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
        firstEvent_ = {true,
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

    records_.push_back({lastFinalizedOrdinal_,
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
                        classification});
    if (repeat) {
        ++repeated_;
    } else {
        ++displayedUnique_;
        gapTrueDrop_ += trueDropBefore;
        lastUniqueFrame_ = candidate.presentedSourceFrame;
    }
    if (forward)
        ++forwardReconciliationCount_;
    lostOpportunityCount_ += lostOpportunities;
    lastFinalizedOrdinal_ = ordinal;
    pendingOpportunity_ = false;
    pendingOpportunityOrdinal_ = -1;
    pendingSupersededCount_ = 0;
    pendingCandidate_ = {};
    return true;
}

bool PresentationOpportunityScheduler::close() {
    if (!started_ || closed_ || error_ != PresentationOpportunityError::None)
        return fail(PresentationOpportunityError::InvalidConfiguration);
    if (pendingRender_) {
        captureFirstEvent(PresentationOpportunityClassification::PairingDefect, -1, -1, 0, {}, -1,
                          false);
        return fail(PresentationOpportunityError::RenderWithoutSwap);
    }
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
            records_};
}

bool PresentationOpportunityScheduler::fail(PresentationOpportunityError error) {
    if (error_ == PresentationOpportunityError::None)
        error_ = error;
    return false;
}

bool PresentationOpportunityScheduler::targetFor(long long ordinal, long long& target) const {
    long long numerator = 0;
    long long denominator = 0;
    return checkedMultiply(ordinal, config_.sourceFpsNumerator, numerator) &&
           checkedMultiply(numerator, config_.refreshDenominator, numerator) &&
           checkedMultiply(config_.sourceFpsDenominator, config_.refreshNumerator, denominator) &&
           denominator > 0 && ((target = numerator / denominator), true);
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

} // namespace mvm::gpu
