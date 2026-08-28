#include "app/preview/native_present_hook.h"

#include <cstdio>

namespace {

mvm::gpu::CompositionLayerFrame layer(std::uint64_t sourceId, std::uint64_t generation,
                                      std::uint64_t resourceEpoch, long long frameNumber) {
    mvm::gpu::CompositionLayerFrame result;
    result.frame.sourceId = {sourceId};
    result.frame.sourceGeneration = {generation};
    result.frame.resourceEpoch = {resourceEpoch};
    result.frame.frameNumber = frameNumber;
    return result;
}

bool check(bool condition, const char* message) {
    if (!condition)
        std::fprintf(stderr, "%s\n", message);
    return condition;
}

constexpr std::uint64_t expectedLayoutMix(std::uint64_t signature, std::uint64_t value) {
    return (signature ^ value) * 1099511628211ULL;
}

constexpr std::uint64_t expectedSourceIdentitySemanticLayoutSignature() {
    std::uint64_t signature = 1469598103934665603ULL;
    signature = expectedLayoutMix(signature, sizeof(MvmNativePresentSourceIdentity));
    signature = expectedLayoutMix(signature, alignof(MvmNativePresentSourceIdentity));
    signature = expectedLayoutMix(signature, offsetof(MvmNativePresentSourceIdentity, sourceId));
    signature =
        expectedLayoutMix(signature, offsetof(MvmNativePresentSourceIdentity, sourceGeneration));
    signature =
        expectedLayoutMix(signature, offsetof(MvmNativePresentSourceIdentity, resourceEpoch));
    return expectedLayoutMix(signature, offsetof(MvmNativePresentSourceIdentity, frameNumber));
}

constexpr std::uint64_t expectedCompositionTokenSemanticLayoutSignature() {
    std::uint64_t signature = 1469598103934665603ULL;
    signature = expectedLayoutMix(signature, sizeof(MvmNativePresentCompositionToken));
    signature = expectedLayoutMix(signature, alignof(MvmNativePresentCompositionToken));
    signature = expectedLayoutMix(signature, expectedSourceIdentitySemanticLayoutSignature());
    signature =
        expectedLayoutMix(signature, offsetof(MvmNativePresentCompositionToken, tokenSerial));
    signature =
        expectedLayoutMix(signature, offsetof(MvmNativePresentCompositionToken, compositionEpoch));
    signature =
        expectedLayoutMix(signature, offsetof(MvmNativePresentCompositionToken, compositionState));
    signature =
        expectedLayoutMix(signature, offsetof(MvmNativePresentCompositionToken, outputFrameNumber));
    signature =
        expectedLayoutMix(signature, offsetof(MvmNativePresentCompositionToken, intentOrdinal));
    signature = expectedLayoutMix(signature,
                                  offsetof(MvmNativePresentCompositionToken, intentOrdinalValid));
    signature =
        expectedLayoutMix(signature, offsetof(MvmNativePresentCompositionToken, sourceCount));
    signature =
        expectedLayoutMix(signature, offsetof(MvmNativePresentCompositionToken, propagationSerial));
    return expectedLayoutMix(signature, offsetof(MvmNativePresentCompositionToken, sources));
}

constexpr std::uint64_t expectedReceiptSemanticLayoutSignature(bool mutateSameSizeOffset) {
    std::uint64_t signature = 1469598103934665603ULL;
    signature = expectedLayoutMix(signature, sizeof(MvmNativePresentFrameSwappedReceipt));
    signature = expectedLayoutMix(signature, alignof(MvmNativePresentFrameSwappedReceipt));
    signature =
        expectedLayoutMix(signature, offsetof(MvmNativePresentFrameSwappedReceipt, presentSerial));
    signature = expectedLayoutMix(signature,
                                  offsetof(MvmNativePresentFrameSwappedReceipt, swapchainIdentity));
    signature = expectedLayoutMix(signature,
                                  mutateSameSizeOffset
                                      ? offsetof(MvmNativePresentFrameSwappedReceipt, tokenPresent)
                                      : offsetof(MvmNativePresentFrameSwappedReceipt, hresult));
    signature =
        expectedLayoutMix(signature, offsetof(MvmNativePresentFrameSwappedReceipt, tokenPresent));
    signature =
        expectedLayoutMix(signature, offsetof(MvmNativePresentFrameSwappedReceipt, tokenSerial));
    signature =
        expectedLayoutMix(signature, offsetof(MvmNativePresentFrameSwappedReceipt, intentOrdinal));
    return expectedLayoutMix(signature,
                             offsetof(MvmNativePresentFrameSwappedReceipt, intentOrdinalValid));
}

constexpr std::uint64_t expectedSnapshotLayoutSignature(bool mutateSameSizeOffset,
                                                        bool mutateNestedReceipt) {
    std::uint64_t signature = 1469598103934665603ULL;
    signature = expectedLayoutMix(signature, sizeof(MvmNativePresentOneShotSnapshot));
    signature = expectedLayoutMix(signature, alignof(MvmNativePresentOneShotSnapshot));
    signature = expectedLayoutMix(signature, expectedCompositionTokenSemanticLayoutSignature());
    signature =
        expectedLayoutMix(signature, expectedReceiptSemanticLayoutSignature(mutateNestedReceipt));
    signature = expectedLayoutMix(signature, offsetof(MvmNativePresentOneShotSnapshot, abiVersion));
    signature =
        expectedLayoutMix(signature, offsetof(MvmNativePresentOneShotSnapshot, snapshotSize));
    signature =
        expectedLayoutMix(signature, offsetof(MvmNativePresentOneShotSnapshot, layoutSignature));
    signature =
        expectedLayoutMix(signature, offsetof(MvmNativePresentOneShotSnapshot, captureEpoch));
    signature = expectedLayoutMix(
        signature, mutateSameSizeOffset ? offsetof(MvmNativePresentOneShotSnapshot, captureThreadId)
                                        : offsetof(MvmNativePresentOneShotSnapshot, captureActive));
    signature =
        expectedLayoutMix(signature, offsetof(MvmNativePresentOneShotSnapshot, captureThreadId));
    signature =
        expectedLayoutMix(signature, offsetof(MvmNativePresentOneShotSnapshot, callerThreadId));
    signature =
        expectedLayoutMix(signature, offsetof(MvmNativePresentOneShotSnapshot, callerThreadExact));
    signature =
        expectedLayoutMix(signature, offsetof(MvmNativePresentOneShotSnapshot, pendingTokenValid));
    signature = expectedLayoutMix(signature,
                                  offsetof(MvmNativePresentOneShotSnapshot, pendingReceiptValid));
    signature =
        expectedLayoutMix(signature, offsetof(MvmNativePresentOneShotSnapshot, pendingToken));
    return expectedLayoutMix(signature, offsetof(MvmNativePresentOneShotSnapshot, pendingReceipt));
}

} // namespace

