// 実 Manim + LaTeX で Equation Sequence (P3-3) の renderer と artifact を確かめる手動の受け入れ。
//
// 通常の CTest には入れない (Manim と MiKTeX の導入を build・試験の必須条件にしない。
// mvm_math_transform_smoke と同じ扱い)。latex と dvisvgm は、この process の PATH から探す。
//
//   mvm_equation_sequence_smoke <manim.exe> <証拠の directory (存在しないこと)>
//
// 各ケースは Project の EquationSequenceClipData を作り、P3-2 の compileEquationSequence →
// equationSequenceRenderSpecFor を通して backend に渡す (所有 ID は backend に届かない)。
// 状態の静止は製品の静止の描画 (renderManimMathTex) で描き、製品の loader で読む。
// backend の合否に加え、次を backend とは別に確かめる (期待値は手で決めた値):
// - 変形の frame 0 と照合用の終状態を読み直し、両端の静止と全画素一致・1 画素ずらすと不一致
// - action の各 frame が空でない (pulse)、frame 0 と中央で画素が変わる
// - outline / pulse の N=3 の中央の frame は、N=1 の唯一の frame と byte 単位で一致する
//   (同じ進み具合 1/2・重み 1。先行 frame の無い描画と同じ = 再生の履歴に依存しない)
// - P3-1 の評価で source frame から区間と frame を引く (action の frame 0・中央・最後・直後)
// - MathRasterCache で実際に公開し、provenance・全 frame の SHA-256・合成の色を確かめる
// 結果の生データは <証拠>/results.json (検査数・失敗・ケースごとの値)。

#include "app/equation_sequence_compile.h"
#include "app/equation_sequence_render.h"
#include "app/math_clip_render.h"
#include "math_raster_cache.h"
#include "media/manim/manim_equation_sequence.h"
#include "media/manim/manim_math_tex.h"
#include "util/mvm_sha256.h"
#include "util/mvm_win_utf8.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QEventLoop>

namespace {

namespace math = mvm::math;
namespace manim = mvm::manim;
namespace project = mvm::project;
namespace app = mvm::app;

int checks = 0;
int failures = 0;
std::vector<std::string> failureMessages;

bool check(bool condition, const std::string& message) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        failureMessages.push_back(message);
        ++failures;
    }
    return condition;
}

std::filesystem::path fromUtf8(const char* text) {
    wchar_t* wide = mvm_utf8_to_wide(text ? text : "");
    const std::filesystem::path result = wide ? wide : L"";
    mvm_str_free(wide);
    return result;
}

std::string jsonString(const std::string& text) {
    static const char kHex[] = "0123456789abcdef";
    std::string json = "\"";
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        if (c == '"')
            json += "\\\"";
        else if (c == '\\')
            json += "\\\\";
        else if (c < 0x20) {
            json += "\\u00";
            json += kHex[c >> 4];
            json += kHex[c & 0x0F];
        } else
            json += static_cast<char>(c);
    }
    return json + "\"";
}

