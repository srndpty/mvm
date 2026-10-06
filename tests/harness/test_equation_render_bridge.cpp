// Equation Sequence (P3-3) の app 層の橋渡し: P3-2 の正準入力 → 中立な描画要求、
// source frame → 区間と frame (P3-1 の評価が正)、backend の構造検証による実行可能性。
// 期待値は手で決めた値。

#include "app/equation_sequence_compile.h"
#include "app/equation_sequence_render.h"
#include "project/equation_sequence.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace mvm;
using namespace project;
using namespace app;

namespace {
int checks = 0, failures = 0;

void check(bool ok, const std::string& message) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "失敗: %s\n", message.c_str());
    }
}

void part(EquationState& s, std::string id, const std::string& text, int occurrence = 0) {
    std::size_t at = std::string::npos, from = 0;
    for (int i = 0; i <= occurrence; ++i) {
        at = s.equation.source.find(text, from);
        from = at + 1;
    }
    s.parts.push_back({{std::move(id)},
                       "ラベルは描画に渡さない",
                       {s.revision, static_cast<std::int64_t>(at),
                        static_cast<std::int64_t>(at + text.size()), text, BindingStatus::Bound}});
}

// 状態 0 (hold 20、action 2 個) → 変形 5 → 状態 1 (hold 4)。
EquationSequenceClipData sequence() {
    EquationState a;
    a.id = {"state-a"};
    a.revision = "rev-a";
    a.equation.source = "x=b^2-4ac";
    a.equation.color = "#FF112233";
    a.holdFrames = 20;
    part(a, "disc", "b^2-4ac");
    EquationState b;
    b.id = {"state-b"};
    b.revision = "rev-b";
    b.equation.source = "x+b^2-4ac";
    b.equation.fontSize = 64;
    b.holdFrames = 4;
    part(b, "disc-b", "b^2-4ac");
    EquationSequenceClipData d;
    d.states = {a, b};
    EquationStepTransition t;
    t.id = {"edge"};
    t.from = a.id;
    t.to = b.id;
    t.frames = 5;
    t.correspondence = {{{"disc"}, {"disc-b"}}};
    d.transitions = {t};
    EquationAction outline;
    outline.id = {"emph-1"};
    outline.state = a.id;
    outline.target = {"disc"};
    outline.start = 2;
    outline.duration = 3;
    outline.operation = EquationOperation::Outline;
    EquationAction pulse = outline;
    pulse.id = {"emph-2"};
    pulse.start = 10;
    pulse.duration = 6;
    pulse.operation = EquationOperation::Pulse;
    d.actions = {pulse, outline}; // 保存順は start 順でなくてよい
    return d;
}

void conversion() {
    const auto d = sequence();
    const auto compiled = compileEquationSequence(d);
    check(compiled.value.has_value(), "compile できる");
    std::string error;
    const auto render = equationSequenceRenderSpecFor(*compiled.value, error);
    check(render.has_value(), "中立な描画要求へ写せる: " + error);
    if (!render)
        return;
    check(render->states.size() == 2 && render->states[0].still.source == "x=b^2-4ac" &&
              render->states[0].foregroundArgb == 0xFF112233u &&
              render->states[1].still.fontSize == 64 && render->states[0].holdFrames == 20,
          "状態の式・色・font・hold を写す");
    check(render->states[0].backgroundArgb == 0, "背景は透明 (数値)");
    check(render->transitions.size() == 1 && render->transitions[0].frames == 5, "変形の尺を写す");
    check(render->actions.size() == 2 && render->actions[0].start == 2 &&
              render->actions[0].operation == math::EquationRenderOperation::Outline &&
              render->actions[1].start == 10 &&
              render->actions[1].operation == math::EquationRenderOperation::Pulse,
          "action は正準順 (状態・start の順) で写す");
    // semantic の handle は、explicit の対で結ぶ。
    const auto& segments = render->states[0].segments;
    std::size_t disc = segments.size();
    for (std::size_t k = 0; k < segments.size(); ++k)
        if (segments[k].kind == math::EquationRenderSegmentKind::Semantic)
            disc = k;
    check(disc < segments.size() && segments[disc].text == "b^2-4ac" &&
              render->actions[0].segment == disc,
          "action の対象は semantic の handle の番号");

    // copy / remap した同じ意味の sequence は同じ描画要求・同じ key。
    auto copy = d;
    int next = 0;
    check(remapEquationSequenceIds(
              copy, [&] { return "fresh-" + std::to_string(next++); }, error),
          "remap できる: " + error);
    for (auto& s : copy.states)
        for (auto& p : s.parts)
            p.label = "別の表示名";
    check(copy.states[0].id.value != d.states[0].id.value, "remap で ID が変わる");
    const auto copied = compileEquationSequence(copy);
    const auto copyRender = equationSequenceRenderSpecFor(*copied.value, error);
    check(copyRender && *copyRender == *render, "remap した複製は同じ中立な描画要求");
    const math::MathToolchainFingerprint toolchain{"manim-mathtex", "manim=1\n"};
    check(copyRender && math::equationSequenceRenderKey(*copyRender, toolchain, "t/1") ==
                            math::equationSequenceRenderKey(*render, toolchain, "t/1"),
          "remap した複製は同じ key (所有 ID・label・revision に依存しない)");
    auto renamedRevision = d;
    renamedRevision.states[1].revision = "rev-other";
    renamedRevision.states[1].parts[0].binding.revision = "rev-other";
    const auto revised = compileEquationSequence(renamedRevision);
    const auto revisedRender = equationSequenceRenderSpecFor(*revised.value, error);
    check(revisedRender && *revisedRender == *render, "revision の文字列は描画要求に入らない");

    auto bad = *compiled.value;
    bad.states[0].equation.color = "white";
    check(!equationSequenceRenderSpecFor(bad, error), "色の形式が不正なら写さない");
    auto opaque = *compiled.value;
    opaque.states[0].equation.backgroundColor = "#FF000000";
    check(!equationSequenceRenderSpecFor(opaque, error), "不透明な背景は写さない");
}

