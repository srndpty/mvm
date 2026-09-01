#include "media/gpu_preview/compositor_coordinator.h"

#include "core/clip_fade.h"

#include <algorithm>
#include <limits>
#include <set>

namespace mvm::gpu {
namespace {

bool sameRect(const RectF& a, const RectF& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

bool sameLayout(const LayerLayout& a, const LayerLayout& b) {
    return a.sourceId == b.sourceId && sameRect(a.destination, b.destination) &&
           sameRect(a.sourceUv, b.sourceUv) && a.opacity == b.opacity && a.zOrder == b.zOrder &&
           a.effectsEnabled == b.effectsEnabled && a.rotationDegrees == b.rotationDegrees &&
           a.sourceInFrame == b.sourceInFrame && a.sourceDurationFrames == b.sourceDurationFrames &&
           a.fadeInFrames == b.fadeInFrames && a.fadeOutFrames == b.fadeOutFrames;
}

bool validLayout(const std::vector<LayerLayout>& layout,
                 const std::map<SourceId, SourceGeneration>& generations) {
    if (layout.empty() || layout.size() != generations.size())
        return false;
    std::set<SourceId> ids;
    for (const auto& layer : layout)
        if (layer.sourceId.value == 0 || !generations.contains(layer.sourceId) ||
            !ids.insert(layer.sourceId).second)
            return false;
    return true;
}

bool sameLayouts(const std::vector<LayerLayout>& a, const std::vector<LayerLayout>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), sameLayout);
}

} // namespace

float resolveLayerOpacity(const LayerLayout& layout, long long decodedSourceFrame) {
    double opacity = layout.opacity;
    if (layout.effectsEnabled) {
        const long long localFrame = decodedSourceFrame - layout.sourceInFrame;
        opacity *= core::clipFadeFactor(localFrame, layout.sourceDurationFrames,
                                        layout.fadeInFrames, layout.fadeOutFrames);
    }
    return static_cast<float>(opacity);
}

ConfigureResult
CompositorCoordinator::configure(std::vector<LayerLayout> layout,
                                 const std::map<SourceId, SourceGeneration>& generations) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (configured_)
        return ConfigureResult::RejectedAlreadyConfigured;
    if (!validLayout(layout, generations))
        return ConfigureResult::RejectedInvalid;
    layout_ = std::move(layout);
    generations_ = generations;
    epoch_ = CompositionEpoch{1};
    configured_ = true;
    return ConfigureResult::Configured;
}

LayoutUpdateResult CompositorCoordinator::updateLayout(std::vector<LayerLayout> layout) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!configured_ || !validLayout(layout, generations_))
        return LayoutUpdateResult::Rejected;
    if (sameLayouts(layout, layout_))
        return LayoutUpdateResult::NoOp;
    if (epoch_.value == std::numeric_limits<unsigned long long>::max())
        return LayoutUpdateResult::Rejected;
    layout_ = std::move(layout);
    ++epoch_.value;
    return LayoutUpdateResult::Updated;
}

CompositionStateAdoptionResult
CompositorCoordinator::adoptCompositionState(CompositionStateId requested) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!configured_ || !requested.valid())
        return CompositionStateAdoptionResult::Rejected;
    if (requested == state_)
        return CompositionStateAdoptionResult::NoOp;
    if (epoch_.value == std::numeric_limits<unsigned long long>::max())
        return CompositionStateAdoptionResult::Rejected;

    state_ = requested;
    ++epoch_.value;
    return CompositionStateAdoptionResult::Adopted;
}

CompositionStateAdoptionResult
CompositorCoordinator::adoptCompositionSnapshot(CompositionStateId requestedState,
                                                std::vector<LayerLayout> requestedLayout) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!configured_ || !requestedState.valid() || !validLayout(requestedLayout, generations_))
        return CompositionStateAdoptionResult::Rejected;

    if (requestedState == state_)
        return sameLayouts(requestedLayout, layout_) ? CompositionStateAdoptionResult::NoOp
                                                     : CompositionStateAdoptionResult::Rejected;
    if (epoch_.value == std::numeric_limits<unsigned long long>::max())
        return CompositionStateAdoptionResult::Rejected;

    state_ = requestedState;
    layout_ = std::move(requestedLayout);
    ++epoch_.value;
    return CompositionStateAdoptionResult::Adopted;
}

