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
    if (pending_) {
        auto duplicate = pendingDecision_;
        duplicate.duplicateCallback = true;
        return duplicate;
    }

    long long ordinal = 0;
    if (lastOpportunityOrdinal_ >= 0) {
        long long intervals = 0;
        if (callbackQpc <= lastSwapQpc_ ||
            !roundedRefreshIntervals(callbackQpc - lastSwapQpc_, intervals)) {
            fail(PresentationOpportunityError::AmbiguousOpportunity);
            return {};
        }
        // 通常のthreaded render loopでは次callbackが直前swap直後に始まる。
        // 完了済みauthorityから見た「次」は最低1 opportunity先である。
        if (intervals == 0)
            intervals = 1;
        if (intervals > std::numeric_limits<long long>::max() - lastOpportunityOrdinal_) {
            fail(PresentationOpportunityError::ArithmeticOverflow);
            return {};
        }
        ordinal = lastOpportunityOrdinal_ + intervals;
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
    decision.lastCommittedOpportunityOrdinal = lastOpportunityOrdinal_;
    decision.renderBeginQpc = callbackQpc;
    decision.renderOrdinal = renderOrdinal;
    decision.preRenderAuthority = preRenderAuthority;
    if (target >= config_.requiredFrameCount) {
        decision.pastSourceDomain = true;
        pastSourceDomain_ = true;
        return decision;
    }
    decision.repeat = target == lastUniqueFrame_;
    decision.trueDropBefore = decision.repeat ? 0 : target - lastUniqueFrame_ - 1;
    if (decision.trueDropBefore < 0) {
        fail(PresentationOpportunityError::OpportunityRegression);
        return {};
    }
    pendingDecision_ = decision;
    pending_ = true;
    pendingRenderCompleted_ = false;
    pendingRenderEndQpc_ = 0;
    pendingRenderedSourceFrame_ = -1;
    return decision;
}

