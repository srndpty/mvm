#ifndef MVM_GPU_PREVIEW_TRANSITION_PROBE_H
#define MVM_GPU_PREVIEW_TRANSITION_PROBE_H

#include "media/gpu_preview/composed_frame.h"
#include "media/gpu_preview/gpu_completion.h"

#include <array>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace mvm::gpu {

enum class TransitionProbePoint { TL = 0, BR };

const char* transitionProbePointName(TransitionProbePoint point);

struct TransitionProbeRequest {
    long long boundary = -1;
    long long actualOutputFrame = -1;
    CompositionStateId compositionState;
    CompositionEpoch compositionEpoch;
    TransitionProbePoint point = TransitionProbePoint::TL;
    int x = 0;
    int y = 0;
    std::vector<SourceFrameIdentity> sources;
};

struct TransitionProbeResult {
    TransitionProbeRequest request;
    unsigned long long ticket = 0;
    unsigned long long completionSerial = 0;
    bool completionObserved = false;
    std::array<unsigned char, 4> rgba{};
};

struct TransitionProbeCounters {
    long long issuedCount = 0;
    long long completedCount = 0;
    long long notReadyPollCount = 0;
    long long renderThreadBlockingWaitCount = 0;
    long long untrackedSubmissionCount = 0;
    long long completionFailureCount = 0;
    long long retirementTimeoutCount = 0;
    size_t pendingAfterDrainCount = 0;
};

enum class TransitionProbeDrainStatus { Pending = 0, Complete, Failed };

// actual display frameから、まだprobeしていないboundaryだけを一度だけ選ぶ純粋state machine。
class TransitionProbeSelector {
public:
    TransitionProbeSelector() = default;
    explicit TransitionProbeSelector(std::vector<long long> boundaries);
    // workload開始前に一度だけ設定する。選択開始後の変更は拒否する。
    bool configure(std::vector<long long> boundaries);
    std::optional<long long> select(long long actualOutputFrame);

    size_t selectedCount() const { return selectedCount_; }

private:
    std::vector<long long> boundaries_;
    std::vector<bool> selected_;
    size_t selectedCount_ = 0;
};

// 1x1 copyだけをrender threadで発行し、完了後のDO_NOT_WAIT Mapを別地点で行う。
class AsyncTransitionProbeReadback {
public:
    ~AsyncTransitionProbeReadback();
    bool initialize(SharedD3D11Device& device, std::string& err);
    bool issue(ID3D11Texture2D* source, const TransitionProbeRequest& request,
               unsigned long long& ticket, std::string& err);
    bool poll(std::vector<TransitionProbeResult>& completed, std::string& err);
    bool beginDrain(int timeoutMs, std::string& err);
    TransitionProbeDrainStatus pollDrain(std::vector<TransitionProbeResult>& completed,
                                         std::string& err);
    void release();

    TransitionProbeCounters counters() const;

    // 決定論的test専用。次のpollを未完了として返し、Mapしない。
    void setTestDeferCompletionPollOnce() { testDeferCompletionPollOnce_ = true; }

private:
    struct Pending {
        TransitionProbeRequest request;
        unsigned long long ticket = 0;
        unsigned long long serial = 0;
        ID3D11Texture2D* staging = nullptr;
    };

    void releasePending();

    mutable std::mutex mutex_;
    SharedD3D11Device* shared_ = nullptr;
    GpuCompletionTracker completion_;
    std::deque<Pending> pending_;
    unsigned long long nextTicket_ = 1;
    TransitionProbeCounters counters_;
    bool testDeferCompletionPollOnce_ = false;
    bool drainStarted_ = false;
    long long drainDeadlineQpc_ = 0;
};

} // namespace mvm::gpu

#endif