CompositionStateAdoptionResult CompositorCoordinator::adoptCompositionRuntimeSnapshot(
    CompositionStateId requestedState, std::vector<LayerLayout> requestedLayout,
    std::map<SourceId, SourceGeneration> requestedGenerations) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!requestedState.valid() || !validLayout(requestedLayout, requestedGenerations))
        return CompositionStateAdoptionResult::Rejected;

    // 現在追跡中のsourceのgenerationが巻き戻る要求は受理しない。
    // layoutから外れたsourceのgenerationは保持しないので、歴史的なfloorは持たない
    // (`SourceGeneration`のownerはsource側である)。
    for (const auto& [source, generation] : requestedGenerations) {
        const auto known = generations_.find(source);
        if (known != generations_.end() && generation < known->second)
            return CompositionStateAdoptionResult::Rejected;
    }

    if (configured_ && requestedState == state_ && !sameLayouts(requestedLayout, layout_)) {
        // 同じ`CompositionStateId`が別のlayoutを指すことは許さない。
        // ここを通すとstate idがcomposition identityを表さなくなる。
        return CompositionStateAdoptionResult::Rejected;
    }
    const bool sameState =
        configured_ && requestedState == state_ && sameLayouts(requestedLayout, layout_);
    if (sameState) {
        // resolved composition stateは変わっていない。generationの追随だけ行い
        // epochは進めない。epochはcomposition identityのownerであって、
        // source generationのownerではない。
        generations_ = std::move(requestedGenerations);
        return CompositionStateAdoptionResult::NoOp;
    }
    if (epoch_.value == std::numeric_limits<unsigned long long>::max())
        return CompositionStateAdoptionResult::Rejected;

    state_ = requestedState;
    layout_ = std::move(requestedLayout);
    generations_ = std::move(requestedGenerations);
    configured_ = true;
    ++epoch_.value;
    return CompositionStateAdoptionResult::Adopted;
}

bool CompositorCoordinator::advanceCompositionEpochForTest() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!configured_ || epoch_.value == std::numeric_limits<unsigned long long>::max())
        return false;
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

CompositionStateId CompositorCoordinator::compositionState() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

CompositionResult
CompositorCoordinator::validateSourcesLocked(const std::vector<DecodedGpuFrame>& frames) const {
    if (!configured_)
        return CompositionResult::MissingSource;
    if (frames.size() != layout_.size())
        return CompositionResult::MissingSource;
    std::set<SourceId> seen;
    for (const auto& frame : frames) {
        const auto expected = generations_.find(frame.sourceId);
        if (expected == generations_.end())
            return CompositionResult::UnknownSource;
        if (!seen.insert(frame.sourceId).second)
            return CompositionResult::MissingSource;
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
    std::vector<DecodedFrameEnvelope> envelopes;
    envelopes.reserve(frames.size());
    for (const auto& frame : frames)
        envelopes.push_back({frame.frameNumber, frame});
    return composeEnvelopes(outputFrameNumber, envelopes, out);
}

CompositionResult
CompositorCoordinator::composeEnvelopes(long long outputFrameNumber,
                                        const std::vector<DecodedFrameEnvelope>& frames,
                                        ComposedFrame& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DecodedGpuFrame> decodedFrames;
    decodedFrames.reserve(frames.size());
    for (const auto& envelope : frames) {
        if (envelope.outputFrameNumber != outputFrameNumber) {
            ++mixedFrame_;
            return CompositionResult::MixedFrame;
        }
        decodedFrames.push_back(envelope.frame);
    }
    const CompositionResult result = validateSourcesLocked(decodedFrames);
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
    out.compositionState = state_;
    for (const auto& spec : layout_) {
        const auto it =
            std::find_if(decodedFrames.begin(), decodedFrames.end(),
                         [&](const auto& frame) { return frame.sourceId == spec.sourceId; });
        CompositionLayerFrame layer;
        layer.frame = *it;
        layer.destination = spec.destination;
        layer.sourceUv = spec.sourceUv;
        layer.opacity = resolveLayerOpacity(spec, it->frameNumber);
        layer.zOrder = spec.zOrder;
        layer.effectsEnabled = spec.effectsEnabled;
        layer.rotationDegrees = spec.rotationDegrees;
        out.layers.push_back(std::move(layer));
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
    return validateSourcesLocked(frames);
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
