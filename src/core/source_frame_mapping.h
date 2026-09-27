#ifndef MVM_CORE_SOURCE_FRAME_MAPPING_H
#define MVM_CORE_SOURCE_FRAME_MAPPING_H

#include <cstdint>
#include <optional>

namespace mvm::core {

// 素材 frame と output (timeline) frame の対応を 1 か所で決める。
// Project・preview・書き出しのすべてがこれを使う。
//
// 位置は「素材の 0 frame を原点にした output frame 位置」p で数える。
// R = output fps / 素材 fps とすると:
//
//   境界      素材境界 s の output 位置 = ceil(s R)          (clip の尺と cut の範囲)
//   表示      output 位置 p が表示する素材 frame = floor(p / R + 1/2)
//
// 表示の四捨五入は MLT の avformat / timewarp producer と同じである
// (docs/premiere-like-editing.md §16.7 で実測)。preview が floor だと書き出しと
// 最大 1 素材 frame ずれる。ちょうど 1/2 の境界だけは MLT が double で計算するため
// まれに下へ丸まる。これは再現せず、有理数で厳密に計算する。
struct FrameRate {
    std::int64_t num = 0;
    std::int64_t den = 1;
};

// rate x factor を約分して返す。速度 s の clip の実効 fps (素材 fps x s) に使う。
// どちらかが正でない、または積を int64 で表せなければ nullopt。
std::optional<FrameRate> multiplyFrameRate(FrameRate rate, FrameRate factor);

// 素材境界 s の output 位置。roundUp なら ceil(s R)、そうでなければ floor(s R)。
// from / to を入れ替えれば output 境界 -> 素材境界にも使える。
std::optional<std::int64_t> convertFrameBoundary(std::int64_t frame, FrameRate from, FrameRate to,
                                                 bool roundUp);

// output 位置 p が表示する素材 frame floor(p / R + 1/2)。p >= 0。
std::optional<std::int64_t> sourceFrameAtOutputPosition(std::int64_t outputPosition,
                                                        FrameRate source, FrameRate output);

// 素材 frame s を表示する最初の output 位置 ceil((s - 1/2) R)。0 未満は 0 にする。
// sourceFrameAtOutputPosition の逆で、s の表示区間は [first(s), first(s + 1)) になる。
std::optional<std::int64_t> firstOutputPositionOfSourceFrame(std::int64_t sourceFrame,
                                                             FrameRate source, FrameRate output);

} // namespace mvm::core

#endif // MVM_CORE_SOURCE_FRAME_MAPPING_H
