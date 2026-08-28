/*
 * F3-C0 diagnostic-only Qt D3D11 Present hook ABI。
 *
 * QtGui.dllとmvm_compositor_spikeの間で共有する固定POD境界である。
 * hot pathではallocation、mutex、I/O、loggingを行わない。
 */
#ifndef MVM_APP_PREVIEW_NATIVE_PRESENT_HOOK_ABI_H
#define MVM_APP_PREVIEW_NATIVE_PRESENT_HOOK_ABI_H

#include <cstddef>
#include <cstdint>

inline constexpr std::uint32_t MVM_NATIVE_PRESENT_HOOK_ABI_VERSION = 6;
inline constexpr std::uint32_t MVM_NATIVE_PRESENT_HOOK_RING_CAPACITY = 8192;
inline constexpr std::uint32_t MVM_NATIVE_PRESENT_HOOK_MAX_SOURCES = 2;

struct MvmNativePresentSourceIdentity {
    std::uint64_t sourceId = 0;
    std::uint64_t sourceGeneration = 0;
    std::uint64_t resourceEpoch = 0;
    std::int64_t frameNumber = -1;
};

struct MvmNativePresentCompositionToken {
    std::uint64_t tokenSerial = 0;
    std::uint64_t compositionEpoch = 0;
    std::uint64_t compositionState = 0;
    std::int64_t outputFrameNumber = -1;
    std::uint64_t intentOrdinal = 0;
    std::uint32_t intentOrdinalValid = 0;
    std::uint32_t sourceCount = 0;
    std::uint64_t propagationSerial = 0;
    MvmNativePresentSourceIdentity sources[MVM_NATIVE_PRESENT_HOOK_MAX_SOURCES]{};
};

struct MvmNativePresentRecord {
    std::uint64_t presentSerial = 0;
    std::uint64_t swapchainIdentity = 0;
    std::uint32_t threadId = 0;
    std::uint32_t syncInterval = 0;
    std::uint32_t presentFlags = 0;
    std::int32_t hresult = 0;
    std::int64_t presentEnterQpc = 0;
    std::int64_t presentReturnQpc = 0;
    std::uint32_t tokenPresent = 0;
    std::uint32_t reserved = 0;
    std::uint64_t propagationSerial = 0;
    std::uint64_t intentOrdinal = 0;
    std::uint32_t intentOrdinalValid = 0;
    std::uint32_t reservedIntent = 0;
    MvmNativePresentCompositionToken token{};
};

// actual Present returnからDirectConnectionのframeSwapped callbackへ渡すone-shot receipt。
// presentSerialはrecordと同じmint済み値であり、app側で推定しない。
struct MvmNativePresentFrameSwappedReceipt {
    std::uint64_t presentSerial = 0;
    std::uint64_t swapchainIdentity = 0;
    std::int32_t hresult = 0;
    std::uint32_t tokenPresent = 0;
    std::uint64_t tokenSerial = 0;
    std::uint64_t intentOrdinal = 0;
    std::uint32_t intentOrdinalValid = 0;
    std::uint32_t reserved = 0;
};

// B3-I5A。patched Qtの同一render-threadにあるone-shot stateをread-onlyで返す。
// abiVersion/snapshotSize/layoutSignatureはcallerが設定し、Qtが検証してからraw stateをcopyする。
struct MvmNativePresentOneShotSnapshot {
    std::uint32_t abiVersion = MVM_NATIVE_PRESENT_HOOK_ABI_VERSION;
    std::uint32_t snapshotSize = 0;
    std::uint64_t layoutSignature = 0;
    std::uint64_t captureEpoch = 0;
    std::uint32_t captureActive = 0;
    std::uint32_t captureThreadId = 0;
    std::uint32_t callerThreadId = 0;
    std::uint32_t callerThreadExact = 0;
    std::uint32_t pendingTokenValid = 0;
    std::uint32_t pendingReceiptValid = 0;
    std::uint32_t reserved[2]{};
    MvmNativePresentCompositionToken pendingToken{};
    MvmNativePresentFrameSwappedReceipt pendingReceipt{};
};

