#ifndef MVM_MEDIA_GPU_PREVIEW_QUALIFIED_PRESENT_COMMIT_JOIN_H
#define MVM_MEDIA_GPU_PREVIEW_QUALIFIED_PRESENT_COMMIT_JOIN_H

#include <cstdint>

namespace mvm::gpu {

enum class QualifiedCommitResult { Rejected = 0, QualifiedCommit };

enum class QualifiedCommitError {
    None = 0,
    EnvelopeNotStarted,
    InvalidReservation,
    ReservationAlreadyActive,
    ReservationMismatch,
    CompositionTokenMismatch,
    RenderCompletionMissing,
    NativePresentMissing,
    PresentSerialInvalid,
    SwapchainIdentityInvalid,
    SwapchainIdentityMismatch,
    PresentFailed,
    FrameSwappedMissing,
    PresentSerialMismatch,
    DuplicateCommit,
};

struct QualifiedCommitReservation {
    std::uint64_t reservationId = 0;
    long long intentOrdinal = -1;
    std::uint64_t tokenSerial = 0;
};

// actual native Present recordから変換する。presentSerialはpatched Qtが
// IDXGISwapChain::Present直前にmintし、record自身が保持する値だけを受理する。
struct QualifiedNativePresentEvidence {
    bool recordPresent = false;
    std::uint64_t presentSerial = 0;
    std::uint64_t swapchainIdentity = 0;
    std::int32_t hresult = 0;
    bool tokenPresent = false;
    std::uint64_t tokenSerial = 0;
    long long intentOrdinal = -1;
    bool intentOrdinalExact = false;
};

// patched Qtのone-shot receiptをDirectConnectionのframeSwapped callbackで
// consumeした事実。QPCやring位置から再構成しない。
struct QualifiedFrameSwappedEvidence {
    bool observed = false;
    std::uint64_t reservationId = 0;
    long long intentOrdinal = -1;
    std::uint64_t tokenSerial = 0;
    std::uint64_t presentSerial = 0;
    std::uint64_t swapchainIdentity = 0;
    std::int32_t hresult = 0;
};

class ExactQualifiedCommitJoin {
public:
    bool startEnvelope();
    bool reserve(const QualifiedCommitReservation& reservation);
    bool markRenderComplete(std::uint64_t reservationId, long long intentOrdinal,
                            std::uint64_t tokenSerial);
    bool bindNativePresent(const QualifiedNativePresentEvidence& evidence);
    QualifiedCommitResult commitFrameSwapped(const QualifiedFrameSwappedEvidence& evidence);

    bool hasActiveReservation() const;

    const QualifiedCommitReservation& reservation() const { return reservation_; }

    std::uint64_t expectedPresentSerial() const { return expectedPresentSerial_; }

    std::uint64_t boundSwapchainIdentity() const { return boundSwapchainIdentity_; }

    QualifiedCommitError error() const { return error_; }

private:
    enum class State { Idle, Reserved, RenderCompleted, NativePresentBound, Committed };

    bool fail(QualifiedCommitError error);
    QualifiedCommitResult reject(QualifiedCommitError error);
    bool matchesReservation(std::uint64_t reservationId, long long intentOrdinal,
                            std::uint64_t tokenSerial) const;

    bool envelopeStarted_ = false;
    State state_ = State::Idle;
    QualifiedCommitError error_ = QualifiedCommitError::None;
    QualifiedCommitReservation reservation_{};
    std::uint64_t expectedPresentSerial_ = 0;
    std::int32_t expectedHresult_ = 0;
    std::uint64_t boundSwapchainIdentity_ = 0;
};

const char* qualifiedCommitErrorName(QualifiedCommitError error);

} // namespace mvm::gpu

#endif
