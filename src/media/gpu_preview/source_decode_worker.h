#ifndef MVM_GPU_PREVIEW_SOURCE_DECODE_WORKER_H
#define MVM_GPU_PREVIEW_SOURCE_DECODE_WORKER_H

#include "media/gpu_preview/ffmpeg_d3d11_decoder.h"
#include "media/gpu_preview/source_frame_buffer.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace mvm::gpu {

struct SeekTicket {
    unsigned long long requestId = 0;
    long long targetFrame = -1;
    long long outputFrameNumber = -1;
};

enum class SeekRequestResult { Accepted, RejectedBusy, RejectedStopped, RejectedInvalid };

enum class SeekCompletionStatus { Completed, Failed, Stopped };

struct SeekCompletion {
    unsigned long long requestId = 0;
    long long targetFrame = -1;
    SeekCompletionStatus status = SeekCompletionStatus::Failed;
    long long requestQpc = 0;
    long long beginQpc = 0;
    long long decodeReadyQpc = 0;
    double decodeReadyMs = 0.0;
    SourceGeneration sourceGeneration{};
    ResourceEpoch resourceEpoch{};
    long long decodedFrameNumber = -1;
    std::string error;
};

enum class SeekWaitResult { Ready, Timeout, StaleTicket };

enum class SeekCompletionPublishResult {
    Published,
    RejectedNoOutstanding,
    RejectedAlreadyPublished,
    RejectedRequestMismatch,
    RejectedStoppedSuperseded
};

enum class SeekExecutionPhase {
    Idle,
    Queued,
    WaitingDecoderMutex,
    DecoderSeek,
    GenerationReset,
    RequestExactFrame,
    SubmitExactFrame,
    PublishingCompletion,
    Completed
};

struct SourceSeekMailboxSnapshot {
    bool stopped = true;
    bool outstanding = false;
    bool pending = false;
    bool completionReady = false;
    SeekTicket currentTicket;
    unsigned long long completionRequestId = 0;
};

struct SourceSeekDiagnosticSnapshot {
    SeekExecutionPhase phase = SeekExecutionPhase::Idle;
    unsigned long long requestId = 0;
    long long targetFrame = -1;
    long long phaseEnterQpc = 0;
    long long lastProgressQpc = 0;
    SourceSeekMailboxSnapshot mailbox;
    long long completionPublishRejectCount = 0;
    long long completionRequestMismatchCount = 0;
    long long completionStoppedSupersededCount = 0;
};

const char* toString(SeekCompletionPublishResult result);
const char* toString(SeekExecutionPhase phase);

// sourceごとに1件だけを受理するseek mailbox。decoderやGPUに依存しないため、
// request identity / busy / stale / stop wakeを決定論的に検査できる。
class SourceSeekMailbox {
public:
    void restart();
    SeekRequestResult request(long long frameNumber, long long requestQpc, SeekTicket& ticket,
                              std::string& err);
    SeekRequestResult request(long long sourceFrameNumber, long long outputFrameNumber,
                              long long requestQpc, SeekTicket& ticket, std::string& err);
    bool takePending(SeekTicket& ticket, long long& requestQpc);
    SeekCompletionPublishResult publish(const SeekCompletion& completion);
    SeekWaitResult wait(const SeekTicket& ticket, int timeoutMs, SeekCompletion& completion);
    void stop();
    bool hasPending() const;
    bool hasOutstanding() const;
    SourceSeekMailboxSnapshot snapshot() const;

private:
    mutable std::mutex mutex_;
    std::condition_variable completed_;
    bool stopped_ = true;
    bool outstanding_ = false;
    bool pending_ = false;
    unsigned long long nextRequestId_ = 0;
    SeekTicket ticket_;
    long long requestQpc_ = 0;
    bool completionReady_ = false;
    SeekCompletion completion_;
};

struct SourceDecoderSnapshot {
    bool open = false;
    bool running = false;
    bool playing = false;
    bool eof = false;
    bool joined = true;
    bool fatal = false;
    VideoStreamInfo info;
    AdapterInfo adapter;
    unsigned long long decodeDevicePointer = 0;
    SourceId sourceId{};
    SourceGeneration sourceGeneration{};
    ResourceEpoch resourceEpoch{};
    size_t bufferCapacity = 0;
    size_t bufferDepth = 0;
    long long decodedFrameCount = 0;
    long long decodeErrorCount = 0;
    long long softwareFrameRejectCount = 0;
    long long seekBackoffCount = 0;
    long long submittedCount = 0;
    long long queueFullCount = 0;
    long long backpressureWaitCount = 0;
    long long seekInterruptedSubmitWaitCount = 0;
    bool submitBackpressureWaitActive = false;
    long long staleGenerationRejectCount = 0;
    long long futureGenerationRejectCount = 0;
    long long invalidFrameRejectCount = 0;
    long long deviceMismatchCount = 0;
    std::string lastError;
};

