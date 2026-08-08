/*
 * mvm Phase 1 / P1.2-finalize - preview shutdown の単一 state machine
 *
 * resource の解放順を Qt object や main のローカル変数の破棄順に委ねない。
 * GUI thread は decode の join を記録してから render teardown を要求し、
 * render thread は mayTeardown() が true の場合だけ resource を解放する。
 */
#ifndef MVM_GPU_PREVIEW_LIFECYCLE_H
#define MVM_GPU_PREVIEW_LIFECYCLE_H

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <set>
#include <string>

namespace mvm::gpu {

enum class ShutdownState {
    Running = 0,
    DecodeStopRequested,
    DecodeStopped,
    RenderTeardownRequested,
    RenderTornDown,
    FinalReportWritten,
    Exit,
};

const char* toString(ShutdownState state);

struct ShutdownReport {
    unsigned long long submittedSerial = 0;
    unsigned long long completedSerial = 0;
    std::string backend;
    bool completionFatal = false;
    std::string completionFatalReason;
    long long untrackedCount = 0;
    long long pollFailureCount = 0;
    long long retirementDepthBeforeDrain = 0;
    long long retirementDepthAfterDrain = 0;
    long long retirementTimeoutCount = 0;
    long long payloadsReleasedBeforeCompletion = 0;
    double drainElapsedMs = 0.0;
    bool teardownSuccess = false;
    long long lifecycleOrderViolationCount = 0;
};

class LifecycleCoordinator {
public:
    bool requestDecodeStop(const std::string& reason, bool fatal);
    bool noteDecodeStopped();
    bool requestRenderTeardown();
    bool mayTeardown() const;
    bool noteRenderTornDown(ShutdownReport report);
    bool waitForRenderTornDown(int timeoutMs, ShutdownReport& report) const;
    bool noteFinalReportWritten();
    bool noteExit();

    // renderer destructor 専用。正常経路として数えず、順序違反と fatal shutdown を
    // 記録する。worker の join を確認できない異常経路である。
    void noteDestructorFallback();

    ShutdownState state() const;
    std::string reason() const;
    bool fatal() const;
    long long orderViolationCount() const;
    ShutdownReport report() const;

private:
    bool violationLocked();

    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    ShutdownState state_ = ShutdownState::Running;
    std::string reason_;
    bool fatal_ = false;
    long long violations_ = 0;
    ShutdownReport report_;
};

// completion fatal の最初の 1 件で閉じ、その後の draw/submission を機械的に拒否する。
class RenderFatalGate {
public:
    bool tryBeginDraw() {
        if (fatal_.load(std::memory_order_acquire))
            return false;
        drawCount_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void noteSubmission() { submissionCount_.fetch_add(1, std::memory_order_relaxed); }

    bool noteFatal() { return !fatal_.exchange(true, std::memory_order_acq_rel); }

    bool fatal() const { return fatal_.load(std::memory_order_acquire); }

    long long drawCount() const { return drawCount_.load(std::memory_order_relaxed); }

    long long submissionCount() const { return submissionCount_.load(std::memory_order_relaxed); }

private:
    std::atomic<bool> fatal_{false};
    std::atomic<long long> drawCount_{0};
    std::atomic<long long> submissionCount_{0};
};

// 複数 worker の shutdown を sleep なしで合流させる。全 worker の join を
// 記録するまで render teardown を要求してはいけない。
class WorkerJoinBarrier {
public:
    explicit WorkerJoinBarrier(size_t required) : required_(required) {}

    bool noteJoined(size_t workerIndex);
    bool allJoined() const;
    size_t joinedCount() const;

private:
    mutable std::mutex mutex_;
    size_t required_ = 0;
    std::set<size_t> joined_;
};

} // namespace mvm::gpu

#endif // MVM_GPU_PREVIEW_LIFECYCLE_H