void lookup() {
    const auto d = sequence();
    const auto spec = *compileEquationSequence(d).value;

    struct Expect {
        std::int64_t frame;
        EquationFrameKind kind;
        std::size_t state, index;
        std::int64_t local, frames;
    };

    // 手計算: hold 0 は [0,20)、outline は [2,5)、pulse は [10,16)、変形は [20,25)、hold 1 は
    // [25,29)。
    const std::vector<Expect> expects = {
        {0, EquationFrameKind::Hold, 0, 0, 0, 0},
        {1, EquationFrameKind::Hold, 0, 0, 0, 0},
        {2, EquationFrameKind::HoldAction, 0, 0, 0, 3},
        {3, EquationFrameKind::HoldAction, 0, 0, 1, 3},
        {4, EquationFrameKind::HoldAction, 0, 0, 2, 3},
        {5, EquationFrameKind::Hold, 0, 0, 0, 0},
        {9, EquationFrameKind::Hold, 0, 0, 0, 0},
        {10, EquationFrameKind::HoldAction, 0, 1, 0, 6},
        {13, EquationFrameKind::HoldAction, 0, 1, 3, 6},
        {15, EquationFrameKind::HoldAction, 0, 1, 5, 6},
        {16, EquationFrameKind::Hold, 0, 0, 0, 0},
        {19, EquationFrameKind::Hold, 0, 0, 0, 0},
        {20, EquationFrameKind::Transition, 0, 0, 0, 5},
        {24, EquationFrameKind::Transition, 0, 0, 4, 5},
        {25, EquationFrameKind::Hold, 1, 0, 0, 0},
        {28, EquationFrameKind::Hold, 1, 0, 0, 0},
    };
    // 前から・後ろから・飛び飛びのどの順に引いても同じ (再生の履歴を持たない)。
    for (const auto order : {0, 1, 2}) {
        for (std::size_t n = 0; n < expects.size(); ++n) {
            const std::size_t i = order == 0   ? n
                                  : order == 1 ? expects.size() - 1 - n
                                               : (n * 7) % expects.size();
            const auto& e = expects[i];
            std::string error;
            const auto got = equationSequenceFrameAt(d, spec, e.frame, error);
            EquationFrameLookup want;
            want.kind = e.kind;
            want.state = e.state;
            if (e.kind == EquationFrameKind::Transition)
                want.transition = e.index;
            if (e.kind == EquationFrameKind::HoldAction)
                want.action = e.index;
            want.frame = e.local;
            want.frames = e.frames;
            check(got && *got == want, "source frame " + std::to_string(e.frame) +
                                           " の見え方 (順 " + std::to_string(order) + ") " + error);
        }
    }
    std::string error;
    check(!equationSequenceFrameAt(d, spec, 29, error) &&
              !equationSequenceFrameAt(d, spec, -1, error),
          "範囲外は nullopt");
    auto mismatched = spec;
    mismatched.actions.pop_back();
    check(!equationSequenceFrameAt(d, mismatched, 3, error),
          "正準入力が Project と対応しなければ nullopt");
    auto otherDuration = spec;
    otherDuration.transitions[0].frames = 6;
    check(!equationSequenceFrameAt(d, otherDuration, 21, error),
          "変形の尺が対応しなければ nullopt");
}

void readiness() {
    math::EquationBackendValidation validation;
    check(equationTargetReadiness(EquationTargetProof::BackendValidationRequired, validation, 0,
                                  1) == EquationCompileFailure::BackendValidationRequired,
          "未検証なら実行可能にしない");
    check(equationTargetReadiness(EquationTargetProof::Empty, validation, 0, 1) ==
              EquationCompileFailure::UnsupportedEmptyTarget,
          "P3-2 の Empty は検証によらず拒否");
    validation.failure = math::EquationBackendFailure::None;
    validation.segments = {{0, 0, "MathTexPart", 1, 1, 1, true, {0}},
                           {0, 1, "MathTexPart", 0, 0, 0, false, {}}};
    check(equationTargetReadiness(EquationTargetProof::BackendValidationRequired, validation, 0,
                                  0) == EquationCompileFailure::None,
          "検証済みで glyph を持つ対象は実行可能");
    check(equationTargetReadiness(EquationTargetProof::BackendValidationRequired, validation, 0,
                                  1) == EquationCompileFailure::UnsupportedEmptyTarget,
          "検証で glyph を持たない対象は空の対象");
    check(equationTargetReadiness(EquationTargetProof::BackendValidationRequired, validation, 1,
                                  0) == EquationCompileFailure::BackendValidationRequired,
          "検証に無い segment は実行可能にしない");
    validation.failure = math::EquationBackendFailure::SharedDescendant;
    check(equationTargetReadiness(EquationTargetProof::BackendValidationRequired, validation, 0,
                                  0) == EquationCompileFailure::BackendValidationRequired,
          "検証が失敗なら実行可能にしない");
    // P3-2 の既存の関数は変えない (backend の結果なしでは常に実行不可)。
    check(equationTargetReadiness(EquationTargetProof::BackendValidationRequired) ==
              EquationCompileFailure::BackendValidationRequired,
          "P3-2 の equationTargetReadiness は BackendValidationRequired のまま");
}
} // namespace

int main() {
    conversion();
    lookup();
    readiness();
    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}
