#ifndef MVM_CORE_WAVEFORM_PEAKS_H
#define MVM_CORE_WAVEFORM_PEAKS_H

#include <cstdint>
#include <string>
#include <vector>

namespace mvm::core {

// timeline 上の波形表示用の min/max peak。decode や描画には依存しない。
//
// 値は [-1, 1] の振幅を int8 (-127..127) へ量子化して持つ。表示の縦解像度は
// 高々数十 pixel なので、float で持つ意味が無く、1 時間の stereo でも約 11MB に収まる。
struct WaveformPeakLevel {
    std::int64_t samplesPerPeak = 0;
    std::int64_t peakCount = 0;
    // channel-major。channel c の i 番目は [c * peakCount + i]。
    std::vector<std::int8_t> minimum;
    std::vector<std::int8_t> maximum;
};

struct WaveformPeaks {
    int sampleRate = 0;
    int channels = 0;
    // levels[0] が最も細かい。以降は kWaveformLevelFactor 倍ずつ粗くなる。
    // zoom out 時に 1 pixel あたり数千 peak を走査しないためのもの。
    std::vector<WaveformPeakLevel> levels;
};

inline constexpr std::int64_t kWaveformLevelFactor = 4;

// 素材の sample rate から最細 level の peak 幅を決める。約 1/750 秒。
// timeline の最大 zoom (24 px/frame @ 60fps = 1440 px/s) で 1 peak が 2 pixel 程度になる。
std::int64_t waveformSamplesPerPeak(int sampleRate);

class WaveformPeakBuilder final {
public:
    bool reset(int sampleRate, int channels, std::int64_t samplesPerPeak, std::string& error);

    // planar float の sample 列を、素材内 sample 位置 startSample から置く。
    // 位置で置くので、decode 結果に隙間や重なりがあっても時刻がずれない。
    // 素材開始より前 (負の位置) の sample は捨てる。
    bool addPlanar(std::int64_t startSample, const float* const* planes, int frameCount,
                   std::string& error);

    // 最後に sample を置いた位置までを peak 化し、粗い level も作る。
    // 1 sample も置かれていなければ失敗にする (空の波形を成功にしない)。
    bool finish(WaveformPeaks& peaks, std::string& error);

private:
    int sampleRate_ = 0;
    int channels_ = 0;
    std::int64_t samplesPerPeak_ = 0;
    std::int64_t peakCount_ = 0;
    // channel ごとの未量子化 min/max。未到達の bucket は無音 (0, 0)。
    std::vector<std::vector<float>> minimum_;
    std::vector<std::vector<float>> maximum_;
};

struct WaveformColumn {
    bool valid = false;
    float minimum = 0.0f;
    float maximum = 0.0f;
};

// 素材内の時刻区間 [startSeconds, endSeconds) の min/max を返す。
// 区間幅に見合う level を選ぶ。素材の範囲外なら valid=false。
WaveformColumn waveformColumn(const WaveformPeaks& peaks, int channel, double startSeconds,
                              double endSeconds);

} // namespace mvm::core

#endif
