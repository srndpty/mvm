// 式から式への変形 (P2-2) の backend 中立な契約: TeX の式の分け方、n 番目の出現の照合、
// 変形の cache key、端点の配置 (半画素の補正)、文字色の補間。
//
// 期待値は実装から作らない。分け方・照合・配置・色は手で数えた値。key の golden 値は、
// header に書いた正準形を `printf ... | sha256sum` で別に計算したもの。

#include "media/math/math_raster_layout.h"
#include "media/math/math_tex_segments.h"
#include "media/math/math_transform.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
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

using mvm::math::MathSegmentMatching;
using mvm::math::MathSegmentPair;
using mvm::math::MathTexSegment;

const std::string kE1 = "x^2 + \\frac{b}{a}x = -\\frac{c}{a}";
const std::string kE2 = "x^2 + \\frac{b}{a}x + \\left(\\frac{b}{2a}\\right)^2 = "
                        "-\\frac{c}{a} + \\left(\\frac{b}{2a}\\right)^2";
const std::string kE3 = "\\left(x + \\frac{b}{2a}\\right)^2 = \\frac{b^2 - 4ac}{4a^2}";
const std::string kD1 = "x + x = 2x";
const std::string kD2 = "x + x + x = 3x";

std::string join(const std::vector<MathTexSegment>& segments) {
    std::string text;
    for (const auto& segment : segments)
        text += segment.text;
    return text;
}

std::string describe(const std::vector<MathTexSegment>& segments) {
    std::string text;
    for (const auto& segment : segments)
        text += "[" + segment.text + "|" + segment.key + "]";
    return text;
}

// texts の各要素の前後の空白を手で除いた key を付ける。
std::vector<MathTexSegment> expected(std::vector<std::pair<std::string, std::string>> pieces) {
    std::vector<MathTexSegment> segments;
    for (auto& [text, key] : pieces)
        segments.push_back({text, key});
    return segments;
}

void expectSegments(const std::string& source, const std::vector<MathTexSegment>& want,
                    const std::string& what) {
    const auto got = mvm::math::segmentMathTex(source);
    check(got == want, what + " の分け方: " + describe(got));
    check(join(got) == source, what + " の部分を連結すると入力に戻る");
}

void testGoldenSegments() {
    expectSegments(kE1,
                   expected({{"x^2", "x^2"},
                             {" + ", "+"},
                             {"\\frac{b}{a}x", "\\frac{b}{a}x"},
                             {" = ", "="},
                             {"-\\frac{c}{a}", "-\\frac{c}{a}"}}),
                   "E1");
    expectSegments(kE2,
                   expected({{"x^2", "x^2"},
                             {" + ", "+"},
                             {"\\frac{b}{a}x", "\\frac{b}{a}x"},
                             {" + ", "+"},
                             {"\\left(\\frac{b}{2a}\\right)^2", "\\left(\\frac{b}{2a}\\right)^2"},
                             {" = ", "="},
                             {"-\\frac{c}{a}", "-\\frac{c}{a}"},
                             {" + ", "+"},
                             {"\\left(\\frac{b}{2a}\\right)^2", "\\left(\\frac{b}{2a}\\right)^2"}}),
                   "E2");
    expectSegments(
        kE3,
        expected({{"\\left(x + \\frac{b}{2a}\\right)^2", "\\left(x + \\frac{b}{2a}\\right)^2"},
                  {" = ", "="},
                  {"\\frac{b^2 - 4ac}{4a^2}", "\\frac{b^2 - 4ac}{4a^2}"}}),
        "E3");
    expectSegments(kD1,
                   expected({{"x", "x"}, {" + ", "+"}, {"x", "x"}, {" = ", "="}, {"2x", "2x"}}),
                   "重複項 D1");
    expectSegments(kD2,
                   expected({{"x", "x"},
                             {" + ", "+"},
                             {"x", "x"},
                             {" + ", "+"},
                             {"x", "x"},
                             {" = ", "="},
                             {"3x", "3x"}}),
                   "重複項 D2");
    expectSegments("\\boxed{x^2 + y^2 = r^2}",
                   expected({{"\\boxed{x^2 + y^2 = r^2}", "\\boxed{x^2 + y^2 = r^2}"}}),
                   "中括弧の中では分けない (boxed)");
}

