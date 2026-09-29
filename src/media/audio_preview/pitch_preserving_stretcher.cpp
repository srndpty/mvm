#include "media/audio_preview/pitch_preserving_stretcher.h"

#include <algorithm>
#include <cmath>

namespace mvm::audio {

namespace {
// 1 回の rubberband_process に渡す最大 frame 数。realtime mode は宣言した上限を超える
// 入力を想定しないため、decode した frame が大きい素材 (PCM 等) でも分割して渡す。
constexpr std::size_t kMaxProcessFrames = 4096;
} // namespace

PitchPreservingStretcher::PitchPreservingStretcher(double timeRatio) : timeRatio_(timeRatio) {
    if (timeRatio > 0.0)
        state_ = rubberband_new(48000, 2,
                                RubberBandOptionProcessRealTime | RubberBandOptionEngineFaster |
                                    RubberBandOptionChannelsTogether,
                                timeRatio, 1.0);
    if (state_) {
        rubberband_set_max_process_size(state_, static_cast<unsigned int>(kMaxProcessFrames));
        reset();
    }
}

PitchPreservingStretcher::~PitchPreservingStretcher() {
    if (state_)
        rubberband_delete(state_);
}

void PitchPreservingStretcher::reset() {
    if (!state_)
        return;
    rubberband_reset(state_);
    discard_ = rubberband_get_start_delay(state_);
    std::size_t pad = rubberband_get_preferred_start_pad(state_);
    std::vector<float> zero(std::min(pad, kMaxProcessFrames), 0.0F);
    const float* channels[] = {zero.data(), zero.data()};
    while (pad > 0) {
        const auto frames = std::min(pad, kMaxProcessFrames);
        rubberband_process(state_, channels, static_cast<unsigned int>(frames), 0);
        pad -= frames;
    }
}

std::vector<float> PitchPreservingStretcher::process(const float* interleaved, std::size_t frames,
                                                     bool final) {
    std::vector<float> output;
    const auto drain = [&]() {
        while (const int available = rubberband_available(state_)) {
            if (available < 0)
                break;
            std::vector<float> outLeft(static_cast<std::size_t>(available));
            std::vector<float> outRight(static_cast<std::size_t>(available));
            float* out[] = {outLeft.data(), outRight.data()};
            const auto received =
                rubberband_retrieve(state_, out, static_cast<unsigned int>(available));
            for (unsigned int i = 0; i < received; ++i) {
                if (discard_ > 0) {
                    --discard_;
                    continue;
                }
                output.push_back(outLeft[i]);
                output.push_back(outRight[i]);
            }
        }
    };
    std::vector<float> left, right;
    std::size_t offset = 0;
    // frames == 0 でも final を伝えるため、少なくとも 1 回は process を呼ぶ。
    do {
        const auto count = std::min(frames - offset, kMaxProcessFrames);
        left.resize(count);
        right.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            left[i] = interleaved[2 * (offset + i)];
            right[i] = interleaved[2 * (offset + i) + 1];
        }
        offset += count;
        const float* channels[] = {left.data(), right.data()};
        rubberband_process(state_, channels, static_cast<unsigned int>(count),
                           final && offset == frames ? 1 : 0);
        drain();
    } while (offset < frames);
    return output;
}

std::vector<float> PitchPreservingStretcher::finish() {
    // 出力側の遅延 (start delay) を入力の frame 数へ戻した分だけ無音を足す。
    const auto delay = static_cast<double>(rubberband_get_start_delay(state_));
    const auto padFrames = static_cast<std::size_t>(std::ceil(delay / timeRatio_)) + 1;
    const std::vector<float> silence(padFrames * 2, 0.0F);
    auto output = process(silence.data(), padFrames, false);
    auto tail = process(nullptr, 0, true);
    output.insert(output.end(), tail.begin(), tail.end());
    return output;
}

} // namespace mvm::audio