inline constexpr std::uint64_t mvmNativePresentLayoutMix(std::uint64_t signature,
                                                         std::uint64_t value) {
    return (signature ^ value) * 1099511628211ULL;
}

inline constexpr std::uint64_t mvmNativePresentSourceIdentitySemanticLayoutSignature() {
    std::uint64_t signature = 1469598103934665603ULL;
    signature = mvmNativePresentLayoutMix(signature, sizeof(MvmNativePresentSourceIdentity));
    signature = mvmNativePresentLayoutMix(signature, alignof(MvmNativePresentSourceIdentity));
    signature =
        mvmNativePresentLayoutMix(signature, offsetof(MvmNativePresentSourceIdentity, sourceId));
    signature = mvmNativePresentLayoutMix(
        signature, offsetof(MvmNativePresentSourceIdentity, sourceGeneration));
    signature = mvmNativePresentLayoutMix(signature,
                                          offsetof(MvmNativePresentSourceIdentity, resourceEpoch));
    return mvmNativePresentLayoutMix(signature,
                                     offsetof(MvmNativePresentSourceIdentity, frameNumber));
}

inline constexpr std::uint64_t mvmNativePresentCompositionTokenSemanticLayoutSignature() {
    std::uint64_t signature = 1469598103934665603ULL;
    signature = mvmNativePresentLayoutMix(signature, sizeof(MvmNativePresentCompositionToken));
    signature = mvmNativePresentLayoutMix(signature, alignof(MvmNativePresentCompositionToken));
    signature = mvmNativePresentLayoutMix(signature,
                                          mvmNativePresentSourceIdentitySemanticLayoutSignature());
    signature = mvmNativePresentLayoutMix(signature,
                                          offsetof(MvmNativePresentCompositionToken, tokenSerial));
    signature = mvmNativePresentLayoutMix(
        signature, offsetof(MvmNativePresentCompositionToken, compositionEpoch));
    signature = mvmNativePresentLayoutMix(
        signature, offsetof(MvmNativePresentCompositionToken, compositionState));
    signature = mvmNativePresentLayoutMix(
        signature, offsetof(MvmNativePresentCompositionToken, outputFrameNumber));
    signature = mvmNativePresentLayoutMix(
        signature, offsetof(MvmNativePresentCompositionToken, intentOrdinal));
    signature = mvmNativePresentLayoutMix(
        signature, offsetof(MvmNativePresentCompositionToken, intentOrdinalValid));
    signature = mvmNativePresentLayoutMix(signature,
                                          offsetof(MvmNativePresentCompositionToken, sourceCount));
    signature = mvmNativePresentLayoutMix(
        signature, offsetof(MvmNativePresentCompositionToken, propagationSerial));
    return mvmNativePresentLayoutMix(signature,
                                     offsetof(MvmNativePresentCompositionToken, sources));
}

inline constexpr std::uint64_t mvmNativePresentFrameSwappedReceiptSemanticLayoutSignature() {
    std::uint64_t signature = 1469598103934665603ULL;
    signature = mvmNativePresentLayoutMix(signature, sizeof(MvmNativePresentFrameSwappedReceipt));
    signature = mvmNativePresentLayoutMix(signature, alignof(MvmNativePresentFrameSwappedReceipt));
    signature = mvmNativePresentLayoutMix(
        signature, offsetof(MvmNativePresentFrameSwappedReceipt, presentSerial));
    signature = mvmNativePresentLayoutMix(
        signature, offsetof(MvmNativePresentFrameSwappedReceipt, swapchainIdentity));
    signature = mvmNativePresentLayoutMix(signature,
                                          offsetof(MvmNativePresentFrameSwappedReceipt, hresult));
    signature = mvmNativePresentLayoutMix(
        signature, offsetof(MvmNativePresentFrameSwappedReceipt, tokenPresent));
    signature = mvmNativePresentLayoutMix(
        signature, offsetof(MvmNativePresentFrameSwappedReceipt, tokenSerial));
    signature = mvmNativePresentLayoutMix(
        signature, offsetof(MvmNativePresentFrameSwappedReceipt, intentOrdinal));
    return mvmNativePresentLayoutMix(
        signature, offsetof(MvmNativePresentFrameSwappedReceipt, intentOrdinalValid));
}

