#include "media/gpu_preview/source_registry.h"

namespace mvm::gpu {

SourceId SourceRegistry::registerSource() {
    std::lock_guard<std::mutex> lock(mutex_);
    const SourceId id{next_++};
    sources_.insert(id);
    return id;
}

bool SourceRegistry::unregisterSource(SourceId source) {
    std::lock_guard<std::mutex> lock(mutex_);
    return sources_.erase(source) == 1;
}

bool SourceRegistry::contains(SourceId source) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sources_.contains(source);
}

size_t SourceRegistry::registeredSourceCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sources_.size();
}

} // namespace mvm::gpu
