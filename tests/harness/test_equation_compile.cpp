#include "app/equation_sequence_compile.h"
#include "core/text_offsets.h"
#include "project/equation_binding_edit.h"
#include "project/equation_sequence_edit.h"
#include "project/project_json.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <set>
#include <tuple>

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

EquationState state(std::string source = "a+x+x+z") {
    EquationState s;
    s.id = {"s"};
    s.revision = "r1";
    s.equation.source = std::move(source);
    s.holdFrames = 10;
    return s;
}

void part(EquationState& s, std::string id, std::int64_t begin, std::int64_t end) {
    s.parts.push_back({{std::move(id)},
                       "項",
                       {s.revision, begin, end,
                        s.equation.source.substr(static_cast<std::size_t>(begin),
                                                 static_cast<std::size_t>(end - begin)),
                        BindingStatus::Bound}});
}

EquationSequenceClipData sequence() {
    auto s = state();
    part(s, "first", 2, 3);
    part(s, "second", 4, 5);
    EquationSequenceClipData d;
    d.states = {s};
    return d;
}

void offsets() {
    for (const auto& [text, boundaries] :
         std::vector<std::pair<std::string, std::vector<std::pair<std::size_t, std::size_t>>>>{
             {"", {{0, 0}}},
             {"abc", {{0, 0}, {1, 1}, {3, 3}}},
             {"日a本", {{0, 0}, {3, 1}, {4, 2}, {7, 3}}},
             {"a\xF0\x9F\x98\x80"
              "b",
              {{0, 0}, {1, 1}, {5, 3}, {6, 4}}},
             {"e\xCC\x81", {{0, 0}, {1, 1}, {3, 2}}}}) {
        for (const auto& [byte, unit] : boundaries) {
            check(core::utf8ToUtf16Offset(text, byte) == unit, "手計算 byte→unit 境界");
            check(core::utf16ToUtf8Offset(text, unit) == byte, "手計算 unit→byte 境界");
        }
        check(!core::utf8ToUtf16Offset(text, text.size() + 1), "終端外 byte 拒否");
    }
    check(!core::utf8ToUtf16Offset("日本", 1), "継続 byte 拒否");
    check(!core::utf16ToUtf8Offset("a\xF0\x9F\x98\x80"
                                   "b",
                                   2),
          "surrogate 内部拒否");
    for (const auto& invalid : std::vector<std::string>{
             "\x80", "\xC0\xAF", "\xE3\x81", "\xED\xA0\x80", "\xF4\x90\x80\x80", "a\xFF"}) {
        check(!core::utf8ToUtf16Offset(invalid, 0), "不正 UTF-8 は開始境界でも拒否");
        check(!core::utf16ToUtf8Offset(invalid, 0), "不正 UTF-8 の逆変換拒否");
    }
}