int main() {
    constexpr auto expectedSnapshotSignature = expectedSnapshotLayoutSignature(false, false);
    constexpr auto sameSizeOffsetMutation = expectedSnapshotLayoutSignature(true, false);
    constexpr auto nestedSameSizeOffsetMutation = expectedSnapshotLayoutSignature(false, true);
    if (!check(mvmNativePresentOneShotSnapshotLayoutSignature() == expectedSnapshotSignature,
               "one-shot snapshot署名が全semantic fieldの独立期待値と一致しません") ||
        !check(sameSizeOffsetMutation != expectedSnapshotSignature &&
                   !mvmNativePresentOneShotSnapshotLayoutCompatible(
                       6, sizeof(MvmNativePresentOneShotSnapshot), sameSizeOffsetMutation),
               "同一sizeのsnapshot semantic-field offset mutationを拒否しません") ||
        !check(nestedSameSizeOffsetMutation != expectedSnapshotSignature &&
                   !mvmNativePresentOneShotSnapshotLayoutCompatible(
                       6, sizeof(MvmNativePresentOneShotSnapshot), nestedSameSizeOffsetMutation),
               "同一sizeのnested receipt semantic-field offset mutationを拒否しません"))
        return 1;

    mvm::gpu::ComposedFrame frame;
    frame.outputFrameNumber = 42;
    frame.compositionEpoch = {7};
    frame.compositionState = {9};
    frame.layers = {layer(2, 12, 22, 42), layer(1, 11, 21, 42)};
    MvmNativePresentCompositionToken token;
    if (!check(mvm::app::makeNativePresentCompositionToken(frame, 5, 0, true, token),
               "正常なcomposition tokenを構築できません") ||
        !check(token.tokenSerial == 5 && token.outputFrameNumber == 42 &&
                   token.compositionEpoch == 7 && token.compositionState == 9 &&
                   token.intentOrdinalValid == 1 && token.intentOrdinal == 0,
               "composition token headerが一致しません") ||
        !check(token.sourceCount == 2 && token.sources[0].sourceId == 1 &&
                   token.sources[1].sourceId == 2 && token.sources[0].sourceGeneration == 11 &&
                   token.sources[1].resourceEpoch == 22,
               "source identityがsource id順に固定されません"))
        return 1;

    MvmNativePresentCompositionToken rejected;
    if (!check(!mvm::app::makeNativePresentCompositionToken(frame, 0, 0, false, rejected),
               "token serial 0を拒否しません"))
        return 1;
    frame.layers.clear();
    if (!check(!mvm::app::makeNativePresentCompositionToken(frame, 6, 0, false, rejected),
               "source 0件を拒否しません"))
        return 1;
    frame.layers = {layer(1, 1, 1, 42), layer(2, 1, 1, 42), layer(3, 1, 1, 42)};
    if (!check(!mvm::app::makeNativePresentCompositionToken(frame, 7, 0, false, rejected),
               "固定ABI上限を超えるsourceを拒否しません"))
        return 1;
    frame.layers = {layer(1, 0, 1, 42), layer(2, 1, 1, 42)};
    if (!check(!mvm::app::makeNativePresentCompositionToken(frame, 8, 0, false, rejected),
               "generation 0を拒否しません"))
        return 1;
    if (!check(mvmNativePresentHookLayoutCompatible(
                   6, MVM_NATIVE_PRESENT_HOOK_RING_CAPACITY,
                   sizeof(MvmNativePresentCompositionToken), sizeof(MvmNativePresentRecord),
                   sizeof(MvmNativePresentOneShotSnapshot),
                   mvmNativePresentOneShotSnapshotLayoutSignature(),
                   mvmNativePresentHookLayoutSignature()),
               "ABI v6 layoutを受理しません") ||
        !check(!mvmNativePresentHookLayoutCompatible(
                   5, MVM_NATIVE_PRESENT_HOOK_RING_CAPACITY,
                   sizeof(MvmNativePresentCompositionToken), sizeof(MvmNativePresentRecord),
                   sizeof(MvmNativePresentOneShotSnapshot),
                   mvmNativePresentOneShotSnapshotLayoutSignature(),
                   mvmNativePresentHookLayoutSignature()),
               "v5 app / v6 Qt相当のABI mismatchを拒否しません") ||
        !check(!mvmNativePresentHookLayoutCompatible(
                   6, MVM_NATIVE_PRESENT_HOOK_RING_CAPACITY,
                   sizeof(MvmNativePresentCompositionToken) - 1, sizeof(MvmNativePresentRecord),
                   sizeof(MvmNativePresentOneShotSnapshot),
                   mvmNativePresentOneShotSnapshotLayoutSignature(),
                   mvmNativePresentHookLayoutSignature()),
               "composition token layout mutationを拒否しません") ||
        !check(!mvmNativePresentHookLayoutCompatible(
                   6, MVM_NATIVE_PRESENT_HOOK_RING_CAPACITY,
                   sizeof(MvmNativePresentCompositionToken), sizeof(MvmNativePresentRecord) + 1,
                   sizeof(MvmNativePresentOneShotSnapshot),
                   mvmNativePresentOneShotSnapshotLayoutSignature(),
                   mvmNativePresentHookLayoutSignature()),
               "native record size mutationを拒否しません") ||
        !check(!mvmNativePresentHookLayoutCompatible(
                   6, MVM_NATIVE_PRESENT_HOOK_RING_CAPACITY,
                   sizeof(MvmNativePresentCompositionToken), sizeof(MvmNativePresentRecord),
                   sizeof(MvmNativePresentOneShotSnapshot),
                   mvmNativePresentOneShotSnapshotLayoutSignature(),
                   mvmNativePresentHookLayoutSignature() ^ 1ULL),
               "同一sizeのoffset layout mutationを拒否しません") ||
        !check(!mvmNativePresentHookLayoutCompatible(
                   6, MVM_NATIVE_PRESENT_HOOK_RING_CAPACITY,
                   sizeof(MvmNativePresentCompositionToken), sizeof(MvmNativePresentRecord),
                   sizeof(MvmNativePresentOneShotSnapshot) - 1,
                   mvmNativePresentOneShotSnapshotLayoutSignature(),
                   mvmNativePresentHookLayoutSignature()),
               "one-shot snapshot size mutationを拒否しません") ||
        !check(!mvmNativePresentHookLayoutCompatible(
                   6, MVM_NATIVE_PRESENT_HOOK_RING_CAPACITY,
                   sizeof(MvmNativePresentCompositionToken), sizeof(MvmNativePresentRecord),
                   sizeof(MvmNativePresentOneShotSnapshot),
                   mvmNativePresentOneShotSnapshotLayoutSignature() ^ 1ULL,
                   mvmNativePresentHookLayoutSignature()),
               "one-shot snapshot layout mutationを拒否しません"))
        return 1;
    if (!check(!mvmNativePresentHookAbiVersionsCompatible(5, 6), "v5 app / v6 Qtを拒否しません") ||
        !check(!mvmNativePresentHookAbiVersionsCompatible(6, 5), "v6 app / v5 Qtを拒否しません"))
        return 1;

    MvmNativePresentOneShotSnapshot oneShot;
    oneShot.snapshotSize = sizeof(MvmNativePresentOneShotSnapshot);
    oneShot.layoutSignature = mvmNativePresentOneShotSnapshotLayoutSignature();
    oneShot.captureEpoch = 17;
    oneShot.captureActive = 1;
    oneShot.captureThreadId = 23;
    oneShot.callerThreadId = 23;
    oneShot.callerThreadExact = 1;
    oneShot.pendingTokenValid = 1;
    oneShot.pendingToken = token;
    oneShot.pendingReceiptValid = 1;
    oneShot.pendingReceipt = {
        31, 41, 0, 1, token.tokenSerial, token.intentOrdinal, token.intentOrdinalValid, 0};
    if (!check(mvmNativePresentOneShotSnapshotExact(oneShot, 17, 23),
               "exact one-shot snapshotを受理しません") ||
        !check(oneShot.pendingTokenValid == 1 && oneShot.pendingToken.tokenSerial == 5 &&
                   oneShot.pendingReceiptValid == 1 && oneShot.pendingReceipt.presentSerial == 31 &&
                   oneShot.pendingReceipt.tokenSerial == 5,
               "one-shot snapshotのraw identityが一致しません") ||
        !check(!mvmNativePresentOneShotSnapshotExact(oneShot, 18, 23),
               "capture epoch mismatchを拒否しません") ||
        !check(!mvmNativePresentOneShotSnapshotExact(oneShot, 17, 24),
               "thread mismatchを拒否しません"))
        return 1;
    oneShot.captureActive = 0;
    if (!check(!mvmNativePresentOneShotSnapshotExact(oneShot, 17, 23),
               "capture外snapshotを拒否しません"))
        return 1;
    oneShot.captureActive = 1;
    oneShot.callerThreadExact = 0;
    if (!check(!mvmNativePresentOneShotSnapshotExact(oneShot, 17, 23),
               "thread exactness欠損を拒否しません"))
        return 1;
    oneShot.callerThreadExact = 1;
    oneShot.layoutSignature ^= 1ULL;
    if (!check(!mvmNativePresentOneShotSnapshotExact(oneShot, 17, 23),
               "snapshot layout mismatchを拒否しません"))
        return 1;
    return 0;
}
