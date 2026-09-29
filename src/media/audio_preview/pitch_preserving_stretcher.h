#ifndef MVM_AUDIO_PREVIEW_PITCH_PRESERVING_STRETCHER_H
#define MVM_AUDIO_PREVIEW_PITCH_PRESERVING_STRETCHER_H

#include <cstddef>
#include <rubberband/rubberband-c.h>
#include <vector>

namespace mvm::audio {

class PitchPreservingStretcher {
public:
    explicit PitchPreservingStretcher(double timeRatio);
    ~PitchPreservingStretcher();
    PitchPreservingStretcher(const PitchPreservingStretcher&) = delete;
    PitchPreservingStretcher& operator=(const PitchPreservingStretcher&) = delete;

    bool valid() const { return state_ != nullptr; }

    void reset();
    std::vector<float> process(const float* interleaved, std::size_t frames, bool final);
    // 入力の終端。内部遅延に残った末尾を無音の入力で押し出してから final を伝える。
    // 出力は押し出した無音の分だけ本来の尺より長くなりうるので、呼び出し側が終端で切り詰める。
    std::vector<float> finish();

private:
    RubberBandState state_ = nullptr;
    double timeRatio_ = 1.0;
    std::size_t discard_ = 0;
};

} // namespace mvm::audio
#endif
