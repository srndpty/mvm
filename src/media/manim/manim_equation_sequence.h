#ifndef MVM_MEDIA_MANIM_MANIM_EQUATION_SEQUENCE_H
#define MVM_MEDIA_MANIM_MANIM_EQUATION_SEQUENCE_H

// Equation Sequence (P3-3) の Manim の実装。外へは math::EquationSequenceRenderFunction
// (中立な契約) として preflight が束ねる。Manim の class 名・Python の記法はここに閉じる。
//
// 描画は 2 段階の別 process で行う。
//   1. structure: 各状態の MathTex(*segments) を作り、top-level の部分と点を持つ子孫の所有を
//      structure.txt へ事実として書くだけで、何も描かない。mvm が検証する。
//   2. render: 検証を通ったときだけ起動する。状態・変形・action を区間ごとに直接標本化して
//      PNG を書き、同じ構造の報告を再び書く (1 と同じでなければ失敗)。
// Manim の終了コード 0・入力の文字列の数・Python の object の同一性・file の存在だけでは成功に
// しない。部分の数・種類・文字列・点を持つ子孫の排他的な所有と、静止の描画との全画素の一致で決める。

#include "media/math/equation_sequence_render.h"
#include "media/math/math_transform.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mvm::manim {

// script を変えたら版を上げる (equationSequenceRenderKey だけに入る)。
inline constexpr char kEquationSequenceTemplateId[] = "manim-equation-sequence";
inline constexpr int kEquationSequenceTemplateVersion = 1;
// 1 区間の枚数の上限。変形は照合の 1 枚を足して書く。名前は 5 桁。
inline constexpr std::int64_t kMaximumEquationIntervalFrames = 9998;
// 一時的な canvas の、静止の矩形からの各辺の余白 (px)。P2 の変形と同じ値。
inline constexpr int kEquationCanvasPadding = 200;

struct ManimEquationPlan {
    struct Endpoint {
        int staticWidth = 0;
        int staticHeight = 0;
        int canvasWidth = 0;
        int canvasHeight = 0;
        math::MathEndpointPlacement placement; // raster の座標 (+Y は下)
    };

    struct Transition {
        int sourceWidth = 0;
        int sourceHeight = 0;
        int targetWidth = 0;
        int targetHeight = 0;
        int canvasWidth = 0;
        int canvasHeight = 0;
        math::MathTransformPlacement placement;
    };

    std::vector<Endpoint> states; // 状態の描画の照合用
    std::vector<Transition> transitions;
    std::vector<Endpoint> actions;
};

// 状態の静止の大きさから各区間の canvas と配置を決める。spec が不正なら false と error。
bool planManimEquationSequence(const math::EquationSequenceRenderSpec& spec,
                               const std::vector<math::MathCoverage>& stateStatics,
                               ManimEquationPlan& plan, std::string& error);

// script が読む request.json。phase は "structure" か "render"。
// 所有 ID は含まず、状態・segment・handle の番号と、frame ごとの Animation の alpha
// (整数の分子・分母) だけを渡す。進み具合は mvm が決め、Python は計算しない。
std::string manimEquationSequenceRequestJson(const math::EquationSequenceRenderSpec& spec,
                                             const ManimEquationPlan& plan, const char* phase);

// script の全文 (試験が内容を確かめる)。
std::string manimEquationSequenceScript();

// structure.txt を検証する。失敗は最初に見つけた理由と場所。成功なら全 segment の所有を返す。
math::EquationBackendValidation
checkManimEquationStructure(const std::string& report,
                            const math::EquationSequenceRenderSpec& spec);

math::EquationSequenceRenderResult
renderManimEquationSequence(const std::filesystem::path& manimExecutablePath,
                            const math::EquationSequenceRenderRequest& request,
                            const math::MathCoverageLoader& loader,
                            const std::atomic<bool>* cancel);

} // namespace mvm::manim

#endif // MVM_MEDIA_MANIM_MANIM_EQUATION_SEQUENCE_H
