#include "media/gpu_preview/shutdown_sequence.h"

namespace mvm::gpu {
namespace {

void runStep(const std::function<void()>& action, ShutdownStep step,
             ShutdownSequenceResult& result) {
    if (!action)
        return;
    action();
    result.executed.push_back(step);
}

} // namespace

const char* toString(ShutdownStep step) {
    switch (step) {
    case ShutdownStep::DisableSchedulers:
        return "DisableSchedulers";
    case ShutdownStep::StopAudioSink:
        return "StopAudioSink";
    case ShutdownStep::StopAudioDecodeWorker:
        return "StopAudioDecodeWorker";
    case ShutdownStep::StopVideoWorkerA:
        return "StopVideoWorkerA";
    case ShutdownStep::StopVideoWorkerB:
        return "StopVideoWorkerB";
    case ShutdownStep::DetachSharedWorkerRefs:
        return "DetachSharedWorkerRefs";
    case ShutdownStep::RequestRenderTeardown:
        return "RequestRenderTeardown";
    }
    return "Unknown";
}

ShutdownSequenceResult runFrozenShutdownSequence(const ShutdownActions& actions) {
    ShutdownSequenceResult result;
    runStep(actions.disableSchedulers, ShutdownStep::DisableSchedulers, result);
    runStep(actions.stopAudioSink, ShutdownStep::StopAudioSink, result);
    runStep(actions.stopAudioDecodeWorker, ShutdownStep::StopAudioDecodeWorker, result);
    runStep(actions.stopVideoWorkerA, ShutdownStep::StopVideoWorkerA, result);
    runStep(actions.stopVideoWorkerB, ShutdownStep::StopVideoWorkerB, result);
    runStep(actions.detachSharedWorkerRefs, ShutdownStep::DetachSharedWorkerRefs, result);

    // join を確認できないまま render teardown を要求しない。
    result.joinVerified = static_cast<bool>(actions.allWorkersJoined) && actions.allWorkersJoined();
    if (!result.joinVerified) {
        ++result.orderViolationCount;
        return result;
    }
    runStep(actions.requestRenderTeardown, ShutdownStep::RequestRenderTeardown, result);
    result.renderTeardownRequested =
        !result.executed.empty() && result.executed.back() == ShutdownStep::RequestRenderTeardown;
    return result;
}

} // namespace mvm::gpu