void edits(const std::filesystem::path& dir) {
    std::string error;

    struct Case {
        std::size_t begin, end;
        std::string replacement;
        std::int64_t first, second;
        bool invalidFirst, invalidSecond;
    };

    for (const auto& c : std::vector<Case>{{0, 0, "Q", 3, 5, false, false},
                                           {2, 2, "Q", 3, 5, false, false},
                                           {3, 3, "Q", 2, 5, false, false},
                                           {7, 7, "Q", 2, 4, false, false},
                                           {2, 3, "x", 2, 4, true, false},
                                           {1, 3, "", 2, 2, true, false},
                                           {2, 4, "", 2, 2, true, false},
                                           {1, 6, "Q", 2, 4, true, true}}) {
        auto d = sequence();
        const auto old = d.states[0].equation.source;
        const auto next = old.substr(0, c.begin) + c.replacement + old.substr(c.end);
        check(editEquationSourceTrusted(d, {"s"}, {old, c.begin, c.end, c.replacement, next}, "r2",
                                        1080, error),
              "編集成立: " + error);
        for (std::size_t i = 0; i < 2; ++i) {
            const auto& b = d.states[0].parts[i].binding;
            const bool invalid = i == 0 ? c.invalidFirst : c.invalidSecond;
            check(b.status == (invalid ? BindingStatus::Invalid : BindingStatus::Bound),
                  "境界の手計算 status");
            check(b.begin == (i == 0 ? c.first : c.second), "境界の手計算 byte");
            check(b.revision == (invalid ? "r1" : "r2") && b.expectedText == "x",
                  "revision と証人を保持");
        }
        check(d.states[0].parts[0].id == PartId{"first"} &&
                  d.states[0].parts[1].id == PartId{"second"},
              "同じ文字の identity を保持");
    }
    auto d = sequence();
    const auto old = d;
    check(!editEquationSourceTrusted(d, {"s"}, {"違う", 0, 0, "Q", "Q"}, "r2", 1080, error) &&
              d == old,
          "旧 source 不一致は原子的に拒否");
    check(!editEquationSourceTrusted(d, {"s"}, {"a+x+x+z", 0, 0, "Q", "誤り"}, "r2", 1080, error) &&
              d == old,
          "新 source 不一致拒否");
    auto unicode = state("日\xF0\x9F\x98\x80+x");
    part(unicode, "u", 8, 9);
    d.states = {unicode};
    check(editEquationSourceTrusted(d, {"s"}, {unicode.equation.source, 1, 3, "本", "日本+x"}, "r2",
                                    1080, error) &&
              d.states[0].parts[0].binding.begin == 7,
          "UTF-16 surrogate 全体の置換を byte 差へ変換");
    const auto ud = d;
    check(!rebindEquationPart(d, {"s"}, {"u"}, {"r2", 1, 3, "aa", BindingStatus::Bound}, 1080,
                              error) &&
              d == ud,
          "再 binding の継続 byte 拒否");
    check(!editEquationSourceTrusted(d, {"s"}, {"日本+x", 0, 0, "\xFF", "\xFF日本+x"}, "r3", 1080,
                                     error) &&
              d == ud,
          "不正 replacement 拒否");
    d = sequence();
    auto wide = state("a+xyz+z");
    part(wide, "wide", 2, 5);
    d.states = {wide};
    check(editEquationSourceTrusted(d, {"s"}, {"a+xyz+z", 3, 3, "Q", "a+xQyz+z"}, "r2", 1080,
                                    error) &&
              d.states[0].parts[0].binding.status == BindingStatus::Invalid &&
              d.states[0].parts[0].binding.expectedText == "xyz",
          "内部挿入は旧証人を残して Invalid");
    d = sequence();
    check(replaceEquationSource(d, {"s"}, "a+x+x+z", "r2", 1080, error),
          "同じ source の未信頼置換");
    check(d.states[0].parts[0].binding.status == BindingStatus::Invalid,
          "文字一致で自動再 binding しない");
    const auto invalid = d;
    check(!rebindEquationPart(d, {"s"}, {"first"}, {"r1", 2, 3, "x", BindingStatus::Bound}, 1080,
                              error) &&
              d == invalid,
          "古い revision 拒否");
    check(!rebindEquationPart(d, {"s"}, {"unknown"}, {"r2", 2, 3, "x", BindingStatus::Bound}, 1080,
                              error),
          "存在しない PartId 拒否");
    check(!rebindEquationPart(d, {"s"}, {"first"}, {"r2", 2, 3, "y", BindingStatus::Bound}, 1080,
                              error),
          "証人不一致拒否");
    check(!rebindEquationPart(d, {"s"}, {"first"}, {"r2", 2, 2, "", BindingStatus::Bound}, 1080,
                              error),
          "空範囲拒否");
    check(rebindEquationPart(d, {"s"}, {"first"}, {"r2", 4, 5, "x", BindingStatus::Bound}, 1080,
                             error),
          "既存 identity の明示的再 binding");
    check(!rebindEquationPart(d, {"s"}, {"second"}, {"r2", 4, 5, "x", BindingStatus::Bound}, 1080,
                              error),
          "重複範囲拒否");
    check(d.states[0].parts[0].id == PartId{"first"}, "再 binding で identity を置換しない");
    d = sequence();
    check(editEquationSourceTrusted(d, {"s"}, {"a+x+x+z", 2, 2, "日", "a+日x+x+z"}, "r2", 1080,
                                    error),
          "保存対象の信頼編集");
    auto p = createDefaultProject();
    check(addEquationSequence(p, d, "clip", "数式", {TrackKind::Video, 0}, 0).success,
          "保存対象 Project");
    std::filesystem::create_directories(dir);
    const auto path = dir / "trusted.json";
    const auto saved = saveProjectJson(p, path);
    check(saved.success, "実保存: " + saved.error);
    const auto reopened = loadProjectJson(path);
    check(reopened.success && reopened.project.timelineClips[0].equationSequence == d,
          "保存再読込で authoritative range/revision を保持");
}

