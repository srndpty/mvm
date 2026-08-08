#ifndef MVM_GPU_PREVIEW_COMPOSITOR_COORDINATOR_H
#define MVM_GPU_PREVIEW_COMPOSITOR_COORDINATOR_H

#include "media/gpu_preview/composed_frame.h"

#include <map>
#include <mutex>
#include <vector>

namespace mvm::gpu {

enum class CompositionResult {
    Accepted = 0,
    MissingSource,
    MixedFrame,
    StaleGeneration,
    FutureGeneration,
    StaleEpoch,
    UnknownSource,
};

enum class ConfigureResult {
    Configured = 0,
    RejectedInvalid,
    RejectedAlreadyConfigured,
};

enum class LayoutUpdateResult {
    Updated = 0,
    NoOp,
    Rejected,
};

struct LayerLayout {
    SourceId sourceId{};
    RectF destination{};
    RectF sourceUv{};
    float opacity = 1.0f;
    int zOrder = 0;
};

class CompositorCoordinator {
public:
    ConfigureResult configure(std::vector<LayerLayout> layout,
                              const std::map<SourceId, SourceGeneration>& generations);
    LayoutUpdateResult updateLayout(std::vector<LayerLayout> layout);
    bool setSourceGeneration(SourceId source, SourceGeneration generation);
    SourceGeneration sourceGeneration(SourceId source) const;
    CompositionEpoch compositionEpoch() const;
    CompositionResult compose(long long outputFrameNumber,
                              const std::vector<DecodedGpuFrame>& frames, ComposedFrame& out);
    CompositionResult validateForDisplay(const ComposedFrame& frame) const;

    long long mixedSourceFrameCount() const;
    long long mixedGenerationCount() const;
    long long staleCompositionEpochCount() const;
    long long missingSourceFrameCount() const;

private:
    CompositionResult validateLocked(long long outputFrameNumber,
                                     const std::vector<DecodedGpuFrame>& frames) const;
    mutable std::mutex mutex_;
    std::vector<LayerLayout> layout_;
    std::map<SourceId, SourceGeneration> generations_;
    CompositionEpoch epoch_{};
    bool configured_ = false;
    long long mixedFrame_ = 0;
    long long mixedGeneration_ = 0;
    mutable long long staleEpoch_ = 0;
    long long missing_ = 0;
};

} // namespace mvm::gpu
#endif
