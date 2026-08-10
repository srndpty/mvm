#include "preview_engine/preview_engine.h"

#include <type_traits>

static_assert(std::is_same_v<decltype(mvm::preview::PreviewSourceId::value), std::uint64_t>);
static_assert(std::is_copy_constructible_v<mvm::preview::PreviewStatus>);
static_assert(std::is_copy_constructible_v<mvm::preview::PreviewCapabilities>);
static_assert(std::is_copy_constructible_v<mvm::preview::PreviewTelemetry>);

int main() {
    const mvm::preview::PreviewCapabilities capabilities;
    return capabilities.maxQualifiedActiveVideoSources == 2 &&
                   capabilities.maxQualifiedCompositionLayers == 2 &&
                   capabilities.maxQualifiedActiveAudioSources == 1 &&
                   !capabilities.duplicateSourceLayersSupported &&
                   !capabilities.deviceRecoverySupported
               ? 0
               : 1;
}
