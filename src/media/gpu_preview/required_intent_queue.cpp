#include "media/gpu_preview/required_intent_queue.h"

#include <limits>

namespace mvm::gpu {

bool RequiredIntentQueue::start(long long requiredCount) {
    *this = {};
    if (requiredCount <= 0 ||
        static_cast<unsigned long long>(requiredCount) > std::numeric_limits<std::size_t>::max())
        return fail(RequiredIntentQueueError::InvalidRequiredSet);
    requiredIntentOrdinals_.reserve(static_cast<std::size_t>(requiredCount));
    for (long long ordinal = 0; ordinal < requiredCount; ++ordinal)
        requiredIntentOrdinals_.push_back(ordinal);
    started_ = true;
    return true;
}

RequiredIntentReserveDecision RequiredIntentQueue::reserveHead() {
    if (!started_) {
        fail(RequiredIntentQueueError::NotStarted);
        return {};
    }
    if (closed_) {
        fail(RequiredIntentQueueError::Closed);
        return {};
    }
    if (error_ != RequiredIntentQueueError::None)
        return {};
    if (active_)
        return {RequiredIntentReserveResult::Duplicate, activeReservation_};
    if (headIndex_ == static_cast<long long>(requiredIntentOrdinals_.size()))
        return {RequiredIntentReserveResult::Exhausted, {}};

    activeReservation_ = {++reservationSerial_,
                          requiredIntentOrdinals_[static_cast<std::size_t>(headIndex_)]};
    active_ = true;
    rendered_ = false;
    ++issuedCount_;
    if (!conservationValid()) {
        fail(RequiredIntentQueueError::ConservationViolation);
        return {};
    }
    return {RequiredIntentReserveResult::Reserved, activeReservation_};
}

bool RequiredIntentQueue::markRenderComplete(std::uint64_t reservationId, long long intentOrdinal) {
    if (!started_)
        return fail(RequiredIntentQueueError::NotStarted);
    if (closed_)
        return fail(RequiredIntentQueueError::Closed);
    if (error_ != RequiredIntentQueueError::None)
        return false;
    if (!active_ || !matches(reservationId, intentOrdinal))
        return fail(RequiredIntentQueueError::ReservationMismatch);
    if (rendered_)
        return fail(RequiredIntentQueueError::DuplicateRenderCompletion);
    rendered_ = true;
    ++renderedCount_;
    return true;
}

bool RequiredIntentQueue::commitQualified(std::uint64_t reservationId, long long intentOrdinal) {
    if (!started_)
        return fail(RequiredIntentQueueError::NotStarted);
    if (closed_)
        return fail(RequiredIntentQueueError::Closed);
    if (error_ != RequiredIntentQueueError::None)
        return false;
    if (!active_ || !matches(reservationId, intentOrdinal))
        return fail(RequiredIntentQueueError::ReservationMismatch);
    if (!rendered_)
        return fail(RequiredIntentQueueError::RenderCompletionMissing);

    ++headIndex_;
    ++qualifiedCommitCount_;
    active_ = false;
    rendered_ = false;
    activeReservation_ = {};
    if (!conservationValid())
        return fail(RequiredIntentQueueError::ConservationViolation);
    return true;
}

bool RequiredIntentQueue::closePlannedWindow() {
    return close(true);
}

bool RequiredIntentQueue::closeWithoutNormalCompletion() {
    return close(false);
}

bool RequiredIntentQueue::close(bool plannedWindowEnded) {
    if (!started_)
        return fail(RequiredIntentQueueError::NotStarted);
    if (closed_)
        return fail(RequiredIntentQueueError::Closed);
    // active reservationとunissued tailは意図的に保持する。closeはconsumeではない。
    plannedWindowEnded_ = plannedWindowEnded;
    closed_ = true;
    if (!conservationValid())
        return fail(RequiredIntentQueueError::ConservationViolation);
    return error_ == RequiredIntentQueueError::None;
}

RequiredIntentQueueSnapshot RequiredIntentQueue::snapshot() const {
    const long long activeCount = active_ ? 1 : 0;
    return {started_,
            closed_,
            plannedWindowEnded_,
            started_,
            false,
            error_,
            requiredIntentOrdinals_,
            static_cast<long long>(requiredIntentOrdinals_.size()),
            issuedCount_,
            renderedCount_,
            qualifiedCommitCount_,
            headIndex_,
            activeCount,
            static_cast<long long>(requiredIntentOrdinals_.size()) - issuedCount_,
            activeReservation_,
            rendered_,
            conservationValid()};
}

bool RequiredIntentQueue::matches(std::uint64_t reservationId, long long intentOrdinal) const {
    return reservationId != 0 && reservationId == activeReservation_.reservationId &&
           intentOrdinal == activeReservation_.intentOrdinal;
}

bool RequiredIntentQueue::conservationValid() const {
    const long long required = static_cast<long long>(requiredIntentOrdinals_.size());
    const long long activeCount = active_ ? 1 : 0;
    const long long activeRenderedCount = active_ && rendered_ ? 1 : 0;
    const long long unissued = required - issuedCount_;
    return headIndex_ == qualifiedCommitCount_ && headIndex_ >= 0 && headIndex_ <= issuedCount_ &&
           issuedCount_ <= required && issuedCount_ == headIndex_ + activeCount && unissued >= 0 &&
           renderedCount_ == qualifiedCommitCount_ + activeRenderedCount &&
           required == headIndex_ + activeCount + unissued;
}

bool RequiredIntentQueue::fail(RequiredIntentQueueError error) {
    if (error_ == RequiredIntentQueueError::None)
        error_ = error;
    return false;
}

const char* requiredIntentQueueErrorName(RequiredIntentQueueError error) {
    switch (error) {
    case RequiredIntentQueueError::None:
        return "NONE";
    case RequiredIntentQueueError::InvalidRequiredSet:
        return "INVALID_REQUIRED_SET";
    case RequiredIntentQueueError::NotStarted:
        return "NOT_STARTED";
    case RequiredIntentQueueError::Closed:
        return "CLOSED";
    case RequiredIntentQueueError::ReservationMismatch:
        return "RESERVATION_MISMATCH";
    case RequiredIntentQueueError::RenderCompletionMissing:
        return "RENDER_COMPLETION_MISSING";
    case RequiredIntentQueueError::DuplicateRenderCompletion:
        return "DUPLICATE_RENDER_COMPLETION";
    case RequiredIntentQueueError::ConservationViolation:
        return "CONSERVATION_VIOLATION";
    }
    return "UNKNOWN";
}

} // namespace mvm::gpu
