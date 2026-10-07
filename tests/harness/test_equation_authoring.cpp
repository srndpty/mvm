// P3-5: authoring の domain 操作 (並べ替え・部分式・対応・action) と、Qt に依存しない表示の判断
// (部分式の状態・UTF-16 の範囲・信頼済み編集の順の適用・既定の尺・選択の回復) の純粋な試験。
// 期待値は試験の側で手で書き、実装の式を共有しない。
#include "app/equation_sequence_authoring.h"
#include "project/equation_binding_edit.h"
#include "project/equation_sequence.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <tuple>

using namespace mvm::project;
using namespace mvm::app;

namespace {
int checks = 0, failures = 0;
constexpr int kHeight = 1080;

void check(bool ok, const std::string& message) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "失敗: %s\n", message.c_str());
    }
}

EquationState state(const std::string& id, const std::string& source = "x+y=z",
                    std::int64_t hold = 10) {
    EquationState s;
    s.id = {id};
    s.equation.source = source;
    s.revision = "r-" + id;
    s.holdFrames = hold;
    return s;
}

SemanticPart part(const EquationState& s, const std::string& id, std::int64_t begin,
                  std::int64_t end, const std::string& label = "") {
    return {{id},
            label,
            {s.revision, begin, end,
             s.equation.source.substr(static_cast<std::size_t>(begin),
                                      static_cast<std::size_t>(end - begin)),
             BindingStatus::Bound}};
}

// s0..s3、各状態に部分式 p<i> = "x" [0,1)。t0/t1 に明示対応、action は s1 の p1。
EquationSequenceClipData four() {
    EquationSequenceClipData d;
    for (int i = 0; i < 4; ++i) {
        auto s = state("s" + std::to_string(i));
        s.parts.push_back(part(s, "p" + std::to_string(i), 0, 1));
        d.states.push_back(s);
    }
    d.transitions = {{{"t0"}, {"s0"}, {"s1"}, 3, {{{"p0"}, {"p1"}}}},
                     {{"t1"}, {"s1"}, {"s2"}, 4, {{{"p1"}, {"p2"}}}},
                     {{"t2"}, {"s2"}, {"s3"}, 5, {}}};
    d.actions.push_back(
        {{"a1"}, {"s1"}, {"p1"}, EquationTargetStatus::Present, 2, 3, EquationOperation::Pulse});
    std::string error;
    check(validateEquationSequence(d, kHeight, error), "対照の 4 状態が有効: " + error);
    return d;
}

std::function<std::string()> counter(const std::string& prefix) {
    auto n = std::make_shared<int>(0);
    return [=] { return prefix + std::to_string((*n)++); };
}

std::vector<std::string> order(const EquationSequenceClipData& d) {
    std::vector<std::string> ids;
    for (const auto& s : d.states)
        ids.push_back(s.id.value);
    return ids;
}

const EquationStepTransition* edge(const EquationSequenceClipData& d, const std::string& from,
                                   const std::string& to) {
    for (const auto& t : d.transitions)
        if (t.from.value == from && t.to.value == to)
            return &t;
    return nullptr;
}

