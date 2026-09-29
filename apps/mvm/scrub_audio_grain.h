#ifndef MVM_APPS_MVM_SCRUB_AUDIO_GRAIN_H
#define MVM_APPS_MVM_SCRUB_AUDIO_GRAIN_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace mvm::app {

// scrub 音声は、drag 位置が変わるたびにその位置から等速で短い断片 (grain) を鳴らす。
// 位置が変わらなければ新しい grain は作らず、鳴らし終えたら無音になる (Premiere と同じ)。
// ここは WASAPI にも decoder にも依存しない部分で、sample は 48kHz stereo interleave。
inline constexpr std::int64_t kScrubGrainSamples = 3840; // 80 ms
inline constexpr std::int64_t kScrubGrainFadeSamples = 240; // 5 ms

// grain の先頭と末尾に線形 fade を掛ける。断片の切れ目で click が出ないようにする。
// 長さが fade 2 回分に満たない grain は、中央で折り返す三角形になる。
void applyScrubGrainFade(std::vector<float>& pcm, std::int64_t fadeSamples);

// 最新の scrub 位置を受け取り、前回取り出した位置と違うときだけ取り出させる。
class ScrubTargetLatch final {
public:
    void set(std::int64_t frame) { latest_ = frame; }
    // 前回 take した位置から変わっていれば、その位置を返す。
    std::optional<std::int64_t> take();

private:
    std::optional<std::int64_t> latest_;
    std::optional<std::int64_t> taken_;
};

// 出力 stream へ流す grain の残り。新しい grain が来たら、古い grain の未再生分は捨てる。
class ScrubGrainStream final {
public:
    void replace(std::vector<float> grain);
    // count sample を destination へ書き、grain 由来の sample 数を返す。尽きた分は無音。
    std::int64_t fill(float* destination, std::int64_t count);

private:
    std::vector<float> grain_;
    std::size_t offset_ = 0;
};

} // namespace mvm::app

#endif // MVM_APPS_MVM_SCRUB_AUDIO_GRAIN_H
