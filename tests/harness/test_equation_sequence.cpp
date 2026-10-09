#include "project/equation_sequence_edit.h"
#include "project/project_json.h"

#include <cstdio>
#include <limits>
#include <unordered_set>

using namespace mvm::project;

namespace {
int checks = 0, failures = 0;

void check(bool ok, const std::string& message) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "失敗: %s\n", message.c_str());
    }
}

EquationState state(const std::string& id, std::int64_t hold) {
    EquationState s;
    s.id = {id};
    s.holdFrames = hold;
    s.equation.source = "x+x";
    s.revision = "rev-" + id;
    s.parts = {{{"part-" + id}, "項", {s.revision, 0, 1, "x", BindingStatus::Bound}}};
    return s;
}

EquationSequenceClipData sequence(int count = 3) {
    EquationSequenceClipData d;
    for (int i = 0; i < count; ++i) {
        const auto id = "s" + std::to_string(i);
        d.states.push_back(state(id, 4));
        if (i > 0)
            d.transitions.push_back({{"t" + std::to_string(i - 1)},
                                     d.states[static_cast<std::size_t>(i - 1)].id,
                                     {id},
                                     2,
                                     {}});
    }
    d.actions.push_back({{"a0"},
                         {"s0"},
                         {"part-s0"},
                         EquationTargetStatus::Present,
                         1,
                         2,
                         EquationOperation::Outline});
    return d;
}

Project project(EquationSequenceClipData d = sequence(), std::int64_t fps = 60) {
    auto p = createDefaultProject();
    p.timelineFpsNum = fps;
    const auto result =
        addEquationSequence(p, std::move(d), "clip", "数式", {TrackKind::Video, 0}, 0);
    check(result.success, "対照 Project の作成: " + result.error);
    return p;
}

void boundaries() {
    std::string error;
    auto d = sequence(1);
    check(validateEquationSequence(d, 1080, error), "一状態の対照");
    for (int n : {1, 2, 3}) {
        d = sequence(n);
        const int length = n * 4 + (n - 1) * 2;
        for (int f = 0; f < length; ++f) {
            const auto e = evaluateEquationSequence(d, f, error);
            const auto i = f / 6;
            const auto local = f % 6;
            check(e && e->state == StateId{"s" + std::to_string(i)} &&
                      e->transition == (local >= 4) &&
                      e->localFrame == (local >= 4 ? local - 4 : local),
                  "手計算 H/T 境界 frame " + std::to_string(f));
            if (e && e->transition)
                check(e->progressNumerator == local - 4 && e->progressDenominator == 2,
                      "i/N は 1 を含まない");
        }
        check(!evaluateEquationSequence(d, -1, error) &&
                  !evaluateEquationSequence(d, length, error),
              "最初と最後の範囲外");
    }
    d = sequence(2);
    d.transitions[0].frames = 1;
    auto e = evaluateEquationSequence(d, 4, error);
    check(e && e->transition && e->progressNumerator == 0 && e->progressDenominator == 1,
          "N=1 の source 標本");
    e = evaluateEquationSequence(d, 5, error);
    check(e && !e->transition && e->localFrame == 0, "N=1 の次は target hold 0");
    for (int f = 0; f < 5; ++f) {
        e = evaluateEquationSequence(d, f, error);
        check(e && e->activeAction.has_value() == (f == 1 || f == 2), "action の半開区間");
    }
}

