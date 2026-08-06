#include "media/gpu_preview/timebase.h"

namespace mvm::gpu {
namespace {

// GCC 拡張。-Wpedantic は警告するが、ここは 64bit では溢れる計算であり
// 代替 (long double や多倍長の自前実装) の方が事故を招く。
__extension__ using i128 = __int128;

// 対称丸め (round half away from zero)。
// 負側を切り捨てにすると、start_pts より前の PTS を持つフレーム
// (B frame や編集リスト付きの素材で実際に起きる) が 1 frame ずれる。
i128 divRound(i128 n, i128 d) {
    if (d == 0)
        return 0;
    if (d < 0) {
        n = -n;
        d = -d;
    }
    if (n >= 0)
        return (n + d / 2) / d;
    return -((-n + d / 2) / d);
}

} // namespace

long long ptsToFrameNumber(long long pts, long long startPts, Rational timeBase,
                           Rational frameRate) {
    if (pts == kNoPts || startPts == kNoPts)
        return -1;
    if (!timeBase.valid() || !frameRate.valid())
        return -1;

    const i128 n = static_cast<i128>(pts - startPts) * timeBase.num * frameRate.num;
    const i128 d = static_cast<i128>(timeBase.den) * frameRate.den;
    const i128 f = divRound(n, d);

    // frame number は timeline の座標なので負にはならない。
    // 負になったら「変換できなかった」として扱う (0 に丸めない)。
    if (f < 0)
        return -1;
    return static_cast<long long>(f);
}

long long frameNumberToPts(long long frameNumber, long long startPts, Rational timeBase,
                           Rational frameRate) {
    if (frameNumber < 0 || startPts == kNoPts)
        return kNoPts;
    if (!timeBase.valid() || !frameRate.valid())
        return kNoPts;

    const i128 n = static_cast<i128>(frameNumber) * frameRate.den * timeBase.den;
    const i128 d = static_cast<i128>(frameRate.num) * timeBase.num;
    return startPts + static_cast<long long>(divRound(n, d));
}

long long secondsToPts(double seconds, Rational timeBase) {
    if (!timeBase.valid())
        return 0;
    // ここだけは秒が double で来るので double を使う。
    // 用途は許容誤差の指定であり、frame number の同定には使わない。
    const double ticks =
        seconds * static_cast<double>(timeBase.den) / static_cast<double>(timeBase.num);
    return static_cast<long long>(ticks < 0 ? ticks - 0.5 : ticks + 0.5);
}

} // namespace mvm::gpu
