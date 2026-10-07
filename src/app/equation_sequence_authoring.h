#ifndef MVM_APP_EQUATION_SEQUENCE_AUTHORING_H
#define MVM_APP_EQUATION_SEQUENCE_AUTHORING_H
// P3-5 の authoring UI が使う、Qt に依存しない判断と表示用の値。
// UI (QML) は TeX・PartId の参照・時間・compile の状態・byte offset を解釈しない。ここで型の付いた
// 状態から表示の値を作り、利用者の意図は controller が domain の操作へ写す。
#include "app/equation_sequence_compile.h"
#include "media/math/equation_sequence_render.h"
#include "project/equation_binding_edit.h"
#include "project/equation_sequence.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace mvm::app {
// 部分式の状態。表示の文はこの型から作り、backend のメッセージの文字列から分類しない。
enum class EquationPartStatus {
    Bound,
    InvalidBinding,         // 式の編集で範囲が壊れた (明示 rebind で修復)
    MissingTarget,          // 部分式が削除され、action が欠落 ID を参照している (同じ ID で作り直す)
    UnsupportedTexBoundary, // TeX の構造の途中を切る範囲 (compiler が分離できない)
    UnsupportedEmptyTarget  // 空白・括弧だけで描く文字が無い
};
const char* equationPartStatusName(EquationPartStatus);
std::string equationPartStatusText(EquationPartStatus);
// 修復の操作 (同じ PartId に今の範囲を結び直す) を出してよい状態か。
bool equationPartRepairable(EquationPartStatus);

// state の中の一つの部分式を単独で compile して状態を決める (他の部分式の失敗に巻き込まない)。
EquationPartStatus equationPartStatus(const project::EquationState&, const project::SemanticPart&);

struct EquationPartView {
    project::PartId id;
    std::string label;
    std::string expectedText;
    EquationPartStatus status = EquationPartStatus::Bound;
    bool exists = true; // MissingTarget は state の parts に無い (action の参照だけが残る)
    // 今の式での UTF-16 の範囲 (Bound / Unsupported のとき)。InvalidBinding は旧 revision の証人
    // なので今の式の範囲として出さない。
    std::optional<std::pair<std::size_t, std::size_t>> rangeUtf16;
    std::vector<project::ActionId> actions;
    std::vector<project::TransitionId> correspondences;
};
// 状態の部分式を source 順 (Bound) → 無効 → 欠落の順に並べる。欠落は action の参照から作る。
std::vector<EquationPartView> equationPartViews(const project::EquationSequenceClipData&,
                                                const project::StateId&);

// compile / backend 検証の失敗の表示。InvalidSequence は構造の不正で、修復の操作を出さない。
std::string equationCompileFailureText(EquationCompileFailure);
bool equationCompileFailureRepairable(EquationCompileFailure);
// backend の構造検証・artifact の失敗。内容の問題 (式・部分式の選び方で直る) と
// 描画結果・環境の問題 (CorruptFrame など) を分ける。
std::string equationBackendFailureText(math::EquationBackendFailure);
bool equationBackendFailureIsContent(math::EquationBackendFailure);

// 新しい sequence の既定 (状態の hold は 1 秒、変形は 0.5 秒。どちらも 1 frame 以上)。
std::int64_t equationDefaultHoldFrames(std::int64_t fpsNum, std::int64_t fpsDen);
std::int64_t equationDefaultTransitionFrames(std::int64_t fpsNum, std::int64_t fpsDen);
// frame の尺の表示。秒は派生の表示だけで、正は frame。
std::string equationFramesText(std::int64_t frames, std::int64_t fpsNum, std::int64_t fpsDen);

// 一つの状態・透明背景・部分式・辺・action なしの最小の sequence。
project::EquationSequenceClipData newEquationSequenceData(std::string source,
                                                          std::int64_t holdFrames,
                                                          const std::function<std::string()>& id);
// 挿入する状態。式の書式 (文字サイズ・色) は reference を引き継ぎ、背景は透明。
project::EquationState newEquationState(const project::MathClipData& reference,
                                        std::string source, std::int64_t holdFrames,
                                        const std::function<std::string()>& id);

// 編集欄で記録した信頼済み編集を順に適用する。全体を一つの候補で検証して確定する
// (Project の操作は一回)。revision は編集ごとに新しく発行する。
bool applyTrustedEquationEdits(project::EquationSequenceClipData&, const project::StateId&,
                               const std::vector<project::TrustedEquationEdit>&,
                               const std::function<std::string()>& newRevision, int outputHeight,
                               std::string& error);

// 選択中の ID が消えたら、同じ位置 (末尾を超えるなら最後) の生存を選ぶ。空なら nullopt。
std::optional<std::size_t> nearestSurvivingIndex(std::size_t previousIndex, std::size_t count);
} // namespace mvm::app
#endif