void testSegmentRules() {
    expectSegments("", {}, "空の入力");
    expectSegments("   ", expected({{"   ", ""}}), "空白だけの入力");
    expectSegments("x=y", expected({{"x", "x"}, {"=", "="}, {"y", "y"}}), "空白の無い関係子");
    expectSegments(" = x", expected({{" = ", "="}, {"x", "x"}}), "先頭の関係子");
    expectSegments("x =", expected({{"x", "x"}, {" =", "="}}), "末尾の関係子");
    expectSegments("  x  ", expected({{"  x  ", "x"}}), "前後の空白は項に残る");
    expectSegments("-x + y", expected({{"-x", "-x"}, {" + ", "+"}, {"y", "y"}}),
                   "先頭の単項の符号");
    expectSegments("a + + b", expected({{"a", "a"}, {" + ", "+"}, {"+ b", "+ b"}}),
                   "演算子の直後の + は単項");
    expectSegments("(-a + b) = c",
                   expected({{"(-a", "(-a"}, {" + ", "+"}, {"b)", "b)"}, {" = ", "="}, {"c", "c"}}),
                   "( の直後の符号は単項、( ) は深さを変えない");
    expectSegments("x^-1 + y", expected({{"x^-1", "x^-1"}, {" + ", "+"}, {"y", "y"}}),
                   "^ の引数の - では分けない");
    expectSegments("x_= y", expected({{"x_= y", "x_= y"}}), "_ の引数の = では分けない");
    expectSegments("x^ {2} + y", expected({{"x^ {2}", "x^ {2}"}, {" + ", "+"}, {"y", "y"}}),
                   "^ の後の空白と中括弧");
    expectSegments("a \\not= b = c",
                   expected({{"a \\not= b", "a \\not= b"}, {" = ", "="}, {"c", "c"}}),
                   "\\not= では分けない");
    expectSegments("a \\le b \\leq c \\left( d \\right)",
                   expected({{"a", "a"},
                             {" \\le ", "\\le"},
                             {"b", "b"},
                             {" \\leq ", "\\leq"},
                             {"c \\left( d \\right)", "c \\left( d \\right)"}}),
                   "\\le と \\leq と \\left は別の命令");
    expectSegments("a \\ne b", expected({{"a", "a"}, {" \\ne ", "\\ne"}, {"b", "b"}}), "\\ne");
    expectSegments("x \\pm y \\cdot z \\times w - v",
                   expected({{"x", "x"},
                             {" \\pm ", "\\pm"},
                             {"y", "y"},
                             {" \\cdot ", "\\cdot"},
                             {"z", "z"},
                             {" \\times ", "\\times"},
                             {"w", "w"},
                             {" - ", "-"},
                             {"v", "v"}}),
                   "二項演算子");
    expectSegments("\\{a = b\\}", expected({{"\\{a", "\\{a"}, {" = ", "="}, {"b\\}", "b\\}"}}),
                   "\\{ \\} は深さを変えない");
    expectSegments("\\left\\{ a = b \\right.",
                   expected({{"\\left\\{ a = b \\right.", "\\left\\{ a = b \\right."}}),
                   "\\left と \\right の間では分けない");
    expectSegments("\\begin{aligned} a &= b \\\\ c &= d \\end{aligned}",
                   expected({{"\\begin{aligned} a &= b \\\\ c &= d \\end{aligned}",
                              "\\begin{aligned} a &= b \\\\ c &= d \\end{aligned}"}}),
                   "環境の中では分けない");
    expectSegments("x = %c = d\n y",
                   expected({{"x", "x"}, {" = ", "="}, {"%c = d\n y", "%c = d\n y"}}),
                   "comment の中では分けない");
    expectSegments(
        "x = 50\\% + y",
        expected({{"x", "x"}, {" = ", "="}, {"50\\%", "50\\%"}, {" + ", "+"}, {"y", "y"}}),
        "\\% は comment でない");
    expectSegments("x = y\\", expected({{"x", "x"}, {" = ", "="}, {"y\\", "y\\"}}), "末尾の \\");
    expectSegments("a } = b", expected({{"a } = b", "a } = b"}}),
                   "閉じ過ぎた中括弧の後では分けない");
    expectSegments("a { = b", expected({{"a { = b", "a { = b"}}), "閉じない中括弧の中では分けない");
    expectSegments("\xCE\xB1 = \xCE\xB2",
                   expected({{"\xCE\xB1", "\xCE\xB1"}, {" = ", "="}, {"\xCE\xB2", "\xCE\xB2"}}),
                   "UTF-8 の文字");
    expectSegments("x\t=\r\ny", expected({{"x", "x"}, {"\t=\r\n", "="}, {"y", "y"}}),
                   "tab と CRLF");
}

void testSegmentDeterminism() {
    for (const auto& source : {kE1, kE2, kE3, kD1, kD2}) {
        const auto first = mvm::math::segmentMathTex(source);
        check(!first.empty(), "部分がある: " + source);
        for (int i = 0; i < 3; ++i)
            check(mvm::math::segmentMathTex(source) == first, "分け方は決定的: " + source);
        for (const auto& segment : first)
            check(!segment.text.empty(), "空の部分は無い: " + source);
    }
}

void expectMatching(const std::string& source, const std::string& target,
                    const MathSegmentMatching& want, const std::string& what) {
    const auto a = mvm::math::segmentMathTex(source);
    const auto b = mvm::math::segmentMathTex(target);
    const auto got = mvm::math::matchMathTexSegments(a, b);
    std::string pairs;
    for (const auto& pair : got.pairs)
        pairs += std::to_string(pair.source) + "->" + std::to_string(pair.target) + " ";
    check(got == want, what + " の照合: " + pairs);
    for (int i = 0; i < 3; ++i)
        check(mvm::math::matchMathTexSegments(a, b) == got, what + " の照合は決定的");
    // 対応と余りで、両側の部分をちょうど 1 回ずつ数える。
    std::vector<int> sourceUses(a.size(), 0);
    std::vector<int> targetUses(b.size(), 0);
    for (const auto& pair : got.pairs) {
        ++sourceUses[pair.source];
        ++targetUses[pair.target];
        check(a[pair.source].key == b[pair.target].key, what + " の対応は同じ key どうし");
    }
    for (const auto i : got.unmatchedSource)
        ++sourceUses[i];
    for (const auto j : got.unmatchedTarget)
        ++targetUses[j];
    bool once = true;
    for (const int uses : sourceUses)
        once = once && uses == 1;
    for (const int uses : targetUses)
        once = once && uses == 1;
    check(once, what + " の各部分は対応か余りのどちらか 1 回");
}

