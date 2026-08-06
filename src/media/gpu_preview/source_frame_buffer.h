#ifndef MVM_GPU_PREVIEW_SOURCE_FRAME_BUFFER_H
#define MVM_GPU_PREVIEW_SOURCE_FRAME_BUFFER_H

#include "media/gpu_preview/preview_surface.h"

#include <condition_variable>
#include <deque>
#include <mutex>

namespace mvm::gpu {

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
    void noteDisplayed(long long frameNumber);
    bool waitForSpace(int timeoutMs);
    void stop();
    void restart();
    bool stopped() const;
    size_t depth() const;

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