std::string sha256(const std::vector<std::uint8_t>& bytes) {
    char hex[MVM_SHA256_HEX_SIZE] = {};
    mvm_sha256_hex(bytes.data(), bytes.size(), hex);
    return hex;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

std::filesystem::path manimExe;
std::filesystem::path evidence;
int jobCounter = 0;
std::vector<std::string> records; // results.json の cases

std::filesystem::path nextJob(const std::string& label) {
    return evidence / "jobs" / (std::to_string(++jobCounter) + "-" + label);
}

// ---- 式の定義 ----

struct StateDef {
    std::string source;
    int font = 72;
    std::string color = "#FFFFFFFF";
    std::int64_t hold = 12;
    // 部分式: (文字列, 何番目の出現か)。ID は P<状態>_<番号>。
    std::vector<std::pair<std::string, int>> parts;
};

struct TransitionDef {
    std::int64_t frames = 6;
    std::vector<std::pair<int, int>> pairs; // (前の状態の部分の番号, 後の状態の部分の番号)
};

struct ActionDef {
    int state = 0;
    int part = 0;
    std::int64_t start = 0;
    std::int64_t duration = 3;
    project::EquationOperation operation = project::EquationOperation::Outline;
};

struct CaseDef {
    std::string label;
    std::string group; // partition / transition / action / negative
    std::vector<StateDef> states;
    std::vector<TransitionDef> transitions;
    std::vector<ActionDef> actions;
    math::EquationBackendFailure expected = math::EquationBackendFailure::None;
    // 期待する理由が 2 通りありうる負例 (どちらでも構造の拒否)。None なら expected だけ。
    math::EquationBackendFailure alternative = math::EquationBackendFailure::None;
};

std::size_t nthOccurrence(const std::string& text, const std::string& needle, int n) {
    std::size_t at = std::string::npos;
    std::size_t from = 0;
    for (int i = 0; i <= n; ++i) {
        at = text.find(needle, from);
        if (at == std::string::npos)
            return at;
        from = at + 1;
    }
    return at;
}

project::EquationSequenceClipData buildSequence(const CaseDef& c) {
    project::EquationSequenceClipData data;
    for (std::size_t s = 0; s < c.states.size(); ++s) {
        const auto& def = c.states[s];
        project::EquationState state;
        state.id = {"S" + std::to_string(s)};
        state.revision = "r" + std::to_string(s);
        state.equation.source = def.source;
        state.equation.fontSize = def.font;
        state.equation.color = def.color;
        state.holdFrames = def.hold;
        for (std::size_t k = 0; k < def.parts.size(); ++k) {
            const auto& [text, occurrence] = def.parts[k];
            const auto begin = nthOccurrence(def.source, text, occurrence);
            if (begin == std::string::npos)
                std::fprintf(stderr, "定義の誤り: %s に %s が無い\n", def.source.c_str(),
                             text.c_str());
            state.parts.push_back({{"P" + std::to_string(s) + "_" + std::to_string(k)},
                                   "部分",
                                   {state.revision, static_cast<std::int64_t>(begin),
                                    static_cast<std::int64_t>(begin + text.size()), text,
                                    project::BindingStatus::Bound}});
        }
        data.states.push_back(std::move(state));
    }
    for (std::size_t t = 0; t < c.transitions.size(); ++t) {
        project::EquationStepTransition transition;
        transition.id = {"T" + std::to_string(t)};
        transition.from = data.states[t].id;
        transition.to = data.states[t + 1].id;
        transition.frames = c.transitions[t].frames;
        for (const auto& [a, b] : c.transitions[t].pairs)
            transition.correspondence.push_back(
                {{"P" + std::to_string(t) + "_" + std::to_string(a)},
                 {"P" + std::to_string(t + 1) + "_" + std::to_string(b)}});
        data.transitions.push_back(std::move(transition));
    }
    for (std::size_t a = 0; a < c.actions.size(); ++a) {
        const auto& def = c.actions[a];
        project::EquationAction action;
        action.id = {"A" + std::to_string(a)};
        action.state = data.states[static_cast<std::size_t>(def.state)].id;
        action.target = {"P" + std::to_string(def.state) + "_" + std::to_string(def.part)};
        action.start = def.start;
        action.duration = def.duration;
        action.operation = def.operation;
        data.actions.push_back(std::move(action));
    }
    return data;
}

// (式, 文字サイズ) ごとに 1 回だけ製品の静止の描画で描いて読む。
std::map<std::pair<std::string, int>, math::MathCoverage> statics;

bool staticCoverage(const math::MathRenderSpec& spec, math::MathCoverage& coverage) {
    const auto key = std::make_pair(spec.source, spec.fontSize);
    if (const auto found = statics.find(key); found != statics.end()) {
        coverage = found->second;
        return true;
    }
    math::MathStaticRenderRequest request;
    request.spec = spec;
    request.jobDirectory = nextJob("static");
    const auto rendered = manim::renderManimMathTex(manimExe, request, nullptr);
    std::string error;
    if (!check(rendered.status == math::MathRenderStatus::Ok &&
                   app::loadMathCoverage(rendered.png, coverage, error),
               "静止を描いて読める: " + spec.source + " / " + rendered.message + error))
        return false;
    statics.emplace(key, coverage);
    return true;
}

struct Compiled {
    project::EquationSequenceClipData data;
    app::EquationSequenceSpec spec;
    math::EquationSequenceRenderSpec render;
};

bool compileCase(const CaseDef& c, Compiled& out) {
    out.data = buildSequence(c);
    std::string error;
    check(project::validateEquationSequence(out.data, 2160, error),
          c.label + ": Project として有効: " + error);
    const auto compiled = app::compileEquationSequence(out.data);
    if (!check(compiled.value.has_value(), c.label + ": P3-2 で compile できる (失敗 " +
                                               std::to_string(static_cast<int>(compiled.failure)) +
                                               ")"))
        return false;
    out.spec = *compiled.value;
    const auto render = app::equationSequenceRenderSpecFor(out.spec, error);
    if (!check(render.has_value(), c.label + ": 中立な描画要求へ写せる: " + error))
        return false;
    out.render = *render;
    return true;
}

std::string segmentsJson(const math::EquationSequenceRenderSpec& spec) {
    std::string json = "[";
    for (std::size_t s = 0; s < spec.states.size(); ++s) {
        if (s > 0)
            json += ", ";
        json += "[";
        for (std::size_t k = 0; k < spec.states[s].segments.size(); ++k) {
            const auto& segment = spec.states[s].segments[k];
            if (k > 0)
                json += ", ";
            json +=
                "{\"kind\": " +
                jsonString(segment.kind == math::EquationRenderSegmentKind::Semantic ? "semantic"
                                                                                     : "auto") +
                ", \"text\": " + jsonString(segment.text) + "}";
        }
        json += "]";
    }
    return json + "]";
}

std::string ownershipJson(const std::vector<math::EquationSegmentOwnership>& ownership) {
    std::string json = "[";
    for (std::size_t i = 0; i < ownership.size(); ++i) {
        const auto& o = ownership[i];
        if (i > 0)
            json += ", ";
        std::string set;
        for (std::size_t j = 0; j < o.ownership.size(); ++j)
            set += (j > 0 ? ", " : "") + std::to_string(o.ownership[j]);
        json += "{\"state\": " + std::to_string(o.state) +
                ", \"segment\": " + std::to_string(o.segment) +
                ", \"top_level\": " + jsonString(o.topLevelType) +
                ", \"children\": " + std::to_string(o.children) +
                ", \"descendants\": " + std::to_string(o.descendants) +
                ", \"point_bearing\": " + std::to_string(o.pointBearing) +
                ", \"non_empty\": " + (o.nonEmpty ? "true" : "false") + ", \"ownership\": [" + set +
                "]}";
    }
    return json + "]";
}

std::vector<std::uint8_t> loadAlpha(const std::filesystem::path& path, int& width, int& height) {
    math::MathCoverage coverage;
    std::string error;
    if (!app::loadMathCoverage(path, coverage, error))
        return {};
    width = coverage.width;
    height = coverage.height;
    return coverage.alpha;
}

std::int64_t nonZero(const std::vector<std::uint8_t>& alpha) {
    std::int64_t count = 0;
    for (const auto value : alpha)
        count += value != 0 ? 1 : 0;
    return count;
}

struct BackendRun {
    math::EquationSequenceRenderResult result;
    math::EquationSequenceRenderRequest request;
    long long ms = 0;
};

BackendRun runBackend(const std::string& label, const math::EquationSequenceRenderSpec& spec) {
    BackendRun run;
    run.request.spec = spec;
    for (const auto& state : spec.states) {
        math::MathCoverage coverage;
        staticCoverage(state.still, coverage);
        run.request.stateStatics.push_back(coverage);
    }
    run.request.jobDirectory = nextJob(label);
    const auto started = std::chrono::steady_clock::now();
    run.result =
        manim::renderManimEquationSequence(manimExe, run.request, app::loadMathCoverage, nullptr);
    run.ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - started)
                 .count();
    return run;
}

