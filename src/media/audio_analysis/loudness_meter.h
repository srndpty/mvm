#ifndef MVM_AUDIO_ANALYSIS_LOUDNESS_METER_H
#define MVM_AUDIO_ANALYSIS_LOUDNESS_METER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace mvm::audio {
struct LoudnessMeasurement {
    bool measurable = false;
    double integratedLufs = 0;
    double truePeakDb = 0;
    std::int64_t samples = 0;
};

// 48 kHz・ステレオの PCM を少量ずつ測定する。FFmpeg の型を公開しない。
class LoudnessMeter final {
public:
    LoudnessMeter();
    ~LoudnessMeter();
    bool start(std::string& error);
    bool push(const float* stereo, std::size_t frames, std::string& error);
    bool finish(LoudnessMeasurement& result, std::string& error);

private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace mvm::audio
#endif
