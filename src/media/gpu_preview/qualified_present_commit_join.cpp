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
    if (error_ != QualifiedCommitError::None)
        return false;
    if (state_ != State::RenderCompleted)
        return fail(QualifiedCommitError::RenderCompletionMissing);
    if (!evidence.recordPresent)
        return fail(QualifiedCommitError::NativePresentMissing);
    if (!evidence.tokenPresent || !evidence.intentOrdinalExact)
        return fail(QualifiedCommitError::CompositionTokenMismatch);
    if (evidence.intentOrdinal != reservation_.intentOrdinal)
        return fail(QualifiedCommitError::ReservationMismatch);
    if (evidence.tokenSerial != reservation_.tokenSerial)
        return fail(QualifiedCommitError::CompositionTokenMismatch);
    if (evidence.presentSerial == 0)
        return fail(QualifiedCommitError::PresentSerialInvalid);
    if (evidence.swapchainIdentity == 0)
        return fail(QualifiedCommitError::SwapchainIdentityInvalid);
    if (evidence.hresult < 0)
        return fail(QualifiedCommitError::PresentFailed);
    if (boundSwapchainIdentity_ == 0)
        boundSwapchainIdentity_ = evidence.swapchainIdentity;
    else if (evidence.swapchainIdentity != boundSwapchainIdentity_)
        return fail(QualifiedCommitError::SwapchainIdentityMismatch);

    // expected_present_serialの唯一のauthority。last+1、latest record、array位置、
    // QPC proximityからは生成しない。
    expectedPresentSerial_ = evidence.presentSerial;
    expectedHresult_ = evidence.hresult;
    state_ = State::NativePresentBound;
    return true;
}

QualifiedCommitResult
ExactQualifiedCommitJoin::commitFrameSwapped(const QualifiedFrameSwappedEvidence& evidence) {
    if (state_ == State::Committed)
        return reject(QualifiedCommitError::DuplicateCommit);
    if (error_ != QualifiedCommitError::None)
        return QualifiedCommitResult::Rejected;
    if (state_ != State::NativePresentBound)
        return reject(QualifiedCommitError::NativePresentMissing);
    if (!evidence.observed)
        return reject(QualifiedCommitError::FrameSwappedMissing);
    if (evidence.reservationId != reservation_.reservationId ||
        evidence.intentOrdinal != reservation_.intentOrdinal)
        return reject(QualifiedCommitError::ReservationMismatch);
    if (evidence.tokenSerial != reservation_.tokenSerial)
        return reject(QualifiedCommitError::CompositionTokenMismatch);
    if (evidence.presentSerial != expectedPresentSerial_)
        return reject(QualifiedCommitError::PresentSerialMismatch);
    if (evidence.swapchainIdentity != boundSwapchainIdentity_)
        return reject(QualifiedCommitError::SwapchainIdentityMismatch);
    if (evidence.hresult != expectedHresult_)
        return reject(QualifiedCommitError::PresentFailed);
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

QualifiedCommitResult ExactQualifiedCommitJoin::reject(QualifiedCommitError error) {
    fail(error);
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

} // namespace mvm::gpu