// 1 ケースを backend で描き、独立に検査して results へ記録する。
BackendRun runCase(const CaseDef& c) {
    std::printf("[%s]\n", c.label.c_str());
    Compiled compiled;
    BackendRun run;
    std::string extra;
    const int failuresBefore = failures;
    if (!compileCase(c, compiled)) {
        records.push_back("{\"label\": " + jsonString(c.label) + ", \"group\": " +
                          jsonString(c.group) + ", \"compiled\": false, \"pass\": false}");
        return run;
    }
    run = runBackend(c.label, compiled.render);
    const auto& r = run.result;
    std::printf("  %s %s (%lld ms) %s\n", math::mathRenderStatusName(r.status),
                math::equationBackendFailureName(r.validation.failure), run.ms, r.message.c_str());
    const bool expectOk = c.expected == math::EquationBackendFailure::None;
    if (expectOk) {
        check(r.status == math::MathRenderStatus::Ok && r.validation.ready(),
              c.label + ": backend で描けて構造検証を通る: " + r.message + "\n" +
                  r.log.substr(r.log.size() > 3000 ? r.log.size() - 3000 : 0));
    } else {
        const bool expectedReason = r.validation.failure == c.expected ||
                                    (c.alternative != math::EquationBackendFailure::None &&
                                     r.validation.failure == c.alternative);
        check(r.status == math::MathRenderStatus::Failed && expectedReason,
              c.label + ": 期待した理由で拒否する (期待 " +
                  math::equationBackendFailureName(c.expected) + "、実際 " +
                  math::equationBackendFailureName(r.validation.failure) + "): " + r.message);
        // 構造検証で拒否したなら、描画の段階は起動していない (出力の directory が無い)。
        std::error_code error;
        const bool rendered =
            std::filesystem::exists(run.request.jobDirectory / L"sequence", error);
        check(!rendered, c.label + ": 構造検証で拒否したので、何も描いていない");
        extra += ", \"render_phase_output\": " + std::string(rendered ? "true" : "false");
    }

    std::string transitions = "[";
    if (r.status == math::MathRenderStatus::Ok) {
        for (std::size_t t = 0; t < r.transitions.size(); ++t) {
            const auto& raster = r.transitions[t];
            const auto& item = compiled.render.transitions[t];
            const auto& source = run.request.stateStatics[item.fromState];
            const auto& target = run.request.stateStatics[item.toState];
            check(static_cast<std::int64_t>(raster.interval.frames.size()) == item.frames,
                  c.label + ": 変形は N 枚");
            int w = 0;
            int h = 0;
            const auto first = loadAlpha(raster.interval.frames.front(), w, h);
            const math::MathCoverage firstCoverage{w, h, first};
            const auto& p = raster.placement;
            const auto exact =
                math::mathEndpointDifference(firstCoverage, source, p.source.left, p.source.top);
            const auto shifted = math::mathEndpointDifference(firstCoverage, source, p.source.left,
                                                              p.source.top + 1);
            check(exact == 0, c.label + ": 変形の frame 0 は前の状態の静止と全画素一致");
            check(shifted > 0, c.label + ": 1 画素ずらすと一致しない (照合は位置に敏感)");
            // 照合用の終状態 (timeline の frame ではない) は backend の作業 directory に残る。
            wchar_t name[32] = {};
            std::swprintf(name, std::size(name), L"%05lld.png",
                          static_cast<long long>(item.frames));
            const auto endPath =
                run.request.jobDirectory / L"sequence" / (L"t" + std::to_wstring(t)) / name;
            const auto end = loadAlpha(endPath, w, h);
            const auto endDifference =
                math::mathEndpointDifference({w, h, end}, target, p.target.left, p.target.top);
            check(endDifference == 0, c.label + ": 照合用の終状態は後の状態の静止と全画素一致");
            if (t > 0)
                transitions += ", ";
            transitions +=
                "{\"frames\": " + std::to_string(item.frames) + ", \"canvas\": [" +
                std::to_string(raster.interval.canvasWidth) + ", " +
                std::to_string(raster.interval.canvasHeight) + "], \"artifact\": [" +
                std::to_string(raster.interval.artifact.x) + ", " +
                std::to_string(raster.interval.artifact.y) + ", " +
                std::to_string(raster.interval.artifact.width) + ", " +
                std::to_string(raster.interval.artifact.height) +
                "], \"source_endpoint_difference\": " + std::to_string(exact) +
                ", \"source_shifted_difference\": " + std::to_string(shifted) +
                ", \"target_inspection_difference\": " + std::to_string(endDifference) +
                ", \"pairs\": " + std::to_string(item.matching.pairs.size()) +
                ", \"unmatched_source\": " + std::to_string(item.matching.unmatchedSource.size()) +
                ", \"unmatched_target\": " + std::to_string(item.matching.unmatchedTarget.size()) +
                "}";
        }
    }
    transitions += "]";

    std::string actions = "[";
    if (r.status == math::MathRenderStatus::Ok) {
        for (std::size_t a = 0; a < r.actions.size(); ++a) {
            const auto& raster = r.actions[a];
            const auto& item = compiled.render.actions[a];
            check(static_cast<std::int64_t>(raster.interval.frames.size()) == item.duration,
                  c.label + ": action は N 枚");
            std::set<std::string> hashes;
            std::int64_t emptyFrames = 0;
            std::string coverage;
            for (std::size_t i = 0; i < raster.interval.frames.size(); ++i) {
                int w = 0;
                int h = 0;
                const auto alpha = loadAlpha(raster.interval.frames[i], w, h);
                hashes.insert(sha256(alpha));
                const auto count = nonZero(alpha);
                emptyFrames += count == 0 ? 1 : 0;
                coverage += (i > 0 ? ", " : "") + std::to_string(count);
            }
            if (item.operation == math::EquationRenderOperation::Pulse)
                check(emptyFrames == 0, c.label + ": pulse の全 frame で対象が描かれる");
            if (item.duration >= 3)
                check(hashes.size() > 1, c.label + ": action の frame が時間で変わる");
            if (a > 0)
                actions += ", ";
            actions +=
                "{\"operation\": " + jsonString(math::equationRenderOperationName(item.operation)) +
                ", \"state\": " + std::to_string(item.state) +
                ", \"segment\": " + std::to_string(item.segment) +
                ", \"frames\": " + std::to_string(item.duration) +
                ", \"distinct_frames\": " + std::to_string(hashes.size()) +
                ", \"accent_nonzero\": [" + coverage + "]}";
        }
    }
    actions += "]";

    records.push_back(
        "{\"label\": " + jsonString(c.label) + ", \"group\": " + jsonString(c.group) +
        ", \"compiled\": true, \"status\": " + jsonString(math::mathRenderStatusName(r.status)) +
        ", \"backend_failure\": " +
        jsonString(math::equationBackendFailureName(r.validation.failure)) +
        ", \"expected_failure\": " + jsonString(math::equationBackendFailureName(c.expected)) +
        ", \"message\": " + jsonString(r.message) + ", \"ms\": " + std::to_string(run.ms) +
        ", \"segments\": " + segmentsJson(compiled.render) +
        ", \"ownership\": " + ownershipJson(r.validation.segments) +
        ", \"transitions\": " + transitions + ", \"actions\": " + actions + extra +
        ", \"pass\": " + (failures == failuresBefore ? "true" : "false") + "}");
    return run;
}

