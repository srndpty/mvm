#ifndef MVM_MEDIA_MATH_EQUATION_SEQUENCE_RENDER_H
#define MVM_MEDIA_MATH_EQUATION_SEQUENCE_RENDER_H

// Equation Sequence (P3-3) の backend 中立な描画契約。
//
// 入力は P3-2 の正準入力 (app::EquationSequenceSpec) を Project の型を含まない値へ写したもの。
// StateId / PartId / ActionId / TransitionId、revision、label、Project JSON の構造は含めず、
// 状態・segment・transition・action は正準順序の番号だけで参照する。
// 時間の正は P3-1 の整数区間で、ここでは各区間の整数の枚数と、その中の frame の進み具合
// (整数の分子 / 分母) だけを決める。Manim の秒・class 名は含めない (backend の内部)。

#include "media/math/math_render.h"
#include "media/math/math_tex_segments.h"
#include "media/math/math_transform.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace mvm::math {

// cache key の名前空間。静止 (mvm-math-static)・Write (mvm-math-sequence)・
// P2 変形 (mvm-math-transform) と別。形を変えたら版を上げる。
inline constexpr char kEquationSequenceKeyVersion[] = "mvm-equation-sequence/1";
// 出力の raster の前提 (A8 の被覆を artifact の矩形で切り出す、行間の余白なし)。
inline constexpr char kEquationSequenceRasterVersion[] = "a8-crop/1";
// frame の進み具合の規則 (下の equation*Progress)。規則を変えたら版を上げる。
inline constexpr char kEquationSequenceProgressVersion[] =
    "transition=i/N;outline=(2i+1)/(2N);pulse=(N-|2i+1-N|)/N";
// outline の線と pulse の強調の色 (合成の側の値、0xAARRGGBB)。mask は白で描き、色は合成で付ける。
inline constexpr std::uint32_t kEquationActionAccentArgb = 0xFFFFFF00u;

enum class EquationRenderSegmentKind { Semantic, Auto };
enum class EquationRenderOperation { Outline, Pulse };

struct EquationRenderSegment {
    EquationRenderSegmentKind kind = EquationRenderSegmentKind::Auto;
    std::string text; // source の連続した一部。全 segment を連結すると source に一致する
    bool operator==(const EquationRenderSegment&) const = default;
};

struct EquationRenderState {
    MathRenderSpec still;                       // 静止の描画と同じ値 (syntax / source / font)
    std::uint32_t foregroundArgb = 0xFFFFFFFFu; // 合成の色
    std::uint32_t backgroundArgb = 0;           // alpha 0 だけを受け付ける
    std::int64_t holdFrames = 0;
    std::string segmenterVersion = kMathTexSegmenterVersion;
    std::vector<EquationRenderSegment> segments;
    bool operator==(const EquationRenderState&) const = default;
};

// P3-2 が決めた handle の対。backend は独自に照合しない。
struct EquationRenderTransition {
    std::size_t fromState = 0;
    std::size_t toState = 0;
    std::int64_t frames = 0;
    MathSegmentMatching matching;
    std::string matcherVersion = kMathTexMatchingVersion;
    bool operator==(const EquationRenderTransition&) const = default;
};

struct EquationRenderAction {
    std::size_t state = 0;
    std::size_t segment = 0;
    std::int64_t start = 0; // hold の先頭からの整数 frame
    std::int64_t duration = 0;
    EquationRenderOperation operation = EquationRenderOperation::Outline;
    bool operator==(const EquationRenderAction&) const = default;
};

struct EquationSequenceRenderSpec {
    std::string compilerVersion;
    std::vector<EquationRenderState> states;
    std::vector<EquationRenderTransition> transitions;
    std::vector<EquationRenderAction> actions; // 状態の番号・start の順
    bool operator==(const EquationSequenceRenderSpec&) const = default;
};

const char* equationRenderOperationName(EquationRenderOperation operation);

// 値の形だけを検査する (描けるかは見ない)。違反なら false と理由。
// - 状態は 1 個以上、transition は i → i+1 の n−1 個、尺は 1 以上
// - segment は空でなく、連結すると source に byte 単位で一致する
// - matching は source / target の全 handle をちょうど一回ずつ使い、pairs は source 順
// - action は存在する状態・segment、hold の内側、尺 1 以上。同じ状態の区間は重ならない
// - 背景は alpha 0、syntax は latex、font は正
bool validateEquationSequenceRenderSpec(const EquationSequenceRenderSpec& spec, std::string& error);

// cache key (小文字 16 進 64 桁、失敗なら空)。kEquationSequenceKeyVersion の名前空間で、
// spec の全 field (所有 ID を含まない正準入力)、raster と進み具合の版、backend の toolchain、
// 描画 template を byte 数前置の正準形にして SHA-256 を取る。
std::string equationSequenceRenderKey(const EquationSequenceRenderSpec& spec,
                                      const MathToolchainFingerprint& toolchain,
                                      const std::string& sequenceTemplate);

