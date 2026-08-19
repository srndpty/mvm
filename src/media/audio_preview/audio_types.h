#ifndef MVM_AUDIO_PREVIEW_AUDIO_TYPES_H
#define MVM_AUDIO_PREVIEW_AUDIO_TYPES_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mvm::audio {

inline constexpr int kInternalSampleRate = 48000;
inline constexpr int kInternalChannels = 2;
inline constexpr int kQueueTargetMs = 250;
inline constexpr int kQueueHardMaxMs = 500;
inline constexpr int kAudioPrerollMs = 100;
inline constexpr int kPrerollTimeoutMs = 5000;
inline constexpr std::int64_t kQueueHardMaxSamples = 24000;
inline constexpr std::int64_t kQueueTargetSamples = 12000;
inline constexpr std::int64_t kAudioPrerollSamples = 4800;
inline constexpr std::int64_t kNoAudioPts = INT64_MIN;

// 検証アプリ (spike / smoke) が使う既定の endpoint session volume。
// P3 fixture には 1 kHz / amplitude 0.8 の marker 音が入っており、既定音量で
// CTest を回すと驚くほど大きい。これは Windows の per-process session volume
// であり、PCM そのものは変えないため、計測値・marker 判定には影響しない。
// 製品既定は unity のままとし、この値を使うのは検証アプリだけである。
inline constexpr float kVerificationSessionVolume = 0.15F;

struct SourceId {
    std::uint64_t value = 0;
    friend bool operator==(SourceId, SourceId) = default;
};

struct SourceGeneration {
    std::uint64_t value = 0;
    friend bool operator==(SourceGeneration, SourceGeneration) = default;

    friend bool operator<(SourceGeneration a, SourceGeneration b) { return a.value < b.value; }
};

struct AudioResourceEpoch {
    std::uint64_t value = 0;
    friend bool operator==(AudioResourceEpoch, AudioResourceEpoch) = default;
};

struct Rational {
    std::int64_t num = 0;
    std::int64_t den = 1;

    bool valid() const { return num > 0 && den > 0; }
};

struct AudioChunk {
    // internal PCM domain の sample 型。qualified domain の "float32" はこの型が根拠。
    using PcmSample = float;

    SourceId sourceId{};
    SourceGeneration sourceGeneration{};
    AudioResourceEpoch resourceEpoch{};
    std::int64_t startSample = -1;
    std::int64_t sampleCount = 0;
    std::int64_t pts = kNoAudioPts;
    Rational timeBase{};
    int sampleRate = 0;
    int channels = 0;
    std::shared_ptr<std::vector<PcmSample>> pcm;
    std::size_t offsetSamples = 0;

    bool valid() const {
        return sourceId.value != 0 && sourceGeneration.value != 0 && resourceEpoch.value != 0 &&
               startSample >= 0 && sampleCount > 0 && sampleRate == kInternalSampleRate &&
               channels == kInternalChannels && pcm &&
               pcm->size() >= (offsetSamples + static_cast<std::size_t>(sampleCount)) *
                                  static_cast<std::size_t>(channels);
    }
};

struct AudioFormatInfo {
    int sampleRate = 0;
    int channels = 0;
    std::string sampleFormat;
};

} // namespace mvm::audio

#endif