bool PresentationOpportunityScheduler::markRenderComplete(long long renderEndQpc,
                                                          long long renderedSourceFrame,
                                                          long long renderOrdinal) {
    if (!started_ || closed_ || error_ != PresentationOpportunityError::None || !pending_) {
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
    if (!pending_) {
        captureFirstEvent(PresentationOpportunityClassification::PairingDefect, -1, -1, swapQpc,
                          postSwapAuthority, swapOrdinal, false);
        return fail(PresentationOpportunityError::SwapWithoutRender);
    }
    if (!pendingRenderCompleted_) {
        captureFirstEvent(PresentationOpportunityClassification::PairingDefect, -1, -1, swapQpc,
                          postSwapAuthority, swapOrdinal, false);
        return fail(PresentationOpportunityError::RenderNotCompleted);
    }
    if (swapQpc <= 0 || (lastSwapQpc_ > 0 && swapQpc <= lastSwapQpc_)) {
        captureFirstEvent(PresentationOpportunityClassification::Regression, -1, -1, swapQpc,
                          postSwapAuthority, swapOrdinal, false);
        return fail(PresentationOpportunityError::OpportunityRegression);
    }
    if ((pendingDecision_.renderOrdinal >= 0 &&
         pendingDecision_.renderOrdinal != lastRenderOrdinal_ + 1) ||
        (swapOrdinal >= 0 && swapOrdinal != lastSwapOrdinal_ + 1)) {
        captureFirstEvent(PresentationOpportunityClassification::PairingDefect, -1, -1, swapQpc,
                          postSwapAuthority, swapOrdinal, false);
        return fail(PresentationOpportunityError::SwapOrdinalMismatch);
    }

    long long actualOrdinal = 0;
    if (lastOpportunityOrdinal_ >= 0) {
        long long intervals = 0;
        if (!roundedRefreshIntervals(swapQpc - lastSwapQpc_, intervals) || intervals <= 0) {
            captureFirstEvent(PresentationOpportunityClassification::PairingDefect, -1, -1, swapQpc,
                              postSwapAuthority, swapOrdinal, false);
            return fail(PresentationOpportunityError::AmbiguousOpportunity);
        }
        if (intervals > std::numeric_limits<long long>::max() - lastOpportunityOrdinal_)
            return fail(PresentationOpportunityError::ArithmeticOverflow);
        actualOrdinal = lastOpportunityOrdinal_ + intervals;
    }
    long long actualTarget = -1;
    if (!targetFor(actualOrdinal, actualTarget))
        return fail(PresentationOpportunityError::ArithmeticOverflow);
    const bool continuous =
        authorityContinuous(pendingDecision_.preRenderAuthority, postSwapAuthority);
    if (!continuous) {
        captureFirstEvent(PresentationOpportunityClassification::AuthorityDiscontinuity,
                          actualOrdinal, actualTarget, swapQpc, postSwapAuthority, swapOrdinal,
                          false);
        return fail(PresentationOpportunityError::AuthorityDiscontinuity);
    }
    if (actualOrdinal < pendingDecision_.opportunityOrdinal) {
        captureFirstEvent(PresentationOpportunityClassification::Regression, actualOrdinal,
                          actualTarget, swapQpc, postSwapAuthority, swapOrdinal, true);
        return fail(PresentationOpportunityError::OpportunityRegression);
    }
    if (pendingRenderedSourceFrame_ > actualTarget) {
        captureFirstEvent(PresentationOpportunityClassification::PairingDefect, actualOrdinal,
                          actualTarget, swapQpc, postSwapAuthority, swapOrdinal, true);
        return fail(PresentationOpportunityError::PresentedFrameMismatch);
    }

    const bool repeat = pendingRenderedSourceFrame_ == lastUniqueFrame_;
    const long long trueDropBefore =
        repeat ? 0 : pendingRenderedSourceFrame_ - lastUniqueFrame_ - 1;
    if (trueDropBefore < 0)
        return fail(PresentationOpportunityError::OpportunityRegression);
    const long long lostOpportunities = actualOrdinal - pendingDecision_.opportunityOrdinal;
    const auto classification = lostOpportunities > 0
                                    ? PresentationOpportunityClassification::ForwardOpportunityLoss
                                    : PresentationOpportunityClassification::Exact;
    if (lostOpportunities > 0) {
        ++forwardReconciliationCount_;
        lostOpportunityCount_ += lostOpportunities;
        captureFirstEvent(classification, actualOrdinal, actualTarget, swapQpc, postSwapAuthority,
                          swapOrdinal, true);
    }

    records_.push_back({lastOpportunityOrdinal_,
                        pendingDecision_.opportunityOrdinal,
                        actualOrdinal,
                        pendingDecision_.renderBeginQpc,
                        pendingRenderEndQpc_,
                        swapQpc,
                        pendingDecision_.renderOrdinal,
                        swapOrdinal,
                        config_.refreshNumerator,
                        config_.refreshDenominator,
                        pendingDecision_.preRenderAuthority,
                        postSwapAuthority,
                        continuous,
                        pendingDecision_.targetFrame,
                        actualTarget,
                        pendingRenderedSourceFrame_,
                        repeat,
                        trueDropBefore,
                        lostOpportunities,
                        classification});
    if (repeat) {
        ++repeated_;
    } else {
        ++displayedUnique_;
        gapTrueDrop_ += trueDropBefore;
        lastUniqueFrame_ = pendingRenderedSourceFrame_;
    }
    lastOpportunityOrdinal_ = actualOrdinal;
    lastSwapQpc_ = swapQpc;
    lastRenderOrdinal_ = pendingDecision_.renderOrdinal;
    lastSwapOrdinal_ = swapOrdinal;
    lastPostSwapAuthority_ = postSwapAuthority;
    pending_ = false;
    pendingDecision_ = {};
    pendingRenderCompleted_ = false;
    pendingRenderEndQpc_ = 0;
    pendingRenderedSourceFrame_ = -1;
    return true;
}

bool PresentationOpportunityScheduler::close() {
    if (!started_ || closed_ || error_ != PresentationOpportunityError::None)
        return fail(PresentationOpportunityError::InvalidConfiguration);
    if (pending_) {
        captureFirstEvent(PresentationOpportunityClassification::PairingDefect, -1, -1, 0, {}, -1,
                          false);
        return fail(PresentationOpportunityError::RenderWithoutSwap);
    }
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
            displayedUnique_,
            repeated_,
            gapTrueDrop_,
            tailTrueDrop_,
            gapTrueDrop_ + tailTrueDrop_,
            lastOpportunityOrdinal_,
            lastUniqueFrame_,
            forwardReconciliationCount_,
            lostOpportunityCount_,
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

bool PresentationOpportunityScheduler::roundedRefreshIntervals(long long deltaQpc,
                                                               long long& intervals) const {
    long long scaled = 0;
    long long divisor = 0;
    if (!checkedMultiply(deltaQpc, config_.refreshNumerator, scaled) ||
        !checkedMultiply(config_.qpcFrequency, config_.refreshDenominator, divisor) || divisor <= 0)
        return false;
    const long long quotient = scaled / divisor;
    const long long remainder = scaled % divisor;
    // exactly半周期はどちらのopportunityか一意に決められない。
    if (remainder == divisor - remainder)
        return false;
    intervals = quotient + (remainder > divisor - remainder ? 1 : 0);
    return true;
}

bool PresentationOpportunityScheduler::authorityContinuous(
    const PresentationAuthoritySample& pre, const PresentationAuthoritySample& post) const {
    if (!config_.requireAuthoritySamples)
        return true;
    const auto valid = [&](const PresentationAuthoritySample& value) {
        return value.available && value.refreshCount > 0 && value.qpcVBlank > 0 &&
               value.refreshNumerator == config_.refreshNumerator &&
               value.refreshDenominator == config_.refreshDenominator;
    };
    if (!valid(pre) || !valid(post) || post.refreshCount < pre.refreshCount ||
        post.qpcVBlank < pre.qpcVBlank)
        return false;
    if (lastOpportunityOrdinal_ >= 0 && (pre.refreshCount < lastPostSwapAuthority_.refreshCount ||
                                         pre.qpcVBlank < lastPostSwapAuthority_.qpcVBlank))
        return false;
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
                   pendingDecision_.valid ? pendingDecision_.lastCommittedOpportunityOrdinal
                                          : lastOpportunityOrdinal_,
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
    case PresentationOpportunityError::AmbiguousOpportunity:
        return "AMBIGUOUS_OPPORTUNITY";
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