inline constexpr std::uint64_t mvmNativePresentTokenRecordLayoutSignature() {
    std::uint64_t signature = 1469598103934665603ULL;
    signature = mvmNativePresentLayoutMix(signature, sizeof(MvmNativePresentCompositionToken));
    signature = mvmNativePresentLayoutMix(signature, alignof(MvmNativePresentCompositionToken));
    signature = mvmNativePresentLayoutMix(signature,
                                          offsetof(MvmNativePresentCompositionToken, tokenSerial));
    signature = mvmNativePresentLayoutMix(
        signature, offsetof(MvmNativePresentCompositionToken, intentOrdinal));
    signature = mvmNativePresentLayoutMix(
        signature, offsetof(MvmNativePresentCompositionToken, intentOrdinalValid));
    signature = mvmNativePresentLayoutMix(signature,
                                          offsetof(MvmNativePresentCompositionToken, sourceCount));
    signature = mvmNativePresentLayoutMix(
        signature, offsetof(MvmNativePresentCompositionToken, propagationSerial));
    signature =
        mvmNativePresentLayoutMix(signature, offsetof(MvmNativePresentCompositionToken, sources));
    signature = mvmNativePresentLayoutMix(signature, sizeof(MvmNativePresentRecord));
    signature = mvmNativePresentLayoutMix(signature, alignof(MvmNativePresentRecord));
    signature =
        mvmNativePresentLayoutMix(signature, offsetof(MvmNativePresentRecord, presentSerial));
    signature =
        mvmNativePresentLayoutMix(signature, offsetof(MvmNativePresentRecord, tokenPresent));
    signature =
        mvmNativePresentLayoutMix(signature, offsetof(MvmNativePresentRecord, propagationSerial));
    signature =
        mvmNativePresentLayoutMix(signature, offsetof(MvmNativePresentRecord, intentOrdinal));
    signature =
        mvmNativePresentLayoutMix(signature, offsetof(MvmNativePresentRecord, intentOrdinalValid));
    return mvmNativePresentLayoutMix(signature, offsetof(MvmNativePresentRecord, token));
}

inline constexpr std::uint64_t mvmNativePresentOneShotSnapshotLayoutSignature() {
    std::uint64_t signature = 1469598103934665603ULL;
    signature = mvmNativePresentLayoutMix(signature, sizeof(MvmNativePresentOneShotSnapshot));
    signature = mvmNativePresentLayoutMix(signature, alignof(MvmNativePresentOneShotSnapshot));
    signature = mvmNativePresentLayoutMix(
        signature, mvmNativePresentCompositionTokenSemanticLayoutSignature());
    signature = mvmNativePresentLayoutMix(
        signature, mvmNativePresentFrameSwappedReceiptSemanticLayoutSignature());
    signature =
        mvmNativePresentLayoutMix(signature, offsetof(MvmNativePresentOneShotSnapshot, abiVersion));
    signature = mvmNativePresentLayoutMix(signature,
                                          offsetof(MvmNativePresentOneShotSnapshot, snapshotSize));
    signature = mvmNativePresentLayoutMix(
        signature, offsetof(MvmNativePresentOneShotSnapshot, layoutSignature));
    signature = mvmNativePresentLayoutMix(signature,
                                          offsetof(MvmNativePresentOneShotSnapshot, captureEpoch));
    signature = mvmNativePresentLayoutMix(signature,
                                          offsetof(MvmNativePresentOneShotSnapshot, captureActive));
    signature = mvmNativePresentLayoutMix(
        signature, offsetof(MvmNativePresentOneShotSnapshot, captureThreadId));
    signature = mvmNativePresentLayoutMix(
        signature, offsetof(MvmNativePresentOneShotSnapshot, callerThreadId));
    signature = mvmNativePresentLayoutMix(
        signature, offsetof(MvmNativePresentOneShotSnapshot, callerThreadExact));
    signature = mvmNativePresentLayoutMix(
        signature, offsetof(MvmNativePresentOneShotSnapshot, pendingTokenValid));
    signature = mvmNativePresentLayoutMix(
        signature, offsetof(MvmNativePresentOneShotSnapshot, pendingReceiptValid));
    signature = mvmNativePresentLayoutMix(signature,
                                          offsetof(MvmNativePresentOneShotSnapshot, pendingToken));
    return mvmNativePresentLayoutMix(signature,
                                     offsetof(MvmNativePresentOneShotSnapshot, pendingReceipt));
}