void testMatching() {
    // E1 [x^2,+,F,=,-c] -> E2 [x^2,+,F,+,L,=,-c,+,L]。E1 の部分はすべて残り、E2 の 2 つ目の + と
    // 括弧の項 2 つ、3 つ目の + が現れる。
    expectMatching(kE1, kE2, {{{0, 0}, {1, 1}, {2, 2}, {3, 5}, {4, 6}}, {}, {3, 4, 7, 8}}, "E1→E2");
    // E2 -> E3 [L3,=,R3]。= だけが対応する (P2-0 の観測)。
    expectMatching(kE2, kE3, {{{5, 1}}, {0, 1, 2, 3, 4, 6, 7, 8}, {0, 2}}, "E2→E3");
    // 重複項: x の 1・2 番目、+ の 1 番目、= が対応し、3 つ目の x と 2 つ目の + と 3x が現れ、
    // 2x が消える。
    expectMatching(kD1, kD2, {{{0, 0}, {1, 1}, {2, 2}, {3, 5}}, {4}, {3, 4, 6}}, "重複項 D1→D2");
    // 逆向き: source の 3 つ目の x と 2 つ目の + は相手が無いので消える。
    expectMatching(kD2, kD1, {{{0, 0}, {1, 1}, {2, 2}, {5, 3}}, {3, 4, 6}, {4}}, "重複項 D2→D1");
    // 並べ替え: 対応は交差してよい。
    expectMatching("a + b", "b + a", {{{0, 2}, {1, 1}, {2, 0}}, {}, {}}, "項の並べ替え");
    // 前後の空白は key に含めない。
    expectMatching("x+y", "x + y", {{{0, 0}, {1, 1}, {2, 2}}, {}, {}}, "空白の違い");
    // 対応しない部分だけ。
    expectMatching("a", "b", {{}, {0}, {0}}, "共通の部分が無い");
    expectMatching("", kD1, {{}, {}, {0, 1, 2, 3, 4}}, "空の source");
    expectMatching(kD1, "", {{}, {0, 1, 2, 3, 4}, {}}, "空の target");
    expectMatching("", "", {{}, {}, {}}, "両方空");
}

using mvm::math::MathRenderSpec;
using mvm::math::MathToolchainFingerprint;
using mvm::math::MathTransformAlgorithms;
using mvm::math::MathTransformSpec;

MathTransformSpec baseTransform() {
    return {{"latex", kD1, 64}, {"latex", kD2, 64}, 30};
}

MathToolchainFingerprint baseToolchain() {
    return {"manim-mathtex", "backend=manim-mathtex\ntemplate=1\nmanim=M\nlatex=L\ndvisvgm=D\n"};
}

const std::string kTemplate = "manim-transform/1";