void invalid() {
    const auto good = sequence();
    std::string error;
    auto reject = [&](auto mutate, const char* message) {
        auto d = good;
        mutate(d);
        check(!validateEquationSequence(d, 1080, error), message);
    };
    reject([](auto& d) { d.states.clear(); }, "空状態");
    reject([](auto& d) { d.states[0].holdFrames = 0; }, "hold 0");
    reject([](auto& d) { d.transitions[0].frames = 0; }, "transition 0");
    reject([](auto& d) { d.actions[0].duration = 0; }, "action 0");
    reject([](auto& d) { d.states[0].holdFrames = std::numeric_limits<std::int64_t>::max(); },
           "全長 overflow");
    reject([](auto& d) { d.actions[0].start = std::numeric_limits<std::int64_t>::max(); },
           "action 終端 overflow");
    reject([](auto& d) { d.states[1].id = d.states[0].id; }, "重複 StateId");
    reject([](auto& d) { d.transitions[1].id = d.transitions[0].id; }, "重複 TransitionId");
    reject([](auto& d) { d.states[1].parts[0].id = d.states[0].parts[0].id; }, "重複 PartId");
    reject(
        [](auto& d) {
            auto a = d.actions[0];
            a.start = 3;
            a.duration = 1;
            d.actions.push_back(a);
        },
        "重複 ActionId");
    reject([](auto& d) { d.actions[0].target = {"part-s1"}; }, "別状態の PartId");
    reject([](auto& d) { d.actions[0].state = {"missing"}; }, "欠落 StateId");
    reject([](auto& d) { d.actions[0].target = {"missing"}; }, "明示しない欠落 PartId");
    reject([](auto& d) { d.actions[0].start = 3; }, "hold を跨ぐ action");
    reject(
        [](auto& d) {
            auto a = d.actions[0];
            a.id = {"a1"};
            d.actions.push_back(a);
        },
        "同じ対象の重複 action");
    reject(
        [](auto& d) {
            d.states[0].parts.push_back(
                {{"another"}, "項", {d.states[0].revision, 2, 3, "x", BindingStatus::Bound}});
            auto a = d.actions[0];
            a.id = {"a1"};
            a.target = {"another"};
            d.actions.push_back(a);
        },
        "異なる対象の同時 action");
    reject([](auto& d) { d.transitions[0].to = {"s2"}; }, "vector 順と異なる辺");
    reject([](auto& d) { d.transitions.pop_back(); }, "辺の数");
    reject(
        [](auto& d) {
            d.transitions[0].correspondence = {{{"part-s0"}, {"part-s1"}},
                                               {{"part-s0"}, {"part-s1"}}};
        },
        "一対一でない対応");
    reject([](auto& d) { d.transitions[0].correspondence = {{{"part-s0"}, {"part-s2"}}}; },
           "隣接でない対応");
    reject([](auto& d) { d.actions[0].operation = static_cast<EquationOperation>(7); },
           "未知 operation");
    reject([](auto& d) { d.states[0].equation.backgroundColor = "#FF000000"; }, "不透明背景");
    reject([](auto& d) { d.states[0].equation.backgroundColor = "#80000000"; }, "半透明背景");
    reject([](auto& d) { d.states[0].parts[0].binding.end = 9; }, "bound 範囲外");
    reject(
        [](auto& d) {
            d.states[0].equation.source = "あx";
            d.states[0].parts[0].binding.begin = 1;
            d.states[0].parts[0].binding.end = 2;
        },
        "UTF-8 codepoint の途中を bound 境界にしない");
    reject(
        [](auto& d) {
            auto& b = d.states[0].parts[0].binding;
            b.status = BindingStatus::Invalid;
            b.end = std::numeric_limits<std::int64_t>::max();
        },
        "invalid でも旧範囲の証人長を検証");
    auto d = good;
    d.states[0].equation.backgroundColor = "#00aBcDef";
    check(validateEquationSequence(d, 1080, error), "既存の色文法と alpha=0 の契約");
    d = good;
    d.actions[0].target = {"missing"};
    d.actions[0].targetStatus = EquationTargetStatus::Missing;
    check(validateEquationSequence(d, 1080, error), "明示 missing の修復可能な対照");
    d = good;
    d.states[0].equation.source = "あx";
    d.states[0].parts[0].binding.end = 3;
    d.states[0].parts[0].binding.expectedText = "あ";
    check(validateEquationSequence(d, 1080, error), "UTF-8 byte 範囲の独立な対照");
    auto p = project();
    p.timelineClips[0].speedNum = 2;
    check(!validateTimeline(p).success, "unsupported retime");
}