// 整数の進み具合。frames = N >= 1、0 <= frame < N でなければ false。
// transition: frame i は i / N (frame 0 は source の静止と一致し、N は timeline に出さない
//             照合用の終状態)。P2 と同じ数え方。
// outline:    frame i は (2i + 1) / (2N)。区間の全 frame が (0, 1) の内側にあり、N = 1 でも
//             中央の 1/2 を見せる。区間の前後の frame は通常の静止。
// pulse:      強調の重み (N − |2i + 1 − N|) / N。進み具合 (2i + 1) / (2N) の三角波で、
//             拡大と色の重みの両方に同じ値を使う。区間の前後は重み 0 の通常の静止。
bool equationTransitionProgress(std::int64_t frame, std::int64_t frames, std::int64_t& numerator,
                                std::int64_t& denominator);
bool equationOutlineProgress(std::int64_t frame, std::int64_t frames, std::int64_t& numerator,
                             std::int64_t& denominator);
bool equationPulseWeight(std::int64_t frame, std::int64_t frames, std::int64_t& numerator,
                         std::int64_t& denominator);

// ---- backend の構造検証 ----

// backend が実際の描画物の木を確かめた結果の失敗理由 (fail-closed)。
enum class EquationBackendFailure {
    None,
    NotValidated,                  // まだ検証していない (P3-2 の BackendValidationRequired のまま)
    InvalidRequest,                // 描画要求そのものが不正
    StructureReportMissing,        // backend が構造を報告しない
    StructureReportMalformed,      // 報告を読めない
    GroupingFallback,              // backend が部分を見つけられず式全体などで代用した
    SegmentCountMismatch,          // top-level の部分の数が segment の数と違う
    MissingSegmentObject,          // segment の番号に対応する部分が無い
    SegmentTypeMismatch,           // 部分が期待した種類の object でない
    SegmentTextMismatch,           // 部分の文字列が segment と違う
    EmptyActionTarget,             // action の対象が描画される glyph を持たない
    EmptyTransitionHandle,         // 変形の対の片側だけが空 (空から glyph へ・glyph から空へ)
    SharedDescendant,              // 同じ子孫 object を 2 つの handle が所有する
    AliasedPointData,              // 別の子孫 object が同じ点列の領域を共有する
    UnclaimedDescendant,           // どの handle にも属さない glyph がある (group の崩れ)
    StructureChangedBetweenPhases, // 検証の段階と描画の段階で構造が違う
    FrameCountMismatch,
    FrameSizeMismatch,
    CorruptFrame,
    EdgeContact,        // 一時的な canvas の縁に触れた (はみ出した可能性)
    StaticMismatch,     // 状態の描画が通常の静止の描画と一致しない
    EndpointMismatch,   // 変形の端点が両端の静止と一致しない
    ActionMutatedState, // action の後の式が静止と一致しない (端点を変えた)
    // pulse の base と、通常の大きさ・位置の対象を合成しても状態の静止と一致しない
    // (base が対象を含む・別の segment を欠く・対象の層がずれている)
    PulseBaseMismatch
};

const char* equationBackendFailureName(EquationBackendFailure failure);

// ---- 層の合成 (A8) ----
//
// Equation Sequence の層の被覆 (alpha) の合成規則。下の層 under の上に over を重ねる
// (source-over を alpha だけで行う)。1 画素ごとに整数で
//   out = over + round(under * (255 - over) / 255)
// round は最も近い整数 (x / 255 はちょうど半分にならない)。P3-4 の preview / export の合成も
// この規則を使う。順序は base の上に accent (対象)。
std::uint8_t equationCoverageOver(std::uint8_t under, std::uint8_t over);
// 同じ大きさの 2 枚を合成する。大きさが不正・違うなら false。
bool composeEquationCoverage(const MathCoverage& under, const MathCoverage& over,
                             MathCoverage& out);

// ---- 色付きの層の合成 (P3-4) ----
//
// preview (と後の書き出し) が A8 の層に provenance の色を付けて重ねる規則。合成は CPU で行い、
// GPU には結果の RGBA (straight alpha) を 1 枚の静止画 layer として渡す (shader に別の丸めを
// 持たない)。
//
// 1 層の画素の alpha は round(coverage * 色の alpha / 255)。単層の静止・Write・変形の
// composeMathPatch (背景が透明のとき) と同じ値になる。
std::uint8_t equationLayerAlpha(std::uint8_t coverage, std::uint32_t argb);
// base (下) の上に accent (上) を重ねた 1 画素 (RGBA8 straight alpha)。整数で
//   a = baseCoverage * base の alpha、b = accentCoverage * accent の alpha  (0..255*255)
//   alpha = equationCoverageOver(equationLayerAlpha(base), equationLayerAlpha(accent))
//   色    = round((C_accent * b * 65025 + C_base * a * (65025 - b)) / (b * 65025 + a * (65025 -
//   b)))
// 分母が 0 (どちらの層も何も無い) なら (0, 0, 0, 0)。両方の色が不透明なら alpha は
// composeEquationCoverage と同じ値。accent が無い (被覆 0) 画素は base だけの composeMathPatch と
// byte 単位で同じになる。
void composeEquationLayerPixel(std::uint8_t baseCoverage, std::uint32_t baseArgb,
                               std::uint8_t accentCoverage, std::uint32_t accentArgb,
                               std::uint8_t* out);
