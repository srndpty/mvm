// 実 Manim + LaTeX で式から式への変形の backend (renderManimMathTransform) を確かめる手動の smoke。
//
// 通常の CTest には入れない (Manim と MiKTeX の導入を build・試験の必須条件にしない。
// mvm_math_manim_smoke と同じ扱い)。latex と dvisvgm は、この process の PATH から探す。
//
//   mvm_math_transform_smoke <manim.exe> <作業 directory (存在しないこと)>
//
// 各ケースで、端点の静止を製品の静止の描画 (renderManimMathTex) で描き、製品の loader
// (loadMathCoverage) で読んで、変形の backend へ渡す。backend の合否に加え、次を backend とは
// 別に確かめる (期待値は手で数えた値):
// - frame 0 を読み直し、source の静止を返された配置に置いたものと全画素で一致する
// - 同じ frame を縦に 1 画素ずらした位置とは一致しない (照合が位置に敏感で、空振りでない)
// - artifact が両端の矩形を含み、一時的な canvas の縁から離れている
// - request.json の対応が mvm の n 番目の出現の照合のまま (重複項)
// 奇数の大きさの端点で半画素の補正 (±0.5) が横・縦の両方で使われたことも確かめる。

#include "app/math_clip_render.h"
#include "media/manim/manim_math_tex.h"
#include "util/mvm_win_utf8.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const std::string& message) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

namespace math = mvm::math;
namespace manim = mvm::manim;

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

std::filesystem::path manimExe;
std::filesystem::path workRoot;
int jobCounter = 0;

std::filesystem::path nextJob(const std::string& label) {
    return workRoot / (std::to_string(++jobCounter) + "-" + label);
}

// (式, 文字サイズ) ごとに 1 回だけ静止を描いて読む。
std::map<std::pair<std::string, int>, math::MathCoverage> statics;

bool staticCoverage(const std::string& source, int fontSize, math::MathCoverage& coverage) {
    const auto key = std::make_pair(source, fontSize);
    if (const auto found = statics.find(key); found != statics.end()) {
        coverage = found->second;
        return true;
    }
    math::MathStaticRenderRequest request;
    request.spec = {"latex", source, fontSize};
    request.jobDirectory = nextJob("static");
    const auto started = std::chrono::steady_clock::now();
    const auto rendered = manim::renderManimMathTex(manimExe, request, nullptr);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started)
                        .count();
    std::string error;
    if (rendered.status != math::MathRenderStatus::Ok ||
        !mvm::app::loadMathCoverage(rendered.png, coverage, error)) {
        check(false, "静止を描いて読める: " + source + " / " + rendered.message + error);
        return false;
    }
    std::printf("  静止 %dx%d (%lld ms) fs=%d %s\n", coverage.width, coverage.height,
                static_cast<long long>(ms), fontSize, source.c_str());
    statics.emplace(key, coverage);
    return true;
}

int halfPixelX = 0; // 横の補正が 0 でなかった端点の数
int halfPixelY = 0;

struct Case {
    std::string label;
    std::string source;
    std::string target;
    int sourceFont = 64;
    int targetFont = 64;
    std::int64_t frames = 30;
    // request.json に含まれるべき対応 (空なら見ない)。
    std::string expectedMatching;
};

