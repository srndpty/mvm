#ifndef MVM_APP_EQUATION_SEQUENCE_PREVIEW_H
#define MVM_APP_EQUATION_SEQUENCE_PREVIEW_H

// Equation Sequence の product preview (P3-4) の提示の決め方。Qt・Manim・cache に依存しない。
//
// 正の分担 (docs/math-equation-sequence-p30.md の P3-4):
//   Project (schema 21)   意味の正
//   P3-2 compiler         partition・照合・action の plan (EquationSequenceSpec)
//   P3-1 評価             時間の正。output frame → source frame は clipSourceFrameAt、
//                         source frame → 区間は equationSequenceFrameAt (P3-3) だけで決める
//   P3-3 provenance       artifact の大きさ・配置・各層の色の正 (ここで計算し直さない)
//   residency             cache (ここへは「読めた層」として渡るだけ)
//   preview               提示だけ。再生の履歴を持たず、output frame だけから決まる
//
// 提示の規則 (fail-safe。書き出しはこの代用を引き継がない):
//   Hold(s)               状態 s の通常の静止 (Ready のとき。無ければ何も見せない)
//   HoldAction(s, a, i)   action a の frame i の 2 層 (base と accent が両方あるときだけ)。
//                         どちらかが無ければ状態 s の通常の静止
//   Transition(t, i)      変形 t の frame i。無ければ前の状態の静止 (区間の全 frame)。
//                         区間の後の target hold の frame 0 で後の状態の静止へ切り替わる
// compile に失敗した (spec が無い) sequence は P3-1 の評価で状態だけを決め、静止だけを見せる
// (古い spec の action・変形を出さない)。Blend などの代用は合成しない。

#include "app/equation_sequence_compile.h"
#include "app/equation_sequence_render.h"
#include "media/math/math_transform.h"
#include "project/project.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mvm::app {

// P3-1 の時間の正だけで決まる、ある output frame の区間と区間内の frame。
struct EquationPreviewTime {
    std::int64_t sourceFrame = -1;
    EquationFrameLookup lookup;
    bool operator==(const EquationPreviewTime&) const = default;
};

// output frame (timeline の frame) の区間。clip の可視範囲の外・換算できない・spec と data が
// 合わなければ nullopt と error。spec が null なら action を付けず、状態と変形だけを決める
// (compile の失敗中の静止の代用のため)。
std::optional<EquationPreviewTime>
equationPreviewTimeAt(const project::TimelineClip& clip, std::int64_t timelineFpsNum,
                      std::int64_t timelineFpsDen, const EquationSequenceSpec* spec,
                      std::int64_t outputFrame, std::string& error);

// 実際に見せるもの (代用を解決した結果)。
enum class EquationPreviewShownKind { None, Static, Transition, Action };

struct EquationPreviewShown {
    EquationPreviewShownKind kind = EquationPreviewShownKind::None;
    std::size_t index = 0;  // Static は状態、Transition は変形、Action は spec の action の番号
    std::int64_t frame = 0; // Transition / Action の区間内の frame
    bool operator==(const EquationPreviewShown&) const = default;
};

const char* equationPreviewShownKindName(EquationPreviewShownKind kind);

// 区間ごとに「その frame の内容が揃っているか」。
class EquationPreviewAvailability {
public:
    virtual ~EquationPreviewAvailability() = default;
    virtual bool staticReady(std::size_t state) const = 0;
    virtual bool transitionFrameReady(std::size_t transition, std::int64_t frame) const = 0;
    // base と accent の両方 (2 層が揃わない action は見せない)。
    virtual bool actionFrameReady(std::size_t action, std::int64_t frame) const = 0;
};

// 上の提示の規則。lookup は equationPreviewTimeAt の結果。
EquationPreviewShown resolveEquationPreview(const EquationFrameLookup& lookup,
                                            const EquationPreviewAvailability& availability);

// preview が memory に要求する A8 の層 1 枚 (cache の層の key と同じ形)。
enum class EquationPreviewLayerRole { TransitionFrame, ActionBase, ActionAccent };

struct EquationPreviewLayerId {
    EquationPreviewLayerRole role = EquationPreviewLayerRole::TransitionFrame;
    std::size_t interval = 0;
    std::int64_t frame = 0;
    bool operator==(const EquationPreviewLayerId&) const = default;
};

// 今の frame に要る層の束 (最優先で読む)。Hold は空 (通常の静止の mask を使う)。
//   Transition  変形の frame 1 枚
//   HoldAction  action の base と今の accent の 2 枚 (揃って初めて見せる)
std::vector<EquationPreviewLayerId> equationPreviewCurrentLayers(const EquationFrameLookup& lookup);

// 先読みの束 (frame ごと、見せる順): 今の区間の残りの frame と、その次の区間の全 frame。
// 広い先読みはしない (limit 束まで)。今の frame の束より後に、何も追い出さずに読む。
std::vector<std::vector<EquationPreviewLayerId>>
equationPreviewUpcomingLayers(const EquationSequenceSpec& spec, const EquationPreviewTime& time,
                              std::size_t limit);