void edits() {
    std::string error;
    auto rangeCase = [&](const std::string& name, std::int64_t out, std::int64_t expectedLength,
                         std::int64_t expectedOut, const auto& edit, bool succeeds = true) {
        auto p = project();
        p.timelineClips[0].sourceInFrame = 2;
        p.timelineClips[0].sourceOutFrame = out;
        const auto before = p;
        const auto result = editEquationSequence(p, "clip", edit);
        check(result.success == succeeds, name + "の成否");
        if (succeeds) {
            const auto& clip = p.timelineClips[0];
            check(clip.sourceFrameCount == expectedLength && clip.sourceInFrame == 2 &&
                      clip.sourceOutFrame == expectedOut,
                  name + "の外側 source 範囲");
        } else {
            check(p == before, name + "の原子的拒否");
        }
    };
    auto hold = [](std::int64_t frames) {
        return
            [=](auto& data, auto& e) { return changeEquationHold(data, {"s2"}, frames, 1080, e); };
    };
    auto transition = [](std::int64_t frames) {
        return [=](auto& data, auto& e) {
            return changeEquationTransition(data, {"t0"}, frames, 1080, e);
        };
    };
    rangeCase("全体末尾 hold 延長", 16, 18, 18, hold(6));
    rangeCase("全体末尾 hold 短縮", 16, 14, 14, hold(2));
    rangeCase("全体末尾 transition 延長", 16, 18, 18, transition(4));
    rangeCase("全体末尾 transition 短縮", 16, 15, 15, transition(1));
    rangeCase("状態挿入", 16, 19, 19, [](auto& data, auto& e) {
        return insertEquationState(
            data, 1, state("insert", 3),
            {{{"ea"}, {"s0"}, {"insert"}, 1, {}}, {{"eb"}, {"insert"}, {"s1"}, 1, {}}}, 1080, e);
    });
    rangeCase("状態削除", 16, 9, 9, [](auto& data, auto& e) {
        return deleteEquationState(
            data, {"s1"}, EquationStepTransition{{"edge"}, {"s0"}, {"s2"}, 1, {}}, 1080, e);
    });
    rangeCase("右 trim 延長", 12, 18, 12, hold(6));
    rangeCase("右 trim 内で短縮", 12, 14, 12, hold(2));
    rangeCase("可視末尾より短縮", 15, 13, 15, hold(1), false);
    auto d = sequence();
    d.transitions[0].correspondence = {{{"part-s0"}, {"part-s1"}}};
    d.transitions[1].correspondence = {{{"part-s1"}, {"part-s2"}}};
    d.actions.push_back({{"a1"},
                         {"s1"},
                         {"part-s1"},
                         EquationTargetStatus::Present,
                         0,
                         1,
                         EquationOperation::Pulse});
    check(deleteEquationState(
              d, {"s1"}, EquationStepTransition{{"new-edge"}, {"s0"}, {"s2"}, 1, {}}, 1080, error),
          "中間状態の原子的削除: " + error);
    check(d.states.size() == 2 && d.actions.size() == 1 && d.transitions.size() == 1 &&
              d.transitions[0].correspondence.empty(),
          "所有 action を削除し新しい辺は対応を継承しない");
    check(insertEquationState(
              d, 1, state("insert", 3),
              {{{"edge-a"}, {"s0"}, {"insert"}, 1, {}}, {{"edge-b"}, {"insert"}, {"s2"}, 1, {}}},
              1080, error),
          "中間状態の挿入");
    check(changeEquationHold(d, {"insert"}, 5, 1080, error) &&
              changeEquationTransition(d, {"edge-a"}, 3, 1080, error),
          "内部尺編集");
    const auto before = d;
    check(!changeEquationHold(d, {"s0"}, 1, 1080, error) && d == before,
          "action を切る編集は原子的に拒否");
    check(replaceEquationSource(d, {"s0"}, "x+x", "new-revision", 1080, error),
          "同一文字列への source 置換");
    check(d.states[0].parts[0].binding.status == BindingStatus::Invalid,
          "同じ文字で binding を復活させない");
    check(deleteEquationPart(d, {"s0"}, {"part-s0"}, 1080, error) &&
              d.actions[0].targetStatus == EquationTargetStatus::Missing,
          "部分式削除は明示 missing を保持");
    auto identical = sequence();
    identical.states[0].parts.push_back(
        {{"same-text"},
         "同じ文字",
         {identical.states[0].revision, 2, 3, "x", BindingStatus::Bound}});
    check(deleteEquationPart(identical, {"s0"}, {"part-s0"}, 1080, error) &&
              identical.actions[0].target == PartId{"part-s0"} &&
              identical.actions[0].targetStatus == EquationTargetStatus::Missing,
          "同じ文字の別 PartId へ欠落対象を自動で付け替えない");
    auto p = project(d);
    --p.timelineClips[0].sourceOutFrame;
    const auto beforeProject = p;
    auto result = editEquationSequence(p, "clip", [&](auto& data, auto& e) {
        return changeEquationHold(data, {"s2"}, 1, 1080, e);
    });
    check(!result.success && p == beforeProject, "内部編集で可視範囲を勝手に短縮しない");
}

