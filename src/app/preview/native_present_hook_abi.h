/*
 * F3-C0 diagnostic-only Qt D3D11 Present hook ABI。
 *
 * QtGui.dllとmvm_compositor_spikeの間で共有する固定POD境界である。
 * hot pathではallocation、mutex、I/O、loggingを行わない。
 */
#ifndef MVM_APP_PREVIEW_NATIVE_PRESENT_HOOK_ABI_H
#define MVM_APP_PREVIEW_NATIVE_PRESENT_HOOK_ABI_H

#include <cstdint>

inline constexpr std::uint32_t MVM_NATIVE_PRESENT_HOOK_ABI_VERSION = 3;
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
    std::uint32_t sourceCount = 0;
    std::uint32_t reserved = 0;
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
    MvmNativePresentCompositionToken token{};
};

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
    std::uint32_t enabled = 0;
    std::uint32_t recordCount = 0;
    std::uint32_t overflowCount = 0;
    std::uint32_t missingTokenCount = 0;
    std::uint32_t duplicateTokenCount = 0;
    std::uint32_t staleTokenCount = 0;
    std::uint32_t failedPresentCount = 0;
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

enum : std::uint32_t {
    MVM_SUBMISSION_MODE_CONTROL = 0,
    MVM_SUBMISSION_MODE_DWM_FLUSH_AFTER_PRESENT = 1,
    MVM_SUBMISSION_MODE_FRAME_LATENCY_1 = 2,
};

using MvmNativePresentHookAbiVersionFn = std::uint32_t (*)();
using MvmNativePresentHookBeginFn = int (*)(MvmNativePresentRing*);
using MvmNativePresentHookSetTokenFn = int (*)(const MvmNativePresentCompositionToken*);
using MvmNativePresentHookEndFn = int (*)();
using MvmDirtyPropagationBeginFn = std::uint64_t (*)();
using MvmDirtyPropagationStageFn = std::uint64_t (*)(std::uint32_t, std::uint64_t);

#endif
