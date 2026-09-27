#ifndef MVM_MEDIA_AUDIO_WAVEFORM_AUDIO_WAVEFORM_DECODER_H
#define MVM_MEDIA_AUDIO_WAVEFORM_AUDIO_WAVEFORM_DECODER_H

#include "core/waveform_peaks.h"

#include <atomic>
#include <string>

namespace mvm::audio {

struct AudioWaveformResult {
    bool success = false;
    bool cancelled = false;
    core::WaveformPeaks peaks;
    std::string error;
};

// 素材の best audio stream を頭から最後まで decode し、表示用 peak を作る。
//
// sample rate は素材のまま保つ。channel は mono / stereo なら素材のまま、
// 3ch 以上 (5.1 など) は stereo へ downmix する。表示は最大 2 行になる。
// decode 自体は preview の再生経路 (48kHz stereo へ変換) とは独立している。
// sample 位置は preview と同じく stream start_time を 0 とする。
//
// cancel が true になったら途中で打ち切り、cancelled=true で失敗を返す。
AudioWaveformResult decodeAudioWaveform(const std::string& utf8Path,
                                        const std::atomic<bool>* cancel = nullptr);

} // namespace mvm::audio

#endif