void ownership() {
    std::string error;
    auto p = project();
    auto& data = p.timelineClips[0].equationSequence;
    check(deleteEquationPart(data, {"s0"}, {"part-s0"}, 1080, error), "コピーの missing 対照");
    auto a = data.actions[0];
    a.id = {"another-action"};
    a.start = 3;
    a.duration = 1;
    data.actions.push_back(a);
    data.states[1].parts[0].id = {"fresh-1"};
    data.actions.push_back({{"dangling-action"},
                            {"s1"},
                            {"fresh-2"},
                            EquationTargetStatus::Missing,
                            0,
                            1,
                            EquationOperation::Pulse});
    data.transitions[1].correspondence = {{{"fresh-1"}, {"part-s2"}}};
    int serial = 0;
    auto newId = [&] { return "fresh-" + std::to_string(++serial); };
    TimelineClip copy;
    check(copyEquationSequenceClip(p.timelineClips[0], copy, newId, error), "コピー ID 発行");
    check(copy.id != p.timelineClips[0].id &&
              copy.equationSequence.states[0].id != data.states[0].id &&
              copy.equationSequence.actions[0].target == copy.equationSequence.actions[1].target &&
              copy.equationSequence.actions[0].targetStatus == EquationTargetStatus::Missing,
          "欠落参照も同じ新 ID へ remap");
    check(copy.equationSequence.states[0].equation == data.states[0].equation &&
              copy.equationSequence.actions[0].start == data.actions[0].start,
          "所有 ID の変更で意味と時刻を変えない");
    check(validateEquationSequence(copy.equationSequence, 1080, error), "コピー後の整合");
    const auto& remapped = copy.equationSequence;
    check(remapped.transitions[1].from == remapped.states[1].id &&
              remapped.transitions[1].to == remapped.states[2].id &&
              remapped.transitions[1].correspondence[0].from == remapped.states[1].parts[0].id &&
              remapped.transitions[1].correspondence[0].to == remapped.states[2].parts[0].id &&
              remapped.actions[2].state == remapped.states[1].id,
          "全辺の端点・対応・action 所有状態を一括 remap");
    check(remapped.states[0].id.value != "fresh-1" && remapped.states[0].id.value != "fresh-2" &&
              remapped.actions[2].target.value != "fresh-1" &&
              remapped.actions[2].target.value != "fresh-2",
          "発行候補が実在 ID と欠落 ID に衝突したら両方を避ける");
    TimelineClip duplicate;
    check(copyEquationSequenceClip(copy, duplicate, newId, error) && duplicate.id != copy.id,
          "複製 ID 発行");
    const auto before = copy;
    check(!remapEquationSequenceIds(
              copy.equationSequence, [] { return "s0"; }, error) &&
              copy == before,
          "衝突発行器は有限回で原子的に拒否");
    for (auto operation : {EquationOperation::Outline, EquationOperation::Pulse})
        for (int split : {2, 5, 8}) {
            auto original = project();
            original.timelineClips[0].equationSequence.actions[0].operation = operation;
            auto divided = original;
            const auto result =
                splitTimelineClips(divided, {"clip"}, split, newId, LinkMode::Single);
            check(result.success, "hold/transition/action 中の分割: " + result.error);
            if (!result.success)
                continue;
            const auto& left = divided.timelineClips[0];
            const auto& right = divided.timelineClips[1];
            check(left.id == "clip" &&
                      left.equationSequence == original.timelineClips[0].equationSequence &&
                      left.sourceOutFrame == split && right.sourceInFrame == split &&
                      right.sourceOutFrame == 16 && right.equationSequence.states.size() == 3 &&
                      right.equationSequence.states[0].id != left.equationSequence.states[0].id,
                  "左 ID 維持と完全データの独立所有");
            const auto old = evaluateEquationClip(original.timelineClips[0], {60, 1}, split, error);
            const auto first = evaluateEquationClip(right, {60, 1}, 0, error);
            check(old && first && old->transition == first->transition &&
                      old->localFrame == first->localFrame &&
                      old->progressNumerator == first->progressNumerator &&
                      old->activeAction.has_value() == first->activeAction.has_value(),
                  "右片の初回 seek は分割前と同じ局所時刻");
        }
}

