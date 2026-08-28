#include "media/gpu_preview/qualified_present_commit_join.h"

namespace mvm::gpu {

bool ExactQualifiedCommitJoin::startEnvelope() {
    *this = {};
    envelopeStarted_ = true;
    return true;
}

bool ExactQualifiedCommitJoin::reserve(const QualifiedCommitReservation& reservation) {
    if (!envelopeStarted_)
        return fail(QualifiedCommitError::EnvelopeNotStarted);
    if (error_ != QualifiedCommitError::None)
        return false;
    if (state_ != State::Idle && state_ != State::Committed)
        return fail(QualifiedCommitError::ReservationAlreadyActive);
    if (reservation.reservationId == 0 || reservation.intentOrdinal < 0 ||
        reservation.tokenSerial == 0)
        return fail(QualifiedCommitError::InvalidReservation);
    reservation_ = reservation;
    attribution_ = {};
    attribution_.reservation = reservation;
    expectedPresentSerial_ = 0;
    expectedHresult_ = 0;
    state_ = State::Reserved;
    return true;
}

bool ExactQualifiedCommitJoin::markRenderComplete(std::uint64_t reservationId,
                                                  long long intentOrdinal,
                                                  std::uint64_t tokenSerial) {
    if (error_ != QualifiedCommitError::None)
        return false;
    if (state_ != State::Reserved)
        return fail(QualifiedCommitError::RenderCompletionMissing);
    if (!matchesReservation(reservationId, intentOrdinal, tokenSerial))
        return fail(QualifiedCommitError::ReservationMismatch);
    state_ = State::RenderCompleted;
    return true;
}

bool ExactQualifiedCommitJoin::bindNativePresent(const QualifiedNativePresentEvidence& evidence) {
    attribution_.nativePresent = evidence;
    attribution_.nativePresentObserved = true;
    if (error_ != QualifiedCommitError::None)
        return false;
    if (state_ != State::RenderCompleted)
        return failAt(QualifiedCommitFailurePhase::BindNativePresent,
                      QualifiedCommitFailurePredicate::BindStateRenderCompleted,
                      QualifiedCommitError::RenderCompletionMissing);
    if (!evidence.recordPresent)
        return failAt(QualifiedCommitFailurePhase::BindNativePresent,
                      QualifiedCommitFailurePredicate::BindRecordPresent,
                      QualifiedCommitError::NativePresentMissing);
    if (!evidence.tokenPresent)
        return failAt(QualifiedCommitFailurePhase::BindNativePresent,
                      QualifiedCommitFailurePredicate::BindTokenPresent,
                      QualifiedCommitError::CompositionTokenMismatch);
    if (!evidence.intentOrdinalExact)
        return failAt(QualifiedCommitFailurePhase::BindNativePresent,
                      QualifiedCommitFailurePredicate::BindIntentOrdinalExact,
                      QualifiedCommitError::CompositionTokenMismatch);
    if (evidence.intentOrdinal != reservation_.intentOrdinal)
        return failAt(QualifiedCommitFailurePhase::BindNativePresent,
                      QualifiedCommitFailurePredicate::BindIntentOrdinalEqualsReservation,
                      QualifiedCommitError::ReservationMismatch);
    if (evidence.tokenSerial != reservation_.tokenSerial)
        return failAt(QualifiedCommitFailurePhase::BindNativePresent,
                      QualifiedCommitFailurePredicate::BindTokenSerialEqualsReservation,
                      QualifiedCommitError::CompositionTokenMismatch);
    if (evidence.presentSerial == 0)
        return failAt(QualifiedCommitFailurePhase::BindNativePresent,
                      QualifiedCommitFailurePredicate::BindPresentSerialNonzero,
                      QualifiedCommitError::PresentSerialInvalid);
    if (evidence.swapchainIdentity == 0)
        return failAt(QualifiedCommitFailurePhase::BindNativePresent,
                      QualifiedCommitFailurePredicate::BindSwapchainIdentityNonzero,
                      QualifiedCommitError::SwapchainIdentityInvalid);
    if (evidence.hresult < 0)
        return failAt(QualifiedCommitFailurePhase::BindNativePresent,
                      QualifiedCommitFailurePredicate::BindHresultSucceeded,
                      QualifiedCommitError::PresentFailed);
    if (boundSwapchainIdentity_ == 0)
        boundSwapchainIdentity_ = evidence.swapchainIdentity;
    else if (evidence.swapchainIdentity != boundSwapchainIdentity_)
        return failAt(QualifiedCommitFailurePhase::BindNativePresent,
                      QualifiedCommitFailurePredicate::BindSwapchainIdentityPinned,
                      QualifiedCommitError::SwapchainIdentityMismatch);

    // expected_present_serialの唯一のauthority。last+1、latest record、array位置、
    // QPC proximityからは生成しない。
    expectedPresentSerial_ = evidence.presentSerial;
    expectedHresult_ = evidence.hresult;
    state_ = State::NativePresentBound;
    return true;
}

QualifiedCommitResult
ExactQualifiedCommitJoin::commitFrameSwapped(const QualifiedFrameSwappedEvidence& evidence) {
    attribution_.frameSwapped = evidence;
    attribution_.frameSwappedObserved = true;
    if (state_ == State::Committed)
        return reject(QualifiedCommitError::DuplicateCommit);
    if (error_ != QualifiedCommitError::None)
        return QualifiedCommitResult::Rejected;
    if (state_ != State::NativePresentBound)
        return rejectAt(QualifiedCommitFailurePhase::CommitFrameSwapped,
                        QualifiedCommitFailurePredicate::CommitStateNativePresentBound,
                        QualifiedCommitError::NativePresentMissing);
    if (!evidence.observed)
        return rejectAt(QualifiedCommitFailurePhase::CommitFrameSwapped,
                        QualifiedCommitFailurePredicate::CommitFrameSwappedObserved,
                        QualifiedCommitError::FrameSwappedMissing);
    if (evidence.reservationId != reservation_.reservationId)
        return rejectAt(QualifiedCommitFailurePhase::CommitFrameSwapped,
                        QualifiedCommitFailurePredicate::CommitReservationIdEqualsReservation,
                        QualifiedCommitError::ReservationMismatch);
    if (evidence.intentOrdinal != reservation_.intentOrdinal)
        return rejectAt(QualifiedCommitFailurePhase::CommitFrameSwapped,
                        QualifiedCommitFailurePredicate::CommitIntentOrdinalEqualsReservation,
                        QualifiedCommitError::ReservationMismatch);
    if (evidence.tokenSerial != reservation_.tokenSerial)
        return rejectAt(QualifiedCommitFailurePhase::CommitFrameSwapped,
                        QualifiedCommitFailurePredicate::CommitTokenSerialEqualsReservation,
                        QualifiedCommitError::CompositionTokenMismatch);
    if (evidence.presentSerial != expectedPresentSerial_)
        return rejectAt(QualifiedCommitFailurePhase::CommitFrameSwapped,
                        QualifiedCommitFailurePredicate::CommitPresentSerialEqualsNative,
                        QualifiedCommitError::PresentSerialMismatch);
    if (evidence.swapchainIdentity != boundSwapchainIdentity_)
        return rejectAt(QualifiedCommitFailurePhase::CommitFrameSwapped,
                        QualifiedCommitFailurePredicate::CommitSwapchainIdentityEqualsNative,
                        QualifiedCommitError::SwapchainIdentityMismatch);
    if (evidence.hresult != expectedHresult_)
        return rejectAt(QualifiedCommitFailurePhase::CommitFrameSwapped,
                        QualifiedCommitFailurePredicate::CommitHresultEqualsNative,
                        QualifiedCommitError::PresentFailed);
    state_ = State::Committed;
    return QualifiedCommitResult::QualifiedCommit;
}

bool ExactQualifiedCommitJoin::hasActiveReservation() const {
    return state_ == State::Reserved || state_ == State::RenderCompleted ||
           state_ == State::NativePresentBound;
}

bool ExactQualifiedCommitJoin::fail(QualifiedCommitError error) {
    if (error_ == QualifiedCommitError::None)
        error_ = error;
    return false;
}

bool ExactQualifiedCommitJoin::failAt(QualifiedCommitFailurePhase phase,
                                      QualifiedCommitFailurePredicate predicate,
                                      QualifiedCommitError error) {
    attribution_.failurePhase = phase;
    attribution_.failurePredicate = predicate;
    attribution_.error = error;
    return fail(error);
}

QualifiedCommitResult ExactQualifiedCommitJoin::reject(QualifiedCommitError error) {
    fail(error);
    return QualifiedCommitResult::Rejected;
}

QualifiedCommitResult ExactQualifiedCommitJoin::rejectAt(QualifiedCommitFailurePhase phase,
                                                         QualifiedCommitFailurePredicate predicate,
                                                         QualifiedCommitError error) {
    failAt(phase, predicate, error);
    return QualifiedCommitResult::Rejected;
}

bool ExactQualifiedCommitJoin::matchesReservation(std::uint64_t reservationId,
                                                  long long intentOrdinal,
                                                  std::uint64_t tokenSerial) const {
    return reservationId == reservation_.reservationId &&
           intentOrdinal == reservation_.intentOrdinal && tokenSerial == reservation_.tokenSerial;
}

const char* qualifiedCommitErrorName(QualifiedCommitError error) {
    switch (error) {
    case QualifiedCommitError::None:
        return "NONE";
    case QualifiedCommitError::EnvelopeNotStarted:
        return "ENVELOPE_NOT_STARTED";
    case QualifiedCommitError::InvalidReservation:
        return "INVALID_RESERVATION";
    case QualifiedCommitError::ReservationAlreadyActive:
        return "RESERVATION_ALREADY_ACTIVE";
    case QualifiedCommitError::ReservationMismatch:
        return "RESERVATION_MISMATCH";
    case QualifiedCommitError::CompositionTokenMismatch:
        return "COMPOSITION_TOKEN_MISMATCH";
    case QualifiedCommitError::RenderCompletionMissing:
        return "RENDER_COMPLETION_MISSING";
    case QualifiedCommitError::NativePresentMissing:
        return "NATIVE_PRESENT_MISSING";
    case QualifiedCommitError::PresentSerialInvalid:
        return "PRESENT_SERIAL_INVALID";
    case QualifiedCommitError::SwapchainIdentityInvalid:
        return "SWAPCHAIN_IDENTITY_INVALID";
    case QualifiedCommitError::SwapchainIdentityMismatch:
        return "SWAPCHAIN_IDENTITY_MISMATCH";
    case QualifiedCommitError::PresentFailed:
        return "PRESENT_FAILED";
    case QualifiedCommitError::FrameSwappedMissing:
        return "FRAME_SWAPPED_MISSING";
    case QualifiedCommitError::PresentSerialMismatch:
        return "PRESENT_SERIAL_MISMATCH";
    case QualifiedCommitError::DuplicateCommit:
        return "DUPLICATE_COMMIT";
    }
    return "UNKNOWN";
}

const char* qualifiedCommitFailurePhaseName(QualifiedCommitFailurePhase phase) {
    switch (phase) {
    case QualifiedCommitFailurePhase::None:
        return "NONE";
    case QualifiedCommitFailurePhase::PreJoinBoundarySwap:
        return "PRE_JOIN_BOUNDARY_SWAP";
    case QualifiedCommitFailurePhase::BindNativePresent:
        return "BIND_NATIVE_PRESENT";
    case QualifiedCommitFailurePhase::CommitFrameSwapped:
        return "COMMIT_FRAME_SWAPPED";
    }
    return "UNKNOWN";
}

const char* qualifiedCommitFailurePredicateName(QualifiedCommitFailurePredicate predicate) {
    switch (predicate) {
    case QualifiedCommitFailurePredicate::None:
        return "NONE";
    case QualifiedCommitFailurePredicate::BoundarySwapRequiresNoActiveReservation:
        return "BOUNDARY_SWAP_REQUIRES_NO_ACTIVE_RESERVATION";
    case QualifiedCommitFailurePredicate::BindStateRenderCompleted:
        return "STATE_IS_RENDER_COMPLETED";
    case QualifiedCommitFailurePredicate::BindRecordPresent:
        return "NATIVE_RECORD_PRESENT";
    case QualifiedCommitFailurePredicate::BindTokenPresent:
        return "NATIVE_TOKEN_PRESENT";
    case QualifiedCommitFailurePredicate::BindIntentOrdinalExact:
        return "NATIVE_INTENT_ORDINAL_VALID";
    case QualifiedCommitFailurePredicate::BindIntentOrdinalEqualsReservation:
        return "NATIVE_INTENT_ORDINAL_EQUALS_RESERVATION";
    case QualifiedCommitFailurePredicate::BindTokenSerialEqualsReservation:
        return "NATIVE_TOKEN_SERIAL_EQUALS_RESERVATION";
    case QualifiedCommitFailurePredicate::BindPresentSerialNonzero:
        return "NATIVE_PRESENT_SERIAL_NONZERO";
    case QualifiedCommitFailurePredicate::BindSwapchainIdentityNonzero:
        return "NATIVE_SWAPCHAIN_IDENTITY_NONZERO";
    case QualifiedCommitFailurePredicate::BindHresultSucceeded:
        return "NATIVE_HRESULT_SUCCEEDED";
    case QualifiedCommitFailurePredicate::BindSwapchainIdentityPinned:
        return "NATIVE_SWAPCHAIN_IDENTITY_EQUALS_PINNED";
    case QualifiedCommitFailurePredicate::CommitStateNativePresentBound:
        return "STATE_IS_NATIVE_PRESENT_BOUND";
    case QualifiedCommitFailurePredicate::CommitFrameSwappedObserved:
        return "FRAME_SWAPPED_OBSERVED";
    case QualifiedCommitFailurePredicate::CommitReservationIdEqualsReservation:
        return "RECEIPT_RESERVATION_ID_EQUALS_RESERVATION";
    case QualifiedCommitFailurePredicate::CommitIntentOrdinalEqualsReservation:
        return "RECEIPT_INTENT_ORDINAL_EQUALS_RESERVATION";
    case QualifiedCommitFailurePredicate::CommitTokenSerialEqualsReservation:
        return "RECEIPT_TOKEN_SERIAL_EQUALS_RESERVATION";
    case QualifiedCommitFailurePredicate::CommitPresentSerialEqualsNative:
        return "RECEIPT_PRESENT_SERIAL_EQUALS_NATIVE";
    case QualifiedCommitFailurePredicate::CommitSwapchainIdentityEqualsNative:
        return "RECEIPT_SWAPCHAIN_IDENTITY_EQUALS_NATIVE";
    case QualifiedCommitFailurePredicate::CommitHresultEqualsNative:
        return "RECEIPT_HRESULT_EQUALS_NATIVE";
    }
    return "UNKNOWN";
}

} // namespace mvm::gpu