void testTransformKey() {
    // printf 'mvm-math-transform/1\nsegmenter=18:mvm-tex-segments/1\nmatching=15:mvm-tex-match/1\n
    //   frames=30\nsource_syntax=5:latex\nsource_source=10:x + x = 2x\nsource_font_size=64\n
    //   target_syntax=5:latex\ntarget_source=14:x + x + x = 3x\ntarget_font_size=64\n
    //   backend=13:manim-mathtex\ntoolchain=59:backend=manim-mathtex\ntemplate=1\nmanim=M\n
    //   latex=L\ndvisvgm=D\n\ntransform_template=17:manim-transform/1\n' | sha256sum
    const std::string golden = "366317d231c31763e60b27f856ed63dbc067533a1af9d5e57fb98294ae70696c";
    const std::string key =
        mvm::math::mathTransformKey(baseTransform(), baseToolchain(), kTemplate);
    check(key == golden, "変形の key が独立に計算した golden 値と一致する: " + key);
    check(mvm::math::mathTransformKey(baseTransform(), baseToolchain(), kTemplate) == key,
          "変形の key は決定的");
    check(std::string(mvm::math::kMathTexSegmenterVersion) == "mvm-tex-segments/1" &&
              std::string(mvm::math::kMathTexMatchingVersion) == "mvm-tex-match/1",
          "現在の版は golden 値の正準形と同じ");
    check(key != mvm::math::mathRenderKey(baseTransform().source, baseToolchain()) &&
              key != mvm::math::mathRenderKey(baseTransform().target, baseToolchain()),
          "変形の key は端点の静止の key と別");

    auto differs = [&](const MathTransformSpec& spec, const MathToolchainFingerprint& toolchain,
                       const std::string& templateText, const MathTransformAlgorithms& algorithms,
                       const std::string& what) {
        check(mvm::math::mathTransformKey(spec, toolchain, templateText, algorithms) != key,
              what + " を変えると変形の key が変わる");
    };
    const MathTransformAlgorithms current;
    auto spec = baseTransform();
    spec.source.source = "x + x = 2y";
    differs(spec, baseToolchain(), kTemplate, current, "source の式");
    spec = baseTransform();
    spec.source.syntax = "typst";
    differs(spec, baseToolchain(), kTemplate, current, "source の記法");
    spec = baseTransform();
    spec.source.fontSize = 65;
    differs(spec, baseToolchain(), kTemplate, current, "source の文字サイズ");
    spec = baseTransform();
    spec.target.source = "x + x + x = 3y";
    differs(spec, baseToolchain(), kTemplate, current, "target の式");
    spec = baseTransform();
    spec.target.syntax = "typst";
    differs(spec, baseToolchain(), kTemplate, current, "target の記法");
    spec = baseTransform();
    spec.target.fontSize = 65;
    differs(spec, baseToolchain(), kTemplate, current, "target の文字サイズ");
    spec = baseTransform();
    std::swap(spec.source, spec.target);
    differs(spec, baseToolchain(), kTemplate, current, "向き (source と target の入れ替え)");
    spec = baseTransform();
    spec.frames = 31;
    differs(spec, baseToolchain(), kTemplate, current, "frame 数");
    MathTransformAlgorithms algorithms;
    algorithms.segmenter = "mvm-tex-segments/2";
    differs(baseTransform(), baseToolchain(), kTemplate, algorithms, "分け方の版");
    algorithms = {};
    algorithms.matching = "mvm-tex-match/2";
    differs(baseTransform(), baseToolchain(), kTemplate, algorithms, "照合の版");
    auto toolchain = baseToolchain();
    toolchain.backendId = "latex-dvisvgm";
    differs(baseTransform(), toolchain, kTemplate, current, "backend id");
    toolchain = baseToolchain();
    toolchain.canonical += "x";
    differs(baseTransform(), toolchain, kTemplate, current, "toolchain");
    differs(baseTransform(), baseToolchain(), "manim-transform/2", current, "変形の script の識別");

    // byte 数を前置しているので、field の境界をずらした入力は同じ key にならない。
    spec = baseTransform();
    spec.source.source = "x + x = 2x\nsource_font_size=64\ntarget_syntax=5:latex";
    spec.target.syntax = "";
    differs(spec, baseToolchain(), kTemplate, current, "field の境界");
}

void testEndpointPlacement() {
    struct Row {
        int maskWidth, maskHeight, canvasWidth, canvasHeight;
        int left, top;
        double shiftX, shiftY;
    };

    // 幅・高さ・canvas の幅・canvas の高さの偶奇の 16 通り。
    // left = floor((W_C - W) / 2)、shift = left + W/2 - W_C/2 を手で数えた。
    const Row rows[] = {
        {4, 2, 10, 8, 3, 3, 0.0, 0.0},   {4, 3, 10, 8, 3, 2, 0.0, -0.5},
        {5, 2, 10, 8, 2, 3, -0.5, 0.0},  {5, 3, 10, 8, 2, 2, -0.5, -0.5},
        {4, 2, 11, 9, 3, 3, -0.5, -0.5}, {4, 3, 11, 9, 3, 3, -0.5, 0.0},
        {5, 2, 11, 9, 3, 3, 0.0, -0.5},  {5, 3, 11, 9, 3, 3, 0.0, 0.0},
        {4, 2, 10, 9, 3, 3, 0.0, -0.5},  {4, 3, 10, 9, 3, 3, 0.0, 0.0},
        {5, 2, 10, 9, 2, 3, -0.5, -0.5}, {5, 3, 10, 9, 2, 3, -0.5, 0.0},
        {4, 2, 11, 8, 3, 3, -0.5, 0.0},  {4, 3, 11, 8, 3, 2, -0.5, -0.5},
        {5, 2, 11, 8, 3, 3, 0.0, 0.0},   {5, 3, 11, 8, 3, 2, 0.0, -0.5},
    };
    int compared = 0;
    for (const auto& row : rows) {
        const std::string what =
            std::to_string(row.maskWidth) + "x" + std::to_string(row.maskHeight) + " in " +
            std::to_string(row.canvasWidth) + "x" + std::to_string(row.canvasHeight);
        mvm::math::MathEndpointPlacement placement;
        if (!mvm::math::mathEndpointPlacement(row.maskWidth, row.maskHeight, row.canvasWidth,
                                              row.canvasHeight, placement)) {
            check(false, what + " を置ける");
            continue;
        }
        ++compared;
        check(placement.left == row.left && placement.top == row.top &&
                  placement.shiftX == row.shiftX && placement.shiftY == row.shiftY,
              what + " の配置が手計算と一致する: " + std::to_string(placement.left) + "," +
                  std::to_string(placement.top) + " " + std::to_string(placement.shiftX) + "," +
                  std::to_string(placement.shiftY));
        // 不変条件: 静止の mask の中心が、backend の式の中心 (canvas の中心 + shift) に重なる。
        check(placement.left + row.maskWidth / 2.0 == row.canvasWidth / 2.0 + placement.shiftX &&
                  placement.top + row.maskHeight / 2.0 == row.canvasHeight / 2.0 + placement.shiftY,
              what + " で mask の中心と描画の中心が一致する");
        check(placement.left >= 0 && placement.left + row.maskWidth <= row.canvasWidth &&
                  placement.top >= 0 && placement.top + row.maskHeight <= row.canvasHeight,
              what + " で mask が canvas に収まる");
        // 静止の配置 (preview・書き出し) と同じ整数の位置。
        int left = -1;
        int top = -1;
        check(mvm::math::mathRasterPlacement(row.maskWidth, row.maskHeight, row.canvasWidth,
                                             row.canvasHeight, left, top) &&
                  left == placement.left && top == placement.top,
              what + " の整数の位置は静止の配置と同じ");
    }
    check(compared == 16, "16 通りすべてを比べた: " + std::to_string(compared));

    mvm::math::MathEndpointPlacement same;
    check(mvm::math::mathEndpointPlacement(7, 5, 7, 5, same) &&
              same == mvm::math::MathEndpointPlacement{0, 0, 0.0, 0.0},
          "canvas と同じ大きさの mask は (0,0) で補正なし");
    mvm::math::MathEndpointPlacement untouched{9, 9, 9.0, 9.0};
    check(!mvm::math::mathEndpointPlacement(12, 2, 11, 8, untouched) &&
              !mvm::math::mathEndpointPlacement(4, 9, 11, 8, untouched) &&
              !mvm::math::mathEndpointPlacement(0, 2, 11, 8, untouched) &&
              !mvm::math::mathEndpointPlacement(4, -1, 11, 8, untouched),
          "canvas より大きい・大きさが不正な mask は置かない");
    check(untouched == mvm::math::MathEndpointPlacement{9, 9, 9.0, 9.0}, "失敗では出力を変えない");
}

