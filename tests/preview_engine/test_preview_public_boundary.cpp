#include "preview_engine/preview_engine.h"

#include <type_traits>

static_assert(std::is_same_v<decltype(mvm::preview::PreviewSourceId::value), std::uint64_t>);
static_assert(std::is_copy_constructible_v<mvm::preview::PreviewStatus>);
static_assert(std::is_copy_constructible_v<mvm::preview::PreviewCapabilities>);
static_assert(std::is_copy_constructible_v<mvm::preview::PreviewTelemetry>);

int main() {
    const mvm::preview::PreviewCapabilities capabilities;
    return capabilities.maxQualifiedActiveVideoSources == 1 &&
                   capabilities.maxQualifiedCompositionLayers == 1 &&
                   capabilities.maxQualifiedActiveAudioSources == 0 &&
                   capabilities.qualifiedAudioSampleRate == 0 &&
                   capabilities.qualifiedAudioChannelCount == 0 &&
                   !capabilities.duplicateSourceLayersSupported &&
                   !capabilities.deviceRecoverySupported
               ? 0
               : 1;
}