void testMoveState() {
    std::string error;
    // s1 を位置 2 へ: s0,s2,s1,s3。どの隣接も旧い同じ順の組ではない。逆向きの (s2,s1) は旧 t1
    // (s1→s2) の対応を引き継がない。
    {
        auto d = four();
        check(moveEquationState(d, {"s1"}, 2, counter("n"), 7, kHeight, error),
              "s1 を 2 へ移動: " + error);
        check(order(d) == std::vector<std::string>{"s0", "s2", "s1", "s3"}, "移動後の順");
        check(d.transitions.size() == 3, "辺は n-1");
        for (const auto& [a, b] : {std::pair{"s0", "s2"}, {"s2", "s1"}, {"s1", "s3"}}) {
            const auto* t = edge(d, a, b);
            check(t && t->frames == 7 && t->correspondence.empty() &&
                      t->id.value.rfind("n", 0) == 0,
                  std::string("新しい辺 (尺 7・対応なし) ") + a + "→" + b);
        }
        check(std::none_of(d.transitions.begin(), d.transitions.end(),
                           [](const auto& t) { return t.id.value == "t1"; }),
              "逆向きになった t1 を残さない (対応を持ち越さない)");
        check(d.actions.size() == 1 && d.actions[0].state.value == "s1",
              "action は所有状態と一緒に移る");
    }
    // s3 を先頭へ: s3,s0,s1,s2。(s0,s1)・(s1,s2) は同じ順の組なので ID・尺・対応を保つ。
    {
        auto d = four();
        check(moveEquationState(d, {"s3"}, 0, counter("n"), 7, kHeight, error),
              "s3 を先頭へ: " + error);
        const auto* kept0 = edge(d, "s0", "s1");
        const auto* kept1 = edge(d, "s1", "s2");
        check(kept0 && kept0->id.value == "t0" && kept0->frames == 3 &&
                  kept0->correspondence.size() == 1,
              "隣接が残る t0 は ID・尺・対応を保つ");
        check(kept1 && kept1->id.value == "t1" && kept1->frames == 4 &&
                  kept1->correspondence.size() == 1,
              "隣接が残る t1 は ID・尺・対応を保つ");
        const auto* fresh = edge(d, "s3", "s0");
        check(fresh && fresh->correspondence.empty() && fresh->frames == 7, "新しい隣接は新しい辺");
        check(!edge(d, "s2", "s3"), "隣接でなくなった t2 は消える");
    }
    // 同じ位置は変更なし。範囲外・存在しない状態は拒否し data を変えない。
    {
        auto d = four();
        const auto before = d;
        check(moveEquationState(d, {"s2"}, 2, counter("n"), 7, kHeight, error) && d == before,
              "同じ位置への移動は変更なし");
        check(!moveEquationState(d, {"s2"}, 4, counter("n"), 7, kHeight, error) && d == before,
              "範囲外の位置は拒否");
        check(!moveEquationState(d, {"nope"}, 0, counter("n"), 7, kHeight, error) && d == before,
              "存在しない状態は拒否");
        check(!moveEquationState(d, {"s0"}, 3, counter("n"), 0, kHeight, error) && d == before,
              "尺 0 の新しい辺は拒否 (全体検証)");
    }
    // 発行器が既存の辺 ID を返しても衝突させない。返し続けるなら有限回で失敗し data を変えない。
    {
        auto d = four();
        int calls = 0;
        auto colliding = [&]() -> std::string {
            return ++calls <= 2 ? "t0" : "fresh" + std::to_string(calls);
        };
        check(moveEquationState(d, {"s3"}, 0, colliding, 7, kHeight, error) &&
                  edge(d, "s3", "s0")->id.value != "t0",
              "既存 ID の衝突を避けて発行する");
        auto e = four();
        const auto before = e;
        check(!moveEquationState(
                  e, {"s1"}, 2, [] { return std::string("t2"); }, 7, kHeight, error) &&
                  e == before,
              "衝突し続ける発行器は有限回で失敗し data を変えない");
    }
}