// 同じ大きさ (width x height) の 2 層を、幅 outWidth 画素の RGBA8 の画像 out の (left, top) から
// 書く (矩形の画素を置き換える)。範囲の検査は呼び出し側が行う。
void composeEquationLayersAt(const std::uint8_t* base, std::uint32_t baseArgb,
                             const std::uint8_t* accent, std::uint32_t accentArgb, int width,
                             int height, std::uint8_t* out, int outWidth, int left, int top);

// segment ごとの backend の事実 (診断・provenance 用。Project には保存しない)。
struct EquationSegmentOwnership {
    std::size_t state = 0;
    std::size_t segment = 0;
    std::string topLevelType;      // backend の top-level の部分の種類 (例 "MathTexPart")
    std::int64_t children = 0;     // 直接の子の数
    std::int64_t descendants = 0;  // 子孫 object の数 (自身を除く)
    std::int64_t pointBearing = 0; // 点を持つ子孫 (自身を含む) の数 = 描画される輪郭の数
    bool nonEmpty = false;         // pointBearing > 0
    // 所有する点を持つ object の、式全体の点を持つ object の並び (backend の木の順) での番号。
    // 昇順。異なる handle の集合は交わらない。
    std::vector<std::int64_t> ownership;
    bool operator==(const EquationSegmentOwnership&) const = default;
};

struct EquationBackendValidation {
    EquationBackendFailure failure = EquationBackendFailure::NotValidated;
    std::size_t state = 0; // 失敗の場所 (該当するとき)
    std::size_t segment = 0;
    std::string detail;                             // 利用者に見せる短い説明
    std::vector<EquationSegmentOwnership> segments; // 状態・segment の順

    bool ready() const { return failure == EquationBackendFailure::None; }
};

// ---- 描画の要求と結果 ----

struct EquationSequenceRenderRequest {
    EquationSequenceRenderSpec spec;
    // 各状態の通常の静止の描画 (同じ backend の静止の artifact を decode した被覆)。
    // 状態の描画・変形の端点・action の後はこれと全画素で照合する。
    std::vector<MathCoverage> stateStatics;
    std::filesystem::path jobDirectory;
    std::chrono::milliseconds timeout{300000};
};

// 一時的な canvas に描いた 1 区間。frames は canvas の大きさの画像 (backend の出力)。
struct EquationIntervalRaster {
    std::vector<std::filesystem::path> frames; // 表示する N 枚 (frame i から順)
    int canvasWidth = 0;
    int canvasHeight = 0;
    // canvas の座標で、全 frame (照合用の 1 枚を含む) の alpha の外接矩形と端点の静止の矩形の和。
    MathRect artifact;
};

struct EquationTransitionRaster {
    EquationIntervalRaster interval;
    MathTransformPlacement placement; // canvas の中の両端の静止の配置
};

// action は 2 層。base は区間中に変わらない層 (outline は式全体、pulse は対象以外)、
// accent は frame ごとの層 (outline は線、pulse は拡大する対象)。合成は base の上に accent。
struct EquationActionRaster {
    EquationIntervalRaster interval; // accent の N 枚
    std::filesystem::path base;
    // pulse だけ: 拡大していない通常の大きさ・位置の対象だけの層 (作業 directory の中の照合用。
    // artifact には保存しない)。composeEquationCoverage(base, normalTarget)
    // が状態の静止に一致する。
    std::filesystem::path normalTarget;
    MathEndpointPlacement placement; // canvas の中の状態の静止の配置
};

struct EquationSequenceRenderResult {
    MathRenderStatus status = MathRenderStatus::Failed;
    EquationBackendValidation validation;
    std::vector<EquationTransitionRaster> transitions; // spec.transitions の順
    std::vector<EquationActionRaster> actions;         // spec.actions の順
    std::string message;
    std::string log;
};

using EquationSequenceRenderFunction = std::function<EquationSequenceRenderResult(
    const EquationSequenceRenderRequest& request, const MathCoverageLoader& loader,
    const std::atomic<bool>* cancel)>;

} // namespace mvm::math

#endif // MVM_MEDIA_MATH_EQUATION_SEQUENCE_RENDER_H
