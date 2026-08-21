#ifndef MVM_GPU_PREVIEW_PRESENTATION_OPPORTUNITY_ATTRIBUTION_H
#define MVM_GPU_PREVIEW_PRESENTATION_OPPORTUNITY_ATTRIBUTION_H

#include <array>
#include <atomic>
#include <cstddef>
#include <vector>

namespace mvm::gpu {

constexpr std::size_t kPresentationRenderRingCapacity = 8192;
constexpr std::size_t kPresentationSwapRingCapacity = 8192;

struct PresentationRenderRecord {
    long long callbackBeginQpc = 0;
    long long renderEndQpc = 0;
    long long renderOrdinal = -1;
    long long selectedOutputFrame = -1;
    long long submittedOutputFrame = -1;
    long long schedulerSkippedDeadlineCount = 0;
    bool repeated = true;
};

struct PresentationSwapRecord {
    long long swapQpc = 0;
    long long swapOrdinal = -1;
    long long completedRenderOrdinal = -1;
    long long submittedRenderOrdinal = -1;
    long long presentedOutputFrame = -1;
};

// render callbackとframeSwapped hookは別arrayへ書く。それぞれsingle writerで、
// hot pathではallocation、mutex、file I/O、loggingを行わない。
class PresentationOpportunityRing {
public:
    void reset();
    void captureRender(const PresentationRenderRecord& record);
    void captureSwap(const PresentationSwapRecord& record);
    std::vector<PresentationRenderRecord> renderSnapshot() const;
    std::vector<PresentationSwapRecord> swapSnapshot() const;
    std::size_t renderPublishedCount() const;
    std::size_t swapPublishedCount() const;
    long long renderOverflowCount() const;
    long long swapOverflowCount() const;

private:
    std::array<PresentationRenderRecord, kPresentationRenderRingCapacity> renders_{};
    std::array<PresentationSwapRecord, kPresentationSwapRingCapacity> swaps_{};
    std::atomic<std::size_t> renderCount_{0};
    std::atomic<std::size_t> swapCount_{0};
    std::atomic<long long> renderOverflow_{0};
    std::atomic<long long> swapOverflow_{0};
};

} // namespace mvm::gpu

#endif
