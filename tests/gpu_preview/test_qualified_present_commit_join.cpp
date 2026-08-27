#include "media/gpu_preview/qualified_present_commit_join.h"

#include <cstdio>

namespace {

using Error = mvm::gpu::QualifiedCommitError;
using Join = mvm::gpu::ExactQualifiedCommitJoin;
using Result = mvm::gpu::QualifiedCommitResult;

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

mvm::gpu::QualifiedCommitReservation reservation() {
    return {17, 42, 9001};
}

mvm::gpu::QualifiedNativePresentEvidence nativePresent() {
    return {true, 7001, 0x12340000, 0, true, 9001, 42, true};
}

mvm::gpu::QualifiedFrameSwappedEvidence frameSwapped() {
    return {true, 17, 42, 9001, 7001, 0x12340000, 0};
}

Join renderedJoin() {
    Join join;
    check(join.startEnvelope(), "capture envelopeを開始できません");
    check(join.reserve(reservation()), "reservationを開始できません");
    check(join.markRenderComplete(17, 42, 9001), "render completionを結合できません");
    return join;
}

void positive() {
    auto join = renderedJoin();
    check(join.bindNativePresent(nativePresent()), "native Present recordを結合できません");
    const auto result = join.commitFrameSwapped(frameSwapped());
    check(result == Result::QualifiedCommit, "QUALIFIED_COMMITを返しません");
    check(join.expectedPresentSerial() == 7001,
          "expected_present_serialがactual native record由来ではありません");
    check(join.boundSwapchainIdentity() == 0x12340000,
          "actual swapchain identityをcapture envelopeへ固定できません");
}

template<typename Mutate>
void rejectNative(Error expected, const char* message, Mutate mutate) {
    auto join = renderedJoin();
    auto evidence = nativePresent();
    mutate(evidence);
    check(!join.bindNativePresent(evidence) && join.error() == expected, message);
}

template<typename Mutate>
void rejectSwap(Error expected, const char* message, Mutate mutate) {
    auto join = renderedJoin();
    check(join.bindNativePresent(nativePresent()), "negativeのnative bind前提が壊れています");
    auto evidence = frameSwapped();
    mutate(evidence);
    check(join.commitFrameSwapped(evidence) == Result::Rejected && join.error() == expected,
          message);
}

void nativeNegatives() {
    rejectNative(Error::NativePresentMissing, "missing recordを拒否しません",
                 [](auto& value) { value.recordPresent = false; });
    rejectNative(Error::ReservationMismatch, "native intent mismatchを拒否しません",
                 [](auto& value) { value.intentOrdinal = 43; });
    rejectNative(Error::CompositionTokenMismatch, "native token mismatchを拒否しません",
                 [](auto& value) { value.tokenSerial = 9002; });
    rejectNative(Error::CompositionTokenMismatch, "token欠損を拒否しません",
                 [](auto& value) { value.tokenPresent = false; });
    rejectNative(Error::CompositionTokenMismatch, "intent provenance欠損を拒否しません",
                 [](auto& value) { value.intentOrdinalExact = false; });
    rejectNative(Error::PresentSerialInvalid, "present serial 0を拒否しません",
                 [](auto& value) { value.presentSerial = 0; });
    rejectNative(Error::SwapchainIdentityInvalid, "swapchain identity 0を拒否しません",
                 [](auto& value) { value.swapchainIdentity = 0; });
    rejectNative(Error::PresentFailed, "Present failureを拒否しません",
                 [](auto& value) { value.hresult = static_cast<std::int32_t>(0x80004005U); });
}

void commitNegatives() {
    rejectSwap(Error::FrameSwappedMissing, "frameSwapped欠損を拒否しません",
               [](auto& value) { value.observed = false; });
    rejectSwap(Error::ReservationMismatch, "frameSwapped reservation mismatchを拒否しません",
               [](auto& value) { value.reservationId = 18; });
    rejectSwap(Error::ReservationMismatch, "frameSwapped intent mismatchを拒否しません",
               [](auto& value) { value.intentOrdinal = 43; });
    rejectSwap(Error::CompositionTokenMismatch, "frameSwapped token mismatchを拒否しません",
               [](auto& value) { value.tokenSerial = 9002; });
    rejectSwap(Error::PresentSerialMismatch, "receipt present serial mismatchを拒否しません",
               [](auto& value) { value.presentSerial = 7002; });
    rejectSwap(Error::SwapchainIdentityMismatch, "swapchain identity mismatchを拒否しません",
               [](auto& value) { value.swapchainIdentity = 0x56780000; });
    rejectSwap(Error::PresentFailed, "receipt HRESULT mismatchを拒否しません",
               [](auto& value) { value.hresult = 1; });

    auto duplicate = renderedJoin();
    check(duplicate.bindNativePresent(nativePresent()), "duplicate前のnative bindに失敗しました");
    check(duplicate.commitFrameSwapped(frameSwapped()) == Result::QualifiedCommit,
          "duplicate前のqualified commitに失敗しました");
    check(duplicate.commitFrameSwapped(frameSwapped()) == Result::Rejected &&
              duplicate.error() == Error::DuplicateCommit,
          "duplicate commitをfail-closeしません");
}

void lifecycleNegatives() {
    Join noRender;
    check(noRender.startEnvelope(), "no-render envelopeを開始できません");
    check(noRender.reserve(reservation()), "no-render reservationを開始できません");
    check(!noRender.bindNativePresent(nativePresent()) &&
              noRender.error() == Error::RenderCompletionMissing,
          "render completionなしのPresentを拒否しません");

    Join mismatch;
    check(mismatch.startEnvelope(), "mismatch envelopeを開始できません");
    check(mismatch.reserve(reservation()), "mismatch reservationを開始できません");
    check(!mismatch.markRenderComplete(18, 42, 9001) &&
              mismatch.error() == Error::ReservationMismatch,
          "render completionのreservation mismatchを拒否しません");

    Join unopened;
    check(!unopened.reserve(reservation()) && unopened.error() == Error::EnvelopeNotStarted,
          "capture envelope外のreservationを拒否しません");

    Join invalid;
    check(invalid.startEnvelope(), "invalid envelopeを開始できません");
    check(!invalid.reserve({0, 42, 9001}) && invalid.error() == Error::InvalidReservation,
          "reservation id 0を拒否しません");

    Join reentrant;
    check(reentrant.startEnvelope(), "reentrant envelopeを開始できません");
    check(reentrant.reserve(reservation()), "reentrant reservationを開始できません");
    check(!reentrant.reserve({18, 43, 9002}) &&
              reentrant.error() == Error::ReservationAlreadyActive,
          "active reservation中の二重reserveを拒否しません");

    // frameSwappedだけが到着した場合。native Present recordなしでcommitしない。
    Join swapOnly;
    check(swapOnly.startEnvelope(), "swap-only envelopeを開始できません");
    check(swapOnly.commitFrameSwapped(frameSwapped()) == Result::Rejected &&
              swapOnly.error() == Error::NativePresentMissing,
          "reservationなしのframeSwapped commitを拒否しません");

    // render完了までしか進んでいないreservationをswapだけでcommitしない。
    auto renderedOnly = renderedJoin();
    check(renderedOnly.commitFrameSwapped(frameSwapped()) == Result::Rejected &&
              renderedOnly.error() == Error::NativePresentMissing,
          "native Present未結合のframeSwapped commitを拒否しません");
}

void pinnedSwapchain() {
    auto join = renderedJoin();
    check(join.bindNativePresent(nativePresent()), "first swapchain bindに失敗しました");
    check(join.commitFrameSwapped(frameSwapped()) == Result::QualifiedCommit,
          "first qualified commitに失敗しました");

    const mvm::gpu::QualifiedCommitReservation second{18, 43, 9002};
    check(join.reserve(second), "second reservationを開始できません");
    check(join.markRenderComplete(18, 43, 9002), "second render completionに失敗しました");
    auto secondNative = nativePresent();
    secondNative.presentSerial = 7002;
    secondNative.tokenSerial = 9002;
    secondNative.intentOrdinal = 43;
    secondNative.swapchainIdentity = 0x56780000;
    check(!join.bindNativePresent(secondNative) && join.error() == Error::SwapchainIdentityMismatch,
          "capture envelope内のswapchain migrationを拒否しません");
}

} // namespace

int main() {
    positive();
    nativeNegatives();
    commitNegatives();
    lifecycleNegatives();
    pinnedSwapchain();
    return failures == 0 ? 0 : 1;
}
