#include "app/preview/native_present_hook.h"

#include <algorithm>
#include <bit>
#include <windows.h>

namespace mvm::app {
namespace {

template<typename Function>
Function resolve(HMODULE module, const char* name) {
    static_assert(sizeof(Function) == sizeof(FARPROC));
    return std::bit_cast<Function>(GetProcAddress(module, name));
}

} // namespace

NativePresentHook::NativePresentHook() = default;

NativePresentHook::~NativePresentHook() = default;

bool NativePresentHook::load(std::string& error) {
    if (available_)
        return true;
    const HMODULE qtGui = GetModuleHandleW(L"Qt6Gui.dll");
    if (!qtGui) {
        error = "loaded Qt6Gui.dllを取得できません";
        return false;
    }
    abiVersion_ =
        resolve<MvmNativePresentHookAbiVersionFn>(qtGui, "mvm_qt_d3d11_present_hook_abi_version");
    begin_ = resolve<MvmNativePresentHookBeginFn>(qtGui, "mvm_qt_d3d11_present_hook_begin");
    setToken_ =
        resolve<MvmNativePresentHookSetTokenFn>(qtGui, "mvm_qt_d3d11_present_hook_set_token");
    end_ = resolve<MvmNativePresentHookEndFn>(qtGui, "mvm_qt_d3d11_present_hook_end");
    dirtyBegin_ = resolve<MvmDirtyPropagationBeginFn>(qtGui, "mvm_qt_dirty_propagation_begin");
    dirtyStage_ = resolve<MvmDirtyPropagationStageFn>(qtGui, "mvm_qt_dirty_propagation_stage");
    if (!abiVersion_ || !begin_ || !setToken_ || !end_ || !dirtyBegin_ || !dirtyStage_) {
        error = "Qt6Gui.dllにF3-C0 native Present hook exportがありません";
        return false;
    }
    observedQtAbiVersion_ = abiVersion_();
    if (!mvmNativePresentHookAbiVersionsCompatible(MVM_NATIVE_PRESENT_HOOK_ABI_VERSION,
                                                   observedQtAbiVersion_)) {
        error = "Qt6Gui.dllのnative Present hook ABI versionが一致しません";
        return false;
    }
    ring_ = std::make_unique<MvmNativePresentRing>();
    ring_->layoutSignature = mvmNativePresentHookLayoutSignature();
    available_ = true;
    return true;
}

bool NativePresentHook::beginCapture(std::string& error) {
    layoutHandshakeAccepted_ = false;
    if (!available_ || !ring_ || captureStarted_ || !begin_ || begin_(ring_.get()) == 0) {
        error = "native Present hook captureを開始できません";
        return false;
    }
    layoutHandshakeAccepted_ = true;
    captureStarted_ = true;
    captureStopped_ = false;
    return true;
}

bool NativePresentHook::setCompositionToken(const MvmNativePresentCompositionToken& token) {
    return captureStarted_ && !captureStopped_ && setToken_ && setToken_(&token) != 0;
}

std::uint64_t NativePresentHook::recordDirtyPropagationStage(MvmDirtyPropagationStage stage,
                                                             std::uint64_t serial) {
    return captureStarted_ && !captureStopped_ && dirtyStage_
               ? dirtyStage_(static_cast<std::uint32_t>(stage), serial)
               : 0;
}

bool NativePresentHook::endCapture(std::string& error) {
    if (!captureStarted_ || captureStopped_ || !end_ || end_() == 0) {
        error = "native Present hook captureを停止できません";
        return false;
    }
    captureStopped_ = true;
    return true;
}

bool NativePresentHook::authorityValid() const {
    return ring_ && captureStarted_ && captureStopped_ && ring_->authorityFailure == 0 &&
           ring_->overflowCount == 0 && ring_->missingTokenCount == 0 &&
           ring_->duplicateTokenCount == 0 && ring_->staleTokenCount == 0 &&
           ring_->failedPresentCount == 0;
}

NativePresentHookSnapshot NativePresentHook::snapshot() const {
    NativePresentHookSnapshot result;
    result.available = available_;
    result.observedQtAbiVersion = observedQtAbiVersion_;
    result.layoutHandshakeAccepted = layoutHandshakeAccepted_;
    result.captureStarted = captureStarted_;
    result.captureStopped = captureStopped_;
    if (ring_) {
        result.overflowCount = ring_->overflowCount;
        result.missingTokenCount = ring_->missingTokenCount;
        result.duplicateTokenCount = ring_->duplicateTokenCount;
        result.staleTokenCount = ring_->staleTokenCount;
        result.failedPresentCount = ring_->failedPresentCount;
        result.submissionMode = ring_->submissionMode;
        result.configuredMaximumFrameLatency = ring_->configuredMaximumFrameLatency;
        result.swapchainMaximumFrameLatency = ring_->swapchainMaximumFrameLatency;
        result.frameLatencyWaitableObjectAvailable =
            ring_->frameLatencyWaitableObjectAvailable != 0;
        result.dwmFlushCallCount = ring_->dwmFlushCallCount;
        result.dwmFlushFailureCount = ring_->dwmFlushFailureCount;
        result.authorityFailure = ring_->authorityFailure != 0;
        result.dirtyPropagationOverflowCount = ring_->dirtyPropagationOverflowCount;
        result.dirtyPropagationDuplicateStageCount = ring_->dirtyPropagationDuplicateStageCount;
        std::copy(std::begin(ring_->dirtyPropagationStageCounts),
                  std::end(ring_->dirtyPropagationStageCounts),
                  std::begin(result.dirtyPropagationStageCounts));
        const auto dirtyCount = std::min(ring_->dirtyPropagationRecordCount, ring_->capacity);
        result.dirtyPropagationRecords.assign(ring_->dirtyPropagationRecords,
                                              ring_->dirtyPropagationRecords + dirtyCount);
        const auto count = std::min(ring_->recordCount, ring_->capacity);
        result.records.assign(ring_->records, ring_->records + count);
    }
    return result;
}

std::uint64_t NativePresentHook::latestSwapchainIdentity() const {
    if (!available_ || ring_ == nullptr)
        return 0;
    const auto count = std::min(ring_->recordCount, ring_->capacity);
    if (count == 0)
        return 0;
    return ring_->records[count - 1].swapchainIdentity;
}

bool makeNativePresentCompositionToken(const gpu::ComposedFrame& frame, std::uint64_t tokenSerial,
                                       std::uint64_t intentOrdinal, bool intentOrdinalValid,
                                       MvmNativePresentCompositionToken& token) {
    if (tokenSerial == 0 || frame.outputFrameNumber < 0 || frame.layers.empty() ||
        frame.layers.size() > MVM_NATIVE_PRESENT_HOOK_MAX_SOURCES)
        return false;
    token = {};
    token.tokenSerial = tokenSerial;
    token.compositionEpoch = frame.compositionEpoch.value;
    token.compositionState = frame.compositionState.value;
    token.outputFrameNumber = frame.outputFrameNumber;
    token.intentOrdinal = intentOrdinal;
    token.intentOrdinalValid = intentOrdinalValid ? 1U : 0U;
    token.sourceCount = static_cast<std::uint32_t>(frame.layers.size());
    for (std::size_t index = 0; index < frame.layers.size(); ++index) {
        const auto identity = gpu::identityOf(frame.layers[index].frame);
        if (identity.sourceId.value == 0 || identity.sourceGeneration.value == 0 ||
            identity.resourceEpoch.value == 0 || identity.frameNumber < 0)
            return false;
        token.sources[index] = {identity.sourceId.value, identity.sourceGeneration.value,
                                identity.resourceEpoch.value, identity.frameNumber};
    }
    std::sort(token.sources, token.sources + token.sourceCount,
              [](const auto& left, const auto& right) { return left.sourceId < right.sourceId; });
    return true;
}

} // namespace mvm::app