void testParts() {
    std::string error;
    auto d = four();
    auto& s0 = d.states[0]; // "x+y=z"
    // 追加: 新 ID・今の revision・正しい証人。
    check(addEquationPart(d, {"s0"}, part(s0, "q", 2, 3, "y"), kHeight, error) &&
              d.states[0].parts.size() == 2,
          "部分式の追加: " + error);
    const auto afterAdd = d;
    check(!addEquationPart(d, {"s0"}, part(d.states[0], "p1", 4, 5), kHeight, error) &&
              d == afterAdd,
          "他の状態の PartId と同じ ID は拒否 (sequence 内で一意)");
    check(!addEquationPart(d, {"s0"}, part(d.states[0], "q2", 0, 3), kHeight, error) &&
              d == afterAdd,
          "Bound の部分式と重なる範囲は拒否");
    auto stale = part(d.states[0], "q3", 4, 5);
    stale.binding.revision = "old";
    check(!addEquationPart(d, {"s0"}, stale, kHeight, error) && d == afterAdd,
          "今の revision でない binding は拒否");
    auto wrongText = part(d.states[0], "q4", 4, 5);
    wrongText.binding.expectedText = "y";
    check(!addEquationPart(d, {"s0"}, wrongText, kHeight, error) && d == afterAdd,
          "証人の文字が範囲と違えば拒否");
    check(!addEquationPart(d, {"nope"}, part(d.states[0], "q5", 4, 5), kHeight, error) &&
              d == afterAdd,
          "存在しない状態は拒否");

    // 改名は ID・範囲を変えない。
    check(renameEquationPart(d, {"s0"}, {"q"}, "変数 y", kHeight, error) &&
              d.states[0].parts[1].label == "変数 y" && d.states[0].parts[1].id.value == "q",
          "改名");

    // 削除: action は missing、対応は外れる。同じ ID で作り直すと action だけ present へ戻る。
    auto e = four();
    check(deleteEquationPart(e, {"s1"}, {"p1"}, kHeight, error), "p1 の削除: " + error);
    check(e.actions[0].targetStatus == EquationTargetStatus::Missing &&
              e.transitions[0].correspondence.empty() && e.transitions[1].correspondence.empty(),
          "削除で action は missing、対応は外れる");
    const auto deleted = e;
    check(!addEquationPart(e, {"s1"}, part(e.states[1], "p1", 0, 1), kHeight, error) &&
              e == deleted,
          "欠落参照の ID で通常の追加はできない (作り直しの操作だけ)");
    check(!restoreMissingEquationPart(e, {"s1"}, part(e.states[1], "zz", 0, 1), kHeight, error) &&
              e == deleted,
          "参照されていない ID は作り直せない");
    check(!restoreMissingEquationPart(e, {"s0"}, part(e.states[0], "p1", 2, 3), kHeight, error) &&
              e == deleted,
          "別の状態に作り直せない");
    auto badBinding = part(e.states[1], "p1", 0, 1);
    badBinding.binding.revision = "old";
    check(!restoreMissingEquationPart(e, {"s1"}, badBinding, kHeight, error) && e == deleted,
          "今の式と合わない binding で作り直せない");
    check(restoreMissingEquationPart(e, {"s1"}, part(e.states[1], "p1", 2, 3, "y"), kHeight, error),
          "同じ PartId で作り直す: " + error);
    check(e.actions[0].targetStatus == EquationTargetStatus::Present &&
              e.actions[0].target.value == "p1" && e.transitions[0].correspondence.empty() &&
              e.transitions[1].correspondence.empty(),
          "作り直しで action は present に戻り、対応は戻らない");
}