// ---- 進み具合が同じ frame は先行 frame の有無によらず同じ (再生の履歴に依存しない) ----

void seekIndependence(const std::string& source, const std::string& target,
                      project::EquationOperation operation) {
    const std::string op = operation == project::EquationOperation::Pulse ? "pulse" : "outline";
    const auto make = [&](std::int64_t duration) {
        CaseDef c;
        c.label = "seek-" + op + "-n" + std::to_string(duration);
        c.group = "action";
        c.states = {{source, 72, "#FFFFFFFF", 6, {{target, 0}}}};
        c.actions = {{0, 0, 1, duration, operation}};
        return c;
    };
    const auto three = runCase(make(3));
    const auto one = runCase(make(1));
    if (three.result.status != math::MathRenderStatus::Ok ||
        one.result.status != math::MathRenderStatus::Ok)
        return;
    int w = 0;
    int h = 0;
    const auto middle = loadAlpha(three.result.actions[0].interval.frames[1], w, h);
    const auto only = loadAlpha(one.result.actions[0].interval.frames[0], w, h);
    const bool same = !middle.empty() && middle == only;
    check(same, "seek-" + op + ": N=3 の中央の frame と N=1 の唯一の frame が一致する");
    records.push_back(
        "{\"label\": " + jsonString("seek-independence-" + op) +
        ", \"group\": \"action\", \"middle_of_3_equals_only_of_1\": " + (same ? "true" : "false") +
        ", \"middle_sha256\": " + jsonString(sha256(middle)) + ", \"only_sha256\": " +
        jsonString(sha256(only)) + ", \"pass\": " + (same ? "true" : "false") + "}");
}