void testTransformPlacement() {
    // source 5x3 と target 4x2 を 11x8 の canvas へ (上の表の 2 行と同じ値)。
    mvm::math::MathTransformPlacement placement;
    check(mvm::math::mathTransformPlacement(5, 3, 4, 2, 11, 8, placement) &&
              placement.source == mvm::math::MathEndpointPlacement{3, 2, 0.0, -0.5} &&
              placement.target == mvm::math::MathEndpointPlacement{3, 3, -0.5, 0.0},
          "source と target の補正は各自の偶奇で決まる");
    mvm::math::MathTransformPlacement other;
    check(mvm::math::mathTransformPlacement(9, 7, 4, 2, 11, 8, other) &&
              other.target == placement.target,
          "target の配置は source の大きさに依らない");
    check(mvm::math::mathTransformPlacement(5, 3, 10, 6, 11, 8, other) &&
              other.source == placement.source,
          "source の配置は target の大きさに依らない");
    mvm::math::MathTransformPlacement failed;
    check(!mvm::math::mathTransformPlacement(5, 3, 12, 2, 11, 8, failed) &&
              !mvm::math::mathTransformPlacement(12, 3, 4, 2, 11, 8, failed),
          "どちらかの端点が canvas に収まらなければ失敗");
}

std::string hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof text, "%08X", value);
    return text;
}

void expectColor(std::uint32_t from, std::uint32_t to, std::int64_t frame, std::int64_t frames,
                 std::uint32_t want, const std::string& what) {
    std::uint32_t got = 0xDEADBEEFu;
    const bool ok = mvm::math::mathTransformColorAt(from, to, frame, frames, got);
    check(ok && got == want, what + ": " + hex(got) + " (期待 " + hex(want) + ")");
}

void testColorInterpolation() {
    const std::uint32_t from = 0x80FF0000u; // 半透明の赤
    const std::uint32_t to = 0xFF00FF40u;
    expectColor(from, to, 0, 30, from, "frame 0 は from そのもの");
    expectColor(from, to, 30, 30, to, "frame == frames (終状態の参照) は to そのもの");
    // 15/30: A (128+255)/2=191.5→192, R 127.5→128, G 127.5→128, B 64/2=32。
    expectColor(from, to, 15, 30, 0xC0808020u, "中点 (0.5 は切り上げ)");
    // 1/3: A 128+127/3=170.33→170, R 170, G 85, B 21.33→21。
    expectColor(from, to, 1, 3, 0xAAAA5515u, "1/3");
    // 2/3: A 212.67→213, R 85, G 170, B 42.67→43。
    expectColor(from, to, 2, 3, 0xD555AA2Bu, "2/3");
    // straight のまま補間する (premultiply すると透明な赤の RGB は 0 になり、R は 0 になる)。
    expectColor(0x00FF0000u, 0xFF0000FFu, 1, 2, 0x80800080u, "透明な色からの補間は straight");
    for (std::int64_t i = 0; i <= 7; ++i)
        expectColor(0x12345678u, 0x12345678u, i, 7, 0x12345678u, "同じ色は全 frame で一定");
    expectColor(0x00000000u, 0xFFFFFFFFu, std::int64_t{1} << 39, std::int64_t{1} << 40, 0x80808080u,
                "上限の frame 数");

    std::uint32_t untouched = 0x01020304u;
    check(!mvm::math::mathTransformColorAt(from, to, 0, 0, untouched) &&
              !mvm::math::mathTransformColorAt(from, to, -1, 30, untouched) &&
              !mvm::math::mathTransformColorAt(from, to, 31, 30, untouched) &&
              !mvm::math::mathTransformColorAt(from, to, 0, (std::int64_t{1} << 40) + 1, untouched),
          "frames <= 0・範囲外の frame・大きすぎる frames は失敗");
    check(untouched == 0x01020304u, "失敗では出力を変えない");
}

