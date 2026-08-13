#include "media/gpu_preview/source_decode_worker.h"

#include "media/gpu_preview/qpc_clock.h"

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace mvm::gpu {

const char* toString(SeekCompletionPublishResult result) {
    switch (result) {
    case SeekCompletionPublishResult::Published:
        return "published";
    case SeekCompletionPublishResult::RejectedNoOutstanding:
        return "rejected_no_outstanding";
    case SeekCompletionPublishResult::RejectedAlreadyPublished:
        return "rejected_already_published";
    case SeekCompletionPublishResult::RejectedRequestMismatch:
        return "rejected_request_mismatch";
    case SeekCompletionPublishResult::RejectedStoppedSuperseded:
        return "rejected_stopped_superseded";
    }
    return "unknown";
}

const char* toString(SeekExecutionPhase phase) {
    switch (phase) {
    case SeekExecutionPhase::Idle:
        return "idle";
    case SeekExecutionPhase::Queued:
        return "queued";
    case SeekExecutionPhase::WaitingDecoderMutex:
        return "waiting_decoder_mutex";
    case SeekExecutionPhase::DecoderSeek:
        return "decoder_seek";
    case SeekExecutionPhase::GenerationReset:
        return "generation_reset";
    case SeekExecutionPhase::RequestExactFrame:
        return "request_exact_frame";
    case SeekExecutionPhase::SubmitExactFrame:
        return "submit_exact_frame";
    case SeekExecutionPhase::PublishingCompletion:
        return "publishing_completion";
    case SeekExecutionPhase::Completed:
        return "completed";
    }
    return "unknown";
}

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

SeekCompletionPublishResult SourceSeekMailbox::publish(const SeekCompletion& completion) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!outstanding_) {
        if (stopped_ && completion.requestId == ticket_.requestId)
            return SeekCompletionPublishResult::RejectedStoppedSuperseded;
        return SeekCompletionPublishResult::RejectedNoOutstanding;
    }
    if (completion.requestId != ticket_.requestId)
        return SeekCompletionPublishResult::RejectedRequestMismatch;
    if (completionReady_) {
        if (stopped_ && completion_.status == SeekCompletionStatus::Stopped)
            return SeekCompletionPublishResult::RejectedStoppedSuperseded;
        return SeekCompletionPublishResult::RejectedAlreadyPublished;
    }
    completion_ = completion;
    completionReady_ = true;
    completed_.notify_all();
    return SeekCompletionPublishResult::Published;
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

SourceSeekMailboxSnapshot SourceSeekMailbox::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {stopped_,         outstanding_, pending_,
            completionReady_, ticket_,      completionReady_ ? completion_.requestId : 0};
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

SourceSeekDiagnosticSnapshot SourceDecodeWorker::seekDiagnosticSnapshot() const {
    SourceSeekDiagnosticSnapshot result;
    result.phase = seekPhase_.load(std::memory_order_acquire);
    result.requestId = seekRequestId_.load(std::memory_order_acquire);
    result.targetFrame = seekTargetFrame_.load(std::memory_order_acquire);
    result.phaseEnterQpc = seekPhaseEnterQpc_.load(std::memory_order_acquire);
    result.lastProgressQpc = seekLastProgressQpc_.load(std::memory_order_acquire);
    result.mailbox = seekMailbox_.snapshot();
    result.completionPublishRejectCount =
        seekCompletionPublishRejectCount_.load(std::memory_order_acquire);
    result.completionRequestMismatchCount =
        seekCompletionRequestMismatchCount_.load(std::memory_order_acquire);
    result.completionStoppedSupersededCount =
        seekCompletionStoppedSupersededCount_.load(std::memory_order_acquire);
    return result;
}

void SourceDecodeWorker::setSeekPhase(SeekExecutionPhase phase, const SeekTicket& ticket) {
    const long long now = qpcTicks();
    seekRequestId_.store(ticket.requestId, std::memory_order_release);
    seekTargetFrame_.store(ticket.targetFrame, std::memory_order_release);
    seekPhaseEnterQpc_.store(now, std::memory_order_release);
    seekLastProgressQpc_.store(now, std::memory_order_release);
    seekPhase_.store(phase, std::memory_order_release);
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
    {
        // wake_ のpredicate更新とwait遷移を同じmutexで直列化し、通知の取りこぼしを防ぐ。
        std::lock_guard<std::mutex> lock(commandMutex_);
        playing_.store(false, std::memory_order_release);
        running_.store(false, std::memory_order_release);
        seekMailbox_.stop();
    }
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
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        if (!running())
            return;
        playing_.store(true, std::memory_order_release);
    }
    wake_.notify_all();
}