// ---- P3-1 の評価で source frame から区間と frame を引く ----

void frameLookup() {
    std::printf("[frame-lookup]\n");
    CaseDef c;
    c.label = "frame-lookup";
    c.states = {{"x=\\frac{-b\\pm\\sqrt{b^2-4ac}}{2a}", 72, "#FFFFFFFF", 20, {{"b^2-4ac", 0}}},
                {"x+1", 72, "#FFFFFFFF", 4, {}}};
    c.transitions = {{5, {}}};
    c.actions = {{0, 0, 4, 6, project::EquationOperation::Pulse}};
    Compiled compiled;
    if (!compileCase(c, compiled))
        return;

    struct Expect {
        std::int64_t frame;
        app::EquationFrameKind kind;
        std::int64_t local;
        const char* meaning;
    };

    const Expect expects[] = {
        {3, app::EquationFrameKind::Hold, 0, "action の直前は通常の静止"},
        {4, app::EquationFrameKind::HoldAction, 0, "action の frame 0"},
        {6, app::EquationFrameKind::HoldAction, 2, "action の中央"},
        {9, app::EquationFrameKind::HoldAction, 5, "action の最後に表示する frame"},
        {10, app::EquationFrameKind::Hold, 0, "action の直後は通常の静止"},
        {20, app::EquationFrameKind::Transition, 0, "変形の frame 0"},
        {24, app::EquationFrameKind::Transition, 4, "変形の最後に表示する frame (4/5)"},
        {25, app::EquationFrameKind::Hold, 0, "変形の後は後の状態の静止"}};
    std::string rows = "[";
    // 順に引くのではなく、後ろから・飛び飛びに引いても同じ (source frame だけから決まる)。
    for (int pass = 0; pass < 2; ++pass) {
        for (std::size_t i = 0; i < std::size(expects); ++i) {
            const auto& e = expects[pass == 0 ? i : std::size(expects) - 1 - i];
            std::string error;
            const auto lookup =
                app::equationSequenceFrameAt(compiled.data, compiled.spec, e.frame, error);
            const bool ok = lookup && lookup->kind == e.kind &&
                            (e.kind == app::EquationFrameKind::Hold || lookup->frame == e.local);
            check(ok, std::string("frame-lookup: ") + e.meaning + " (source " +
                          std::to_string(e.frame) + ") " + error);
            if (pass == 0) {
                rows += (i > 0 ? ", " : "") + std::string("{\"source_frame\": ") +
                        std::to_string(e.frame) + ", \"meaning\": " + jsonString(e.meaning) +
                        ", \"ok\": " + (ok ? "true" : "false") + "}";
            }
        }
    }
    records.push_back("{\"label\": \"frame-lookup\", \"group\": \"action\", \"rows\": " + rows +
                      "], \"pass\": true}");
}

// ---- MathRasterCache で実際に公開する ----

