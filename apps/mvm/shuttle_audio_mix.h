#ifndef MVM_APPS_MVM_SHUTTLE_AUDIO_MIX_H
#define MVM_APPS_MVM_SHUTTLE_AUDIO_MIX_H

#include "project/project.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mvm::app {

// シャトル音声で「どの出力 sample にどの素材 sample を置くか」を決める部分。
// WASAPI にも decoder にも依存しないので、素材 sample の供給元を差し替えて検査できる。
struct ShuttleAudioClip {
    std::string path; // UTF-8
    std::int64_t timelineStartSample = 0;
    std::int64_t timelineEndSample = 0;
    // 素材 sample = timeline sample + sourceOffset
    std::int64_t sourceOffset = 0;
    project::ClipEffects effects;
    std::int64_t timelineStartFrame = 0;
    std::int64_t sourceFpsNum = 0;
    std::int64_t sourceFpsDen = 1;
    std::int64_t sourceDuration = 0;
};

struct ShuttleAudioPlan {
    std::int64_t baseSample = 0;
    std::int64_t endSample = 0;
    int rate = 0;
    std::int64_t timelineFpsNum = 60;
    std::int64_t timelineFpsDen = 1;
    std::vector<ShuttleAudioClip> clips;
};

// シャトル音声の対象になる clip か。mute された audio track 上の clip は鳴らさない。
bool isShuttleAudibleClip(const project::Project& project, const project::TimelineClip& clip);
bool hasShuttleAudibleClip(const project::Project& project);

// 速度・timeline 範囲を検証し、鳴らす clip の sample 範囲を確定する。
// 鳴らす clip が 0 件でも成功する (呼び出し側が音声経路を作るかを決める)。
bool planShuttleAudio(const project::Project& project, int rate, std::int64_t baseFrame,
                      ShuttleAudioPlan& plan, std::string& error);

// plan.clips[clipIndex] の素材 sample [first, first + count) を stereo interleave で返す。
// 素材の終端より後ろは 0 で埋める。
using ShuttleSourceReader =
    std::function<bool(std::size_t clipIndex, std::int64_t first, std::int64_t count,
                       std::vector<float>& pcm, std::string& error)>;

// 出力 sample [outputStart, outputStart + sampleCount) を合成する。
// 重なった clip は加算し、最後に [-1, 1] へ clamp する。sampleCount は正でなければならない。
bool mixShuttleBlock(const ShuttleAudioPlan& plan, std::int64_t outputStart,
                     std::int64_t sampleCount, const ShuttleSourceReader& read,
                     std::vector<float>& pcm, std::string& error);

} // namespace mvm::app

#endif // MVM_APPS_MVM_SHUTTLE_AUDIO_MIX_H