void testCorrespondenceAndActions() {
    std::string error;
    auto d = four();
    const auto start = d;
    check(addEquationCorrespondence(d, {"t2"}, {{"p2"}, {"p3"}}, kHeight, error) &&
              d.transitions[2].correspondence.size() == 1,
          "対応の追加: " + error);
    const auto withPair = d;
    check(!addEquationCorrespondence(d, {"t2"}, {{"p2"}, {"p3"}}, kHeight, error) && d == withPair,
          "同じ側の重複は拒否");
    check(!addEquationCorrespondence(d, {"t2"}, {{"p0"}, {"p3"}}, kHeight, error) && d == withPair,
          "隣接状態でない部分式は拒否");
    check(!addEquationCorrespondence(d, {"tx"}, {{"p2"}, {"p3"}}, kHeight, error) && d == withPair,
          "存在しない辺は拒否");
    check(removeEquationCorrespondence(d, {"t2"}, {{"p2"}, {"p3"}}, kHeight, error) && d == start,
          "対応の削除で元に戻る");
    check(!removeEquationCorrespondence(d, {"t2"}, {{"p2"}, {"p3"}}, kHeight, error) && d == start,
          "無い対応の削除は拒否");

    // action: hold 10。区間は hold の中だけ。同じ状態の重なりは対象によらず拒否。
    const EquationAction outline{
        {"b"}, {"s0"}, {"p0"}, EquationTargetStatus::Present, 0, 4, EquationOperation::Outline};
    check(addEquationAction(d, outline, kHeight, error), "outline の追加: " + error);
    const auto withAction = d;
    auto beyond = outline;
    beyond.id = {"c"};
    beyond.start = 8;
    beyond.duration = 3;
    check(!addEquationAction(d, beyond, kHeight, error) && d == withAction,
          "hold を超える区間は拒否 (8+3 > 10)");
    auto overlapping = outline;
    overlapping.id = {"c"};
    overlapping.start = 3;
    overlapping.operation = EquationOperation::Pulse;
    check(!addEquationAction(d, overlapping, kHeight, error) && d == withAction,
          "同じ状態で重なる action は拒否");
    auto otherState = outline;
    otherState.id = {"c"};
    otherState.target = {"p1"};
    check(!addEquationAction(d, otherState, kHeight, error) && d == withAction,
          "別の状態の部分式は対象にできない");
    auto zero = outline;
    zero.id = {"c"};
    zero.start = 5;
    zero.duration = 0;
    check(!addEquationAction(d, zero, kHeight, error) && d == withAction, "長さ 0 は拒否");

    auto moved = outline;
    moved.start = 6;
    moved.duration = 4;
    moved.operation = EquationOperation::Pulse;
    check(updateEquationAction(d, moved, kHeight, error) && d.actions.back() == moved,
          "action の変更 (区間と operation): " + error);
    auto stolen = moved;
    stolen.state = {"s1"};
    stolen.target = {"p1"};
    const auto beforeSteal = d;
    check(!updateEquationAction(d, stolen, kHeight, error) && d == beforeSteal,
          "所有状態は変えられない");
    auto tooLong = moved;
    tooLong.duration = 5;
    check(!updateEquationAction(d, tooLong, kHeight, error) && d == beforeSteal,
          "変更でも hold を超える区間は拒否");
    check(deleteEquationAction(d, {"b"}, kHeight, error) && d.actions.size() == 1, "action の削除");
    check(!deleteEquationAction(d, {"b"}, kHeight, error), "無い action の削除は拒否");
}

void testPartStatusAndViews() {
    // UTF-8 と UTF-16 の範囲: "α+β" の β は byte [3,5)、UTF-16 [2,3)。
    auto s = state("s", "\xCE\xB1+\xCE\xB2");
    s.parts.push_back(part(s, "beta", 3, 5, "β"));
    EquationSequenceClipData d;
    d.states.push_back(s);
    auto views = equationPartViews(d, {"s"});
    check(views.size() == 1 && views[0].status == EquationPartStatus::Bound &&
              views[0].rangeUtf16 && views[0].rangeUtf16->first == 2 &&
              views[0].rangeUtf16->second == 3,
          "非 ASCII の範囲を UTF-16 で表す");

    auto frac = state("f", "\\frac{a}{b}");
    check(equationPartStatus(frac, part(frac, "x", 0, 7)) ==
              EquationPartStatus::UnsupportedTexBoundary,
          "\\frac{a の範囲は分離できない");
    check(equationPartStatus(frac, part(frac, "x", 6, 7)) == EquationPartStatus::Bound,
          "分子の a は分離できる");
    auto spaced = state("w", "x + y");
    check(equationPartStatus(spaced, part(spaced, "x", 1, 2)) ==
              EquationPartStatus::UnsupportedEmptyTarget,
          "空白だけの範囲は描く文字が無い");
    auto invalid = part(spaced, "x", 0, 1);
    invalid.binding.status = BindingStatus::Invalid;
    check(equationPartStatus(spaced, invalid) == EquationPartStatus::InvalidBinding,
          "Invalid の binding");

    // 欠落参照は action から作り、Invalid は今の範囲を出さない。
    auto e = four();
    std::string error;
    check(deleteEquationPart(e, {"s1"}, {"p1"}, kHeight, error), "削除の準備");
    views = equationPartViews(e, {"s1"});
    check(views.size() == 1 && views[0].status == EquationPartStatus::MissingTarget &&
              !views[0].exists && views[0].actions.size() == 1 && !views[0].rangeUtf16,
          "欠落した部分式を action の参照から示す");
    check(equationPartRepairable(EquationPartStatus::MissingTarget) &&
              equationPartRepairable(EquationPartStatus::InvalidBinding) &&
              !equationPartRepairable(EquationPartStatus::Bound),
          "修復の操作は Bound 以外");
    check(!equationCompileFailureRepairable(EquationCompileFailure::InvalidSequence) &&
              equationCompileFailureRepairable(EquationCompileFailure::InvalidBinding) &&
              equationCompileFailureRepairable(EquationCompileFailure::MissingPart),
          "InvalidSequence には修復を出さない");
    check(equationBackendFailureIsContent(mvm::math::EquationBackendFailure::EmptyActionTarget) &&
              !equationBackendFailureIsContent(mvm::math::EquationBackendFailure::CorruptFrame),
          "空の対象は内容、壊れた frame は描画結果の問題");
}