void mapping() {
    std::string error;

    struct Case {
        std::int64_t source, den, output;
        std::vector<std::int64_t> samples;
    };

    for (const auto& c : std::vector<Case>{{60, 1, 60, {0, 1, 2, 3, 4, 5}},
                                           {24, 1, 60, {0, 0, 0, 1, 1, 2, 2, 2}},
                                           {60, 1, 24, {0, 2, 5, 7, 10, 12}},
                                           {30000, 1001, 60, {0, 0, 0, 1, 1, 2, 2, 3}}}) {
        auto p = project();
        auto clip = p.timelineClips[0];
        clip.sourceFpsNum = c.source;
        clip.sourceFpsDen = c.den;
        for (std::size_t i = 0; i < c.samples.size(); ++i) {
            const auto f = clipSourceFrameAt(clip, c.output, 1, static_cast<std::int64_t>(i));
            check(f.success && f.frame == c.samples[i], "手計算有理数 mapping " +
                                                            std::to_string(c.source) + " frame " +
                                                            std::to_string(i));
        }
    }
    // double では 2^53+1 の整数を保持できない境界。
    const auto large = mvm::core::sourceFrameAtOutputPosition(9007199254740993LL, {60, 1}, {60, 1});
    check(large && *large == 9007199254740993LL, "浮動小数では保持できない厳密境界");
    const auto floorLarge =
        mvm::core::convertFrameBoundary(9007199254740993LL, {60, 1}, {60, 1}, false);
    check(floorLarge && *floorLarge == 9007199254740993LL,
          "Sequence の frame 始点も厳密な整数境界");
    check(!mvm::core::convertFrameBoundary(std::numeric_limits<std::int64_t>::max(), {1, 1},
                                           {60, 1}, true),
          "FPS 変換 result overflow");
    check(!mvm::core::sourceFrameAtOutputPosition(std::numeric_limits<std::int64_t>::max(),
                                                  {std::numeric_limits<std::int64_t>::max(), 1},
                                                  {1, std::numeric_limits<std::int64_t>::max()}),
          "FPS 中間積 overflow");
    for (int start : {1, 2, 5, 8}) {
        auto p = project();
        const auto original = p.timelineClips[0];
        const auto trim = trimTimelineClip(p, "clip", TrimEdge::Left, start, LinkMode::Single);
        check(trim.success && p.timelineClips[0].equationSequence == original.equationSequence,
              "trim はデータを変更しない");
        const auto before = evaluateEquationClip(original, {60, 1}, start, error);
        const auto after = evaluateEquationClip(p.timelineClips[0], {60, 1}, 0, error);
        check(before && after && before->state == after->state &&
                  before->localFrame == after->localFrame &&
                  before->activeAction == after->activeAction &&
                  before->progressNumerator == after->progressNumerator,
              "trim 内の hold/transition/action 位相維持");
    }
    auto p = project();
    p.timelineClips[0].timelineStartFrame = 60;
    const auto clip = p.timelineClips[0];
    check(setTimelineFrameRate(p, 24, 1).success &&
              p.timelineClips[0].equationSequence == clip.equationSequence &&
              p.timelineClips[0].sourceFpsNum == 60 && p.timelineClips[0].sourceFrameCount == 16 &&
              p.timelineClips[0].timelineStartFrame == 24,
          "Project FPS 変更は内部時間と ID を保持");
    auto d = sequence();
    d.actions[0].duration = 1;
    auto low = project(d);
    const auto rendering = equationRenderability(low.timelineClips[0], {24, 1}, 1080);
    check(validateTimeline(low).success && !rendering.renderable &&
              rendering.unsampledActions == std::vector<ActionId>{{"a0"}},
          "構造は有効でも出力標本ゼロを明示する");
    check(equationRenderability(low.timelineClips[0], {60, 1}, 1080).renderable,
          "同 fps は action を表示可能");
    auto fractional = createDefaultProject();
    fractional.timelineFpsNum = 30000;
    fractional.timelineFpsDen = 1001;
    check(
        addEquationSequence(fractional, sequence(), "fractional", "数式", {TrackKind::Video, 0}, 0)
                .success &&
            fractional.timelineClips[0].sourceFpsNum == 30000 &&
            fractional.timelineClips[0].sourceFpsDen == 1001,
        "作成時の非整数 Project FPS を source FPS として保存");
    auto reverse = project();
    for (const auto& [position, expected] : std::vector<std::pair<std::int64_t, std::int64_t>>{
             {0, 0}, {1, 2}, {2, 4}, {3, 6}, {4, 8}, {5, 10}}) {
        const auto mapped = clipSourceFrameAt(reverse.timelineClips[0], 30000, 1001, position);
        check(mapped.success && mapped.frame == expected, "60→30000/1001 も厳密な frame 始点");
    }
    for (auto source : std::vector<mvm::core::FrameRate>{{24, 1}, {60, 1}, {30000, 1001}}) {
        auto mixed = project();
        mixed.timelineClips[0].sourceFpsNum = source.num;
        mixed.timelineClips[0].sourceFpsDen = source.den;
        // [in,out) の始点は ceil(in R)。trim 前の同じ output 位置から開始する。
        const auto original = mixed.timelineClips[0];
        mixed.timelineClips[0].sourceInFrame = 5;
        const auto origin = clipSourceBoundaryToTimeline(original, 5, 60, 1);
        const auto before = evaluateEquationClip(original, {60, 1}, origin.frame, error);
        const auto after = evaluateEquationClip(mixed.timelineClips[0], {60, 1}, 0, error);
        check(origin.success && before && after && before->state == after->state &&
                  before->localFrame == after->localFrame &&
                  before->progressNumerator == after->progressNumerator,
              "異 FPS trim は素材原点の sampling 位相を維持する");
    }
    auto outside = project();
    const auto unchanged = outside;
    check(!trimTimelineClip(outside, "clip", TrimEdge::Right, 1, LinkMode::Single).success &&
              outside == unchanged,
          "L の外へ右延長を clamp せず拒否");
    outside.timelineClips[0].timelineStartFrame = 10;
    const auto leftUnchanged = outside;
    check(!trimTimelineClip(outside, "clip", TrimEdge::Left, -1, LinkMode::Single).success &&
              outside == leftUnchanged,
          "素材 0 より前へ左延長を拒否");
    check(!trimTimelineClip(outside, "clip", TrimEdge::Left,
                            std::numeric_limits<std::int64_t>::min(), LinkMode::Single)
                  .success &&
              outside == leftUnchanged,
          "INT64_MIN trim delta を拒否");
    auto mixed = project();
    mixed.timelineClips[0].sourceFpsNum = 24;
    int serial = 0;
    const auto mixedBefore = mixed;
    check(!splitTimelineClips(
               mixed, {"clip"}, 1, [&] { return "split-" + std::to_string(++serial); },
               LinkMode::Single)
                  .success &&
              mixed == mixedBefore,
          "整数素材境界で表せない split を原子的に拒否");
    check(splitTimelineClips(
              mixed, {"clip"}, 5, [&] { return "split-" + std::to_string(++serial); },
              LinkMode::Single)
                  .success &&
              mixed.timelineClips[0].sourceOutFrame == 2 &&
              mixed.timelineClips[1].sourceInFrame == 2,
          "24→60 の正確な split 境界は source 2 / output 5");
    auto invalidSource = project();
    check(replaceEquationSource(invalidSource.timelineClips[0].equationSequence, {"s0"}, "x+x",
                                "changed", 1080, error) &&
              validateTimeline(invalidSource).success &&
              !equationRenderability(invalidSource.timelineClips[0], {60, 1}, 1080).renderable,
          "修復可能 invalid binding を構造と出力可能性で分ける");
}