void testCoverageInspection() {
    using mvm::math::MathCoverage;
    using mvm::math::MathRect;
    // 6x4、alpha は (2,1) と (4,2) だけ。
    MathCoverage frame{6, 4, std::vector<std::uint8_t>(24, 0)};
    frame.alpha[1 * 6 + 2] = 10;
    frame.alpha[2 * 6 + 4] = 255;
    check(mvm::math::mathCoverageValid(frame), "6x4 で 24 byte は正しい");
    check(!mvm::math::mathCoverageValid({6, 4, std::vector<std::uint8_t>(23, 0)}) &&
              !mvm::math::mathCoverageValid({0, 4, {}}),
          "byte 数の違い・大きさ 0 は不正");
    check(mvm::math::mathCoverageBounds(frame) == MathRect{2, 1, 3, 2},
          "外接矩形は (2,1) から (4,2) まで");
    check(mvm::math::mathCoverageBounds({3, 3, std::vector<std::uint8_t>(9, 0)}).empty(),
          "alpha が無ければ空");
    check(mvm::math::mathRectUnion({2, 1, 3, 2}, {0, 3, 1, 1}) == MathRect{0, 1, 5, 3},
          "和は両方を含む最小の矩形");
    check(mvm::math::mathRectUnion({}, {4, 5, 1, 1}) == MathRect{4, 5, 1, 1} &&
              mvm::math::mathRectUnion({4, 5, 1, 1}, {}) == MathRect{4, 5, 1, 1} &&
              mvm::math::mathRectUnion({}, {}).empty(),
          "空の矩形は和で無視する");

    check(!mvm::math::mathRectTouchesEdge({1, 1, 4, 2}, 6, 4),
          "内側の 1 画素を空けた矩形は触れない");
    check(mvm::math::mathRectTouchesEdge({0, 1, 1, 1}, 6, 4), "左端に触れる");
    check(mvm::math::mathRectTouchesEdge({1, 0, 1, 1}, 6, 4), "上端に触れる");
    check(mvm::math::mathRectTouchesEdge({5, 1, 1, 1}, 6, 4), "右端 (x = 5) に触れる");
    check(mvm::math::mathRectTouchesEdge({1, 3, 1, 1}, 6, 4), "下端 (y = 3) に触れる");
    check(!mvm::math::mathRectTouchesEdge({}, 6, 4), "空の矩形は触れない");

    // mask 2x2 を (2,1) に置く。frame の (2,1)=10 と (3,2)=0 などを手で並べる。
    const MathCoverage mask{2, 2, {10, 0, 0, 0}};
    MathCoverage exact{6, 4, std::vector<std::uint8_t>(24, 0)};
    exact.alpha[1 * 6 + 2] = 10;
    check(mvm::math::mathEndpointDifference(exact, mask, 2, 1) == 0, "置いた mask と同じなら差 0");
    check(mvm::math::mathEndpointDifference(frame, mask, 2, 1) == 1,
          "mask の外の alpha (4,2) は 0 と比べて差 1");
    check(mvm::math::mathEndpointDifference(exact, mask, 3, 1) == 2,
          "1 画素ずらすと (2,1) と (3,1) が違う");
    check(mvm::math::mathEndpointDifference(exact, mask, 5, 1) == -1 &&
              mvm::math::mathEndpointDifference(exact, mask, -1, 0) == -1 &&
              mvm::math::mathEndpointDifference(exact, {2, 2, {1}}, 0, 0) == -1,
          "はみ出す・負の位置・不正な mask は -1");
}