void runCase(const Case& c) {
    std::printf("[%s]\n", c.label.c_str());
    math::MathTransformRenderRequest request;
    request.spec = {{"latex", c.source, c.sourceFont}, {"latex", c.target, c.targetFont}, c.frames};
    if (!staticCoverage(c.source, c.sourceFont, request.sourceStatic) ||
        !staticCoverage(c.target, c.targetFont, request.targetStatic))
        return;
    request.jobDirectory = nextJob(c.label);
    const auto started = std::chrono::steady_clock::now();
    const auto result =
        manim::renderManimMathTransform(manimExe, request, mvm::app::loadMathCoverage, nullptr);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started)
                        .count();
    check(result.status == math::MathRenderStatus::Ok,
          c.label + ": 変形を描ける: " + result.message + "\n" + result.log.substr(0, 2000));
    if (result.status != math::MathRenderStatus::Ok)
        return;
    const auto& p = result.placement;
    std::printf("  変形 N=%lld %lld ms canvas %dx%d artifact (%d,%d %dx%d)\n"
                "  source (%d,%d) shift (%g,%g)  target (%d,%d) shift (%g,%g)\n",
                static_cast<long long>(c.frames), static_cast<long long>(ms), result.canvasWidth,
                result.canvasHeight, result.artifact.x, result.artifact.y, result.artifact.width,
                result.artifact.height, p.source.left, p.source.top, p.source.shiftX,
                p.source.shiftY, p.target.left, p.target.top, p.target.shiftX, p.target.shiftY);
    for (const auto* endpoint : {&p.source, &p.target}) {
        halfPixelX += endpoint->shiftX != 0.0 ? 1 : 0;
        halfPixelY += endpoint->shiftY != 0.0 ? 1 : 0;
    }
    check(static_cast<std::int64_t>(result.frames.size()) == c.frames,
          c.label + ": frames 枚を返す");

    // frame 0 を backend と別に読み直して照合する。
    math::MathCoverage first;
    std::string error;
    check(!result.frames.empty() && mvm::app::loadMathCoverage(result.frames[0], first, error),
          c.label + ": frame 0 を読める: " + error);
    const auto& source = request.sourceStatic;
    check(math::mathEndpointDifference(first, source, p.source.left, p.source.top) == 0,
          c.label + ": frame 0 は source の静止を配置に置いたものと全画素で一致する");
    const std::int64_t shiftedDown =
        math::mathEndpointDifference(first, source, p.source.left, p.source.top + 1);
    const std::int64_t shiftedUp =
        math::mathEndpointDifference(first, source, p.source.left, p.source.top - 1);
    check(shiftedDown > 0 && shiftedUp > 0,
          c.label + ": 縦に 1 画素ずらすと一致しない (照合は位置に敏感): " +
              std::to_string(shiftedDown) + " / " + std::to_string(shiftedUp));

    // artifact は両端の矩形を含み、canvas の縁から離れている。
    const math::MathRect sourceRect{p.source.left, p.source.top, source.width, source.height};
    const math::MathRect targetRect{p.target.left, p.target.top, request.targetStatic.width,
                                    request.targetStatic.height};
    check(math::mathRectUnion(result.artifact, sourceRect) == result.artifact &&
              math::mathRectUnion(result.artifact, targetRect) == result.artifact,
          c.label + ": artifact は両端の静止の矩形を含む");
    check(result.artifact.x > 0 && result.artifact.y > 0 &&
              result.artifact.x + result.artifact.width < result.canvasWidth &&
              result.artifact.y + result.artifact.height < result.canvasHeight,
          c.label + ": artifact は一時的な canvas の縁から離れている");
    if (result.artifact != math::mathRectUnion(sourceRect, targetRect))
        std::printf("  途中の frame が端点の矩形の和の外に出た\n");

    if (!c.expectedMatching.empty()) {
        const std::string json = readFile(request.jobDirectory / L"request.json");
        check(json.find(c.expectedMatching) != std::string::npos,
              c.label + ": request.json の対応は mvm の照合: " + json);
    }
}

void runRejected(const std::string& label, const std::string& source, const std::string& target,
                 const std::string& fragment) {
    std::printf("[%s]\n", label.c_str());
    math::MathTransformRenderRequest request;
    request.spec = {{"latex", source, 64}, {"latex", target, 64}, 10};
    if (!staticCoverage(source, 64, request.sourceStatic) ||
        !staticCoverage(target, 64, request.targetStatic))
        return;
    request.jobDirectory = nextJob(label);
    const auto result =
        manim::renderManimMathTransform(manimExe, request, mvm::app::loadMathCoverage, nullptr);
    std::printf("  %s: %s\n", math::mathRenderStatusName(result.status), result.message.c_str());
    check(result.status == math::MathRenderStatus::Failed &&
              result.message.find(fragment) != std::string::npos,
          label + ": 拒否する: " + result.message);
}

} // namespace

