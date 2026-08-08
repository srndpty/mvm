#include "media/gpu_preview/source_decode_worker.h"

#include "media/gpu_preview/qpc_clock.h"

#include <chrono>
#include <cstdint>

namespace mvm::gpu {

SourceDecodeWorker::SourceDecodeWorker(SourceId sourceId, SharedD3D11Device& device,
                                       ReadbackCounters& counters, size_t bufferCapacity)
    : sourceId_(sourceId), device_(device), counters_(counters),
      buffer_(sourceId, SourceGeneration{}, bufferCapacity) {
    snapshot_.sourceId = sourceId_;
    snapshot_.bufferCapacity = buffer_.capacity();
}

SourceDecodeWorker::~SourceDecodeWorker() {
    stop();
}

void SourceDecodeWorker::refreshSnapshotLocked() {
    SourceDecoderSnapshot next;
    {
        std::lock_guard<std::mutex> lock(snapshotMutex_);
        next = snapshot_;
    }
    next.open = static_cast<bool>(decoder_);
    next.running = running();
    next.playing = playing();
    next.eof = eof();
    next.joined = joined();
    next.sourceId = sourceId_;
    next.bufferCapacity = buffer_.capacity();
    next.bufferDepth = buffer_.depth();
    if (decoder_) {
        next.info = decoder_->info();
        next.adapter = decoder_->decodeAdapter();
        next.decodeDevicePointer = decoder_->decodeDevicePointer();
        next.sourceGeneration = decoder_->sourceGeneration();
        next.resourceEpoch = decoder_->resourceEpoch();
        next.decodedFrameCount = decoder_->decodedFrameCount();
        next.decodeErrorCount = decoder_->decodeErrorCount();
        next.softwareFrameRejectCount = decoder_->softwareFrameRejectCount();
        next.seekBackoffCount = decoder_->seekBackoffCount();
    }
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    snapshot_ = std::move(next);
}

SourceDecoderSnapshot SourceDecodeWorker::snapshot() const {
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    SourceDecoderSnapshot result = snapshot_;
    result.running = running();
    result.playing = playing();
    result.eof = eof();
    result.joined = joined();
    result.bufferDepth = buffer_.depth();
    return result;
}

bool SourceDecodeWorker::start(const std::string& utf8Path, std::string& err) {
    if (startedOnce_) {
        err = "同じSourceIdのworkerは再openできません。新しいSourceIdを登録してください";
        return false;
    }
    if (sourceId_.value == 0) {
        err = "SourceId 0は登録済みsourceとして使用できません";
        return false;
    }
    if (!device_.valid()) {
        err = "共有D3D11 deviceが準備できていません";
        return false;
    }

    auto decoder = std::make_unique<FFmpegD3D11Decoder>(device_, sourceId_, &counters_);
    if (!decoder->open(utf8Path, err))
        return false;
    if (buffer_.setGeneration(decoder->sourceGeneration()) ==
        GenerationUpdateResult::RejectedRegression) {
        err = "source-local bufferのgenerationを初期化できません";
        return false;
    }

    buffer_.restart();
    {
        std::lock_guard<std::mutex> lock(decoderMutex_);
        decoder_ = std::move(decoder);
    }
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        stepsPending_ = 0;
    }
    eof_.store(false, std::memory_order_release);
    playing_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    joined_.store(false, std::memory_order_release);
    startedOnce_ = true;
    {
        std::lock_guard<std::mutex> lock(snapshotMutex_);
        snapshot_ = SourceDecoderSnapshot{};
        snapshot_.sourceId = sourceId_;
        snapshot_.bufferCapacity = buffer_.capacity();
        snapshot_.joined = false;
    }
    {
        std::lock_guard<std::mutex> lock(decoderMutex_);
        refreshSnapshotLocked();
    }
    thread_ = std::thread([this] { run(); });
    return true;
}

void SourceDecodeWorker::stop() {
    playing_.store(false, std::memory_order_release);
    running_.store(false, std::memory_order_release);
    buffer_.stop();
    wake_.notify_all();
    if (thread_.joinable())
        thread_.join();

    {
        std::lock_guard<std::mutex> lock(decoderMutex_);
        if (decoder_) {
            refreshSnapshotLocked();
            decoder_->close();
            decoder_.reset();
        }
    }
    {
        std::lock_guard<std::mutex> snapshotLock(snapshotMutex_);
        snapshot_.open = false;
        snapshot_.running = false;
        snapshot_.playing = false;
        snapshot_.bufferDepth = 0;
        snapshot_.joined = true;
    }
    // joined は decoder close/reset と最終 snapshot の後にだけ公開する。
    joined_.store(true, std::memory_order_release);
}

void SourceDecodeWorker::play() {
    if (!running())
        return;
    playing_.store(true, std::memory_order_release);
    wake_.notify_all();
}

void SourceDecodeWorker::pause() {
    playing_.store(false, std::memory_order_release);
    wake_.notify_all();
}

void SourceDecodeWorker::stepForward() {
    if (!running())
        return;
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        ++stepsPending_;
    }
    wake_.notify_all();
}

