#include "media/gpu_preview/frame_queue.h"

#include <chrono>

namespace mvm::gpu {

PreviewFrameQueue::PreviewFrameQueue(size_t capacity, size_t retainDepth)
    : capacity_(capacity == 0 ? 1 : capacity), retainDepth_(retainDepth) {}

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

void PreviewFrameQueue::setCurrentGeneration(unsigned long long generation) {
    std::lock_guard<std::mutex> g(mutex_);
    generation_ = generation;
    // generation が進んだら、まだ表示していない古いフレームは捨てる。
    // 残すと seek 直後に飛ぶ前の絵が出る。
    pending_.clear();
    spaceAvailable_.notify_all();
}

unsigned long long PreviewFrameQueue::currentGeneration() const {
    std::lock_guard<std::mutex> g(mutex_);
    return generation_;
}

SubmitResult PreviewFrameQueue::submitFrame(const DecodedGpuFrame& frame) {
    std::unique_lock<std::mutex> g(mutex_);

    if (stopped_)
        return SubmitResult::RejectedNotReady;

    if (!frame.valid())
        return SubmitResult::RejectedInvalidFrame;

    if (frame.generation < generation_) {
        staleRejects_++;
        return SubmitResult::RejectedStaleGeneration;
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
    retained_.clear();
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

    // GPU が読み終わるまで解放しない。D3D11 immediate context には
    // fence が無いので、深さで代用する。
    retained_.push_back(frame.lifetime);
    while (retained_.size() > retainDepth_)
        retained_.pop_front();
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
        retained_.clear();
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

long long PreviewFrameQueue::rejectedDeviceMismatchCount() const {
    std::lock_guard<std::mutex> g(mutex_);
    return deviceRejects_;
}

long long PreviewFrameQueue::queueFullCount() const {
    std::lock_guard<std::mutex> g(mutex_);
    return queueFull_;
}

} // namespace mvm::gpu