// 編集欄の順の記録を、P3-2 の信頼済み編集として順に適用する。
std::vector<TrustedEquationEdit>
replay(const std::string& base,
       std::vector<std::tuple<std::size_t, std::size_t, std::string>> steps) {
    std::vector<TrustedEquationEdit> edits;
    std::string current = base;
    for (const auto& [b, e, r] : steps) {
        TrustedEquationEdit edit;
        edit.oldSource = current;
        edit.beginUtf16 = b;
        edit.endUtf16 = e;
        edit.replacement = r;
        current = current.substr(0, b) + r + current.substr(e); // ASCII だけ (UTF-16 = byte)
        edit.newSource = current;
        edits.push_back(edit);
    }
    return edits;
}

void testTrustedEdits() {
    std::string error;
    auto make = [] {
        EquationSequenceClipData d;
        auto s = state("s", "a+bc+d", 10);
        s.parts.push_back(part(s, "bc", 2, 4, "bc"));
        d.states.push_back(s);
        return d;
    };
    auto revision = counter("rev");
    const auto bound = [](const EquationSequenceClipData& d) {
        return d.states[0].parts[0].binding;
    };
    {
        auto d = make();
        check(applyTrustedEquationEdits(d, {"s"}, replay("a+bc+d", {{0, 0, "xy"}}), revision,
                                        kHeight, error) &&
                  bound(d).status == BindingStatus::Bound && bound(d).begin == 4 &&
                  bound(d).end == 6,
              "部分式より前の挿入で範囲が移る: " + error);
    }
    {
        auto d = make();
        check(applyTrustedEquationEdits(d, {"s"}, replay("a+bc+d", {{2, 2, "Q"}}), revision,
                                        kHeight, error) &&
                  bound(d).status == BindingStatus::Bound && bound(d).begin == 3,
              "begin ちょうどの挿入は前 (範囲が移る)");
    }
    {
        auto d = make();
        check(applyTrustedEquationEdits(d, {"s"}, replay("a+bc+d", {{4, 4, "Q"}}), revision,
                                        kHeight, error) &&
                  bound(d).status == BindingStatus::Bound && bound(d).begin == 2 &&
                  bound(d).end == 4,
              "end ちょうどの挿入は後 (範囲はそのまま)");
    }
    {
        auto d = make();
        check(applyTrustedEquationEdits(d, {"s"}, replay("a+bc+d", {{3, 3, "Q"}}), revision,
                                        kHeight, error) &&
                  bound(d).status == BindingStatus::Invalid,
              "部分式の中の編集で Invalid");
    }
    {
        auto d = make();
        check(applyTrustedEquationEdits(d, {"s"}, replay("a+bc+d", {{1, 3, ""}}), revision, kHeight,
                                        error) &&
                  bound(d).status == BindingStatus::Invalid,
              "境界を横切る削除で Invalid");
    }
    // 両側の編集を順に記録すれば部分式は無傷 (両側を覆う一つの範囲にまとめると無効になる)。
    {
        auto d = make();
        check(applyTrustedEquationEdits(d, {"s"}, replay("a+bc+d", {{0, 0, "("}, {7, 7, ")"}}),
                                        revision, kHeight, error) &&
                  bound(d).status == BindingStatus::Bound && bound(d).begin == 3 &&
                  d.states[0].equation.source == "(a+bc+d)",
              "順に適用すれば両側の編集で部分式は無傷: " + error);
    }
    // 無効になった後、同じ文字が式の別の場所にあっても付け直さない。
    {
        auto d = make();
        check(applyTrustedEquationEdits(d, {"s"}, replay("a+bc+d", {{3, 3, "Q"}, {0, 0, "bc+"}}),
                                        revision, kHeight, error) &&
                  bound(d).status == BindingStatus::Invalid && bound(d).begin == 2,
              "同じ文字 (bc) が先頭にできても自動で付け直さない");
    }
    // 記録と式が合わなければ全体を拒否し data を変えない。
    {
        auto d = make();
        const auto before = d;
        auto wrong = replay("a+bc+d", {{0, 0, "x"}, {0, 0, "y"}});
        wrong[1].oldSource = "zzz";
        check(!applyTrustedEquationEdits(d, {"s"}, wrong, revision, kHeight, error) && d == before,
              "途中の記録が合わなければ何も確定しない");
        check(!applyTrustedEquationEdits(d, {"s"}, {}, revision, kHeight, error) && d == before,
              "記録が無ければ拒否");
    }
}