// 変形の artifact を出力 raster に置く位置 (P2-5)。期待値は手で数えた値。
void testRasterPlacement() {
    using mvm::math::MathTransformRasterPlacement;
    MathTransformRasterPlacement placement;
    // 20x10 の出力。artifact 8x6 の中に source 5x3 が (1,2)、target 6x4 が (0,1)。
    // 静止の配置は source (7,3)・target (7,3)。左上は source (6,1)・target (7,2)。
    check(
        mvm::math::mathTransformRasterPlacement(8, 6, 1, 2, 5, 3, 0, 1, 6, 4, 20, 10, placement) &&
            placement == MathTransformRasterPlacement{6, 1, 7, 2},
        "端点の左上 = 静止の配置 - artifact の中の位置");

    // P2-2 の配置で描いた artifact の偶奇の組 (出力 1920x1080)。
    // source 7x2 (幅は奇数、高さは偶数)、target 4x5 (幅は偶数、高さは奇数)。canvas は大きい方 +
    // 各辺 6 で 19x17。canvas の中の source は ((19-7)/2, (17-2)/2) = (6,7)、target は
    // ((19-4)/2, (17-5)/2) = (7,6)。artifact は両者の和 (6,6)-(13,11) で 7x5、中の位置は
    // source (0,1)・target (1,0)。静止の配置は source ((1920-7)/2, (1080-2)/2) = (956,539)、
    // target ((1920-4)/2, (1080-5)/2) = (958,537)。左上は source (956,538)・target (957,537)。
    mvm::math::MathTransformPlacement canvas;
    check(mvm::math::mathTransformPlacement(7, 2, 4, 5, 19, 17, canvas) &&
              canvas.source.left == 6 && canvas.source.top == 7 && canvas.target.left == 7 &&
              canvas.target.top == 6,
          "偶奇の組: canvas の中の端点 (P2-2)");
    check(mvm::math::mathTransformRasterPlacement(7, 5, 0, 1, 7, 2, 1, 0, 4, 5, 1920, 1080,
                                                  placement) &&
              placement == MathTransformRasterPlacement{956, 538, 957, 537},
          "偶奇の違う端点の左上は 1 画素ずれる (横 +1、縦 -1)");

    // 途中の frame は線形に動かし、ちょうど半分は target の側へ丸める。4 枚:
    // 横 956 + i/4 → 956, 956.25→956, 956.5→957, 956.75→957
    // 縦 538 - i/4 → 538, 537.75→538, 537.5→537 (target の側), 537.25→537
    const int wantLeft[] = {956, 956, 957, 957};
    const int wantTop[] = {538, 538, 537, 537};
    for (int i = 0; i < 4; ++i) {
        int left = -1;
        int top = -1;
        const bool placed = mvm::math::mathTransformArtifactOriginAt(placement, i, 4, left, top);
        check(placed && left == wantLeft[i] && top == wantTop[i],
              "4 枚の frame " + std::to_string(i) + " の左上: " + std::to_string(left) + "," +
                  std::to_string(top));
    }
    int left = -1;
    int top = -1;
    check(mvm::math::mathTransformArtifactOriginAt(placement, 0, 1, left, top) && left == 956 &&
              top == 538,
          "1 枚なら source の位置だけ");
    // 2 枚の最後の frame は進み具合 1/2: 横 956.5→957、縦 537.5→537。どちらも target の位置。
    // (「0.5 は切り上げ」なら縦は 538 に残り、次の target の静止で 1 画素跳ぶ。)
    check(mvm::math::mathTransformArtifactOriginAt(placement, 1, 2, left, top) && left == 957 &&
              top == 537,
          "2 枚の最後の frame は target の位置 (縦に小さくなる向きでも)");

    int untouchedLeft = 12345;
    int untouchedTop = 12345;
    check(!mvm::math::mathTransformArtifactOriginAt(placement, 4, 4, untouchedLeft, untouchedTop) &&
              !mvm::math::mathTransformArtifactOriginAt(placement, -1, 4, untouchedLeft,
                                                        untouchedTop) &&
              !mvm::math::mathTransformArtifactOriginAt(placement, 0, 0, untouchedLeft,
                                                        untouchedTop) &&
              !mvm::math::mathTransformArtifactOriginAt(placement, 0, (std::int64_t{1} << 28) + 1,
                                                        untouchedLeft, untouchedTop) &&
              untouchedLeft == 12345 && untouchedTop == 12345,
          "範囲外の frame (終状態の frame == frames を含む)・frames <= 0・大きすぎる frames は"
          "失敗し、出力を変えない");

    MathTransformRasterPlacement failed{1, 2, 3, 4};
    check(
        !mvm::math::mathTransformRasterPlacement(8, 6, 4, 2, 5, 3, 0, 1, 6, 4, 20, 10, failed) &&
            !mvm::math::mathTransformRasterPlacement(8, 6, 1, 2, 5, 3, 0, 3, 6, 4, 20, 10, failed),
        "端点の静止が artifact からはみ出せば失敗");
    // 20 幅の出力に 20 幅の artifact: source の左上は (20-5)/2 - 1 = 6 で、右端が 26 > 20。
    check(!mvm::math::mathTransformRasterPlacement(20, 6, 1, 2, 5, 3, 0, 1, 6, 4, 20, 10, failed),
          "artifact が出力からはみ出せば失敗 (黙って切らない)");
    check(!mvm::math::mathTransformRasterPlacement(8, 6, 1, 2, 5, 3, 0, 1, 6, 4, 4, 10, failed),
          "静止が出力より大きければ失敗");
    check(failed == MathTransformRasterPlacement{1, 2, 3, 4}, "失敗では出力を変えない");
}