void publication() {
    std::printf("[publication]\n");
    const auto cacheDirectory = evidence / "cache";
    app::MathRasterCache cache(
        "p33-acceptance", [](const std::filesystem::path& work, const std::atomic<bool>* cancel) {
            return manim::preflightManimMathTex({manimExe, work, std::chrono::milliseconds(60000)},
                                                cancel);
        });
    cache.setRenderTimeout(std::chrono::milliseconds(180000));
    cache.setAuthority(cacheDirectory, true);
    const auto wait = [&](auto predicate) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(15);
        while (!predicate() && std::chrono::steady_clock::now() < deadline)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        return predicate();
    };
    if (!check(wait([&] {
                   return cache.backendState() != app::MathRasterCache::BackendState::Checking;
               }) &&
                   cache.backendState() == app::MathRasterCache::BackendState::Available,
               "公開: 実 backend の preflight が Available: " +
                   cache.backendMessage().toStdString()))
        return;

    CaseDef c;
    c.label = "publication-quadratic";
    c.states = {
        {"x+\\frac{b}{2a}=\\pm\\frac{\\sqrt{b^2-4ac}}{2a}", 72, "#FFFFFFFF", 8, {{"b^2-4ac", 0}}},
        {"x=\\frac{-b\\pm\\sqrt{b^2-4ac}}{2a}", 64, "#FF40C0FF", 16, {{"b^2-4ac", 0}}}};
    c.transitions = {{6, {{0, 0}}}};
    c.actions = {{1, 0, 2, 4, project::EquationOperation::Outline},
                 {1, 0, 8, 5, project::EquationOperation::Pulse}};
    Compiled compiled;
    if (!compileCase(c, compiled))
        return;
    app::MathRasterCache::EquationSequenceEntry entry;
    const auto started = std::chrono::steady_clock::now();
    wait([&] {
        entry = cache.requestEquationSequence(compiled.render);
        return entry.state != app::MathRasterCache::State::Pending;
    });
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started)
                        .count();
    check(entry.state == app::MathRasterCache::State::Ready,
          "公開: Ready になる: " + entry.message.toStdString());
    const auto key = cache.equationSequenceKeyFor(compiled.render).toStdString();
    const auto artifact = cache.readyEquationSequence(compiled.render);
    std::string colorText = "[]";
    bool colorsOk = false;
    std::int64_t frameCount = 0;
    if (check(artifact.has_value(), "公開: disk の artifact を検査し直して読める")) {
        const auto& transition = artifact->transitions.at(0);
        std::uint32_t last = 0;
        math::mathTransformColorAt(0xFFFFFFFFu, 0xFF40C0FFu, 5, 6, last);
        colorsOk =
            transition.frames.front().colorArgb == 0xFFFFFFFFu &&
            transition.frames.back().colorArgb == last &&
            artifact->actions.at(0).accent.front().colorArgb == math::kEquationActionAccentArgb &&
            artifact->actions.at(0).base.colorArgb == 0xFF40C0FFu &&
            artifact->actions.at(1).base.colorArgb == 0xFF40C0FFu;
        check(colorsOk, "公開: provenance の合成の色が規則どおり");
        for (const auto& t : artifact->transitions)
            for (const auto& frame : t.frames) {
                std::vector<std::uint8_t> bytes;
                std::string error;
                check(app::loadEquationArtifactFrame(frame, t.width, t.height, bytes, error),
                      "公開: 変形の frame を SHA-256 付きで読める: " + error);
                ++frameCount;
            }
        for (const auto& a : artifact->actions) {
            std::vector<std::uint8_t> bytes;
            std::string error;
            check(app::loadEquationArtifactFrame(a.base, a.width, a.height, bytes, error),
                  "公開: action の base を読める: " + error);
            for (const auto& frame : a.accent) {
                check(app::loadEquationArtifactFrame(frame, a.width, a.height, bytes, error),
                      "公開: action の frame を読める: " + error);
                ++frameCount;
            }
        }
        colorText = "[\"" + std::to_string(transition.frames.front().colorArgb) + "\", \"" +
                    std::to_string(transition.frames.back().colorArgb) + "\"]";
    }
    const auto provenance = readFile(app::equationSequenceProvenancePath(cacheDirectory, key));
    check(provenance.rfind(app::kEquationSequenceArtifactFormat, 0) == 0,
          "公開: provenance の 1 行目は artifact の形式");

    // copy/remap した同じ意味の sequence は同じ key。
    auto copy = compiled.data;
    int next = 0;
    std::string error;
    check(project::remapEquationSequenceIds(
              copy, [&] { return "R" + std::to_string(next++); }, error),
          "公開: ID を remap できる: " + error);
    const auto copied = app::compileEquationSequence(copy);
    std::string copyKey;
    if (copied.value) {
        const auto copyRender = app::equationSequenceRenderSpecFor(*copied.value, error);
        if (copyRender)
            copyKey = cache.equationSequenceKeyFor(*copyRender).toStdString();
    }
    check(!copyKey.empty() && copyKey == key, "公開: remap した複製は同じ key");
    // 静止・変形 (P2) の key は別の名前空間で、この key と一致しない。
    const auto staticKey = cache.keyFor(compiled.render.states[0].still).toStdString();
    check(staticKey != key, "公開: 静止の key と別");

    records.push_back(
        "{\"label\": \"publication-quadratic\", \"group\": \"artifact\", \"state\": " +
        std::to_string(static_cast<int>(entry.state)) + ", \"ms\": " + std::to_string(ms) +
        ", \"key\": " + jsonString(key) + ", \"provenance\": " +
        jsonString(app::equationSequenceProvenancePath(cacheDirectory, key).generic_string()) +
        ", \"frames_verified\": " + std::to_string(frameCount) + ", \"colors_ok\": " +
        (colorsOk ? "true" : "false") + ", \"transition_colors\": " + colorText +
        ", \"remap_same_key\": " + (copyKey == key ? "true" : "false") + ", \"pass\": " +
        (entry.state == app::MathRasterCache::State::Ready ? "true" : "false") + "}");
    cache.shutdown();
}