std::string replace(std::string text, const std::string& from, const std::string& to) {
    const auto at = text.find(from);
    check(at != std::string::npos, "JSON 変異の対象が存在する");
    if (at != std::string::npos)
        text.replace(at, from.size(), to);
    return text;
}

void persistence() {
    auto p = project();
    std::string error;
    p.timelineClips[0].equationSequence.transitions[0].correspondence = {
        {{"part-s0"}, {"part-s1"}}};
    auto saved = serializeProjectJson(p, "equation-test.mvm");
    check(saved.success, "schema 21 保存: " + saved.error);
    auto loaded = parseProjectJsonText(saved.json, "equation-test.mvm");
    check(loaded.success && loaded.project == p, "schema 21 exact 往復: " + loaded.error);
    for (const auto* operation : {"set_color", "reveal", "conceal", "unknown"}) {
        const auto bad = replace(saved.json, "\"operation\":\"outline\"",
                                 std::string("\"operation\":\"") + operation + "\"");
        check(!parseProjectJsonText(bad, "equation-test.mvm").success,
              "未知 operation の JSON 拒否");
    }
    check(!parseProjectJsonText(
               replace(saved.json, "\"hold_frames\":4", "\"hold_frames\":4,\"backend\":\"manim\""),
               "equation-test.mvm")
               .success,
          "未知 state field 拒否");
    check(!parseProjectJsonText(
               replace(saved.json, "\"states\": [", "\"cache_key\":\"x\",\"states\": ["),
               "equation-test.mvm")
               .success,
          "未知 sequence field 拒否");
    check(!parseProjectJsonText(replace(saved.json, "\"kind\": \"equation_sequence\"",
                                        "\"backend\":\"manim\",\"kind\": \"equation_sequence\""),
                                "equation-test.mvm")
               .success,
          "kind より前の未知 clip field も拒否");
    for (const auto& mutation : std::vector<std::pair<std::string, std::string>>{
             {"\"label\":", "\"glyph_id\":4,\"label\":"},
             {"\"expected_text\":", "\"segment_index\":0,\"expected_text\":"},
             {"\"frames\":2", "\"frames\":2,\"manim\":\"x\""},
             {"\"target_status\":", "\"cache_key\":\"x\",\"target_status\":"},
             {"\"from\":\"part-s0\"", "\"from\":\"part-s0\",\"renderer_handle\":9"}}) {
        check(!parseProjectJsonText(replace(saved.json, mutation.first, mutation.second),
                                    "equation-test.mvm")
                   .success,
              "新 object の renderer 派生 field を拒否");
    }
    check(!parseProjectJsonText(replace(saved.json, "\"operation\":\"outline\"",
                                        "\"operation\":\"outline\",\"operation\":\"pulse\""),
                                "equation-test.mvm")
               .success,
          "重複 field 拒否");
    check(!parseProjectJsonText(
               replace(saved.json, "\"schema_version\": 22", "\"schema_version\": 20"),
               "equation-test.mvm")
               .success,
          "schema 20 へ新 kind を混入できない");
    auto old = createDefaultProject();
    auto json = serializeProjectJson(old, "old.mvm").json;
    loaded = parseProjectJsonText(replace(json, "\"schema_version\": 22", "\"schema_version\": 20"),
                                  "old.mvm");
    check(loaded.success && loaded.project == old, "schema 20 を読んで現行版へ上げる");
    TimelineClip math;
    math.kind = TimelineClipKind::Math;
    math.id = "old-math";
    math.name = "旧数式";
    math.math.source = "x";
    math.mathAnimation = {MathIntroKind::Write, 2};
    math.sourceFpsNum = 60;
    math.sourceFrameCount = 10;
    math.sourceOutFrame = 10;
    old.timelineClips.push_back(math);
    json = serializeProjectJson(old, "old.mvm").json;
    loaded = parseProjectJsonText(replace(json, "\"schema_version\": 22", "\"schema_version\": 20"),
                                  "old.mvm");
    check(loaded.success && loaded.project == old &&
              loaded.project.timelineClips[0].kind == TimelineClipKind::Math,
          "schema 20 の Math/Write は sequence へ変換せず厳密に保持");
    check(deleteEquationPart(p.timelineClips[0].equationSequence, {"s0"}, {"part-s0"}, 1080, error),
          "missing 保存の対照");
    saved = serializeProjectJson(p, "equation-test.mvm");
    loaded = parseProjectJsonText(saved.json, "equation-test.mvm");
    check(loaded.success && loaded.project == p, "修復可能 missing の exact 往復");
}
} // namespace

int main() {
    boundaries();
    invalid();
    edits();
    ownership();
    mapping();
    persistence();
    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    return checks > 0 && failures == 0 ? 0 : 1;
}