// P2-2 の配置で描いた artifact では、偶奇の 16 通りと出力の偶奇で、端点の静止が静止の配置に
// 重なり、端点の左上の差は各軸 1 画素以内で、最後の frame (2 枚以上) は target の位置になる。
void testRasterPlacementParity() {
    int combinations = 0;
    for (const int outputWidth : {1920, 1921})
        for (const int outputHeight : {1080, 1081})
            for (int bits = 0; bits < 16; ++bits) {
                const int sw = 40 + (bits & 1);
                const int sh = 20 + ((bits >> 1) & 1);
                const int tw = 30 + ((bits >> 2) & 1);
                const int th = 25 + ((bits >> 3) & 1);
                const int cw = std::max(sw, tw) + 12;
                const int ch = std::max(sh, th) + 12;
                mvm::math::MathTransformPlacement canvas;
                if (!mvm::math::mathTransformPlacement(sw, sh, tw, th, cw, ch, canvas)) {
                    check(false, "偶奇: canvas の配置");
                    continue;
                }
                const mvm::math::MathRect artifact =
                    mvm::math::mathRectUnion({canvas.source.left, canvas.source.top, sw, sh},
                                             {canvas.target.left, canvas.target.top, tw, th});
                mvm::math::MathTransformRasterPlacement placement;
                if (!mvm::math::mathTransformRasterPlacement(
                        artifact.width, artifact.height, canvas.source.left - artifact.x,
                        canvas.source.top - artifact.y, sw, sh, canvas.target.left - artifact.x,
                        canvas.target.top - artifact.y, tw, th, outputWidth, outputHeight,
                        placement)) {
                    check(false, "偶奇: 出力への配置");
                    continue;
                }
                // 静止の配置 (中央、余りは左上) を手で数える。
                const int sourceLeft = (outputWidth - sw) / 2;
                const int targetLeft = (outputWidth - tw) / 2;
                const int sourceTop = (outputHeight - sh) / 2;
                const int targetTop = (outputHeight - th) / 2;
                const std::string what = "偶奇 " + std::to_string(sw) + "x" + std::to_string(sh) +
                                         "→" + std::to_string(tw) + "x" + std::to_string(th) +
                                         " 出力 " + std::to_string(outputWidth) + "x" +
                                         std::to_string(outputHeight);
                check(placement.sourceLeft + canvas.source.left - artifact.x == sourceLeft &&
                          placement.sourceTop + canvas.source.top - artifact.y == sourceTop &&
                          placement.targetLeft + canvas.target.left - artifact.x == targetLeft &&
                          placement.targetTop + canvas.target.top - artifact.y == targetTop,
                      what + ": 端点の静止が静止の配置に重なる");
                check(std::abs(placement.sourceLeft - placement.targetLeft) <= 1 &&
                          std::abs(placement.sourceTop - placement.targetTop) <= 1,
                      what + ": 端点の左上の差は 1 画素以内");
                for (const std::int64_t frames : {2, 3, 9}) {
                    int left = 0;
                    int top = 0;
                    check(
                        mvm::math::mathTransformArtifactOriginAt(placement, 0, frames, left, top) &&
                            left == placement.sourceLeft && top == placement.sourceTop,
                        what + ": frame 0 は source の位置");
                    check(mvm::math::mathTransformArtifactOriginAt(placement, frames - 1, frames,
                                                                   left, top) &&
                              left == placement.targetLeft && top == placement.targetTop,
                          what + ": " + std::to_string(frames) +
                              " 枚の最後の frame は target の位置");
                }
                ++combinations;
            }
    check(combinations == 64, "偶奇の 64 通りをすべて確かめた");
}

// composeMathPatchAt は composeMathPatch と同じ画素を、広い画像の中の位置へ書く。
void testComposePatchAt() {
    const std::uint8_t coverage[] = {255, 128, 0, 64};
    mvm::math::MathComposeStyle style;
    style.colorArgb = 0xFF336699u;
    std::vector<std::uint8_t> direct(2 * 2 * 4, 0xEE);
    mvm::math::composeMathPatch(coverage, 2, 2, style, direct.data());
    std::vector<std::uint8_t> wide(4 * 3 * 4, 0xAB);
    mvm::math::composeMathPatchAt(coverage, 2, 2, style, wide.data(), 4, 1, 1);
    bool same = true;
    bool outsideUntouched = true;
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 4; ++x)
            for (int c = 0; c < 4; ++c) {
                const std::uint8_t got =
                    wide[(static_cast<std::size_t>(y) * 4 + static_cast<std::size_t>(x)) * 4 +
                         static_cast<std::size_t>(c)];
                if (x >= 1 && x < 3 && y >= 1 && y < 3)
                    same = same && got == direct[(static_cast<std::size_t>(y - 1) * 2 +
                                                  static_cast<std::size_t>(x - 1)) *
                                                     4 +
                                                 static_cast<std::size_t>(c)];
                else
                    outsideUntouched = outsideUntouched && got == 0xAB;
            }
    check(same, "位置を指定した合成は composeMathPatch と同じ画素");
    check(outsideUntouched, "位置を指定した合成は矩形の外を書かない");
    // 被覆 255 の画素は文字色そのもの、0 は透明。
    check(direct[0] == 0x33 && direct[1] == 0x66 && direct[2] == 0x99 && direct[3] == 255 &&
              direct[2 * 4 + 3] == 0,
          "被覆 255 は文字色、0 は透明");
}

} // namespace

int main() {
    testGoldenSegments();
    testSegmentRules();
    testSegmentDeterminism();
    testMatching();
    testTransformKey();
    testEndpointPlacement();
    testTransformPlacement();
    testColorInterpolation();
    testCoverageInspection();
    testRasterPlacement();
    testRasterPlacementParity();
    testComposePatchAt();
    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}
