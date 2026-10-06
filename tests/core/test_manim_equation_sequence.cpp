// Equation Sequence (P3-3) の Manim backend を偽の Manim で検査する。実 Manim は使わない
// (それは mvm_equation_sequence_smoke の役目)。ここでの成功・失敗は偽の backend の振る舞いであり、
// 実 Manim の証拠ではない。
//
// - 構造の報告の検査 (手で書いた報告): 各失敗理由と、それぞれの対照
// - 2 段階の起動: 構造の失敗では描画の段階を起動しない (偽の Manim の phases.txt で数える)
// - 描画の出力の検査: 枚数・壊れた frame・大きさ・縁・静止/端点/action の後の不一致
// - Manim の終了コード 0 でも構造の失敗なら失敗。取消は外部 process を止めて Cancelled

#include "math_test_transform.h"
#include "media/manim/manim_equation_sequence.h"
#include "media/manim/manim_math_tex.h"
#include "util/mvm_win_utf8.h"

#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace {

namespace math = mvm::math;
namespace manim = mvm::manim;
using F = math::EquationBackendFailure;

int checks = 0;
int failures = 0;

void check(bool condition, const std::string& message) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

std::filesystem::path fromUtf8(const char* text) {
    wchar_t* wide = mvm_utf8_to_wide(text ? text : "");
    const std::filesystem::path result = wide ? wide : L"";
    mvm_str_free(wide);
    return result;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

bool loadFakeCoverage(const std::filesystem::path& file, math::MathCoverage& coverage,
                      std::string& error) {
    if (!mvm::test::parseMathTestCoverage(readFile(file), coverage.width, coverage.height,
                                          coverage.alpha)) {
        error = "偽の被覆率の画像ではありません";
        return false;
    }
    return true;
}

math::MathCoverage pattern(std::size_t state, int width, int height) {
    math::MathCoverage coverage{width, height, {}};
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            coverage.alpha.push_back(mvm::test::mathTestEquationAlpha(state, x, y));
    return coverage;
}

// 状態 2 個 (a+b → b+a)、変形 1 個 (N=3)、action 2 個 (outline N=2、pulse N=3)。
math::EquationSequenceRenderSpec spec(const std::string& first = "a",
                                      const std::string& second = "b") {
    math::EquationSequenceRenderSpec s;
    s.compilerVersion = "equation-neutral/1";
    math::EquationRenderState a;
    a.still = {"latex", first + "+" + second, 72};
    a.holdFrames = 10;
    a.segments = {{math::EquationRenderSegmentKind::Semantic, first},
                  {math::EquationRenderSegmentKind::Auto, "+"},
                  {math::EquationRenderSegmentKind::Semantic, second}};
    math::EquationRenderState b = a;
    b.still.source = second + "+" + first;
    b.segments = {{math::EquationRenderSegmentKind::Semantic, second},
                  {math::EquationRenderSegmentKind::Auto, "+"},
                  {math::EquationRenderSegmentKind::Semantic, first}};
    s.states = {a, b};
    math::EquationRenderTransition t;
    t.fromState = 0;
    t.toState = 1;
    t.frames = 3;
    t.matching.pairs = {{0, 2}, {1, 1}, {2, 0}};
    s.transitions = {t};
    s.actions = {{0, 0, 1, 2, math::EquationRenderOperation::Outline},
                 {1, 2, 4, 3, math::EquationRenderOperation::Pulse}};
    return s;
}

math::EquationSequenceRenderRequest request(const math::EquationSequenceRenderSpec& s,
                                            const std::filesystem::path& job) {
    math::EquationSequenceRenderRequest r;
    r.spec = s;
    r.stateStatics = {pattern(0, 7, 4), pattern(1, 6, 5)};
    r.jobDirectory = job;
    r.timeout = std::chrono::milliseconds(20000);
    return r;
}

// ---- 構造の報告 ----

std::string goodReport() {
    return "state 0 3 3 11,12,13 a1,a2,a3\n"
           "part 0 0 MathTexPart x61 1 1 1 11 a1\n"
           "part 0 1 MathTexPart x2b 1 1 1 12 a2\n"
           "part 0 2 MathTexPart x62 1 1 1 13 a3\n"
           "state 1 3 4 21,22,23,24 b1,b2,b3,b4\n"
           "part 1 0 MathTexPart x62 1 1 1 21 b1\n"
           "part 1 1 MathTexPart x2b 1 1 1 22 b2\n"
           "part 1 2 MathTexPart x61 2 2 2 23,24 b3,b4\n";
}

std::string replaced(std::string text, const std::string& from, const std::string& to) {
    const auto at = text.find(from);
    if (at != std::string::npos)
        text.replace(at, from.size(), to);
    return text;
}

void testStructure() {
    const auto s = spec();
    const auto ok = manim::checkManimEquationStructure(goodReport(), s);
    check(ok.ready(), "対照: 正しい報告は通る: " + ok.detail);
    check(ok.segments.size() == 6 && ok.segments[5].ownership == std::vector<std::int64_t>{2, 3} &&
              ok.segments[5].pointBearing == 2 &&
              ok.segments[1].ownership == std::vector<std::int64_t>{1},
          "所有は式全体の点を持つ object の並びの番号 (python の id ではない)");

    struct Case {
        const char* name;
        std::string report;
        F expected;
    };

    const Case cases[] = {
        {"報告が空", "", F::StructureReportMissing},
        {"壊れた行", goodReport() + "part 0\n", F::StructureReportMalformed},
        {"未知の行", goodReport() + "glyph 1\n", F::StructureReportMalformed},
        {"代用の log", "fallback x41\n" + goodReport(), F::GroupingFallback},
        {"部分の数", replaced(goodReport(), "state 0 3 3", "state 0 2 3"), F::SegmentCountMismatch},
        {"部分の行が無い", replaced(goodReport(), "part 0 1 MathTexPart x2b 1 1 1 12 a2\n", ""),
         F::MissingSegmentObject},
        {"種類", replaced(goodReport(), "part 0 0 MathTexPart", "part 0 0 VGroup"),
         F::SegmentTypeMismatch},
        {"文字列が無い",
         replaced(goodReport(), "part 0 0 MathTexPart x61", "part 0 0 MathTexPart none"),
         F::SegmentTypeMismatch},
        {"文字列", replaced(goodReport(), "part 0 0 MathTexPart x61", "part 0 0 MathTexPart x63"),
         F::SegmentTextMismatch},
        {"同じ子孫を 2 つの handle が所有",
         replaced(goodReport(), "part 0 1 MathTexPart x2b 1 1 1 12 a2",
                  "part 0 1 MathTexPart x2b 1 1 1 11 a1"),
         F::SharedDescendant},
        {"式の木に同じ object が 2 回", replaced(goodReport(), "11,12,13 a1", "11,11,13 a1"),
         F::SharedDescendant},
        {"点列の共有", replaced(goodReport(), "11,12,13 a1,a2,a3", "11,12,13 a1,a1,a3"),
         F::AliasedPointData},
        {"部分と式で点列が違う",
         replaced(goodReport(), "part 0 2 MathTexPart x62 1 1 1 13 a3",
                  "part 0 2 MathTexPart x62 1 1 1 13 a9"),
         F::AliasedPointData},
        {"どの handle にも属さない glyph",
         replaced(goodReport(), "state 0 3 3 11,12,13 a1,a2,a3",
                  "state 0 3 4 11,12,13,14 a1,a2,a3,a4"),
         F::UnclaimedDescendant},
        {"式の木に無い object を部分が所有",
         replaced(goodReport(), "part 0 2 MathTexPart x62 1 1 1 13 a3",
                  "part 0 2 MathTexPart x62 1 1 1 19 a3"),
         F::UnclaimedDescendant},
        {"状態の報告が足りない",
         "state 0 3 3 11,12,13 a1,a2,a3\npart 0 0 MathTexPart x61 1 1 1 11 a1\n",
         F::StructureReportMalformed},
    };
    for (const auto& c : cases) {
        const auto result = manim::checkManimEquationStructure(c.report, s);
        check(result.failure == c.expected,
              std::string("拒否: ") + c.name + " (期待 " +
                  math::equationBackendFailureName(c.expected) + "、実際 " +
                  math::equationBackendFailureName(result.failure) + ")");
    }
    // action の対象が空 (状態 0 の segment 0 が 0 個の子孫)。
    const std::string emptyTarget =
        replaced(replaced(goodReport(), "state 0 3 3 11,12,13 a1,a2,a3", "state 0 3 2 12,13 a2,a3"),
                 "part 0 0 MathTexPart x61 1 1 1 11 a1", "part 0 0 MathTexPart x61 0 0 0 - -");
    const auto empty = manim::checkManimEquationStructure(emptyTarget, s);
    check(empty.failure == F::EmptyActionTarget && empty.state == 0 && empty.segment == 0,
          "action の対象が空なら EmptyActionTarget (場所付き)");
    check(empty.segments.size() == 6 && !empty.segments[0].nonEmpty,
          "空の対象の失敗にも segment ごとの所有を残す");
    // 同じ報告でも action が無く、空の handle が空でない handle と対なら EmptyTransitionHandle。
    auto noAction = s;
    noAction.actions.clear();
    const auto pair = manim::checkManimEquationStructure(emptyTarget, noAction);
    check(pair.failure == F::EmptyTransitionHandle && pair.state == 0 && pair.segment == 0,
          "空の handle と glyph を持つ handle の対は EmptyTransitionHandle");
    // 空と空の対・空の fade は許す (空白だけの auto segment)。
    auto bothEmpty = noAction;
    bothEmpty.transitions[0].matching = {{{1, 1}}, {0, 2}, {0, 2}};
    const std::string emptyPlus =
        replaced(replaced(goodReport(), "state 1 3 4 21,22,23,24 b1,b2,b3,b4",
                          "state 1 3 3 21,23,24 b1,b3,b4"),
                 "part 1 1 MathTexPart x2b 1 1 1 22 b2", "part 1 1 MathTexPart x2b 0 0 0 - -");
    const std::string bothReport =
        replaced(replaced(emptyPlus, "state 0 3 3 11,12,13 a1,a2,a3", "state 0 3 2 11,13 a1,a3"),
                 "part 0 1 MathTexPart x2b 1 1 1 12 a2", "part 0 1 MathTexPart x2b 0 0 0 - -");
    check(manim::checkManimEquationStructure(bothReport, bothEmpty).ready(),
          "対照: 空と空の対は通る");
}

// ---- request.json と script ----

void testRequestAndScript() {
    const auto s = spec();
    manim::ManimEquationPlan plan;
    std::string error;
    check(manim::planManimEquationSequence(s, {pattern(0, 7, 4), pattern(1, 6, 5)}, plan, error),
          "計画を作れる: " + error);
    check(plan.states.size() == 2 && plan.states[0].canvasWidth == 7 + 400 &&
              plan.transitions[0].canvasWidth == 7 + 400 &&
              plan.transitions[0].canvasHeight == 5 + 400,
          "canvas は静止の大きさ + 各辺 200");
    const auto json = manim::manimEquationSequenceRequestJson(s, plan, "render");
    check(json.rfind("{\"phase\": \"render\"", 0) == 0, "phase を渡す");
    check(json.find("\"alpha\": [[0, 3], [1, 3], [2, 3], [3, 3]]") != std::string::npos,
          "変形の alpha は i/N と照合用の N/N: " + json);
    check(json.find("\"alpha\": [[1, 4], [3, 4]]") != std::string::npos,
          "outline の alpha は (2i+1)/(2N)");
    check(json.find("\"alpha\": [[1, 3], [3, 3], [1, 3]]") != std::string::npos,
          "pulse の alpha は重み (N-|2i+1-N|)/N");
    check(json.find("\"pairs\": [[0, 2], [1, 1], [2, 0]]") != std::string::npos,
          "対は P3-2 の handle の対のまま");
    for (const char* forbidden :
         {"StateId", "PartId", "ActionId", "TransitionId", "revision", "label", "\"id\""})
        check(json.find(forbidden) == std::string::npos,
              std::string("request.json に所有 ID・revision・label を含めない: ") + forbidden);
    const auto script = manim::manimEquationSequenceScript();
    check(script.find("class MvmEquationSequence") != std::string::npos &&
              script.find("MathTex(*state[\"segments\"]") != std::string::npos,
          "状態は MathTex(*segments) で作る");
    check(script.find("TransformMatchingTex") == std::string::npos &&
              script.find("ReplacementTransform(source[i], target[j]") != std::string::npos,
          "対応は mvm の pairs で組み、TransformMatchingTex を使わない");
    check(script.find("animation.interpolate(ratio(alpha))") != std::string::npos &&
              script.find("self.play") == std::string::npos,
          "frame は alpha の直接標本化で描き、play (秒の再生) を使わない");
    check(!manim::planManimEquationSequence(s, {pattern(0, 7, 4)}, plan, error),
          "静止の数が状態と違えば計画しない");
    auto tooLong = s;
    tooLong.transitions[0].frames = manim::kMaximumEquationIntervalFrames + 1;
    check(!manim::planManimEquationSequence(tooLong, {pattern(0, 7, 4), pattern(1, 6, 5)}, plan,
                                            error),
          "上限を超える枚数は計画しない");
}

// ---- 偽の Manim での描画 ----

std::filesystem::path manimExe;
std::filesystem::path root;
int jobNumber = 0;

struct Run {
    math::EquationSequenceRenderResult result;
    std::filesystem::path job;
    std::string phases;
};

Run run(const std::string& first, const std::string& second = "b",
        const std::atomic<bool>* cancel = nullptr, int timeoutMs = 20000) {
    Run r;
    r.job = root / ("job-" + std::to_string(++jobNumber));
    auto req = request(spec(first, second), r.job);
    req.timeout = std::chrono::milliseconds(timeoutMs);
    r.result = manim::renderManimEquationSequence(manimExe, req, loadFakeCoverage, cancel);
    r.phases = readFile(r.job / L"phases.txt");
    return r;
}

void testRender() {
    const auto ok = run("a");
    check(ok.result.status == math::MathRenderStatus::Ok && ok.result.validation.ready(),
          "対照: 偽の Manim で描ける: " + ok.result.message);
    check(ok.phases == "structure\nrender\n", "構造の段階の後に描画の段階を起動する: " + ok.phases);
    check(ok.result.transitions.size() == 1 && ok.result.transitions[0].interval.frames.size() == 3,
          "変形は N 枚 (照合用の終状態は含めない)");
    check(ok.result.actions.size() == 2 && ok.result.actions[0].interval.frames.size() == 2 &&
              ok.result.actions[1].interval.frames.size() == 3,
          "action は N 枚");
    const auto& artifact = ok.result.actions[0].interval.artifact;
    check(artifact.x == 2 && artifact.y == 3, "action の artifact は accent の画素を含む外接矩形");

    struct Case {
        const char* marker;
        F expected;
        bool renderPhase; // 描画の段階を起動したか
    };

    const Case cases[] = {
        {"FAKE_EQ_NO_REPORT", F::StructureReportMissing, false},
        {"FAKE_EQ_MALFORMED", F::StructureReportMalformed, false},
        {"FAKE_EQ_FALLBACK", F::GroupingFallback, false},
        {"FAKE_EQ_COUNT", F::SegmentCountMismatch, false},
        {"FAKE_EQ_MISSING", F::MissingSegmentObject, false},
        {"FAKE_EQ_TYPE", F::SegmentTypeMismatch, false},
        {"FAKE_EQ_TEXT", F::SegmentTextMismatch, false},
        {"FAKE_EQ_SHARED", F::SharedDescendant, false},
        {"FAKE_EQ_ALIAS", F::AliasedPointData, false},
        {"FAKE_EQ_UNCLAIMED", F::UnclaimedDescendant, false},
        {"FAKE_EQ_EMPTY", F::EmptyActionTarget, false},
        {"FAKE_EQ_PHASE_DIFF", F::StructureChangedBetweenPhases, true},
        {"FAKE_EQ_SHORT", F::FrameCountMismatch, true},
        {"FAKE_EQ_CORRUPT", F::CorruptFrame, true},
        {"FAKE_EQ_SIZE", F::FrameSizeMismatch, true},
        {"FAKE_EQ_STATIC", F::StaticMismatch, true},
        {"FAKE_EQ_ENDPOINT", F::EndpointMismatch, true},
        {"FAKE_EQ_AFTER", F::ActionMutatedState, true},
        {"FAKE_EQ_EDGE", F::EdgeContact, true},
    };
    for (const auto& c : cases) {
        const auto r = run(std::string("a") + c.marker);
        check(r.result.status == math::MathRenderStatus::Failed &&
                  r.result.validation.failure == c.expected,
              std::string(c.marker) + ": 期待 " + math::equationBackendFailureName(c.expected) +
                  "、実際 " + math::equationBackendFailureName(r.result.validation.failure) + " (" +
                  r.result.message + ")");
        check(r.phases == (c.renderPhase ? "structure\nrender\n" : "structure\n"),
              std::string(c.marker) + ": 起動した段階 = " + r.phases);
        check(!r.result.message.empty(), std::string(c.marker) + ": 理由を説明する");
    }
    // \, は実 Manim と同じく空の部分 (偽の Manim の規則)。action の対象なら描かない。
    const auto thin = run("\\,");
    check(thin.result.validation.failure == F::EmptyActionTarget && thin.phases == "structure\n",
          "\\, の action 対象は構造の段階で拒否し、描画しない");

    // 描画の段階の process の失敗は MathRenderStatus で分類する (構造の失敗とは別)。
    const auto exited = run("aFAKE_EQ_RENDER_EXIT");
    check(exited.result.status == math::MathRenderStatus::Failed &&
              exited.result.validation.failure == F::NotValidated &&
              exited.result.message.find("描画の段階の失敗") != std::string::npos,
          "描画の段階の exit 3 は Failed (error.txt の理由)");
    const auto latex = run("aFAKE_LATEX_ERROR");
    check(latex.result.status == math::MathRenderStatus::InvalidSource && latex.phases.empty(),
          "TeX の誤りは InvalidSource");

    // 取消: 描画の段階で終わらない Manim を外部 process ごと止める。
    std::atomic<bool> cancel{false};
    std::thread stopper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        cancel.store(true);
    });
    const auto started = std::chrono::steady_clock::now();
    const auto cancelled = run("aFAKE_EQ_RENDER_HANG", "b", &cancel);
    stopper.join();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started)
                        .count();
    check(cancelled.result.status == math::MathRenderStatus::Cancelled && ms < 15000,
          "取消で外部 process を止めて Cancelled (" + std::to_string(ms) + " ms)");
    const auto timedOut = run("aFAKE_EQ_RENDER_HANG", "b", nullptr, 1500);
    check(timedOut.result.status == math::MathRenderStatus::TimedOut, "timeout は TimedOut");

    // 要求そのものが不正なら起動しない。
    math::EquationSequenceRenderRequest invalid = request(spec(), root / "invalid");
    invalid.stateStatics.pop_back();
    const auto rejected =
        manim::renderManimEquationSequence(manimExe, invalid, loadFakeCoverage, nullptr);
    check(rejected.validation.failure == F::InvalidRequest &&
              !std::filesystem::exists(root / "invalid" / "phases.txt"),
          "静止の数が足りない要求は起動せずに InvalidRequest");
    const auto noLoader =
        manim::renderManimEquationSequence(manimExe, request(spec(), root / "x"), {}, nullptr);
    check(noLoader.validation.failure == F::InvalidRequest, "loader が無ければ InvalidRequest");
}

