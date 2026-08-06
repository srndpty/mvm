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

bool PreviewFrameQueue::registerSource(SourceId source, SourceGeneration generation) {
    std::lock_guard<std::mutex> g(mutex_);
    if (source.value == 0 || generations_.find(source) != generations_.end())
        return false;
    generations_.emplace(source, generation);
    return true;
}

bool PreviewFrameQueue::unregisterSource(SourceId source) {
    std::lock_guard<std::mutex> g(mutex_);
    if (generations_.erase(source) == 0)
        return false;
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->sourceId == source)
            it = pending_.erase(it);
        else
            ++it;
    }
    spaceAvailable_.notify_all();
    return true;
}

size_t PreviewFrameQueue::registeredSourceCount() const {
    std::lock_guard<std::mutex> g(mutex_);
    return generations_.size();
}

GenerationUpdateResult PreviewFrameQueue::setCurrentGeneration(SourceId source,
                                                               SourceGeneration generation) {
    std::lock_guard<std::mutex> g(mutex_);
    const auto it = generations_.find(source);
    if (it == generations_.end()) {
        generationRegressions_++;
        return GenerationUpdateResult::RejectedRegression;
    } else {
        // 逆行は受け付けない。逆行を黙って通すと、seek 済みの新しい世代を
        // 古い世代で上書きし、飛ぶ前の絵が復活しうる (fail-closed)。
        if (generation < it->second) {
            generationRegressions_++;
            return GenerationUpdateResult::RejectedRegression;
        }
        // 同値は no-op。**pending を破棄しない。** 同じ generation の再設定
        // (例: 冪等な再同期) で表示待ちが消えると、正常な frame を落とす。
        if (generation == it->second)
            return GenerationUpdateResult::NoOp;
    }

    generations_[source] = generation;

    // 前進したときだけ、**その source の**古いフレームを捨てる。
    // 他の source の表示待ちには触らない (P1.2 §2)。
    for (auto p = pending_.begin(); p != pending_.end();) {
        if (p->sourceId == source && p->sourceGeneration < generation)
            p = pending_.erase(p);
        else
            ++p;
    }
    spaceAvailable_.notify_all();
    return GenerationUpdateResult::Updated;
}

SourceGeneration PreviewFrameQueue::currentGeneration(SourceId source) const {
    std::lock_guard<std::mutex> g(mutex_);
    const auto it = generations_.find(source);
    return it == generations_.end() ? SourceGeneration{} : it->second;
}

bool PreviewFrameQueue::knowsSource(SourceId source) const {
    std::lock_guard<std::mutex> g(mutex_);
    return generations_.find(source) != generations_.end();
}

void PreviewFrameQueue::setCompositionEpoch(CompositionEpoch epoch) {
    std::lock_guard<std::mutex> g(mutex_);
    compositionEpoch_ = epoch;
}

CompositionEpoch PreviewFrameQueue::compositionEpoch() const {
    std::lock_guard<std::mutex> g(mutex_);
    return compositionEpoch_;
}

SubmitResult PreviewFrameQueue::submitFrame(const DecodedGpuFrame& frame) {
    std::unique_lock<std::mutex> g(mutex_);

    if (stopped_)
        return SubmitResult::RejectedNotReady;

    if (!frame.valid()) {
        invalidRejects_++;
        return SubmitResult::RejectedInvalidFrame;
    }

    // **判定は source 単位で行う (P1.2 §2)。**
    // 他の source の generation は一切見ない。見ると、source A の seek で
    // source B のフレームが弾かれる。
    const auto it = generations_.find(frame.sourceId);
    if (it == generations_.end()) {
        // 知らない source からのフレームは受け取らない。
        // 「知らないなら通す」にすると、stop 済みの source が復活しうる。
        futureRejects_++;
        return SubmitResult::RejectedFutureGeneration;
    }
    if (frame.sourceGeneration < it->second) {
        staleRejects_++;
        return SubmitResult::RejectedStaleGeneration;
    }
    // 未来の generation は表示側がまだ知らない世代である。受け取ると、
    // まだ設定していない構成のフレームを表示してしまう。fail-closed で拒否する。
    if (it->second < frame.sourceGeneration) {
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
