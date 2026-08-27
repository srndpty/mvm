#include "media/gpu_preview/presentation_opportunity_attribution.h"

#include <algorithm>

namespace mvm::gpu {

void PresentationOpportunityRing::reset() {
    renderOverflow_.store(0, std::memory_order_relaxed);
    swapOverflow_.store(0, std::memory_order_relaxed);
    renderCount_.store(0, std::memory_order_release);
    swapCount_.store(0, std::memory_order_release);
}

void PresentationOpportunityRing::captureRender(const PresentationRenderRecord& record) {
    const std::size_t index = renderCount_.load(std::memory_order_relaxed);
    if (index >= renders_.size()) {
        renderOverflow_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    renders_[index] = record;
    renderCount_.store(index + 1, std::memory_order_release);
}

void PresentationOpportunityRing::captureSwap(const PresentationSwapRecord& record) {
    const std::size_t index = swapCount_.load(std::memory_order_relaxed);
    if (index >= swaps_.size()) {
        swapOverflow_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    swaps_[index] = record;
    swapCount_.store(index + 1, std::memory_order_release);
}

std::vector<PresentationRenderRecord> PresentationOpportunityRing::renderSnapshot() const {
    const std::size_t count = renderPublishedCount();
    return {renders_.begin(), renders_.begin() + static_cast<std::ptrdiff_t>(count)};
}

std::vector<PresentationSwapRecord> PresentationOpportunityRing::swapSnapshot() const {
    const std::size_t count = swapPublishedCount();
    return {swaps_.begin(), swaps_.begin() + static_cast<std::ptrdiff_t>(count)};
}

bool PresentationOpportunityRing::readSwap(std::size_t index,
                                           PresentationSwapRecord& record) const {
    if (index >= swapPublishedCount())
        return false;
    record = swaps_[index];
    return true;
}

std::size_t PresentationOpportunityRing::renderPublishedCount() const {
    return std::min(renderCount_.load(std::memory_order_acquire), renders_.size());
}

std::size_t PresentationOpportunityRing::swapPublishedCount() const {
    return std::min(swapCount_.load(std::memory_order_acquire), swaps_.size());
}

long long PresentationOpportunityRing::renderOverflowCount() const {
    return renderOverflow_.load(std::memory_order_acquire);
}

long long PresentationOpportunityRing::swapOverflowCount() const {
    return swapOverflow_.load(std::memory_order_acquire);
}

} // namespace mvm::gpu
