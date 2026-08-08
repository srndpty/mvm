#include "media/gpu_preview/source_decode_worker.h"

#include "media/gpu_preview/qpc_clock.h"

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace mvm::gpu {

void SourceSeekMailbox::restart() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = false;
    outstanding_ = false;
    pending_ = false;
    completionReady_ = false;
}

SeekRequestResult SourceSeekMailbox::request(long long frameNumber, long long requestQpc,
                                             SeekTicket& ticket, std::string& err) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) {
        err = "停止中のworkerへseekを要求できません";
        return SeekRequestResult::RejectedStopped;
    }
    if (frameNumber < 0) {
        err = "seek targetは0以上である必要があります";
        return SeekRequestResult::RejectedInvalid;
    }
    if (outstanding_) {
        err = "同じsourceに未完了seekが既にあります";
        return SeekRequestResult::RejectedBusy;
    }
    ticket = {++nextRequestId_, frameNumber};
    ticket_ = ticket;
    requestQpc_ = requestQpc;
    outstanding_ = true;
    pending_ = true;
    completionReady_ = false;
    return SeekRequestResult::Accepted;
}

bool SourceSeekMailbox::takePending(SeekTicket& ticket, long long& requestQpc) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pending_)
        return false;
    ticket = ticket_;
    requestQpc = requestQpc_;
    pending_ = false;
    return true;
}

void SourceSeekMailbox::publish(const SeekCompletion& completion) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!outstanding_ || completionReady_ || completion.requestId != ticket_.requestId)
        return;
    completion_ = completion;
    completionReady_ = true;
    completed_.notify_all();
}

SeekWaitResult SourceSeekMailbox::wait(const SeekTicket& ticket, int timeoutMs,
                                       SeekCompletion& completion) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!outstanding_ || ticket.requestId != ticket_.requestId ||
        ticket.targetFrame != ticket_.targetFrame)
        return SeekWaitResult::StaleTicket;
    if (!completed_.wait_for(
            lock, std::chrono::milliseconds(std::max(0, timeoutMs)), [this, &ticket] {
                return completionReady_ && completion_.requestId == ticket.requestId;
            }))
        return SeekWaitResult::Timeout;
    completion = completion_;
    outstanding_ = false;
    completionReady_ = false;
    return SeekWaitResult::Ready;
}

void SourceSeekMailbox::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    pending_ = false;
    if (outstanding_ && !completionReady_) {
        completion_ = {};
        completion_.requestId = ticket_.requestId;
        completion_.targetFrame = ticket_.targetFrame;
        completion_.requestQpc = requestQpc_;
        completion_.status = SeekCompletionStatus::Stopped;
        completion_.error = "seek中にworkerが停止しました";
        completionReady_ = true;
    }
    completed_.notify_all();
}

bool SourceSeekMailbox::hasPending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_;
}

bool SourceSeekMailbox::hasOutstanding() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return outstanding_;
}

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
    seekMailbox_.restart();
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
    seekMailbox_.stop();
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
        // seek request後の旧generation frameは、満杯buffer待ちより先に破棄する。
        // ここで戻らないとworkerがsubmit待ちに留まり、mailboxのseek優先順位を破る。
        if (seekMailbox_.hasPending())
            return false;
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

SeekRequestResult SourceDecodeWorker::requestSeek(long long frameNumber, SeekTicket& ticket,
                                                  std::string& err) {
    if (!running()) {
        err = "停止中のworkerへseekを要求できません";
        return SeekRequestResult::RejectedStopped;
    }
    playing_.store(false, std::memory_order_release);
    const SeekRequestResult result = seekMailbox_.request(frameNumber, qpcTicks(), ticket, err);
    if (result == SeekRequestResult::Accepted)
        wake_.notify_all();
    return result;
}

SeekWaitResult SourceDecodeWorker::waitSeek(const SeekTicket& ticket, int timeoutMs,
                                            SeekCompletion& completion) {
    return seekMailbox_.wait(ticket, timeoutMs, completion);
}

