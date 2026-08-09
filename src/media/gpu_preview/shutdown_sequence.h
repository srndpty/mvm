/*
 * docs/phase4-plan.md §7 で freeze 済みの shutdown 順を 1 箇所に固定する。
 *
 * 順序を controller の関数内へ手書きすると、実装と契約が静かにずれる。
 * 実際 Phase 4/B では audio decode worker を audio sink より先に stop していた。
 *
 * ここは std::function を並べるだけの薄い層である。大きな lifecycle
 * abstraction は作らない。Qt / audio / D3D11 へは依存しない。
 *
 * GPU retirement drain と device release は render thread の責務なので
 * このsequenceには含めない。ここが担うのは
 * 「teardown を要求するまでに何をどの順で終えるか」だけである。
 */
#ifndef MVM_GPU_PREVIEW_SHUTDOWN_SEQUENCE_H
#define MVM_GPU_PREVIEW_SHUTDOWN_SEQUENCE_H

#include <functional>
#include <vector>

namespace mvm::gpu {

enum class ShutdownStep {
    DisableSchedulers = 0,
    StopAudioSink,
    StopAudioDecodeWorker,
    StopVideoWorkerA,
    StopVideoWorkerB,
    DetachSharedWorkerRefs,
    RequestRenderTeardown,
};

const char* toString(ShutdownStep step);

struct ShutdownActions {
    std::function<void()> disableSchedulers;
    std::function<void()> stopAudioSink;         // endpoint stop + render thread join
    std::function<void()> stopAudioDecodeWorker; // stop + join
    std::function<void()> stopVideoWorkerA;      // stop + join
    std::function<void()> stopVideoWorkerB;      // stop + join
    std::function<void()> detachSharedWorkerRefs;
    std::function<void()> requestRenderTeardown;
    // teardown を要求する直前の確認。false なら要求しない (fail-closed)。
    std::function<bool()> allWorkersJoined;
};

struct ShutdownSequenceResult {
    std::vector<ShutdownStep> executed;
    bool joinVerified = false;
    bool renderTeardownRequested = false;
    long long orderViolationCount = 0;
};

// action が空なら「その step は対象が無い」とみなして skip し、executed へも
// 積まない。allWorkersJoined が無い場合は join を確認できていないので
// 順序違反として扱い、teardown を要求しない。
ShutdownSequenceResult runFrozenShutdownSequence(const ShutdownActions& actions);

} // namespace mvm::gpu

#endif