void SourceDecodeWorker::pause() {
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        playing_.store(false, std::memory_order_release);
    }
    wake_.notify_all();
}

void SourceDecodeWorker::stepForward() {
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        if (!running())
            return;
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
    SeekRequestResult result = SeekRequestResult::RejectedStopped;
    {
        // mailbox pendingはwake_のpredicateである。commandMutexなしで更新すると、workerが
        // predicate確認からwaitへ移る隙間のnotifyを失い、pendingのまま停止し得る。
        std::lock_guard<std::mutex> lock(commandMutex_);
        result = seekMailbox_.request(frameNumber, qpcTicks(), ticket, err);
        if (result == SeekRequestResult::Accepted) {
            playing_.store(false, std::memory_order_release);
            setSeekPhase(SeekExecutionPhase::Queued, ticket);
        }
    }
    if (result == SeekRequestResult::Accepted) {
        // mailbox 公開後に initial-buffer wait を起こす。buffer mutex の取得は
        // commandMutex_ 解放後なので mailbox -> buffer の lock inversion を作らない。
        buffer_.notifyWaiters();
        wake_.notify_all();
    }
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
    setSeekPhase(SeekExecutionPhase::WaitingDecoderMutex, ticket);
    std::lock_guard<std::mutex> lock(decoderMutex_);
    setSeekPhase(SeekExecutionPhase::DecoderSeek, ticket);
    if (!decoder_) {
        completion.error = "decoderが開かれていません";
        completion.decodeReadyQpc = qpcTicks();
        return completion;
    }
    if (!decoder_->seek(ticket.targetFrame, completion.error)) {
        setSeekPhase(SeekExecutionPhase::GenerationReset, ticket);
        buffer_.setGeneration(decoder_->sourceGeneration());
        refreshSnapshotLocked();
        completion.decodeReadyQpc = qpcTicks();
        completion.decodeReadyMs = qpcMsBetween(requestQpc, completion.decodeReadyQpc);
        return completion;
    }
    setSeekPhase(SeekExecutionPhase::GenerationReset, ticket);
    if (buffer_.setGeneration(decoder_->sourceGeneration()) ==
        GenerationUpdateResult::RejectedRegression) {
        completion.error = "seek後のsource generationが逆行しました";
        noteFatal(completion.error);
        completion.decodeReadyQpc = qpcTicks();
        completion.decodeReadyMs = qpcMsBetween(requestQpc, completion.decodeReadyQpc);
        return completion;
    }

    DecodedGpuFrame frame;
    setSeekPhase(SeekExecutionPhase::RequestExactFrame, ticket);
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
    setSeekPhase(SeekExecutionPhase::SubmitExactFrame, ticket);
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
    setSeekPhase(SeekExecutionPhase::Completed, {0, frameNumber});
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
                const SeekCompletion completion = executeSeek(seekTicket, seekRequestQpc);
                setSeekPhase(SeekExecutionPhase::PublishingCompletion, seekTicket);
                const SeekCompletionPublishResult published = seekMailbox_.publish(completion);
                if (published == SeekCompletionPublishResult::Published) {
                    setSeekPhase(SeekExecutionPhase::Completed, seekTicket);
                } else if (published == SeekCompletionPublishResult::RejectedStoppedSuperseded) {
                    seekCompletionStoppedSupersededCount_.fetch_add(1, std::memory_order_relaxed);
                } else {
                    seekCompletionPublishRejectCount_.fetch_add(1, std::memory_order_relaxed);
                    if (published == SeekCompletionPublishResult::RejectedRequestMismatch)
                        seekCompletionRequestMismatchCount_.fetch_add(1, std::memory_order_relaxed);
                    if (running())
                        noteFatal(std::string("seek completionを公開できません: ") +
                                  toString(published));
                }
                continue;
            }
            if (!playing() && stepsPending_ > 0)
                --stepsPending_;
        }

        const SourceBufferSpaceWaitResult initialWait = buffer_.waitForSpaceInterruptible(
            50, [this] { return seekMailbox_.hasPending(); });
        if (initialWait != SourceBufferSpaceWaitResult::SpaceAvailable) {
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