// 1 sourceだけを駆動するworker。SharedD3D11Deviceは借用し、decoderとbufferは
// このworkerが所有する。他sourceやglobal PreviewFrameQueueへ触れるAPIを持たない。
class SourceDecodeWorker {
public:
    SourceDecodeWorker(SourceId sourceId, SharedD3D11Device& device, ReadbackCounters& counters,
                       size_t bufferCapacity = 3);
    ~SourceDecodeWorker();

    SourceDecodeWorker(const SourceDecodeWorker&) = delete;
    SourceDecodeWorker& operator=(const SourceDecodeWorker&) = delete;

    bool start(const std::string& utf8Path, std::string& err);
    void stop();
    void play();
    void pause();
    void stepForward();
    SeekRequestResult requestSeek(long long frameNumber, SeekTicket& ticket, std::string& err);
    SeekRequestResult requestSeek(long long sourceFrameNumber, long long outputFrameNumber,
                                  SeekTicket& ticket, std::string& err);
    SeekWaitResult waitSeek(const SeekTicket& ticket, int timeoutMs, SeekCompletion& completion);
    bool seekBlocking(long long frameNumber, double& decodeReadyMs, std::string& err);
    bool flushBlocking(std::string& err);

    SourceFrameBuffer& buffer() { return buffer_; }

    const SourceFrameBuffer& buffer() const { return buffer_; }

    SourceDecoderSnapshot snapshot() const;
    SourceSeekDiagnosticSnapshot seekDiagnosticSnapshot() const;

    bool running() const { return running_.load(std::memory_order_acquire); }

    bool playing() const { return playing_.load(std::memory_order_acquire); }

    bool eof() const { return eof_.load(std::memory_order_acquire); }

    bool joined() const { return joined_.load(std::memory_order_acquire); }

    void injectFatalForTest(const std::string& err) { noteFatal(err); }

    void injectEofForTest();

    void armAfterInitialSpaceBarrierForTest();
    bool waitAfterInitialSpaceBarrierForTest(int timeoutMs);
    void releaseAfterInitialSpaceBarrierForTest();

private:
    void run();
    SeekCompletion executeSeek(const SeekTicket& ticket, long long requestQpc);
    void refreshSnapshotLocked();
    bool submitWithBackpressure(const DecodedGpuFrame& frame, std::string& err);
    bool validateTextureDevice(const DecodedGpuFrame& frame, std::string& err);
    void noteFatal(const std::string& err);
    void setSeekPhase(SeekExecutionPhase phase, const SeekTicket& ticket);

    SourceId sourceId_{};
    SharedD3D11Device& device_;
    ReadbackCounters& counters_;
    SourceFrameBuffer buffer_;
    std::unique_ptr<FFmpegD3D11Decoder> decoder_;

    std::thread thread_;
    mutable std::mutex commandMutex_;
    mutable std::mutex decoderMutex_;
    mutable std::mutex snapshotMutex_;
    std::condition_variable wake_;
    std::mutex testBarrierMutex_;
    std::condition_variable testBarrierChanged_;
    bool testBarrierArmed_ = false;
    bool testBarrierReached_ = false;
    bool testBarrierReleased_ = false;
    SourceSeekMailbox seekMailbox_;
    SourceDecoderSnapshot snapshot_;
    int stepsPending_ = 0;
    bool startedOnce_ = false;

    std::atomic<bool> running_{false};
    std::atomic<bool> playing_{false};
    std::atomic<bool> eof_{false};
    std::atomic<bool> joined_{true};
    std::atomic<SeekExecutionPhase> seekPhase_{SeekExecutionPhase::Idle};
    std::atomic<unsigned long long> seekRequestId_{0};
    std::atomic<long long> seekTargetFrame_{-1};
    std::atomic<long long> sourceFrameAnchor_{0};
    std::atomic<long long> outputFrameAnchor_{0};
    std::atomic<long long> seekPhaseEnterQpc_{0};
    std::atomic<long long> seekLastProgressQpc_{0};
    std::atomic<long long> seekCompletionPublishRejectCount_{0};
    std::atomic<long long> seekCompletionRequestMismatchCount_{0};
    std::atomic<long long> seekCompletionStoppedSupersededCount_{0};
};

} // namespace mvm::gpu

#endif // MVM_GPU_PREVIEW_SOURCE_DECODE_WORKER_H
