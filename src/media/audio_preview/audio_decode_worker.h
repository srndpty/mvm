#ifndef MVM_AUDIO_PREVIEW_AUDIO_DECODE_WORKER_H
#define MVM_AUDIO_PREVIEW_AUDIO_DECODE_WORKER_H

#include "media/audio_preview/audio_frame_queue.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;

namespace mvm::audio {

struct AudioSeekTicket {
    std::uint64_t requestId = 0;
    std::int64_t targetSample = -1;
};

enum class AudioSeekRequestResult { Accepted, RejectedBusy, RejectedStopped, RejectedInvalid };
enum class AudioSeekWaitResult { Ready, Timeout, StaleTicket };

struct AudioSeekCompletion {
    std::uint64_t requestId = 0;
    std::int64_t requestedSample = -1;
    std::int64_t firstOutputSample = -1;
    SourceGeneration seekGeneration{};
    std::int64_t readyQpc = 0;
    std::int64_t discardedPrerollSamples = 0;
    double latencyMs = 0.0;
    bool completed = false;
    std::string error;
};

struct AudioDecoderSnapshot {
    bool open = false;
    bool running = false;
    bool playing = false;
    bool joined = true;
    bool eof = false;
    bool fatal = false;
    AudioFormatInfo sourceFormat;
    SourceId sourceId{};
    SourceGeneration sourceGeneration{};
    AudioResourceEpoch resourceEpoch{};
    std::uint64_t decodedChunkCount = 0;
    std::uint64_t decodedSampleCount = 0;
    std::uint64_t discardedPrerollSamples = 0;
    std::uint64_t backpressureWaitCount = 0;
    std::uint64_t decodeErrorCount = 0;
    std::uint64_t seekTimeoutCount = 0;
    std::uint64_t audioDecodeThreadJoinLeak = 0;
    std::string lastError;
};

class AudioDecodeWorker final {
public:
    explicit AudioDecodeWorker(SourceId sourceId,
                               std::int64_t queueHardMaxSamples = kQueueHardMaxSamples);
    ~AudioDecodeWorker();
    AudioDecodeWorker(const AudioDecodeWorker&) = delete;
    AudioDecodeWorker& operator=(const AudioDecodeWorker&) = delete;

    bool start(const std::string& utf8Path, std::string& error);
    void play();
    void pause();
    void stop();
    AudioSeekRequestResult requestSeek(std::int64_t sample, AudioSeekTicket& ticket,
                                       std::string& error);
    AudioSeekWaitResult waitSeek(const AudioSeekTicket& ticket, int timeoutMs,
                                 AudioSeekCompletion& completion);
    AudioDecoderSnapshot snapshot() const;

    AudioFrameQueue& queue() { return queue_; }

    const AudioFrameQueue& queue() const { return queue_; }

private:
    bool openInput(const std::string& path, std::string& error);
    void closeInput();
    void run();
    bool decodeOne(AudioChunk& chunk, std::string& error);
    AudioSeekCompletion executeSeek(const AudioSeekTicket& ticket);
    void fail(const std::string& error);

    SourceId sourceId_{};
    SourceGeneration generation_{1};
    AudioResourceEpoch resourceEpoch_{1};
    AudioFrameQueue queue_;
    AVFormatContext* format_ = nullptr;
    AVCodecContext* codec_ = nullptr;
    SwrContext* resampler_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    int streamIndex_ = -1;
    bool demuxEof_ = false;

    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable seekDone_;
    AudioDecoderSnapshot metrics_;
    bool pendingSeek_ = false;
    bool seekOutstanding_ = false;
    std::uint64_t nextSeekId_ = 0;
    AudioSeekTicket seekTicket_{};
    AudioSeekCompletion seekCompletion_{};
    std::atomic<bool> running_{false};
    std::atomic<bool> playing_{false};
    std::atomic<bool> joined_{true};
};

} // namespace mvm::audio
#endif