void install(const std::filesystem::path& fake, const std::filesystem::path& dir,
             const wchar_t* name) {
    std::filesystem::create_directories(dir);
    std::filesystem::copy_file(fake, dir / name, std::filesystem::copy_options::overwrite_existing);
}

void testPreflight(const std::filesystem::path& fake, const std::filesystem::path& runtime) {
    const auto tools = root / "tools";
    install(fake, tools, L"latex.exe");
    install(fake, tools, L"dvisvgm.exe");
    SetEnvironmentVariableW(L"PATH", (tools.wstring() + L";" + runtime.wstring()).c_str());
    const auto result = manim::preflightManimMathTex(
        {manimExe, root / "preflight", std::chrono::milliseconds(10000)}, nullptr);
    check(result.status == math::MathPreflightStatus::Available, "preflight: " + result.message);
    check(result.backend.equationSequenceTemplate == "manim-equation-sequence/1" &&
              static_cast<bool>(result.backend.renderEquationSequence) &&
              result.backend.maximumEquationSequenceFrames == 9998,
          "Available なら Equation Sequence の関数・template・上限を束ねる");
    check(result.backend.fingerprint.canonical.find("equation") == std::string::npos,
          "template は fingerprint に入れない (静止・P2 の key を変えない)");
    check(result.backend.transformTemplate == "manim-transform/1" &&
              result.backend.sequenceTemplate == "manim-write/1",
          "P1 Write・P2 変形の template は変えない");
}

} // namespace

int main() {
    mvm_enable_utf8_console();
    int argc = 0;
    char** argv = mvm_win_get_utf8_args(&argc);
    if (!argv || argc != 4) {
        std::fprintf(stderr,
                     "使い方: mvm_test_manim_equation_sequence <偽の Manim> <作業 directory> "
                     "<実行時 DLL の directory>\n");
        mvm_win_free_utf8_args(argv, argc);
        return 2;
    }
    const auto fake = fromUtf8(argv[1]);
    root = std::filesystem::absolute(fromUtf8(argv[2]));
    const auto runtime = fromUtf8(argv[3]);
    mvm_win_free_utf8_args(argv, argc);
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "bin");
    manimExe = root / "bin" / "manim.exe";
    std::filesystem::copy_file(fake, manimExe);

    testStructure();
    testRequestAndScript();
    testRender();
    testPreflight(fake, runtime);
    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}