void partitionTests() {
    std::string error;
    auto s = state("abxyzcd");
    part(s, "middle", 2, 5);
    const auto p = compileEquationPartition(s);
    check(p.value && p.value->spec.segments.size() == 3, "一つの自動 segment を semantic で分割");
    if (p.value) {
        std::string reconstructed;
        std::size_t cursor = 0;
        for (const auto& seg : p.value->spec.segments) {
            check(seg.begin == cursor && seg.end > cursor, "連続かつ非空の partition");
            cursor = seg.end;
            reconstructed += seg.text;
        }
        check(reconstructed == s.equation.source && cursor == s.equation.source.size(),
              "原 byte を一回ずつ再構成");
        check(p.value->spec.segments[0].text == "ab" && p.value->spec.segments[2].text == "cd",
              "semantic の両側は独立 auto region");
    }
    s = state();
    part(s, "z-id", 2, 3);
    part(s, "a-id", 4, 5);
    const auto duplicates = compileEquationPartition(s);
    check(duplicates.value && duplicates.value->owners[2] == PartId{"z-id"} &&
              duplicates.value->owners[4] == PartId{"a-id"},
          "ID 順ではなく source 順で同じ文字を区別");
    part(s, "overlap", 2, 5);
    check(compileEquationPartition(s).failure == EquationCompileFailure::PartitionConflict,
          "重複 partition 拒否");
    for (const auto& [text, begin, end] :
         std::vector<std::tuple<std::string, int, int>>{{"\\alpha+x", 1, 4},
                                                        {"\\begin{matrix}x\\end{matrix}", 7, 10},
                                                        {"x\\", 0, 2},
                                                        {"x% comment\ny", 3, 6},
                                                        {"{x+y}", 1, 5},
                                                        {"{x}{y}", 2, 4},
                                                        {"\\unknown{x}", 9, 10},
                                                        {"x^2", 2, 3},
                                                        {"x^ {2}", 1, 2},
                                                        {"\\frac{x}{y}", 0, 5}}) {
        s = state(text);
        part(s, "bad", begin, end);
        check(validateEquationSequence({{s}, {}, {}}, 1080, error),
              "Project の構造的有効性と TeX 分離可能性を区別");
        check(compileEquationPartition(s).failure == EquationCompileFailure::UnsupportedTexBoundary,
              "危険な TeX 境界拒否: " + text);
    }
    s = state("日本");
    part(s, "split", 1, 3);
    check(compileEquationPartition(s).failure == EquationCompileFailure::InvalidBinding,
          "codepoint 分割拒否");
    s = state("x=\\frac{-b+\\sqrt{b^2-4ac}}{2a}");
    part(s, "discriminant", 17, 24);
    check(s.parts[0].binding.expectedText == "b^2-4ac", "判別式の offset は手計算");
    check(compileEquationPartition(s).value.has_value(), "P3-0 判別式の分数内部範囲を支持");
    s = state("x+{}+y");
    part(s, "empty", 2, 4);
    check(compileEquationPartition(s).failure == EquationCompileFailure::UnsupportedEmptyTarget,
          "空 grouping は semantic target にならない");
    s = state("x+ +y");
    part(s, "space", 2, 3);
    check(compileEquationPartition(s).failure == EquationCompileFailure::UnsupportedEmptyTarget,
          "空白 target 拒否");
}

