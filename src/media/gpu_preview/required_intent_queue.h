#ifndef MVM_MEDIA_GPU_PREVIEW_REQUIRED_INTENT_QUEUE_H
#define MVM_MEDIA_GPU_PREVIEW_REQUIRED_INTENT_QUEUE_H

#include <cstdint>
#include <vector>

namespace mvm::gpu {

enum class RequiredIntentQueueError {
    None = 0,
    InvalidRequiredSet,
    NotStarted,
    Closed,
    ReservationMismatch,
    RenderCompletionMissing,
    DuplicateRenderCompletion,
    ConservationViolation,
};

enum class RequiredIntentReserveResult { Rejected = 0, Reserved, Duplicate, Exhausted };

struct RequiredIntentReservation {
    std::uint64_t reservationId = 0;
    long long intentOrdinal = -1;
};

struct RequiredIntentReserveDecision {
    RequiredIntentReserveResult result = RequiredIntentReserveResult::Rejected;
    RequiredIntentReservation reservation;
};

struct RequiredIntentQueueSnapshot {
    bool started = false;
    bool closed = false;
    bool plannedWindowEnded = false;
    bool requiredSetImmutable = false;
    bool displaySatisfactionImported = false;
    RequiredIntentQueueError error = RequiredIntentQueueError::None;
    std::vector<long long> requiredIntentOrdinals;
    long long requiredCount = 0;
    long long issuedCount = 0;
    long long renderedCount = 0;
    long long qualifiedCommitCount = 0;
    long long dequeuedCount = 0;
    long long activeReservationCount = 0;
    long long unissuedTailCount = 0;
    RequiredIntentReservation activeReservation;
    bool activeReservationRendered = false;
    bool conservationValid = false;
};

// B3-I1のonline issuance authority。required setはstart時の[0,N)から変更せず、
// exact qualified commitだけがheadを1件dequeueする。display outcomeは扱わない。
class RequiredIntentQueue {
public:
    bool start(long long requiredCount);
    RequiredIntentReserveDecision reserveHead();
    bool markRenderComplete(std::uint64_t reservationId, long long intentOrdinal);
    bool commitQualified(std::uint64_t reservationId, long long intentOrdinal);
    bool closePlannedWindow();
    bool closeWithoutNormalCompletion();

    RequiredIntentQueueSnapshot snapshot() const;

    RequiredIntentQueueError error() const { return error_; }

private:
    bool close(bool plannedWindowEnded);
    bool matches(std::uint64_t reservationId, long long intentOrdinal) const;
    bool conservationValid() const;
    bool fail(RequiredIntentQueueError error);

    bool started_ = false;
    bool closed_ = false;
    bool plannedWindowEnded_ = false;
    RequiredIntentQueueError error_ = RequiredIntentQueueError::None;
    std::vector<long long> requiredIntentOrdinals_;
    long long headIndex_ = 0;
    long long issuedCount_ = 0;
    long long renderedCount_ = 0;
    long long qualifiedCommitCount_ = 0;
    std::uint64_t reservationSerial_ = 0;
    bool active_ = false;
    bool rendered_ = false;
    RequiredIntentReservation activeReservation_{};
};

const char* requiredIntentQueueErrorName(RequiredIntentQueueError error);

} // namespace mvm::gpu

#endif