inline constexpr bool mvmNativePresentOneShotSnapshotLayoutCompatible(
    std::uint32_t abiVersion, std::uint32_t snapshotSize, std::uint64_t layoutSignature) {
    return abiVersion == MVM_NATIVE_PRESENT_HOOK_ABI_VERSION &&
           snapshotSize == sizeof(MvmNativePresentOneShotSnapshot) &&
           layoutSignature == mvmNativePresentOneShotSnapshotLayoutSignature();
}

enum MvmDirtyPropagationStage : std::uint32_t {
    MVM_DIRTY_STAGE_RENDERER_UPDATE = 0,
    MVM_DIRTY_STAGE_NODE_SCHEDULE_UPDATE,
    MVM_DIRTY_STAGE_WINDOW_UPDATE,
    MVM_DIRTY_STAGE_NODE_RENDER,
    MVM_DIRTY_STAGE_COMPOSITOR_RENDER,
    MVM_DIRTY_STAGE_COMPOSITION_TOKEN,
    MVM_DIRTY_STAGE_DIRTY_MATERIAL,
    MVM_DIRTY_STAGE_TEXTURE_CHANGED,
    MVM_DIRTY_STAGE_QSG_MAIN_RENDER,
    MVM_DIRTY_STAGE_RHI_END_FRAME,
    MVM_DIRTY_STAGE_SUCCESSFUL_PRESENT,
    MVM_DIRTY_STAGE_TARGET_PIXEL_TOGGLE,
    MVM_DIRTY_STAGE_COUNT,
};

struct MvmDirtyPropagationRecord {
    std::uint64_t propagationSerial = 0;
    std::uint64_t compositionTokenSerial = 0;
    std::uint64_t presentSerial = 0;
    std::int64_t outputFrameNumber = -1;
    std::int64_t stageQpc[MVM_DIRTY_STAGE_COUNT]{};
};

struct MvmNativePresentRing {
    std::uint32_t abiVersion = MVM_NATIVE_PRESENT_HOOK_ABI_VERSION;
    std::uint32_t capacity = MVM_NATIVE_PRESENT_HOOK_RING_CAPACITY;
    std::uint32_t compositionTokenSize = sizeof(MvmNativePresentCompositionToken);
    std::uint32_t presentRecordSize = sizeof(MvmNativePresentRecord);
    // MvmNativePresentRing自身の定義完了後にapp側が設定する。0のままならQtは拒否する。
    std::uint64_t layoutSignature = 0;
    std::uint32_t oneShotSnapshotSize = sizeof(MvmNativePresentOneShotSnapshot);
    std::uint32_t captureThreadId = 0;
    std::uint64_t oneShotSnapshotLayoutSignature = 0;
    std::uint64_t captureEpoch = 0;
    std::uint32_t threadMismatchCount = 0;
    std::uint32_t enabled = 0;
    std::uint32_t recordCount = 0;
    std::uint32_t overflowCount = 0;
    std::uint32_t missingTokenCount = 0;
    std::uint32_t duplicateTokenCount = 0;
    std::uint32_t staleTokenCount = 0;
    std::uint32_t failedPresentCount = 0;
    std::uint32_t missingFrameSwappedReceiptCount = 0;
    std::uint32_t duplicateFrameSwappedReceiptCount = 0;
    std::uint32_t staleFrameSwappedReceiptCount = 0;
    std::uint32_t authorityFailure = 0;
    std::uint32_t submissionMode = 0;
    std::uint32_t configuredMaximumFrameLatency = 0;
    std::uint32_t swapchainMaximumFrameLatency = 0;
    std::uint32_t frameLatencyWaitableObjectAvailable = 0;
    std::uint32_t dwmFlushCallCount = 0;
    std::uint32_t dwmFlushFailureCount = 0;
    std::uint32_t dirtyPropagationRecordCount = 0;
    std::uint32_t dirtyPropagationOverflowCount = 0;
    std::uint32_t dirtyPropagationDuplicateStageCount = 0;
    std::uint32_t reserved = 0;
    std::uint64_t dirtyPropagationStageCounts[MVM_DIRTY_STAGE_COUNT]{};
    MvmDirtyPropagationRecord dirtyPropagationRecords[MVM_NATIVE_PRESENT_HOOK_RING_CAPACITY]{};
    MvmNativePresentRecord records[MVM_NATIVE_PRESENT_HOOK_RING_CAPACITY]{};
};

