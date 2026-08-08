#include "media/gpu_preview/compositor_coordinator.h"

#include <algorithm>
#include <set>

namespace mvm::gpu {

bool CompositorCoordinator::configure(std::vector<LayerLayout> layout,
                                      const std::map<SourceId, SourceGeneration>& generations) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (layout.empty() || layout.size() != generations.size())
        return false;
    std::set<SourceId> ids;
    for (const auto& layer : layout) {
        if (layer.sourceId.value == 0 || !generations.contains(layer.sourceId) ||
            !ids.insert(layer.sourceId).second)
            return false;
    }
    layout_ = std::move(layout);
    generations_ = generations;
    epoch_ = CompositionEpoch{1};
    return true;
}

bool CompositorCoordinator::updateLayout(std::vector<LayerLayout> layout) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (layout.size() != layout_.size())
        return false;
    std::set<SourceId> ids;
    for (const auto& layer : layout) {
        if (!generations_.contains(layer.sourceId) || !ids.insert(layer.sourceId).second)
            return false;
    }
    layout_ = std::move(layout);
    ++epoch_.value;
    return true;
}

bool CompositorCoordinator::setSourceGeneration(SourceId source, SourceGeneration generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = generations_.find(source);
    if (it == generations_.end() || generation < it->second)
        return false;
    it->second = generation;
    return true;
}

SourceGeneration CompositorCoordinator::sourceGeneration(SourceId source) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = generations_.find(source);
    return it == generations_.end() ? SourceGeneration{} : it->second;
}

CompositionEpoch CompositorCoordinator::compositionEpoch() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return epoch_;
}

CompositionResult
CompositorCoordinator::validateLocked(long long outputFrameNumber,
                                      const std::vector<DecodedGpuFrame>& frames) const {
    if (frames.size() != layout_.size())
        return CompositionResult::MissingSource;
    std::set<SourceId> seen;
    for (const auto& frame : frames) {
        const auto expected = generations_.find(frame.sourceId);
        if (expected == generations_.end())
            return CompositionResult::UnknownSource;
        if (!seen.insert(frame.sourceId).second)
            return CompositionResult::MissingSource;
        if (frame.frameNumber != outputFrameNumber)
            return CompositionResult::MixedFrame;
        if (frame.sourceGeneration < expected->second)
            return CompositionResult::StaleGeneration;
        if (expected->second < frame.sourceGeneration)
            return CompositionResult::FutureGeneration;
    }
    return CompositionResult::Accepted;
}

CompositionResult CompositorCoordinator::compose(long long outputFrameNumber,
                                                 const std::vector<DecodedGpuFrame>& frames,
                                                 ComposedFrame& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    const CompositionResult result = validateLocked(outputFrameNumber, frames);
    if (result != CompositionResult::Accepted) {
        if (result == CompositionResult::MissingSource)
            ++missing_;
        else if (result == CompositionResult::MixedFrame)
            ++mixedFrame_;
        else if (result == CompositionResult::StaleGeneration ||
                 result == CompositionResult::FutureGeneration)
            ++mixedGeneration_;
        return result;
    }
    out = {};
    out.outputFrameNumber = outputFrameNumber;
    out.compositionEpoch = epoch_; // mutable global への参照ではなく値で固定
    for (const auto& spec : layout_) {
        const auto it = std::find_if(frames.begin(), frames.end(), [&](const auto& frame) {
            return frame.sourceId == spec.sourceId;
        });
        out.layers.push_back({*it, spec.destination, spec.sourceUv, spec.opacity, spec.zOrder});
    }
    std::stable_sort(out.layers.begin(), out.layers.end(), deterministicLayerLess);
    return CompositionResult::Accepted;
}

CompositionResult CompositorCoordinator::validateForDisplay(const ComposedFrame& frame) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (frame.compositionEpoch != epoch_) {
        ++staleEpoch_;
        return CompositionResult::StaleEpoch;
    }
    std::vector<DecodedGpuFrame> frames;
    frames.reserve(frame.layers.size());
    for (const auto& layer : frame.layers)
        frames.push_back(layer.frame);
    return validateLocked(frame.outputFrameNumber, frames);
}

long long CompositorCoordinator::mixedSourceFrameCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mixedFrame_;
}

long long CompositorCoordinator::mixedGenerationCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mixedGeneration_;
}

long long CompositorCoordinator::staleCompositionEpochCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return staleEpoch_;
}

long long CompositorCoordinator::missingSourceFrameCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return missing_;
}

} // namespace mvm::gpu
