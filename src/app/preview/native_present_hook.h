#ifndef MVM_APP_PREVIEW_NATIVE_PRESENT_HOOK_H
#define MVM_APP_PREVIEW_NATIVE_PRESENT_HOOK_H

#include "app/preview/native_present_hook_abi.h"
#include "media/gpu_preview/composed_frame.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mvm::app {

struct NativePresentHookSnapshot {
    bool available = false;
    bool captureStarted = false;
    bool captureStopped = false;
    std::uint32_t overflowCount = 0;
    std::uint32_t missingTokenCount = 0;
    std::uint32_t duplicateTokenCount = 0;
    std::uint32_t staleTokenCount = 0;
    std::uint32_t failedPresentCount = 0;
    std::uint32_t submissionMode = 0;
    std::uint32_t configuredMaximumFrameLatency = 0;
    std::uint32_t swapchainMaximumFrameLatency = 0;
    bool frameLatencyWaitableObjectAvailable = false;
    std::uint32_t dwmFlushCallCount = 0;
    std::uint32_t dwmFlushFailureCount = 0;
    bool authorityFailure = false;
    std::uint32_t dirtyPropagationOverflowCount = 0;
    std::uint32_t dirtyPropagationDuplicateStageCount = 0;
    std::uint64_t dirtyPropagationStageCounts[MVM_DIRTY_STAGE_COUNT]{};
    std::vector<MvmDirtyPropagationRecord> dirtyPropagationRecords;
    std::vector<MvmNativePresentRecord> records;
};

class NativePresentHook {
public:
    NativePresentHook();
    ~NativePresentHook();

    NativePresentHook(const NativePresentHook&) = delete;
    NativePresentHook& operator=(const NativePresentHook&) = delete;

    bool load(std::string& error);
    bool beginCapture(std::string& error);
    bool setCompositionToken(const MvmNativePresentCompositionToken& token);
    std::uint64_t recordDirtyPropagationStage(MvmDirtyPropagationStage stage,
                                              std::uint64_t serial = 0);
    bool endCapture(std::string& error);
    bool authorityValid() const;
    // patched Qtが記録した実IDXGISwapChainポインタ。0はrecord未取得。
    std::uint64_t latestSwapchainIdentity() const;
    NativePresentHookSnapshot snapshot() const;

private:
    MvmNativePresentHookAbiVersionFn abiVersion_ = nullptr;
    MvmNativePresentHookBeginFn begin_ = nullptr;
    MvmNativePresentHookSetTokenFn setToken_ = nullptr;
    MvmNativePresentHookEndFn end_ = nullptr;
    MvmDirtyPropagationBeginFn dirtyBegin_ = nullptr;
    MvmDirtyPropagationStageFn dirtyStage_ = nullptr;
    std::unique_ptr<MvmNativePresentRing> ring_;
    bool available_ = false;
    bool captureStarted_ = false;
    bool captureStopped_ = false;
};

bool makeNativePresentCompositionToken(const gpu::ComposedFrame& frame, std::uint64_t tokenSerial,
                                       std::uint64_t intentOrdinal, bool intentOrdinalValid,
                                       MvmNativePresentCompositionToken& token);

} // namespace mvm::app

#endif