inline constexpr std::uint64_t mvmNativePresentHookLayoutSignature() {
    std::uint64_t signature = mvmNativePresentTokenRecordLayoutSignature();
    signature = mvmNativePresentLayoutMix(signature, sizeof(MvmNativePresentRing));
    signature = mvmNativePresentLayoutMix(signature, alignof(MvmNativePresentRing));
    signature = mvmNativePresentLayoutMix(signature, offsetof(MvmNativePresentRing, abiVersion));
    signature = mvmNativePresentLayoutMix(signature, offsetof(MvmNativePresentRing, capacity));
    signature =
        mvmNativePresentLayoutMix(signature, offsetof(MvmNativePresentRing, compositionTokenSize));
    signature =
        mvmNativePresentLayoutMix(signature, offsetof(MvmNativePresentRing, presentRecordSize));
    signature =
        mvmNativePresentLayoutMix(signature, offsetof(MvmNativePresentRing, layoutSignature));
    signature =
        mvmNativePresentLayoutMix(signature, offsetof(MvmNativePresentRing, oneShotSnapshotSize));
    signature = mvmNativePresentLayoutMix(
        signature, offsetof(MvmNativePresentRing, oneShotSnapshotLayoutSignature));
    signature = mvmNativePresentLayoutMix(signature, offsetof(MvmNativePresentRing, captureEpoch));
    signature =
        mvmNativePresentLayoutMix(signature, offsetof(MvmNativePresentRing, captureThreadId));
    signature =
        mvmNativePresentLayoutMix(signature, offsetof(MvmNativePresentRing, threadMismatchCount));
    signature = mvmNativePresentLayoutMix(signature, offsetof(MvmNativePresentRing, enabled));
    signature = mvmNativePresentLayoutMix(signature, offsetof(MvmNativePresentRing, recordCount));
    signature = mvmNativePresentLayoutMix(signature, offsetof(MvmNativePresentRing, overflowCount));
    signature =
        mvmNativePresentLayoutMix(signature, offsetof(MvmNativePresentRing, missingTokenCount));
    signature =
        mvmNativePresentLayoutMix(signature, offsetof(MvmNativePresentRing, duplicateTokenCount));
    signature =
        mvmNativePresentLayoutMix(signature, offsetof(MvmNativePresentRing, staleTokenCount));
    signature =
        mvmNativePresentLayoutMix(signature, offsetof(MvmNativePresentRing, failedPresentCount));
    signature = mvmNativePresentLayoutMix(
        signature, offsetof(MvmNativePresentRing, missingFrameSwappedReceiptCount));
    signature = mvmNativePresentLayoutMix(
        signature, offsetof(MvmNativePresentRing, duplicateFrameSwappedReceiptCount));
    signature = mvmNativePresentLayoutMix(
        signature, offsetof(MvmNativePresentRing, staleFrameSwappedReceiptCount));
    signature =
        mvmNativePresentLayoutMix(signature, offsetof(MvmNativePresentRing, authorityFailure));
    signature = mvmNativePresentLayoutMix(
        signature, offsetof(MvmNativePresentRing, dirtyPropagationStageCounts));
    signature = mvmNativePresentLayoutMix(signature,
                                          offsetof(MvmNativePresentRing, dirtyPropagationRecords));
    return mvmNativePresentLayoutMix(signature, offsetof(MvmNativePresentRing, records));
}

