#ifndef MVM_APPS_MVM_CLIP_SAMPLE_READER_H
#define MVM_APPS_MVM_CLIP_SAMPLE_READER_H

#include "media/audio_preview/audio_decode_worker.h"
#include "shuttle_audio_mix.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mvm::app {

// シャトル音声と scrub 音声が共有する、clip ごとの素材 sample の読み出し。
// plan.clips と同じ添字で decode worker を持ち、続きから読める要求は seek しない。
class ClipSampleReaders final {
public:
    // 前回読んだ位置から maxForwardSkip sample 未満だけ先の要求は、seek せずに
    // 続きから decode して手前を捨てる。seek より速い範囲だけに限る。
    void reset(std::size_t clipCount, std::int64_t maxForwardSkip);

    void clear() { readers_.clear(); }

    // clip の素材 sample [first, first + count) を stereo interleave で返す。
    // running が false になったら待ちを打ち切って失敗する。
    bool read(const ShuttleAudioClip& clip, std::size_t clipIndex, std::int64_t first,
              std::int64_t count, std::vector<float>& pcm, const std::atomic<bool>& running,
              std::string& error);

    // seek の完了待ちへ入った回数。decode 待ち中の停止を test で確かめるために使う。
    std::uint64_t seekWaitCount() const { return seekWaits_.load(); }

private:
    struct ClipReader {
        std::unique_ptr<audio::AudioDecodeWorker> worker;
        std::int64_t nextSample = -1;
    };

    std::int64_t maxForwardSkip_ = 0;
    std::vector<ClipReader> readers_;
    std::atomic<std::uint64_t> seekWaits_{0};
};

} // namespace mvm::app

#endif // MVM_APPS_MVM_CLIP_SAMPLE_READER_H
