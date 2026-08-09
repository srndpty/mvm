#include "media/gpu_preview/source_frame_buffer.h"

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

bool SourceFrameBuffer::takeExactPair(SourceFrameBuffer& a, SourceFrameBuffer& b,
                                      long long frameNumber, DecodedGpuFrame& frameA,
                                      DecodedGpuFrame& frameB) {
    if (&a == &b)
        return false;
    std::scoped_lock lock(a.mutex_, b.mutex_);
    if (a.frames_.empty() || b.frames_.empty() || a.frames_.front().frameNumber != frameNumber ||
        b.frames_.front().frameNumber != frameNumber)
        return false;
    frameA = std::move(a.frames_.front());
    frameB = std::move(b.frames_.front());
    a.frames_.pop_front();
    b.frames_.pop_front();
    // composition drawより先にdecode workerを起こすと、shared D3D11 context lockを
    // workerが奪いdeadlineを遅らせる。空き通知はactual display後のnoteDisplayedで行う。
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
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] {
        return stopped_ || frames_.size() < capacity_;
    }) && !stopped_;
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
