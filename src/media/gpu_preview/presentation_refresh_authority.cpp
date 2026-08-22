#include "media/gpu_preview/presentation_refresh_authority.h"

#include <limits>

namespace mvm::gpu {

bool presentationAuthorityUsable(const PresentationAuthoritySample& value,
                                 long long refreshNumerator, long long refreshDenominator) {
    return value.available && value.refreshCount > 0 && value.qpcVBlank > 0 &&
           refreshNumerator > 0 && refreshDenominator > 0 &&
           value.refreshNumerator == refreshNumerator &&
           value.refreshDenominator == refreshDenominator;
}

bool presentationAuthorityMonotonic(const PresentationAuthoritySample& earlier,
                                    const PresentationAuthoritySample& later) {
    return later.refreshCount >= earlier.refreshCount && later.qpcVBlank >= earlier.qpcVBlank;
}

bool presentationOpportunityOrdinal(unsigned long long originRefreshCount,
                                    const PresentationAuthoritySample& sample, long long& ordinal) {
    if (sample.refreshCount < originRefreshCount)
        return false;
    const unsigned long long delta = sample.refreshCount - originRefreshCount;
    if (delta > static_cast<unsigned long long>(std::numeric_limits<long long>::max()))
        return false;
    ordinal = static_cast<long long>(delta);
    return true;
}

} // namespace mvm::gpu
