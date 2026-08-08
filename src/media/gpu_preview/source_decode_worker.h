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
    bool seekBlocking(long long frameNumber, double& decodeReadyMs, std::string& err);
    bool flushBlocking(std::string& err);

    SourceFrameBuffer& buffer() { return buffer_; }

    const SourceFrameBuffer& buffer() const { return buffer_; }

    SourceDecoderSnapshot snapshot() const;

    bool running() const { return running_.load(std::memory_order_acquire); }

    bool playing() const { return playing_.load(std::memory_order_acquire); }

    bool eof() const { return eof_.load(std::memory_order_acquire); }

    bool joined() const { return joined_.load(std::memory_order_acquire); }

private:
    void run();
    void refreshSnapshotLocked();
    bool submitWithBackpressure(const DecodedGpuFrame& frame, std::string& err);
    bool validateTextureDevice(const DecodedGpuFrame& frame, std::string& err);
    void noteFatal(const std::string& err);

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
    SourceDecoderSnapshot snapshot_;
    int stepsPending_ = 0;
    bool startedOnce_ = false;

    std::atomic<bool> running_{false};
    std::atomic<bool> playing_{false};
    std::atomic<bool> eof_{false};
    std::atomic<bool> joined_{true};
};

} // namespace mvm::gpu

#endif // MVM_GPU_PREVIEW_SOURCE_DECODE_WORKER_H
