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

PresentationOpportunityDecision
PresentationOpportunityScheduler::selectForRender(long long callbackQpc) {
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
    return decision;
}

bool PresentationOpportunityScheduler::commitSwap(long long swapQpc,
                                                  long long presentedSourceFrame) {
    if (!started_ || closed_ || error_ != PresentationOpportunityError::None)
        return fail(PresentationOpportunityError::InvalidConfiguration);
    if (!pending_)
        return fail(PresentationOpportunityError::SwapWithoutRender);
    if (swapQpc <= 0 || (lastSwapQpc_ > 0 && swapQpc <= lastSwapQpc_))
        return fail(PresentationOpportunityError::OpportunityRegression);

    long long actualOrdinal = 0;
    if (lastOpportunityOrdinal_ >= 0) {
        long long intervals = 0;
        if (!roundedRefreshIntervals(swapQpc - lastSwapQpc_, intervals) || intervals <= 0)
            return fail(PresentationOpportunityError::AmbiguousOpportunity);
        if (intervals > std::numeric_limits<long long>::max() - lastOpportunityOrdinal_)
            return fail(PresentationOpportunityError::ArithmeticOverflow);
        actualOrdinal = lastOpportunityOrdinal_ + intervals;
    }
    if (actualOrdinal != pendingDecision_.opportunityOrdinal)
        return fail(PresentationOpportunityError::RenderSwapMismatch);
    if (presentedSourceFrame != pendingDecision_.targetFrame)
        return fail(PresentationOpportunityError::PresentedFrameMismatch);

    records_.push_back({actualOrdinal, swapQpc, config_.refreshNumerator,
                        config_.refreshDenominator, pendingDecision_.targetFrame,
                        presentedSourceFrame, pendingDecision_.repeat,
                        pendingDecision_.trueDropBefore});
    if (pendingDecision_.repeat) {
        ++repeated_;
    } else {
        ++displayedUnique_;
        gapTrueDrop_ += pendingDecision_.trueDropBefore;
        lastUniqueFrame_ = pendingDecision_.targetFrame;
    }
    lastOpportunityOrdinal_ = actualOrdinal;
    lastSwapQpc_ = swapQpc;
    pending_ = false;
    pendingDecision_ = {};
    return true;
}

bool PresentationOpportunityScheduler::close() {
    if (!started_ || closed_ || error_ != PresentationOpportunityError::None)
        return fail(PresentationOpportunityError::InvalidConfiguration);
    if (pending_)
        return fail(PresentationOpportunityError::RenderWithoutSwap);
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
    case PresentationOpportunityError::RenderWithoutSwap:
        return "RENDER_WITHOUT_SWAP";
    case PresentationOpportunityError::SwapWithoutRender:
        return "SWAP_WITHOUT_RENDER";
    case PresentationOpportunityError::RenderSwapMismatch:
        return "RENDER_SWAP_MISMATCH";
    case PresentationOpportunityError::PresentedFrameMismatch:
        return "PRESENTED_FRAME_MISMATCH";
    }
    return "UNKNOWN";
}

} // namespace mvm::gpu