void testDefaultsAndSelection() {
    check(equationDefaultHoldFrames(60, 1) == 60 && equationDefaultTransitionFrames(60, 1) == 30,
          "60fps の既定");
    check(equationDefaultHoldFrames(30000, 1001) == 30 &&
              equationDefaultTransitionFrames(30000, 1001) == 15,
          "29.97fps の既定");
    check(equationDefaultTransitionFrames(1, 1) == 1 && equationDefaultHoldFrames(1, 10) == 1,
          "既定は 1 frame 以上");
    check(equationFramesText(30, 60, 1) == "30f (0.50 秒)", "尺の表示は frame と派生の秒");
    auto ids = counter("id");
    const auto data = newEquationSequenceData("x^2", 60, ids);
    std::string error;
    check(data.states.size() == 1 && data.transitions.empty() && data.actions.empty() &&
              data.states[0].parts.empty() && data.states[0].holdFrames == 60 &&
              data.states[0].equation.backgroundColor == "#00000000" &&
              validateEquationSequence(data, kHeight, error),
          "新しい sequence は最小で有効: " + error);
    MathClipData styled;
    styled.fontSize = 64;
    styled.color = "#FF40C0FF";
    styled.backgroundColor = "#FF000000";
    const auto inserted = newEquationState(styled, "y", 10, ids);
    check(inserted.equation.fontSize == 64 && inserted.equation.color == "#FF40C0FF" &&
              inserted.equation.backgroundColor == "#00000000" && inserted.parts.empty() &&
              !inserted.revision.empty() && inserted.id.value != inserted.revision,
          "挿入する状態は書式を写し背景は透明");
    check(!nearestSurvivingIndex(3, 0) && *nearestSurvivingIndex(3, 2) == 1 &&
              *nearestSurvivingIndex(0, 2) == 0,
          "消えた選択は同じ位置か最後へ");
}
} // namespace

int main() {
    testMoveState();
    testParts();
    testCorrespondenceAndActions();
    testPartStatusAndViews();
    testTrustedEdits();
    testDefaultsAndSelection();
    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}
