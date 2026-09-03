#include "preview_engine/preview_engine.h"

#include <type_traits>

static_assert(std::is_same_v<decltype(mvm::preview::PreviewSourceId::value), std::uint64_t>);
static_assert(std::is_copy_constructible_v<mvm::preview::PreviewStatus>);
static_assert(std::is_copy_constructible_v<mvm::preview::PreviewCapabilities>);
static_assert(std::is_copy_constructible_v<mvm::preview::PreviewTelemetry>);

int main() {
    const mvm::preview::PreviewCapabilities capabilities;
    return capabilities.configuredMaxActiveVideoSources == 1 &&
                   capabilities.configuredMaxCompositionLayers == 1 &&
                   capabilities.configuredMaxActiveAudioSources == 0 &&
                   capabilities.configuredAudioSampleRate == 0 &&
                   capabilities.configuredAudioChannelCount == 0 &&
                   !capabilities.duplicateSourceLayersSupported &&
                   !capabilities.deviceRecoverySupported
               ? 0
               : 1;
}