SeekCompletion SourceDecodeWorker::executeSeek(const SeekTicket& ticket, long long requestQpc) {
    D3D11LockRoleScope role(sourceId_.value == 1 ? D3D11LockRole::DecoderA
                                                 : D3D11LockRole::DecoderB);
    SeekCompletion completion;
    completion.requestId = ticket.requestId;
    completion.targetFrame = ticket.targetFrame;
    completion.requestQpc = requestQpc;
    completion.beginQpc = qpcTicks();
    std::lock_guard<std::mutex> lock(decoderMutex_);
    if (!decoder_) {
        completion.error = "decoderが開かれていません";
        completion.decodeReadyQpc = qpcTicks();
        return completion;
    }
    if (!decoder_->seek(ticket.targetFrame, completion.error)) {
        buffer_.setGeneration(decoder_->sourceGeneration());
        refreshSnapshotLocked();
        completion.decodeReadyQpc = qpcTicks();
        completion.decodeReadyMs = qpcMsBetween(requestQpc, completion.decodeReadyQpc);
        return completion;
    }
    if (buffer_.setGeneration(decoder_->sourceGeneration()) ==
        GenerationUpdateResult::RejectedRegression) {
        completion.error = "seek後のsource generationが逆行しました";
        noteFatal(completion.error);
        completion.decodeReadyQpc = qpcTicks();
        completion.decodeReadyMs = qpcMsBetween(requestQpc, completion.decodeReadyQpc);
        return completion;
    }

    DecodedGpuFrame frame;
    const DecodeStatus status = decoder_->requestFrame(frame, completion.error);
    if (status != DecodeStatus::Ok || frame.frameNumber != ticket.targetFrame) {
        if (completion.error.empty())
            completion.error = "exact seekが要求frameへ完全一致しませんでした";
        noteFatal(completion.error);
        refreshSnapshotLocked();
        completion.decodeReadyQpc = qpcTicks();
        completion.decodeReadyMs = qpcMsBetween(requestQpc, completion.decodeReadyQpc);
        return completion;
    }
    completion.decodedFrameNumber = frame.frameNumber;
    if (!submitWithBackpressure(frame, completion.error)) {
        refreshSnapshotLocked();
        completion.status =
            running() ? SeekCompletionStatus::Failed : SeekCompletionStatus::Stopped;
        completion.decodeReadyQpc = qpcTicks();
        completion.decodeReadyMs = qpcMsBetween(requestQpc, completion.decodeReadyQpc);
        return completion;
    }
    eof_.store(false, std::memory_order_release);
    refreshSnapshotLocked();
    completion.decodeReadyQpc = qpcTicks();
    completion.decodeReadyMs = qpcMsBetween(requestQpc, completion.decodeReadyQpc);
    completion.sourceGeneration = decoder_->sourceGeneration();
    completion.resourceEpoch = decoder_->resourceEpoch();
    completion.status = running() ? SeekCompletionStatus::Completed : SeekCompletionStatus::Stopped;
    return completion;
}

bool SourceDecodeWorker::seekBlocking(long long frameNumber, double& decodeReadyMs,
                                      std::string& err) {
    if (!running()) {
        err = "停止中のworkerへseekを要求できません";
        return false;
    }
    if (seekMailbox_.hasOutstanding()) {
        err = "async seek outstanding中にblocking seekを実行できません";
        return false;
    }
    pause();
    const long long requestQpc = qpcTicks();
    const SeekCompletion completion = executeSeek({0, frameNumber}, requestQpc);
    decodeReadyMs = completion.decodeReadyMs;
    err = completion.error;
    return completion.status == SeekCompletionStatus::Completed;
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
    D3D11LockRoleScope role(sourceId_.value == 1 ? D3D11LockRole::DecoderA
                                                 : D3D11LockRole::DecoderB);
    while (running()) {
        {
            std::unique_lock<std::mutex> lock(commandMutex_);
            wake_.wait(lock, [this] {
                return !running() || seekMailbox_.hasPending() || playing() || stepsPending_ > 0;
            });
            if (!running())
                break;
            SeekTicket seekTicket;
            long long seekRequestQpc = 0;
            if (seekMailbox_.takePending(seekTicket, seekRequestQpc)) {
                lock.unlock();
                seekMailbox_.publish(executeSeek(seekTicket, seekRequestQpc));
                continue;
            }
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