// 1 画素 1 byte の被覆 (A8)。cache の常駐の mask を参照で持つ (所有者を生かしておく)。
using EquationPreviewCoverage = std::shared_ptr<const std::vector<std::uint8_t>>;

struct EquationPreviewRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool empty() const { return width <= 0 || height <= 0; }

    bool operator==(const EquationPreviewRect&) const = default;
};

// 状態の通常の静止 (今の key の Ready の mask の被覆と、通常の Math と同じ色)。
struct EquationPreviewStatic {
    int width = 0;
    int height = 0;
    EquationPreviewCoverage coverage;
    std::uint32_t colorArgb = 0xFFFFFFFFu;
};

// disk の artifact (P3-3 の provenance) から取った変形 1 本の幾何と各 frame の色。
struct EquationPreviewTransitionArtifact {
    int width = 0;
    int height = 0;
    int sourceX = 0;
    int sourceY = 0;
    int targetX = 0;
    int targetY = 0;
    std::vector<std::uint32_t> colors; // N 枚 (provenance の色)
};

struct EquationPreviewActionArtifact {
    int width = 0;
    int height = 0;
    int staticX = 0; // artifact の中の状態の静止の左上
    int staticY = 0;
    std::uint32_t baseColor = 0;
    std::vector<std::uint32_t> accentColors; // N 枚
};

struct EquationPreviewInputs {
    project::TimelineClip clip; // data と timeline 上の位置・可視範囲 (今の Project の値)
    std::int64_t timelineFpsNum = 0;
    std::int64_t timelineFpsDen = 1;
    int outputWidth = 0;
    int outputHeight = 0;
    // 今の Project を compile した結果。失敗中は nullopt (静止だけを見せる)。
    std::optional<EquationSequenceSpec> spec;
    // 状態ごと (data.states の順)。Ready でない状態は nullopt。
    std::vector<std::optional<EquationPreviewStatic>> statics;
    // disk の artifact が今の spec の key で Ready のときだけ (spec.transitions / actions の順)。
    bool artifactReady = false;
    std::vector<EquationPreviewTransitionArtifact> transitions;
    std::vector<EquationPreviewActionArtifact> actions;
    // memory に読めた層 (今の artifact の key のものだけを渡すこと)。
    std::map<std::pair<std::size_t, std::int64_t>, EquationPreviewCoverage> transitionFrames;
    std::map<std::size_t, EquationPreviewCoverage> actionBases;
    std::map<std::pair<std::size_t, std::int64_t>, EquationPreviewCoverage> actionAccents;
};

// 1 本の Equation Sequence clip の preview (不変)。output frame だけから見せるものと画素を決める。
// 画素は出力 raster の座標の patchRect の中だけを書く (外は透明)。ClipEffects は含めない
// (layer の側で 1 回だけ掛ける)。
class EquationPreviewModel final : public EquationPreviewAvailability {
public:
    // 配置を決める。大きさの合わない層・出力に置けない区間は使わず (代用で見せ)、理由を
    // diagnostics に残す。
    explicit EquationPreviewModel(EquationPreviewInputs inputs);

    const EquationPreviewInputs& inputs() const { return inputs_; }

    const std::vector<std::string>& diagnostics() const { return diagnostics_; }

    // 何かを見せうるか (静止か区間の層が 1 つでもある)。
    bool hasContent() const { return !rect_.empty(); }

    EquationPreviewRect patchRect() const { return rect_; }

    // 出力 raster での配置 (試験・診断用)。
    std::optional<EquationPreviewRect> staticRect(std::size_t state) const;
    std::optional<math::MathTransformRasterPlacement> transitionPlacement(std::size_t index) const;
    std::optional<EquationPreviewRect> actionRect(std::size_t index) const;

    bool staticReady(std::size_t state) const override;
    bool transitionFrameReady(std::size_t transition, std::int64_t frame) const override;
    bool actionFrameReady(std::size_t action, std::int64_t frame) const override;

    // output frame の区間 (P3-1) と見せるもの。clip の外・換算できない frame は None。
    std::optional<EquationPreviewTime> timeAt(std::int64_t outputFrame) const;
    EquationPreviewShown shownAt(std::int64_t outputFrame) const;
    // 見せるものの番号 (同じ番号は同じ画素)。None は -1。
    std::int64_t stateCode(const EquationPreviewShown& shown) const;
    // 番号の画素を patchRect の大きさの RGBA8 (straight alpha、行間の余白なし) へ書く。
    void fill(std::int64_t code, std::uint8_t* out) const;
    // 番号から見せるものへ戻す (fill と試験が使う)。
    EquationPreviewShown shownForCode(std::int64_t code) const;

private:
    EquationPreviewInputs inputs_;
    std::vector<std::string> diagnostics_;
    std::vector<std::optional<EquationPreviewRect>> staticRects_;
    std::vector<std::optional<math::MathTransformRasterPlacement>> transitionPlacements_;
    std::vector<std::optional<EquationPreviewRect>> actionRects_;
    std::vector<std::int64_t> transitionBase_;
    std::vector<std::int64_t> actionBase_;
    EquationPreviewRect rect_;
};

} // namespace mvm::app

#endif // MVM_APP_EQUATION_SEQUENCE_PREVIEW_H