int main() {
    mvm_enable_utf8_console();
    int argc = 0;
    char** argv = mvm_win_get_utf8_args(&argc);
    if (!argv || argc != 3) {
        std::fprintf(stderr, "使い方: mvm_math_transform_smoke <manim.exe> <作業 directory>\n");
        mvm_win_free_utf8_args(argv, argc);
        return 2;
    }
    manimExe = fromUtf8(argv[1]);
    workRoot = std::filesystem::absolute(fromUtf8(argv[2]));
    mvm_win_free_utf8_args(argv, argc);
    if (std::filesystem::exists(workRoot)) {
        std::fprintf(stderr, "作業 directory が既にあります (結果を上書きしない)\n");
        return 2;
    }
    std::filesystem::create_directories(workRoot);

    const std::string e1 = "x^2 + \\frac{b}{a}x = -\\frac{c}{a}";
    const std::string e2 = "x^2 + \\frac{b}{a}x + \\left(\\frac{b}{2a}\\right)^2 = "
                           "-\\frac{c}{a} + \\left(\\frac{b}{2a}\\right)^2";
    const std::string e3 = "\\left(x + \\frac{b}{2a}\\right)^2 = \\frac{b^2 - 4ac}{4a^2}";
    runCase({"e1-e2", e1, e2, 64, 64, 30,
             "\"pairs\": [[0, 0], [1, 1], [2, 2], [3, 5], [4, 6]], \"unmatched_source\": [], "
             "\"unmatched_target\": [3, 4, 7, 8]"});
    runCase({"e2-e3", e2, e3, 64, 64, 30,
             "\"pairs\": [[5, 1]], \"unmatched_source\": [0, 1, 2, 3, 4, 6, 7, 8], "
             "\"unmatched_target\": [0, 2]"});
    runCase({"dup", "x + x = 2x", "x + x + x = 3x", 64, 64, 30,
             "\"pairs\": [[0, 0], [1, 1], [2, 2], [3, 5]], \"unmatched_source\": [4], "
             "\"unmatched_target\": [3, 4, 6]"});
    runCase({"font-96-to-64", e1, e2, 96, 64, 30, {}});
    runCase({"frac", "\\frac{a}{b} + \\frac{c}{d} = 1", "\\frac{ad + bc}{bd} = 1", 64, 64, 30,
             "\"pairs\": [[3, 1], [4, 2]], \"unmatched_source\": [0, 1, 2], "
             "\"unmatched_target\": [0]"});
    runCase({"left-right", "\\left(a + b\\right)^2 = c", "a^2 + 2ab + b^2 = c", 64, 64, 30,
             "\"pairs\": [[1, 5], [2, 6]], \"unmatched_source\": [0], "
             "\"unmatched_target\": [0, 1, 2, 3, 4]"});
    // 小さい式は静止の大きさの偶奇がばらつき、半画素の補正の両方の軸を通しやすい。
    runCase({"small-a", "a", "a + b", 64, 64, 12, "\"pairs\": [[0, 0]]"});
    runCase({"small-y", "y", "y^2 = 1", 48, 72, 12, {}});
    runCase({"small-k", "k = 1", "k", 72, 50, 12, {}});

    // Manim が {{ }} で部分を分け直すと、mvm の部分と一致しないので拒否する。
    runRejected("brace-resplit", "{{a}} + b = c", "b + a = c", "文字列が分けた部分と違います");

    std::printf("半画素の補正を使った端点: 横 %d、縦 %d\n", halfPixelX, halfPixelY);
    check(halfPixelX > 0 && halfPixelY > 0,
          "奇数の大きさの端点で、横と縦の両方の半画素の補正を実 Manim で通した");

    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}
