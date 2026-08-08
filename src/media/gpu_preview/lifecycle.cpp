#include "media/gpu_preview/lifecycle.h"

#include <chrono>

namespace mvm::gpu {

const char* toString(ShutdownState state) {
    switch (state) {
    case ShutdownState::Running:
        return "running";
    case ShutdownState::DecodeStopRequested:
        return "decode_stop_requested";
    case ShutdownState::DecodeStopped:
        return "decode_stopped";
    case ShutdownState::RenderTeardownRequested:
        return "render_teardown_requested";
    case ShutdownState::RenderTornDown:
        return "render_torn_down";
    case ShutdownState::FinalReportWritten:
        return "final_report_written";
    case ShutdownState::Exit:
        return "exit";
    }
    return "unknown";
}

bool LifecycleCoordinator::violationLocked() {
    ++violations_;
    return false;
}

bool LifecycleCoordinator::requestDecodeStop(const std::string& reason, bool fatal) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != ShutdownState::Running)
        return false;
    reason_ = reason;
    fatal_ = fatal;
    state_ = ShutdownState::DecodeStopRequested;
    changed_.notify_all();
    return true;
}

bool LifecycleCoordinator::noteDecodeStopped() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != ShutdownState::DecodeStopRequested)
        return violationLocked();
    state_ = ShutdownState::DecodeStopped;
    changed_.notify_all();
    return true;
}

bool LifecycleCoordinator::requestRenderTeardown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != ShutdownState::DecodeStopped)
        return violationLocked();
    state_ = ShutdownState::RenderTeardownRequested;
    changed_.notify_all();
    return true;
}

bool LifecycleCoordinator::mayTeardown() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == ShutdownState::RenderTeardownRequested;
}

bool LifecycleCoordinator::noteRenderTornDown(ShutdownReport report) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != ShutdownState::RenderTeardownRequested)
        return violationLocked();
    report.lifecycleOrderViolationCount = violations_;
    report_ = std::move(report);
    state_ = ShutdownState::RenderTornDown;
    changed_.notify_all();
    return true;
}

bool LifecycleCoordinator::waitForRenderTornDown(int timeoutMs, ShutdownReport& report) const {
    std::unique_lock<std::mutex> lock(mutex_);
    const bool done = changed_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                        [this] { return state_ >= ShutdownState::RenderTornDown; });
    if (done)
        report = report_;
    return done;
}

bool LifecycleCoordinator::noteFinalReportWritten() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != ShutdownState::RenderTornDown)
        return violationLocked();
    state_ = ShutdownState::FinalReportWritten;
    changed_.notify_all();
    return true;
}

bool LifecycleCoordinator::noteExit() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != ShutdownState::FinalReportWritten)
        return violationLocked();
    state_ = ShutdownState::Exit;
    changed_.notify_all();
    return true;
}

void LifecycleCoordinator::noteDestructorFallback() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++violations_;
    fatal_ = true;
    if (reason_.empty())
        reason_ = "renderer destructor fallback: decode worker の停止を確認できません";
    report_.teardownSuccess = false;
    report_.lifecycleOrderViolationCount = violations_;
    changed_.notify_all();
}

ShutdownState LifecycleCoordinator::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

std::string LifecycleCoordinator::reason() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return reason_;
}

bool LifecycleCoordinator::fatal() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return fatal_;
}

long long LifecycleCoordinator::orderViolationCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return violations_;
}

ShutdownReport LifecycleCoordinator::report() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return report_;
}

bool WorkerJoinBarrier::noteJoined(size_t workerIndex) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (workerIndex >= required_)
        return false;
    joined_.insert(workerIndex);
    return true;
}

bool WorkerJoinBarrier::allJoined() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return required_ > 0 && joined_.size() == required_;
}

size_t WorkerJoinBarrier::joinedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return joined_.size();
}

} // namespace mvm::gpu
