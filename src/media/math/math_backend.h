#ifndef MVM_MEDIA_MATH_MATH_BACKEND_H
#define MVM_MEDIA_MATH_MATH_BACKEND_H

// 数式 renderer の backend の束 (backend 中立な契約)。preflight が使える backend の描画関数と
// 能力をまとめて返す。Manim を含め特定の backend の型・設定は出さない。

#include "media/math/equation_sequence_render.h"
#include "media/math/math_render.h"
#include "media/math/math_transform.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace mvm::math {

// 変形の連番を描く。端点の静止の mask は request に入れて渡す。backend の出力 (PNG など) を
// 被覆率として読む loader は呼び出し側が渡す (backend は画像の decoder に依存しない)。
using MathTransformRenderFunction = std::function<MathTransformRenderResult(
    const MathTransformRenderRequest& request, const MathCoverageLoader& loader,
    const std::atomic<bool>* cancel)>;

// Project には入らない値。fingerprint が同じ backend は同じ入力から同じ画素を出す。
struct MathRenderBackend {
    MathToolchainFingerprint fingerprint;
    MathRenderFunction render;
    // 連番の描画 script の識別 (例 "manim-write/1")。静止の key に入れないよう fingerprint と
    // 分けて持つ (連番の描き方を変えても静止の cache を無効にしない)。
    std::string sequenceTemplate;
    // 連番を描けない backend では空。
    MathSequenceRenderFunction renderSequence;
    // renderSequence が描ける最大の枚数 (backend の能力)。超える要求は描かずに未対応として
    // 失敗させる。Project の値の正しさとは別 (Project は時間の意味だけで検証する)。
    std::int64_t maximumSequenceFrames = 0;
    // 変形の描画 script の識別 (例 "manim-transform/1")。変形の key (mathTransformKey) だけに入る。
    std::string transformTemplate;
    // 変形を描けない backend では空。
    MathTransformRenderFunction renderTransform;
    // renderTransform が描ける最大の枚数 (backend の能力)。超える要求は Project の誤りではなく、
    // この描画環境の未対応として失敗させる。
    std::int64_t maximumTransformFrames = 0;
    // Equation Sequence (P3-3) の描画 script の識別 (例 "manim-equation-sequence/1")。
    // equationSequenceRenderKey だけに入る (静止・Write・P2 変形の key は変えない)。
    std::string equationSequenceTemplate;
    // Equation Sequence を描けない backend では空。
    EquationSequenceRenderFunction renderEquationSequence;
    // 1 区間 (変形・action) の最大の枚数 (backend の能力)。
    std::int64_t maximumEquationSequenceFrames = 0;
};

enum class MathPreflightStatus { Available, Unavailable, Cancelled };

struct MathPreflightResult {
    MathPreflightStatus status = MathPreflightStatus::Unavailable;
    MathRenderBackend backend; // Available のときだけ有効
    std::string message;       // Unavailable の理由 (導入の案内を含む)
};

} // namespace mvm::math

#endif // MVM_MEDIA_MATH_MATH_BACKEND_H
