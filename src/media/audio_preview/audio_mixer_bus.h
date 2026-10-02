#ifndef MVM_AUDIO_PREVIEW_AUDIO_MIXER_BUS_H
#define MVM_AUDIO_PREVIEW_AUDIO_MIXER_BUS_H

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <vector>

namespace mvm::audio {

// 制御値と計測結果だけをスレッド間で共有する。PCMの集計は出力スレッドだけが行う。
struct AudioMixerBus {
    std::atomic<float> leftGain{1.0F};
    std::atomic<float> rightGain{1.0F};
    std::atomic<float> peakLeft{0.0F};
    std::atomic<float> peakRight{0.0F};
    std::atomic<bool> clipped{false};
    std::vector<float> scratch;

    static void recordPeak(std::atomic<float>& peak, float value) {
        float previous = peak.load(std::memory_order_relaxed);
        while (previous < value &&
               !peak.compare_exchange_weak(previous, value, std::memory_order_relaxed)) {
        }
    }

    void beginBlock(std::size_t values) {
        scratch.resize(values);
        std::fill(scratch.begin(), scratch.end(), 0.0F);
    }

    void addBlock(const float* pcm, std::size_t values) {
        for (std::size_t i = 0; i < values; ++i)
            scratch[i] += pcm[i];
    }

    void publishBlock() {
        float left = 0.0F, right = 0.0F;
        for (std::size_t i = 0; i + 1 < scratch.size(); i += 2) {
            left = std::max(left, std::fabs(scratch[i]));
            right = std::max(right, std::fabs(scratch[i + 1]));
        }
        recordPeak(peakLeft, left);
        recordPeak(peakRight, right);
        if (left > 1.0F || right > 1.0F)
            clipped.store(true, std::memory_order_relaxed);
    }
};

} // namespace mvm::audio
#endif
