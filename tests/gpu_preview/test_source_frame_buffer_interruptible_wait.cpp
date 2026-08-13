#include "media/gpu_preview/source_frame_buffer.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <string>

namespace {

using namespace mvm::gpu;
using namespace std::chrono_literals;

bool require(bool condition, const std::string& message) {
    if (condition)
        return true;
    std::cerr << "FAIL: " << message << '\n';
    return false;
}

DecodedGpuFrame makeFrame(long long frameNumber) {
    DecodedGpuFrame frame;
    frame.frameNumber = frameNumber;
    frame.pts = frameNumber;
    frame.timeBase = Rational{1, 60};
    frame.width = 16;
    frame.height = 16;
    frame.pixelFormat = GpuPixelFormat::NV12;
    frame.texture = reinterpret_cast<ID3D11Texture2D*>(0x1000 + frameNumber);
    frame.sourceId = SourceId{1};
    frame.sourceGeneration = SourceGeneration{1};
    frame.resourceEpoch = ResourceEpoch{1};
    frame.lifetime = FrameLifetimeToken(reinterpret_cast<void*>(1), [](void*) {});
    return frame;
}

class PredicateProbe {
public:
    bool observe(const std::atomic<bool>& interrupted) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++observations_;
        }
        changed_.notify_all();
        return interrupted.load(std::memory_order_acquire);
    }

    bool waitUntilObserved(int expected, std::chrono::milliseconds timeout = 1000ms) {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, timeout,
                                 [this, expected] { return observations_ >= expected; });
    }

    int observations() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return observations_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    int observations_ = 0;
};

bool fill(SourceFrameBuffer& buffer, long long frameNumber) {
    return buffer.submitFrame(makeFrame(frameNumber)) == SubmitResult::Accepted;
}

bool testSpaceReleaseWake() {
    SourceFrameBuffer buffer(SourceId{1}, SourceGeneration{1}, 1);
    if (!require(fill(buffer, 1), "通常 wait 用 buffer を満杯にできる必要があります"))
        return false;
    std::atomic<bool> interrupted{false};
    PredicateProbe probe;
    auto result = std::async(std::launch::async, [&] {
        return buffer.waitForSpaceInterruptible(5000, [&] { return probe.observe(interrupted); });
    });
    if (!require(probe.waitUntilObserved(1), "full buffer wait へ到達する必要があります") ||
        !require(result.wait_for(0ms) == std::future_status::timeout,
                 "command も space も無ければ wait を継続する必要があります")) {
        buffer.stop();
        result.wait();
        return false;
    }
    DecodedGpuFrame frame;
    return require(buffer.take(frame), "consumer が buffer space を解放できる必要があります") &&
           require(result.wait_for(1000ms) == std::future_status::ready,
                   "buffer space 通知で通常 wait が起床する必要があります") &&
           require(result.get() == SourceBufferSpaceWaitResult::SpaceAvailable,
                   "通常 playback は SpaceAvailable として復帰する必要があります");
}

bool testCommandInterruptWhileFull() {
    SourceFrameBuffer buffer(SourceId{1}, SourceGeneration{1}, 1);
    if (!require(fill(buffer, 2), "command interrupt 用 buffer を満杯にできる必要があります"))
        return false;
    std::atomic<bool> interrupted{false};
    PredicateProbe probe;
    auto result = std::async(std::launch::async, [&] {
        return buffer.waitForSpaceInterruptible(5000, [&] { return probe.observe(interrupted); });
    });
    if (!require(probe.waitUntilObserved(1), "command interrupt 前に wait へ到達する必要があります")) {
        buffer.stop();
        result.wait();
        return false;
    }
    interrupted.store(true, std::memory_order_release);
    buffer.notifyWaiters();
    return require(result.wait_for(1000ms) == std::future_status::ready,
                   "command 通知で timeout を待たず起床する必要があります") &&
           require(result.get() == SourceBufferSpaceWaitResult::Interrupted,
                   "full のまま control command を優先する必要があります") &&
           require(buffer.depth() == buffer.capacity(),
                   "command interrupt は space 解放に依存してはいけません");
}

