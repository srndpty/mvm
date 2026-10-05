// 式から式への変形 (P2-2) の backend 中立な契約: TeX の式の分け方、n 番目の出現の照合、
// 変形の cache key、端点の配置 (半画素の補正)、文字色の補間。
//
// 期待値は実装から作らない。分け方・照合・配置・色は手で数えた値。key の golden 値は、
// header に書いた正準形を `printf ... | sha256sum` で別に計算したもの。

#include "media/math/math_raster_layout.h"
#include "media/math/math_tex_segments.h"
#include "media/math/math_transform.h"

#include <cstdint>
#include <cstdio>
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
    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}
