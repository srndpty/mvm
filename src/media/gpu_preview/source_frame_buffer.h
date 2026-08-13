#ifndef MVM_GPU_PREVIEW_SOURCE_FRAME_BUFFER_H
#define MVM_GPU_PREVIEW_SOURCE_FRAME_BUFFER_H

#include "media/gpu_preview/composed_frame.h"
#include "media/gpu_preview/preview_surface.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>

namespace mvm::gpu {

struct SourceFrameBufferSnapshot {
    SourceGeneration generation{};
    long long displayedFrame = -1;
    long long frontFrame = -1;
    long long backFrame = -1;
    size_t depth = 0;
    bool stopped = false;
};

enum class SourceBufferSpaceWaitResult { SpaceAvailable, Interrupted, TimedOut, Stopped };

// 1 source 専用の bounded buffer。他 source の stop/seek を表現する API を持たない。
class SourceFrameBuffer final : public IPreviewSurface {
public:
    SourceFrameBuffer(SourceId source, SourceGeneration generation, size_t capacity = 3);
    SubmitResult submitFrame(const DecodedGpuFrame& frame) override;
    void clear() override;
    long long displayedFrameNumber() const override;
    GenerationUpdateResult setGeneration(SourceGeneration generation);
    SourceGeneration generation() const;
    bool take(DecodedGpuFrame& frame);
    bool peekFrontIdentity(SourceFrameIdentity& identity) const;
    bool takeExact(long long frameNumber, DecodedGpuFrame& frame);
    // 2 source の front を単一 transaction で取得する。両方が requested と
    // 一致する場合だけ commit し、一方でも変化していればどちらも残す。
    static bool takeExactPair(SourceFrameBuffer& a, SourceFrameBuffer& b, long long frameNumber,
                              DecodedGpuFrame& frameA, DecodedGpuFrame& frameB);
    size_t discardBefore(long long frameNumber);
    void noteDisplayed(long long frameNumber);
    bool waitForSpace(int timeoutMs);
    SourceBufferSpaceWaitResult
    waitForSpaceInterruptible(int timeoutMs, const std::function<bool()>& interruptPredicate);
    void notifyWaiters();
    bool waitForFrame(int timeoutMs);
    void stop();
    void restart();
    bool stopped() const;
    size_t depth() const;
    SourceFrameBufferSnapshot snapshot() const;

    size_t capacity() const { return capacity_; }

private:
    SourceId source_{};
    SourceGeneration generation_{};
    size_t capacity_ = 3;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<DecodedGpuFrame> frames_;
    long long displayed_ = -1;
    bool stopped_ = false;
};

} // namespace mvm::gpu
#endif
