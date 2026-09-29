#include "media/audio_preview/pitch_preserving_stretcher.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

// 1 秒の impulse を blockFrames ずつ 2 倍へ伸ばし、総尺と impulse の位置を検査する。
// blockFrames が stretcher の 1 回の上限を超える場合は、内部で分割して渡す経路を通る。
bool stretchImpulse(std::size_t blockFrames) {
    mvm::audio::PitchPreservingStretcher stretcher(2.0);
    if (!stretcher.valid()) {
        std::fprintf(stderr, "ピッチ保持 stretcher を作成できません\n");
        return false;
    }
    constexpr std::size_t kInputFrames = 48000;
    std::vector<float> input(kInputFrames * 2, 0.0F);
    input[24000 * 2] = 1.0F;
    input[24000 * 2 + 1] = 1.0F;
    std::vector<float> output;
    for (std::size_t offset = 0; offset < kInputFrames; offset += blockFrames) {
        const auto frames = std::min<std::size_t>(blockFrames, kInputFrames - offset);
        auto chunk = stretcher.process(input.data() + offset * 2, frames, false);
        output.insert(output.end(), chunk.begin(), chunk.end());
    }
    const auto beforeFlush = output.size() / 2;
    auto tail = stretcher.finish();
    output.insert(output.end(), tail.begin(), tail.end());
    // finish は遅延分の無音で押し出すので本来の尺 (96000) を下回らない。上限は遅延の分だけ緩める。
    if (tail.empty() || beforeFlush >= output.size() / 2 || output.size() / 2 < 96000 ||
        output.size() / 2 > 96000 + 8192) {
        std::fprintf(stderr, "block %zu: 最終 flush または伸縮後の総尺が不正です (%zu)\n",
                     blockFrames, output.size() / 2);
        return false;
    }
    std::size_t peakFrame = 0;
    float peak = 0.0F;
    for (std::size_t frame = 0; frame < output.size() / 2; ++frame) {
        const float amplitude = std::abs(output[frame * 2]);
        if (amplitude > peak) {
            peak = amplitude;
            peakFrame = frame;
        }
    }
    if (peak < 0.05F || peakFrame < 44000 || peakFrame > 52000) {
        std::fprintf(stderr, "block %zu: impulse の出力位置が伸縮後の時間軸と一致しません: %zu\n",
                     blockFrames, peakFrame);
        return false;
    }
    std::fprintf(stderr, "pitch stretcher block %zu: peak=%zu, frames=%zu\n", blockFrames,
                 peakFrame, output.size() / 2);
    return true;
}

} // namespace

int main() {
    // 256: 通常の小さな decode frame。48000: 上限 (4096) を超える 1 回の入力。
    const bool small = stretchImpulse(256);
    const bool large = stretchImpulse(48000);
    if (!small || !large)
        return 1;
    std::fprintf(stderr, "pitch stretcher: PASS\n");
    return 0;
}
