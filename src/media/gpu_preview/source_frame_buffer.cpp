#include "media/gpu_preview/source_frame_buffer.h"

#include <algorithm>
#include <chrono>

namespace mvm::gpu {

SourceFrameBuffer::SourceFrameBuffer(SourceId source, SourceGeneration generation, size_t capacity)
    : source_(source), generation_(generation), capacity_(capacity == 0 ? 1 : capacity) {}

SubmitResult SourceFrameBuffer::submitFrame(const DecodedGpuFrame& frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_)
        return SubmitResult::RejectedNotReady;
    if (!frame.valid() || frame.sourceId != source_)
        return SubmitResult::RejectedInvalidFrame;
    if (frame.sourceGeneration < generation_)
        return SubmitResult::RejectedStaleGeneration;
    if (generation_ < frame.sourceGeneration)
        return SubmitResult::RejectedFutureGeneration;
    if (frames_.size() >= capacity_)
        return SubmitResult::RejectedQueueFull;
    frames_.push_back(frame);
    changed_.notify_all();
    return SubmitResult::Accepted;
}

void SourceFrameBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    frames_.clear();
    displayed_ = -1;
    changed_.notify_all();
}

long long SourceFrameBuffer::displayedFrameNumber() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return displayed_;
}

GenerationUpdateResult SourceFrameBuffer::setGeneration(SourceGeneration generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation < generation_)
        return GenerationUpdateResult::RejectedRegression;
    if (generation == generation_)
        return GenerationUpdateResult::NoOp;
    generation_ = generation;
    frames_.clear();
    changed_.notify_all();
    return GenerationUpdateResult::Updated;
}

SourceGeneration SourceFrameBuffer::generation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
}

bool SourceFrameBuffer::take(DecodedGpuFrame& frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (frames_.empty())
        return false;
    frame = std::move(frames_.front());
    frames_.pop_front();
    changed_.notify_all();
    return true;
}

bool SourceFrameBuffer::peekFrontIdentity(SourceFrameIdentity& identity) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (frames_.empty())
        return false;
    identity = identityOf(frames_.front());
    return true;
}

bool SourceFrameBuffer::takeExact(long long frameNumber, DecodedGpuFrame& frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (frames_.empty() || frames_.front().frameNumber != frameNumber)
        return false;
    frame = std::move(frames_.front());
    frames_.pop_front();
    changed_.notify_all();
    return true;
}

bool SourceFrameBuffer::takeExactAll(const std::vector<SourceFrameBuffer*>& sources,
                                     long long frameNumber, std::vector<DecodedGpuFrame>& frames) {
    frames.clear();
    if (sources.empty())
        return false;
    for (SourceFrameBuffer* source : sources) {
        if (source == nullptr)
            return false;
    }

    // lock順をSourceId昇順で固定する。呼び出し側の並びに依存させると、
    // 同じbuffer集合を別順で渡したときにdeadlockし得る。
    // SourceIdは登録時に一意なので、tie-breakは重複検出のためだけに置く。
    std::vector<SourceFrameBuffer*> ordered = sources;
    std::sort(ordered.begin(), ordered.end(), [](SourceFrameBuffer* a, SourceFrameBuffer* b) {
        if (!(a->source_ == b->source_))
            return a->source_ < b->source_;
        return a < b;
    });
    for (size_t i = 1; i < ordered.size(); ++i) {
        if (ordered[i - 1] == ordered[i])
            return false; // 同一bufferの重複はself-deadlockになる
    }

    std::vector<std::unique_lock<std::mutex>> locks;
    locks.reserve(ordered.size());
    for (SourceFrameBuffer* source : ordered)
        locks.emplace_back(source->mutex_);

    // 全sourceのfrontが一致するまでは一つも消費しない。partial consumeを許すと
    // 片側だけ進んだbufferがexact pairを永久に成立させなくなる。
    for (SourceFrameBuffer* source : ordered) {
        if (source->frames_.empty() || source->frames_.front().frameNumber != frameNumber)
            return false;
    }

    frames.reserve(sources.size());
    for (SourceFrameBuffer* source : sources) {
        frames.push_back(std::move(source->frames_.front()));
        source->frames_.pop_front();
    }
    // composition drawより先にdecode workerを起こすと、shared D3D11 context lockを
    // workerが奪いdeadlineを遅らせる。空き通知はactual display後のnoteDisplayedで行う。
    return true;
}

bool SourceFrameBuffer::takeExactPair(SourceFrameBuffer& a, SourceFrameBuffer& b,
                                      long long frameNumber, DecodedGpuFrame& frameA,
                                      DecodedGpuFrame& frameB) {
    std::vector<DecodedGpuFrame> frames;
    if (!takeExactAll({&a, &b}, frameNumber, frames))
        return false;
    frameA = std::move(frames[0]);
    frameB = std::move(frames[1]);
    return true;
}

size_t SourceFrameBuffer::discardBefore(long long frameNumber) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t discarded = 0;
    while (!frames_.empty() && frames_.front().frameNumber < frameNumber) {
        frames_.pop_front();
        ++discarded;
    }
    if (discarded > 0)
        changed_.notify_all();
    return discarded;
}

void SourceFrameBuffer::noteDisplayed(long long frameNumber) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        displayed_ = frameNumber;
    }
    changed_.notify_all();
}

bool SourceFrameBuffer::waitForSpace(int timeoutMs) {
    return waitForSpaceInterruptible(timeoutMs, {}) == SourceBufferSpaceWaitResult::SpaceAvailable;
}

SourceBufferSpaceWaitResult
SourceFrameBuffer::waitForSpaceInterruptible(int timeoutMs,
                                             const std::function<bool()>& interruptPredicate) {
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this, &interruptPredicate] {
        return stopped_ || (interruptPredicate && interruptPredicate()) ||
               frames_.size() < capacity_;
    });

    if (stopped_)
        return SourceBufferSpaceWaitResult::Stopped;
    // buffer space と command が同時に成立した場合も control plane を優先する。
    if (interruptPredicate && interruptPredicate())
        return SourceBufferSpaceWaitResult::Interrupted;
    if (frames_.size() < capacity_)
        return SourceBufferSpaceWaitResult::SpaceAvailable;
    return SourceBufferSpaceWaitResult::TimedOut;
}

void SourceFrameBuffer::notifyWaiters() {
    {
        // 外部 predicate の公開と wait 開始の間に通知を失わないよう、wait と同じ
        // mutex を一度取得して unlock-and-wait の遷移と直列化する。
        std::lock_guard<std::mutex> lock(mutex_);
    }
    changed_.notify_all();
}

bool SourceFrameBuffer::waitForFrame(int timeoutMs) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                             [this] { return stopped_ || !frames_.empty(); }) &&
           !stopped_ && !frames_.empty();
}

void SourceFrameBuffer::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    frames_.clear();
    changed_.notify_all();
}

void SourceFrameBuffer::restart() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = false;
    frames_.clear();
    changed_.notify_all();
}

bool SourceFrameBuffer::stopped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopped_;
}

size_t SourceFrameBuffer::depth() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_.size();
}

SourceFrameBufferSnapshot SourceFrameBuffer::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    SourceFrameBufferSnapshot result;
    result.generation = generation_;
    result.displayedFrame = displayed_;
    result.depth = frames_.size();
    result.stopped = stopped_;
    if (!frames_.empty()) {
        result.frontFrame = frames_.front().frameNumber;
        result.backFrame = frames_.back().frameNumber;
    }
    return result;
}

} // namespace mvm::gpu
