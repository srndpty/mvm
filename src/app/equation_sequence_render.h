#ifndef MVM_APP_EQUATION_SEQUENCE_RENDER_H
#define MVM_APP_EQUATION_SEQUENCE_RENDER_H

// P3-2 の正準入力 (EquationSequenceSpec) と backend 中立な描画契約
// (src/media/math/equation_sequence_render.h) をつなぐ。Qt・Manim に依存しない。
//
// - 描画の入力は EquationSequenceSpec だけから作る。所有 ID・revision・label は渡さない。
// - 任意の source frame で見せる区間と artifact の frame は P3-1 の evaluateEquationSequence
//   (時間の正) で決める。描画側に別の時間の実装を持たない。

#include "app/equation_sequence_compile.h"
#include "media/math/equation_sequence_render.h"
#include "project/equation_sequence.h"

#include <cstdint>
#include <optional>
#include <string>

namespace mvm::app {

// 正準入力をそのまま中立な値へ写す (色は #AARRGGBB を数値へ)。形が不正なら nullopt と error。
std::optional<math::EquationSequenceRenderSpec>
equationSequenceRenderSpecFor(const EquationSequenceSpec& spec, std::string& error);

// source frame f で見せるもの。
//   Hold          状態 state の通常の静止 (静止の artifact。sequence の artifact は使わない)
//   HoldAction    状態 state の action 番号 action の frame (区間の先頭から 0 起点)
//   Transition    変形 transition の frame (0 <= frame < N)
enum class EquationFrameKind { Hold, HoldAction, Transition };

struct EquationFrameLookup {
    EquationFrameKind kind = EquationFrameKind::Hold;
    std::size_t state = 0;      // Hold / HoldAction の状態、Transition の source の状態
    std::size_t transition = 0; // Transition のとき
    std::size_t action = 0;     // HoldAction のとき (EquationSequenceSpec::actions の番号)
    std::int64_t frame = 0;     // HoldAction / Transition の区間内の frame
    std::int64_t frames = 0;    // 区間の枚数 N
    bool operator==(const EquationFrameLookup&) const = default;
};

// data と spec (data を compileEquationSequence したもの) から、source frame の見え方を引く。
// 再生の履歴を持たず、source frame だけから決まる。範囲外・不整合なら nullopt と error。
std::optional<EquationFrameLookup>
equationSequenceFrameAt(const project::EquationSequenceClipData& data,
                        const EquationSequenceSpec& spec, std::int64_t sourceFrame,
                        std::string& error);

// P3-2 の BackendValidationRequired を、backend の構造検証の結果で実行可能にする。
// 検証が ready で、その segment が描画される glyph を持つときだけ None。
EquationCompileFailure equationTargetReadiness(EquationTargetProof proof,
                                               const math::EquationBackendValidation& validation,
                                               std::size_t state, std::size_t segment);

} // namespace mvm::app

#endif // MVM_APP_EQUATION_SEQUENCE_RENDER_H