bool SourceDecodeWorker::validateTextureDevice(const DecodedGpuFrame& frame, std::string& err) {
    ID3D11Device* owner = nullptr;
    frame.texture->GetDevice(&owner);
    const bool same = owner == device_.device();
    if (owner)
        owner->Release();
    if (same)
        return true;
    err = "decode textureの実owner deviceが共有deviceと一致しません";
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    ++snapshot_.deviceMismatchCount;
    return false;
}

void SourceDecodeWorker::noteFatal(const std::string& err) {
    playing_.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    snapshot_.fatal = true;
    snapshot_.lastError = err;
}

bool SourceDecodeWorker::submitWithBackpressure(const DecodedGpuFrame& frame, std::string& err) {
    if (!validateTextureDevice(frame, err)) {
        noteFatal(err);
        return false;
    }
    while (running()) {
        const SubmitResult result = buffer_.submitFrame(frame);
        if (result == SubmitResult::Accepted) {
            std::lock_guard<std::mutex> lock(snapshotMutex_);
            ++snapshot_.submittedCount;
            return true;
        }
        if (result == SubmitResult::RejectedQueueFull) {
            {
                std::lock_guard<std::mutex> lock(snapshotMutex_);
                ++snapshot_.queueFullCount;
                ++snapshot_.backpressureWaitCount;
            }
            if (!buffer_.waitForSpace(50) && buffer_.stopped())
                return false;
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(snapshotMutex_);
            if (result == SubmitResult::RejectedStaleGeneration)
                ++snapshot_.staleGenerationRejectCount;
            else if (result == SubmitResult::RejectedFutureGeneration)
                ++snapshot_.futureGenerationRejectCount;
            else
                ++snapshot_.invalidFrameRejectCount;
        }
        err = std::string("source-local bufferがframeを拒否しました: ") + toString(result);
        noteFatal(err);
        return false;
    }
    return false;
}

bool SourceDecodeWorker::seekBlocking(long long frameNumber, double& decodeReadyMs,
                                      std::string& err) {
    pause();
    decodeReadyMs = 0.0;
    std::lock_guard<std::mutex> lock(decoderMutex_);
    if (!decoder_) {
        err = "decoderが開かれていません";
        return false;
    }
    const long long begin = qpcTicks();
    if (!decoder_->seek(frameNumber, err)) {
        buffer_.setGeneration(decoder_->sourceGeneration());
        decodeReadyMs = qpcMsBetween(begin, qpcTicks());
        refreshSnapshotLocked();
        return false;
    }
    if (buffer_.setGeneration(decoder_->sourceGeneration()) ==
        GenerationUpdateResult::RejectedRegression) {
        err = "seek後のsource generationが逆行しました";
        decodeReadyMs = qpcMsBetween(begin, qpcTicks());
        noteFatal(err);
        return false;
    }

    DecodedGpuFrame frame;
    const DecodeStatus status = decoder_->requestFrame(frame, err);
    decodeReadyMs = qpcMsBetween(begin, qpcTicks());
    if (status != DecodeStatus::Ok || frame.frameNumber != frameNumber) {
        if (err.empty())
            err = "exact seekが要求frameへ完全一致しませんでした";
        noteFatal(err);
        refreshSnapshotLocked();
        return false;
    }
    if (!submitWithBackpressure(frame, err)) {
        refreshSnapshotLocked();
        return false;
    }
    eof_.store(false, std::memory_order_release);
    refreshSnapshotLocked();
    return true;
}

bool SourceDecodeWorker::flushBlocking(std::string& err) {
    pause();
    std::lock_guard<std::mutex> lock(decoderMutex_);
    if (!decoder_) {
        err = "decoderが開かれていません";
        return false;
    }
    decoder_->flush();
    if (buffer_.setGeneration(decoder_->sourceGeneration()) ==
        GenerationUpdateResult::RejectedRegression) {
        err = "flush後のsource generationが逆行しました";
        noteFatal(err);
        return false;
    }
    eof_.store(false, std::memory_order_release);
    refreshSnapshotLocked();
    return true;
}

void SourceDecodeWorker::run() {
    while (running()) {
        {
            std::unique_lock<std::mutex> lock(commandMutex_);
            wake_.wait(lock, [this] { return !running() || playing() || stepsPending_ > 0; });
            if (!running())
                break;
            if (!playing() && stepsPending_ > 0)
                --stepsPending_;
        }

        if (!buffer_.waitForSpace(50)) {
            if (!running() || buffer_.stopped())
                break;
            std::lock_guard<std::mutex> snapshotLock(snapshotMutex_);
            ++snapshot_.backpressureWaitCount;
            continue;
        }

        std::lock_guard<std::mutex> lock(decoderMutex_);
        if (!decoder_)
            break;
        DecodedGpuFrame frame;
        std::string err;
        const DecodeStatus status = decoder_->requestFrame(frame, err);
        if (status == DecodeStatus::Again)
            continue;
        if (status == DecodeStatus::Eof) {
            eof_.store(true, std::memory_order_release);
            playing_.store(false, std::memory_order_release);
            refreshSnapshotLocked();
            continue;
        }
        if (status != DecodeStatus::Ok) {
            noteFatal(err.empty() ? "decodeに失敗しました" : err);
            refreshSnapshotLocked();
            continue;
        }
        if (!submitWithBackpressure(frame, err)) {
            refreshSnapshotLocked();
            continue;
        }
        refreshSnapshotLocked();
    }
}

} // namespace mvm::gpu
