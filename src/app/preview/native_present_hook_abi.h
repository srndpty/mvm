/*
 * F3-C0 diagnostic-only Qt D3D11 Present hook ABI。
 *
 * QtGui.dllとmvm_compositor_spikeの間で共有する固定POD境界である。
 * hot pathではallocation、mutex、I/O、loggingを行わない。
 */
#ifndef MVM_APP_PREVIEW_NATIVE_PRESENT_HOOK_ABI_H
#define MVM_APP_PREVIEW_NATIVE_PRESENT_HOOK_ABI_H

#include <cstdint>

inline constexpr std::uint32_t MVM_NATIVE_PRESENT_HOOK_ABI_VERSION = 1;
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
    MvmNativePresentCompositionToken token{};
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
    std::uint32_t reserved[6]{};
    MvmNativePresentRecord records[MVM_NATIVE_PRESENT_HOOK_RING_CAPACITY]{};
};

using MvmNativePresentHookAbiVersionFn = std::uint32_t (*)();
using MvmNativePresentHookBeginFn = int (*)(MvmNativePresentRing*);
using MvmNativePresentHookSetTokenFn = int (*)(const MvmNativePresentCompositionToken*);
using MvmNativePresentHookEndFn = int (*)();

#endif
