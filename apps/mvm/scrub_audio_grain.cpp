#include "scrub_audio_grain.h"

#include "media/audio_preview/audio_types.h"

#include <algorithm>

namespace mvm::app {

void applyScrubGrainFade(std::vector<float>& pcm, std::int64_t fadeSamples) {
    const auto samples = static_cast<std::int64_t>(pcm.size()) / audio::kInternalChannels;
    if (samples <= 0 || fadeSamples <= 0)
        return;
    for (std::int64_t i = 0; i < samples; ++i) {
        // 端から何 sample 目か。0 番目と最後の sample は gain 0 になる。
        const std::int64_t edge = std::min(i, samples - 1 - i);
        if (edge >= fadeSamples)
            continue;
        const float gain = static_cast<float>(edge) / static_cast<float>(fadeSamples);
        for (int channel = 0; channel < audio::kInternalChannels; ++channel)
            pcm[static_cast<std::size_t>(i * audio::kInternalChannels + channel)] *= gain;
    }
}

std::optional<std::int64_t> ScrubTargetLatch::take() {
    if (!latest_ || latest_ == taken_)
        return std::nullopt;
    taken_ = latest_;
    return latest_;
}

void ScrubGrainStream::replace(std::vector<float> grain) {
    grain_ = std::move(grain);
    offset_ = 0;
}

std::int64_t ScrubGrainStream::fill(float* destination, std::int64_t count) {
    if (count <= 0)
        return 0;
    const auto total = static_cast<std::size_t>(count) * audio::kInternalChannels;
    const std::size_t available = grain_.size() - offset_;
    const std::size_t copied = std::min(total, available);
    std::copy_n(grain_.begin() + static_cast<std::ptrdiff_t>(offset_), copied, destination);
    std::fill(destination + copied, destination + total, 0.0F);
    offset_ += copied;
    return static_cast<std::int64_t>(copied) / audio::kInternalChannels;
}

} // namespace mvm::app