inline constexpr bool mvmNativePresentHookAbiVersionsCompatible(std::uint32_t appVersion,
                                                                std::uint32_t qtVersion) {
    return appVersion == MVM_NATIVE_PRESENT_HOOK_ABI_VERSION &&
           qtVersion == MVM_NATIVE_PRESENT_HOOK_ABI_VERSION;
}

inline constexpr bool mvmNativePresentHookLayoutCompatible(
    std::uint32_t abiVersion, std::uint32_t capacity, std::uint32_t compositionTokenSize,
    std::uint32_t presentRecordSize, std::uint32_t oneShotSnapshotSize,
    std::uint64_t oneShotSnapshotLayoutSignature, std::uint64_t layoutSignature) {
    return mvmNativePresentHookAbiVersionsCompatible(abiVersion,
                                                     MVM_NATIVE_PRESENT_HOOK_ABI_VERSION) &&
           capacity == MVM_NATIVE_PRESENT_HOOK_RING_CAPACITY &&
           compositionTokenSize == sizeof(MvmNativePresentCompositionToken) &&
           presentRecordSize == sizeof(MvmNativePresentRecord) &&
           oneShotSnapshotSize == sizeof(MvmNativePresentOneShotSnapshot) &&
           oneShotSnapshotLayoutSignature == mvmNativePresentOneShotSnapshotLayoutSignature() &&
           layoutSignature == mvmNativePresentHookLayoutSignature();
}

inline constexpr bool
mvmNativePresentOneShotSnapshotExact(const MvmNativePresentOneShotSnapshot& snapshot,
                                     std::uint64_t expectedCaptureEpoch,
                                     std::uint32_t expectedThreadId) {
    return mvmNativePresentOneShotSnapshotLayoutCompatible(
               snapshot.abiVersion, snapshot.snapshotSize, snapshot.layoutSignature) &&
           expectedCaptureEpoch != 0 && snapshot.captureEpoch == expectedCaptureEpoch &&
           snapshot.captureActive != 0 && expectedThreadId != 0 &&
           snapshot.captureThreadId == expectedThreadId &&
           snapshot.callerThreadId == expectedThreadId && snapshot.callerThreadExact != 0;
}

enum : std::uint32_t {
    MVM_SUBMISSION_MODE_CONTROL = 0,
    MVM_SUBMISSION_MODE_DWM_FLUSH_AFTER_PRESENT = 1,
    MVM_SUBMISSION_MODE_FRAME_LATENCY_1 = 2,
};

using MvmNativePresentHookAbiVersionFn = std::uint32_t (*)();
using MvmNativePresentHookBeginFn = int (*)(MvmNativePresentRing*);
using MvmNativePresentHookSetTokenFn = int (*)(const MvmNativePresentCompositionToken*);
using MvmNativePresentHookEndFn = int (*)();
using MvmNativePresentHookTakeFrameSwappedReceiptFn = int (*)(MvmNativePresentFrameSwappedReceipt*);
using MvmNativePresentHookOneShotSnapshotFn = int (*)(std::uint64_t,
                                                      MvmNativePresentOneShotSnapshot*);
using MvmDirtyPropagationBeginFn = std::uint64_t (*)();
using MvmDirtyPropagationStageFn = std::uint64_t (*)(std::uint32_t, std::uint64_t);

enum MvmNativePresentOneShotSnapshotResult : int {
    MVM_ONE_SHOT_SNAPSHOT_OK = 1,
    MVM_ONE_SHOT_SNAPSHOT_CAPTURE_INACTIVE = 0,
    MVM_ONE_SHOT_SNAPSHOT_INVALID_ARGUMENT = -1,
    MVM_ONE_SHOT_SNAPSHOT_ABI_LAYOUT_MISMATCH = -2,
    MVM_ONE_SHOT_SNAPSHOT_EPOCH_MISMATCH = -3,
    MVM_ONE_SHOT_SNAPSHOT_THREAD_MISMATCH = -4,
};

#endif