bool testCommandWinsWithAvailableSpace() {
    SourceFrameBuffer buffer(SourceId{1}, SourceGeneration{1}, 1);
    if (!require(fill(buffer, 3), "priority test 用 buffer を満杯にできる必要があります"))
        return false;
    std::atomic<bool> interrupted{false};
    PredicateProbe probe;
    auto result = std::async(std::launch::async, [&] {
        return buffer.waitForSpaceInterruptible(5000, [&] { return probe.observe(interrupted); });
    });
    if (!require(probe.waitUntilObserved(1), "priority test の wait へ到達する必要があります")) {
        buffer.stop();
        result.wait();
        return false;
    }
    interrupted.store(true, std::memory_order_release);
    DecodedGpuFrame frame;
    return require(buffer.take(frame), "command pending 中に space も解放できる必要があります") &&
           require(result.wait_for(1000ms) == std::future_status::ready,
                   "同時成立した predicate で起床する必要があります") &&
           require(result.get() == SourceBufferSpaceWaitResult::Interrupted,
                   "space と command が同時なら command を優先する必要があります");
}

bool testStopWake() {
    SourceFrameBuffer buffer(SourceId{1}, SourceGeneration{1}, 1);
    if (!require(fill(buffer, 4), "stop test 用 buffer を満杯にできる必要があります"))
        return false;
    std::atomic<bool> interrupted{false};
    PredicateProbe probe;
    auto result = std::async(std::launch::async, [&] {
        return buffer.waitForSpaceInterruptible(5000, [&] { return probe.observe(interrupted); });
    });
    if (!require(probe.waitUntilObserved(1), "stop 前に wait へ到達する必要があります")) {
        buffer.stop();
        result.wait();
        return false;
    }
    buffer.stop();
    return require(result.wait_for(1000ms) == std::future_status::ready,
                   "stop が wait を起こす必要があります") &&
           require(result.get() == SourceBufferSpaceWaitResult::Stopped,
                   "stop を command や space と混同してはいけません");
}

bool testTimeoutFallback() {
    SourceFrameBuffer buffer(SourceId{1}, SourceGeneration{1}, 1);
    if (!require(fill(buffer, 5), "timeout test 用 buffer を満杯にできる必要があります"))
        return false;
    std::atomic<bool> interrupted{false};
    PredicateProbe probe;
    const auto begin = std::chrono::steady_clock::now();
    const auto result = buffer.waitForSpaceInterruptible(
        50, [&] { return probe.observe(interrupted); });
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    return require(result == SourceBufferSpaceWaitResult::TimedOut,
                   "command 無しの 50 ms timeout は fallback である必要があります") &&
           require(elapsed >= 40ms, "timeout 前に TimedOut を返してはいけません") &&
           require(probe.observations() < 100, "timeout wait で busy-spin してはいけません");
}

bool testRepeatedCommandHasNoLostWake() {
    for (int iteration = 0; iteration < 64; ++iteration) {
        SourceFrameBuffer buffer(SourceId{1}, SourceGeneration{1}, 1);
        if (!require(fill(buffer, 100 + iteration),
                     "反復 interrupt 用 buffer を満杯にできる必要があります"))
            return false;
        std::atomic<bool> interrupted{false};
        PredicateProbe probe;
        auto result = std::async(std::launch::async, [&] {
            return buffer.waitForSpaceInterruptible(
                5000, [&] { return probe.observe(interrupted); });
        });
        if (!require(probe.waitUntilObserved(1), "反復 interrupt で wait へ到達する必要があります")) {
            buffer.stop();
            result.wait();
            return false;
        }
        interrupted.store(true, std::memory_order_release);
        buffer.notifyWaiters();
        if (!require(result.wait_for(1000ms) == std::future_status::ready,
                     "反復 command で lost wake/deadlock があってはいけません") ||
            !require(result.get() == SourceBufferSpaceWaitResult::Interrupted,
                     "反復 command を Interrupted として返す必要があります"))
            return false;
    }
    return true;
}

} // namespace

int main() {
    if (!testSpaceReleaseWake() || !testCommandInterruptWhileFull() ||
        !testCommandWinsWithAvailableSpace() || !testStopWake() || !testTimeoutFallback() ||
        !testRepeatedCommandHasNoLostWake())
        return 1;
    std::cout << "PASS: interruptible initial-buffer wait semantics\n";
    return 0;
}
