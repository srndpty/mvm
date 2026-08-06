#include "media/gpu_preview/frame_queue.h"

#include <chrono>

namespace mvm::gpu {

PreviewFrameQueue::PreviewFrameQueue(size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

PreviewFrameQueue::~PreviewFrameQueue() {
    stop();
}

ID3D11Device* PreviewFrameQueue::deviceOfTexture(ID3D11Texture2D* texture) const {
    if (!texture)
        return nullptr;
    ID3D11Device* dev = nullptr;
    texture->GetDevice(&dev);
    if (dev)
        dev->Release(); // 比較にしか使わないので参照は持たない
    return dev;
}

void PreviewFrameQueue::setExpectedDevice(ID3D11Device* device) {
    std::lock_guard<std::mutex> g(mutex_);
    expectedDevice_ = device;
}

GenerationUpdateResult PreviewFrameQueue::setCurrentGeneration(GenerationId generation) {
    std::lock_guard<std::mutex> g(mutex_);
    // 逆行は受け付けない。逆行を黙って通すと、seek 済みの新しい世代を
    // 古い世代で上書きし、飛ぶ前の絵が復活しうる (fail-closed)。
    if (generation < generation_) {
        generationRegressions_++;
        return GenerationUpdateResult::RejectedRegression;
    }
    // 同値は no-op。**pending を破棄しない。** 同じ generation の再設定
    // (例: 冪等な再同期) で表示待ちが消えると、正常な frame を落とす。
    if (generation == generation_)
        return GenerationUpdateResult::NoOp;

    // 前進したときだけ、まだ表示していない古いフレームを捨てる。
    // 残すと seek 直後に飛ぶ前の絵が出る。
    generation_ = generation;
    pending_.clear();
    spaceAvailable_.notify_all();
    return GenerationUpdateResult::Updated;
}

GenerationId PreviewFrameQueue::currentGeneration() const {
    std::lock_guard<std::mutex> g(mutex_);
    return generation_;
}

SubmitResult PreviewFrameQueue::submitFrame(const DecodedGpuFrame& frame) {
    std::unique_lock<std::mutex> g(mutex_);

    if (stopped_)
        return SubmitResult::RejectedNotReady;

    if (!frame.valid()) {
        invalidRejects_++;
        return SubmitResult::RejectedInvalidFrame;
    }

    if (frame.generation < generation_) {
        staleRejects_++;
        return SubmitResult::RejectedStaleGeneration;
    }
    // 未来の generation は表示側がまだ知らない世代である。受け取ると、
    // まだ設定していない構成のフレームを表示してしまう。fail-closed で拒否する。
    if (generation_ < frame.generation) {
        futureRejects_++;
        return SubmitResult::RejectedFutureGeneration;
    }

    if (expectedDevice_) {
        // 実体を見る。設定値の照合ではない。
        ID3D11Device* owner = deviceOfTexture(frame.texture);
        if (owner != expectedDevice_) {
            deviceRejects_++;
            return SubmitResult::RejectedDeviceMismatch;
        }
    }

    if (pending_.size() >= capacity_) {
        queueFull_++;
        return SubmitResult::RejectedQueueFull;
    }

    pending_.push_back(frame);
    submitted_++;
    return SubmitResult::Accepted;
}

void PreviewFrameQueue::clear() {
    std::lock_guard<std::mutex> g(mutex_);
    pending_.clear();
    displayedFrame_ = -1;
    spaceAvailable_.notify_all();
}

long long PreviewFrameQueue::displayedFrameNumber() const {
    std::lock_guard<std::mutex> g(mutex_);
    return displayedFrame_;
}

bool PreviewFrameQueue::takeForDisplay(DecodedGpuFrame& out) {
    std::lock_guard<std::mutex> g(mutex_);
    if (pending_.empty())
        return false;
    out = pending_.front();
    pending_.pop_front();
    spaceAvailable_.notify_all();
    return true;
}

void PreviewFrameQueue::noteDisplayed(const DecodedGpuFrame& frame) {
    std::lock_guard<std::mutex> g(mutex_);
    displayedFrame_ = frame.frameNumber;
    displayed_++;
    spaceAvailable_.notify_all();
}

bool PreviewFrameQueue::waitForSpace(int timeoutMs) {
    std::unique_lock<std::mutex> g(mutex_);
    if (stopped_)
        return false;
    if (pending_.size() < capacity_)
        return true;
    spaceAvailable_.wait_for(g, std::chrono::milliseconds(timeoutMs),
                             [this] { return stopped_ || pending_.size() < capacity_; });
    return !stopped_ && pending_.size() < capacity_;
}

void PreviewFrameQueue::stop() {
    {
        std::lock_guard<std::mutex> g(mutex_);
        stopped_ = true;
        pending_.clear();
    }
    spaceAvailable_.notify_all();
}

void PreviewFrameQueue::restart() {
    std::lock_guard<std::mutex> g(mutex_);
    stopped_ = false;
}

size_t PreviewFrameQueue::depth() const {
    std::lock_guard<std::mutex> g(mutex_);
    return pending_.size();
}

long long PreviewFrameQueue::submittedCount() const {
    std::lock_guard<std::mutex> g(mutex_);
    return submitted_;
}

long long PreviewFrameQueue::displayedCount() const {
    std::lock_guard<std::mutex> g(mutex_);
    return displayed_;
}

long long PreviewFrameQueue::rejectedStaleCount() const {
    std::lock_guard<std::mutex> g(mutex_);
    return staleRejects_;
}

long long PreviewFrameQueue::rejectedFutureCount() const {
    std::lock_guard<std::mutex> g(mutex_);
    return futureRejects_;
}

long long PreviewFrameQueue::rejectedInvalidCount() const {
    std::lock_guard<std::mutex> g(mutex_);
    return invalidRejects_;
}

long long PreviewFrameQueue::rejectedDeviceMismatchCount() const {
    std::lock_guard<std::mutex> g(mutex_);
    return deviceRejects_;
}

long long PreviewFrameQueue::queueFullCount() const {
    std::lock_guard<std::mutex> g(mutex_);
    return queueFull_;
}

long long PreviewFrameQueue::generationRegressionCount() const {
    std::lock_guard<std::mutex> g(mutex_);
    return generationRegressions_;
}

} // namespace mvm::gpu