void writeResults(const std::string& toolchain) {
    std::string json =
        "{\"schema\": \"mvm-equation-sequence-p33-acceptance/1\", "
        "\"toolchain\": " +
        jsonString(toolchain) + ", \"template\": " +
        jsonString(std::string(manim::kEquationSequenceTemplateId) + "/" +
                   std::to_string(manim::kEquationSequenceTemplateVersion)) +
        ", \"key_namespace\": " + jsonString(math::kEquationSequenceKeyVersion) +
        ", \"artifact_format\": " + jsonString(app::kEquationSequenceArtifactFormat) +
        ", \"progress\": " + jsonString(math::kEquationSequenceProgressVersion) +
        ", \"checks\": " + std::to_string(checks) + ", \"failures\": " + std::to_string(failures) +
        ", \"failure_messages\": [";
    for (std::size_t i = 0; i < failureMessages.size(); ++i)
        json += (i > 0 ? ", " : "") + jsonString(failureMessages[i]);
    json += "], \"cases\": [\n";
    for (std::size_t i = 0; i < records.size(); ++i)
        json += (i > 0 ? ",\n" : "") + records[i];
    json += "\n]}\n";
    std::ofstream(evidence / "results.json", std::ios::binary) << json;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    mvm_enable_utf8_console();
    int count = 0;
    char** args = mvm_win_get_utf8_args(&count);
    if (!args || count != 3) {
        std::fprintf(stderr,
                     "使い方: mvm_equation_sequence_smoke <manim.exe> <証拠の directory>\n");
        mvm_win_free_utf8_args(args, count);
        return 2;
    }
    manimExe = fromUtf8(args[1]);
    evidence = std::filesystem::absolute(fromUtf8(args[2]));
    mvm_win_free_utf8_args(args, count);
    if (std::filesystem::exists(evidence)) {
        std::fprintf(stderr, "証拠の directory が既にあります (結果を上書きしない)\n");
        return 2;
    }
    std::filesystem::create_directories(evidence / "jobs");

    const auto preflight = manim::preflightManimMathTex(
        {manimExe, evidence / "preflight", std::chrono::milliseconds(60000)}, nullptr);
    if (preflight.status != math::MathPreflightStatus::Available) {
        std::fprintf(stderr, "backend を使えません: %s\n", preflight.message.c_str());
        return 2;
    }
    std::printf("toolchain:\n%s", preflight.backend.fingerprint.canonical.c_str());

    using Op = project::EquationOperation;
    const std::string quadratic = "x=\\frac{-b\\pm\\sqrt{b^2-4ac}}{2a}";
    const std::string s4 = "x+\\frac{b}{2a}=\\pm\\frac{\\sqrt{b^2-4ac}}{2a}";

    // ---- partition / backend の構造検証 ----
    runCase({"x-plus-x", "partition", {{"x+x", 72, "#FFFFFFFF", 6, {}}}, {}, {}});
    runCase({"duplicate-semantic-terms",
             "partition",
             {{"x+x=2x", 72, "#FFFFFFFF", 8, {{"x", 0}, {"x", 1}}}},
             {},
             {{0, 1, 1, 3, Op::Outline}}});
    runCase({"quadratic-discriminant-in-frac",
             "partition",
             {{quadratic, 72, "#FFFFFFFF", 8, {{"b^2-4ac", 0}}}},
             {},
             {{0, 0, 1, 3, Op::Outline}}});
    runCase({"greek-command",
             "partition",
             {{"\\alpha+\\beta=\\gamma", 72, "#FFFFFFFF", 8, {{"\\beta", 0}}}},
             {},
             {{0, 0, 0, 3, Op::Pulse}}});
    runCase({"superscript-subscript",
             "partition",
             {{"a^{2}+b_{1}=c_n^3", 72, "#FFFFFFFF", 8, {{"b_{1}", 0}, {"c_n^3", 0}}}},
             {},
             {{0, 1, 2, 3, Op::Pulse}}});
    runCase({"whitespace-auto-segment",
             "partition",
             {{"x x", 72, "#FFFFFFFF", 6, {{"x", 0}, {"x", 1}}},
              {"x x", 96, "#FFFFFFFF", 6, {{"x", 0}, {"x", 1}}}},
             {{4, {{0, 1}, {1, 0}}}},
             {{0, 0, 1, 2, Op::Outline}}});

    // ---- transition ----
    runCase({"p30-quadratic-step",
             "transition",
             {{s4, 72, "#FFFFFFFF", 6, {{"b^2-4ac", 0}}},
              {quadratic, 72, "#FFFFFFFF", 6, {{"b^2-4ac", 0}}}},
             {{12, {{0, 0}}}},
             {}});
    runCase({"explicit-correspondence-swap",
             "transition",
             {{"a+b", 72, "#FFFFFFFF", 4, {{"a", 0}, {"b", 0}}},
              {"b+a", 72, "#FFFFFFFF", 4, {{"b", 0}, {"a", 0}}}},
             {{6, {{0, 1}, {1, 0}}}},
             {}});
    runCase({"duplicate-automatic-terms",
             "transition",
             {{"x + x = 2x", 72, "#FFFFFFFF", 4, {}}, {"x + x + x = 3x", 72, "#FFFFFFFF", 4, {}}},
             {{8, {}}},
             {}});
    runCase({"unmatched-fade",
             "transition",
             {{"a + b = c", 72, "#FFFFFFFF", 4, {}}, {"a = c - b", 72, "#FFFFFFFF", 4, {}}},
             {{6, {}}},
             {}});
    runCase({"n-equals-1",
             "transition",
             {{"a + b", 72, "#FFFFFFFF", 4, {}}, {"a + b + c", 72, "#FFFFFFFF", 4, {}}},
             {{1, {}}},
             {}});
    runCase({"different-font-sizes",
             "transition",
             {{"x^2 + 1 = y", 96, "#FFFFFFFF", 4, {}}, {"x^2 = y - 1", 64, "#FFFFFFFF", 4, {}}},
             {{6, {}}},
             {}});
    runCase({"different-foreground-colors",
             "transition",
             {{"a + b = c", 72, "#FFFFFFFF", 4, {}}, {"a + b = c + 0", 72, "#FFFF8040", 4, {}}},
             {{6, {}}},
             {}});
    runCase(
        {"p30-left-right-auto",
         "transition",
         {{"x^2 + \\frac{b}{a}x + \\left(\\frac{b}{2a}\\right)^2 = -\\frac{c}{a} + "
           "\\left(\\frac{b}{2a}\\right)^2",
           64,
           "#FFFFFFFF",
           4,
           {}},
          {"\\left(x + \\frac{b}{2a}\\right)^2 = \\frac{b^2 - 4ac}{4a^2}", 64, "#FFFFFFFF", 4, {}}},
         {{8, {}}},
         {}});

    // ---- action ----
    runCase({"outline-discriminant",
             "action",
             {{quadratic, 72, "#FFFFFFFF", 12, {{"b^2-4ac", 0}}}},
             {},
             {{0, 0, 2, 7, Op::Outline}}});
    runCase({"pulse-discriminant",
             "action",
             {{quadratic, 72, "#FFFFFFFF", 12, {{"b^2-4ac", 0}}}},
             {},
             {{0, 0, 2, 7, Op::Pulse}}});
    runCase({"multi-glyph-target-both-ops",
             "action",
             {{"y=a^2+2ab+b^2", 72, "#FFFFFFFF", 20, {{"2ab", 0}}}},
             {},
             {{0, 0, 1, 5, Op::Outline}, {0, 0, 8, 5, Op::Pulse}}});
    runCase({"action-then-transition",
             "action",
             {{quadratic, 72, "#FFFFFFFF", 10, {{"b^2-4ac", 0}}}, {"x+1", 72, "#FFFFFFFF", 4, {}}},
             {{4, {}}},
             {{0, 0, 3, 4, Op::Pulse}}});
    seekIndependence(quadratic, "b^2-4ac", Op::Outline);
    seekIndependence(quadratic, "b^2-4ac", Op::Pulse);
    frameLookup();

    // ---- 実 backend の負例 ----
    // \, は P3-2 の字句検査を通るが、Manim では点を持つ子孫が 0 の部分になる (実測の空の対象)。
    runCase({"empty-glyph-target",
             "negative",
             {{"a\\,b", 72, "#FFFFFFFF", 6, {{"\\,", 0}}}},
             {},
             {{0, 0, 1, 2, Op::Outline}},
             math::EquationBackendFailure::EmptyActionTarget});
    runCase({"empty-transition-handle",
             "negative",
             {{"a\\,b", 72, "#FFFFFFFF", 4, {{"\\,", 0}}}, {"a+b", 72, "#FFFFFFFF", 4, {{"+", 0}}}},
             {{4, {{0, 0}}}},
             {},
             math::EquationBackendFailure::EmptyTransitionHandle});
    // Manim が {{ }} で部分を分け直すと、top-level の部分の数か文字列が segment と違う。
    runCase({"brace-resplit",
             "negative",
             {{"{{a}} + b", 72, "#FFFFFFFF", 4, {}}},
             {},
             {},
             math::EquationBackendFailure::SegmentCountMismatch,
             math::EquationBackendFailure::SegmentTextMismatch});

    publication();

    writeResults(preflight.backend.fingerprint.canonical);
    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}
