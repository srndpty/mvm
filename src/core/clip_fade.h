#ifndef MVM_CORE_CLIP_FADE_H
#define MVM_CORE_CLIP_FADE_H

#include <cstdint>

namespace mvm::core {

// clip-local frameだけをauthorityとしてfade係数を求める。
// timeline frame、preview output ordinal、presentation ordinalを渡してはならない。
double clipFadeFactor(std::int64_t localFrame, std::int64_t clipDuration, std::int64_t fadeInFrames,
                      std::int64_t fadeOutFrames);

} // namespace mvm::core

#endif // MVM_CORE_CLIP_FADE_H
