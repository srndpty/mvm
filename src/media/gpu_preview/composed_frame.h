/* P2 の source identity と composition snapshot の値境界。Qt 非依存。 */
#ifndef MVM_GPU_PREVIEW_COMPOSED_FRAME_H
#define MVM_GPU_PREVIEW_COMPOSED_FRAME_H

#include "media/gpu_preview/gpu_frame.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace mvm::gpu {

struct RectF {
    float x = 0.0f;
    float y = 0.0f;
    float width = 1.0f;
    float height = 1.0f;
};

struct SourceFrameIdentity {
    SourceId sourceId{};
    SourceGeneration sourceGeneration{};
    ResourceEpoch resourceEpoch{};
    long long frameNumber = -1;

    friend bool operator==(const SourceFrameIdentity&, const SourceFrameIdentity&) = default;
};

inline SourceFrameIdentity identityOf(const DecodedGpuFrame& frame) {
    return {frame.sourceId, frame.sourceGeneration, frame.resourceEpoch, frame.frameNumber};
}

struct CompositionLayerFrame {
    DecodedGpuFrame frame;
    RectF destination{}; // 出力に対する正規化座標
    RectF sourceUv{};
    float opacity = 1.0f;
    int zOrder = 0;
};

struct ComposedFrame {
    long long outputFrameNumber = -1;
    CompositionEpoch compositionEpoch{};
    std::vector<CompositionLayerFrame> layers;
    CompositionStateId compositionState{};
};

// GPU serial 完了まで全 layer の lifetime を一括保持する。
struct CompositionLifetimeAggregate {
    std::vector<FrameLifetimeToken> sourceLifetimes;
};

inline std::shared_ptr<void> aggregateLifetime(const ComposedFrame& frame) {
    auto aggregate = std::make_shared<CompositionLifetimeAggregate>();
    aggregate->sourceLifetimes.reserve(frame.layers.size());
    for (const auto& layer : frame.layers)
        aggregate->sourceLifetimes.push_back(layer.frame.lifetime);
    return aggregate;
}

// P1.2 の単一 frame adoption test との互換用。epoch は値で固定する。
inline ComposedFrame adoptForComposition(const DecodedGpuFrame& frame, CompositionEpoch epoch) {
    CompositionLayerFrame layer;
    layer.frame = frame;
    return {frame.frameNumber, epoch, {std::move(layer)}, {}};
}

inline bool deterministicLayerLess(const CompositionLayerFrame& a, const CompositionLayerFrame& b) {
    if (a.zOrder != b.zOrder)
        return a.zOrder < b.zOrder;
    return a.frame.sourceId < b.frame.sourceId;
}

inline float straightAlphaBlend(float source, float destination, float opacity) {
    const float a = std::clamp(opacity, 0.0f, 1.0f);
    return source * a + destination * (1.0f - a);
}

} // namespace mvm::gpu

#endif
