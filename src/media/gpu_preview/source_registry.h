#ifndef MVM_GPU_PREVIEW_SOURCE_REGISTRY_H
#define MVM_GPU_PREVIEW_SOURCE_REGISTRY_H

#include "media/gpu_preview/gpu_frame.h"

#include <mutex>
#include <set>

namespace mvm::gpu {

class SourceRegistry {
public:
    SourceId registerSource();
    bool unregisterSource(SourceId source);
    bool contains(SourceId source) const;
    size_t registeredSourceCount() const;

private:
    mutable std::mutex mutex_;
    unsigned long long next_ = 1;
    std::set<SourceId> sources_;
};

} // namespace mvm::gpu
#endif