void plans() {
    std::string error;
    auto d = sequence();
    auto to = state("x+x+y");
    to.id = {"t"};
    part(to, "target", 0, 1);
    d.states.push_back(to);
    d.transitions = {{{"edge"}, {"s"}, {"t"}, 3, {{{"second"}, {"target"}}}}};
    d.actions = {{{"action"},
                  {"s"},
                  {"first"},
                  EquationTargetStatus::Present,
                  0,
                  2,
                  EquationOperation::Pulse}};
    const auto plan = compileEquationSequence(d);
    check(plan.value.has_value(), "explicit と auto の中立 plan");
    if (plan.value) {
        const auto& m = plan.value->transitions[0].matching;
        check(std::find(m.pairs.begin(), m.pairs.end(), math::MathSegmentPair{4, 0}) !=
                  m.pairs.end(),
              "explicit は文字一致より優先");
        std::set<std::size_t> from, toSet;
        for (auto pair : m.pairs) {
            check(from.insert(pair.source).second && toSet.insert(pair.target).second,
                  "一つの segment を二度 transform しない");
        }
        for (auto i : m.unmatchedSource)
            check(from.insert(i).second, "fade-out は transform と排他");
        for (auto i : m.unmatchedTarget)
            check(toSet.insert(i).second, "fade-in は transform と排他");
        check(from.size() == plan.value->states[0].segments.size() &&
                  toSet.size() == plan.value->states[1].segments.size(),
              "全 segment の transform/fade 所有を閉じる");
        check(!m.unmatchedSource.empty() && !m.unmatchedTarget.empty(),
              "不一致は fade-out / fade-in");
        check(equationTargetReadiness(plan.value->actions[0].targetProof) ==
                  EquationCompileFailure::BackendValidationRequired,
              "中立 plan 成功は glyph 存在の証明ではない");
    }
    auto copy = d;
    int count = 0;
    check(remapEquationSequenceIds(
              copy, [&] { return "copy-" + std::to_string(++count); }, error),
          "全 ID を remap");
    for (auto& s : copy.states) {
        s.revision = "copied";
        for (auto& p : s.parts) {
            p.label = "別のラベル";
            p.binding.revision = s.revision;
        }
    }
    const auto copied = compileEquationSequence(copy);
    check(plan.value && copied.value && *plan.value == *copied.value,
          "State/Part/Action/Transition ID と revision/label は正準 plan に依存しない");
    auto bad = d;
    bad.actions[0].targetStatus = EquationTargetStatus::Missing;
    check(compileEquationSequence(bad).failure == EquationCompileFailure::MissingPart,
          "missing action 拒否");
    bad = d;
    bad.states[0].parts[0].binding.status = BindingStatus::Invalid;
    check(compileEquationSequence(bad).failure == EquationCompileFailure::InvalidBinding,
          "invalid action 拒否");
    bad = d;
    bad.transitions[0].correspondence.push_back(bad.transitions[0].correspondence[0]);
    check(compileEquationSequence(bad).failure == EquationCompileFailure::InvalidCorrespondencePlan,
          "二重 explicit 拒否");
    bad = d;
    bad.actions[0].target = {"unknown"};
    check(compileEquationSequence(bad).failure == EquationCompileFailure::MissingPart,
          "存在しない action 対象拒否");
    auto a = state("x+x+x"), b = state("x+x");
    b.id = {"other"};
    const auto pa = compileEquationPartition(a), pb = compileEquationPartition(b);
    const auto autoPlan =
        compileEquationTransition(*pa.value, *pb.value, {{"auto"}, a.id, b.id, 2, {}});
    check(autoPlan.value &&
              autoPlan.value->matching.pairs ==
                  std::vector<math::MathSegmentPair>{{0, 0}, {1, 1}, {2, 2}} &&
              autoPlan.value->matching.unmatchedSource == std::vector<std::size_t>{3, 4},
          "重複 auto は pinned P2 の nth 照合");
    auto edited = d;
    edited.actions[0].target = {"second"};
    check(editEquationSourceTrusted(edited, {"s"}, {"a+x+x+z", 4, 5, "q", "a+x+q+z"}, "r2", 1080,
                                    error) &&
              edited.transitions[0].correspondence.empty(),
          "invalid 対応を除去");
    check(compileEquationSequence(edited).failure == EquationCompileFailure::InvalidBinding,
          "rebind 前の既存 action は未解決");
    check(rebindEquationPart(edited, {"s"}, {"second"}, {"r2", 4, 5, "q", BindingStatus::Bound},
                             1080, error) &&
              edited.transitions[0].correspondence.empty(),
          "明示 rebind 後も対応を自動復旧しない");
    check(compileEquationSequence(edited).value.has_value(), "既存 action は rebind 後に解決可能");
    edited = d;
    check(editEquationSourceTrusted(edited, {"s"}, {"a+x+x+z", 0, 0, "Q", "Qa+x+x+z"}, "r2", 1080,
                                    error) &&
              edited.transitions[0].correspondence == d.transitions[0].correspondence,
          "無傷の explicit 対応は信頼編集後も保持");
    auto unsafe = state("{x+y}");
    part(unsafe, "unsafe", 1, 5);
    EquationSequenceClipData actionSequence{{unsafe},
                                            {},
                                            {{{"unsafe-action"},
                                              unsafe.id,
                                              {"unsafe"},
                                              EquationTargetStatus::Present,
                                              0,
                                              1,
                                              EquationOperation::Outline}}};
    check(compileEquationSequence(actionSequence).failure ==
              EquationCompileFailure::UnsupportedTexBoundary,
          "action の分離不能な target はコンパイル失敗");
}
} // namespace

int main(int argc, char** argv) {
    offsets();
    edits(argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("build/equation-p32"));
    partitionTests();
    plans();
    std::printf("%d 検査 / %d 失敗\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
