#include "media/gpu_preview/device_change.h"

namespace mvm::gpu {

const char* toString(DeviceChangeState s) {
    switch (s) {
    case DeviceChangeState::None:
        return "none";
    case DeviceChangeState::Detected:
        return "detected";
    case DeviceChangeState::WorkerStopped:
        return "worker_stopped";
    case DeviceChangeState::TornDown:
        return "torn_down";
    }
    return "unknown";
}

void DeviceChangeCoordinator::noteDetected(const std::string& reason) {
    std::lock_guard<std::mutex> g(mutex_);
    if (state_ != DeviceChangeState::None)
        return; // すでに進行中。段階を巻き戻さない
    state_ = DeviceChangeState::Detected;
    reason_ = reason;
    detected_++;
}

bool DeviceChangeCoordinator::mayTeardown() const {
    std::lock_guard<std::mutex> g(mutex_);
    // **decode thread の join が済むまで teardown させない。**
    // ここを緩めると、decode 中の device を Release することになる。
    return state_ == DeviceChangeState::WorkerStopped;
}

void DeviceChangeCoordinator::noteTornDown() {
    std::lock_guard<std::mutex> g(mutex_);
    if (state_ == DeviceChangeState::WorkerStopped)
        state_ = DeviceChangeState::TornDown;
}

bool DeviceChangeCoordinator::detected() const {
    std::lock_guard<std::mutex> g(mutex_);
    return state_ == DeviceChangeState::Detected;
}

void DeviceChangeCoordinator::noteWorkerStopped() {
    std::lock_guard<std::mutex> g(mutex_);
    if (state_ == DeviceChangeState::Detected)
        state_ = DeviceChangeState::WorkerStopped;
}

DeviceChangeState DeviceChangeCoordinator::state() const {
    std::lock_guard<std::mutex> g(mutex_);
    return state_;
}

std::string DeviceChangeCoordinator::reason() const {
    std::lock_guard<std::mutex> g(mutex_);
    return reason_;
}

long long DeviceChangeCoordinator::detectedCount() const {
    std::lock_guard<std::mutex> g(mutex_);
    return detected_;
}

long long DeviceChangeCoordinator::failClosedCount() const {
    std::lock_guard<std::mutex> g(mutex_);
    return failClosed_;
}

long long DeviceChangeCoordinator::handledCount() const {
    std::lock_guard<std::mutex> g(mutex_);
    return handled_;
}

void DeviceChangeCoordinator::noteFullyRecovered() {
    std::lock_guard<std::mutex> g(mutex_);
    handled_++;
}

void DeviceChangeCoordinator::noteFailClosed() {
    std::lock_guard<std::mutex> g(mutex_);
    failClosed_++;
}

void DeviceChangeCoordinator::reset() {
    std::lock_guard<std::mutex> g(mutex_);
    state_ = DeviceChangeState::None;
    reason_.clear();
}

} // namespace mvm::gpu
