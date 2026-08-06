/*
 * mvm Phase 1 / P1 - PTS と timeline frame number の相互変換
 *
 * seek 精度と marker 一致の判定はすべて frame number 上で行うので、
 * ここがずれると判定そのものが無意味になる。
 *
 * **double を経由しない。** 60000/1001 のような有理 fps を double で
 * 扱うと、長尺の後半で 1 frame ずれる。Phase 0 でも proxy の尺が
 * 1 frame 伸びる事故が実際に起きている (docs/research/proxy-notes.md)。
 *
 * 依存は gpu_frame.h の Rational だけ。FFmpeg にも D3D11 にも依存しない
 * (だから単体テストできる)。
 */

#ifndef MVM_GPU_PREVIEW_TIMEBASE_H
#define MVM_GPU_PREVIEW_TIMEBASE_H

#include "media/gpu_preview/gpu_frame.h"

namespace mvm::gpu {

// PTS -> frame number。
//
//   frame = round( (pts - startPts) * timeBase * frameRate )
//
// 中間積は 128bit で持つ。1080p60 / 60 秒程度なら 64bit でも溢れないが、
// time base が 1/1000000 (microsecond) の container では
// pts * tb.num * fps.num が容易に 10^18 を超える。
//
// 失敗時 (無効な有理数 / pts 未設定) は -1 を返す。
// 「分からなかった」を 0 (= 先頭フレーム) にしない。
long long ptsToFrameNumber(long long pts, long long startPts, Rational timeBase,
                           Rational frameRate);

// frame number -> PTS (切り捨てではなく最近傍)。
//
//   pts = startPts + round( frame / frameRate * (1/timeBase) )
//
// seek の目標 PTS を作るのに使う。
// ptsToFrameNumber との往復が一致することを自動テストで検査する。
long long frameNumberToPts(long long frameNumber, long long startPts, Rational timeBase,
                           Rational frameRate);

// 秒 -> PTS。seek の許容誤差を秒で指定するときに使う。
long long secondsToPts(double seconds, Rational timeBase);

} // namespace mvm::gpu

#endif // MVM_GPU_PREVIEW_TIMEBASE_H
