// Equation Sequence (P3-3) の backend 中立な描画契約: 値の検査、cache key、frame の進み具合。
// 期待値は手で計算した値 (実装の式を共有しない)。

#include "media/math/equation_sequence_render.h"
#include "media/math/math_raster_layout.h"
#include "media/math/math_render.h"
#include "media/math/math_transform.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace {

namespace math = mvm::math;

int checks = 0;
int failures = 0;

void check(bool condition, const std::string& message) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

math::EquationSequenceRenderSpec base() {
    math::EquationSequenceRenderSpec spec;
    spec.compilerVersion = "equation-neutral/1";
    math::EquationRenderState a;
    a.still = {"latex", "a+b", 72};
    a.holdFrames = 10;
    a.segments = {{math::EquationRenderSegmentKind::Semantic, "a"},
                  {math::EquationRenderSegmentKind::Auto, "+"},
                  {math::EquationRenderSegmentKind::Semantic, "b"}};
    math::EquationRenderState b = a;
    b.still.source = "b+a";
    b.segments = {{math::EquationRenderSegmentKind::Semantic, "b"},
                  {math::EquationRenderSegmentKind::Auto, "+"},
                  {math::EquationRenderSegmentKind::Semantic, "a"}};
    spec.states = {a, b};
    math::EquationRenderTransition t;
    t.fromState = 0;
    t.toState = 1;
    t.frames = 6;
    t.matching.pairs = {{0, 2}, {1, 1}, {2, 0}};
    spec.transitions = {t};
    spec.actions = {{0, 0, 2, 3, math::EquationRenderOperation::Outline},
                    {1, 2, 5, 4, math::EquationRenderOperation::Pulse}};
    return spec;
}

void testProgress() {
    std::int64_t n = 0;
    std::int64_t d = 0;
    // transition: i / N。
    check(math::equationTransitionProgress(0, 4, n, d) && n == 0 && d == 4, "変形 frame 0 は 0/4");
    check(math::equationTransitionProgress(3, 4, n, d) && n == 3 && d == 4,
          "変形の最後に表示する frame は 3/4 (4/4 は連番に含めない)");
    check(!math::equationTransitionProgress(4, 4, n, d), "変形の frame N は範囲外");
    check(math::equationTransitionProgress(0, 1, n, d) && n == 0 && d == 1, "N=1 は 0/1 の 1 枚");
    // outline: (2i+1)/(2N)。
    check(math::equationOutlineProgress(0, 1, n, d) && n == 1 && d == 2, "outline N=1 は中央 1/2");
    check(math::equationOutlineProgress(0, 4, n, d) && n == 1 && d == 8, "outline frame 0 は 1/8");
    check(math::equationOutlineProgress(3, 4, n, d) && n == 7 && d == 8,
          "outline の最後に表示する frame は 7/8");
    check(math::equationOutlineProgress(1, 3, n, d) && n == 3 && d == 6,
          "outline N=3 の中央は 3/6 (N=1 の唯一の frame と同じ進み具合)");
    // pulse: (N - |2i+1-N|)/N。手計算: N=4 → 1/4, 3/4, 3/4, 1/4。N=3 → 1/3, 3/3, 1/3。
    const std::int64_t four[] = {1, 3, 3, 1};
    for (int i = 0; i < 4; ++i)
        check(math::equationPulseWeight(i, 4, n, d) && n == four[i] && d == 4,
              "pulse N=4 の重み frame " + std::to_string(i));
    const std::int64_t three[] = {1, 3, 1};
    for (int i = 0; i < 3; ++i)
        check(math::equationPulseWeight(i, 3, n, d) && n == three[i] && d == 3,
              "pulse N=3 の重み frame " + std::to_string(i));
    check(math::equationPulseWeight(0, 1, n, d) && n == 1 && d == 1, "pulse N=1 は重み 1 (頂点)");
    for (std::int64_t frames = 1; frames <= 9; ++frames)
        for (std::int64_t i = 0; i < frames; ++i) {
            math::equationPulseWeight(i, frames, n, d);
            check(n > 0 && n <= d, "pulse の重みは区間の全 frame で (0, 1]");
        }
    check(!math::equationOutlineProgress(-1, 3, n, d) && !math::equationPulseWeight(0, 0, n, d),
          "負の frame・N=0 は拒否");
}

void testValidation() {
    std::string error;
    check(math::validateEquationSequenceRenderSpec(base(), error), "基準の spec は有効: " + error);
    const std::vector<
        std::pair<std::string, std::function<void(math::EquationSequenceRenderSpec&)>>>
        cases = {
            {"状態が無い",
             [](auto& s) {
                 s.states.clear();
                 s.transitions.clear();
                 s.actions.clear();
             }},
            {"transition の数", [](auto& s) { s.transitions.clear(); }},
            {"隣接でない", [](auto& s) { s.transitions[0].toState = 0; }},
            {"変形の尺 0", [](auto& s) { s.transitions[0].frames = 0; }},
            {"不透明な背景", [](auto& s) { s.states[0].backgroundArgb = 0x01000000u; }},
            {"連結が式と違う", [](auto& s) { s.states[0].segments[1].text = "-"; }},
            {"空の segment", [](auto& s) { s.states[0].segments.push_back({}); }},
            {"handle を二重に使う",
             [](auto& s) { s.transitions[0].matching.unmatchedSource = {0}; }},
            {"handle を使わない", [](auto& s) { s.transitions[0].matching.pairs.pop_back(); }},
            {"対が source 順でない",
             [](auto& s) {
                 std::swap(s.transitions[0].matching.pairs[0], s.transitions[0].matching.pairs[1]);
             }},
            {"範囲外の handle", [](auto& s) { s.transitions[0].matching.pairs[2].target = 7; }},
            {"action の segment が無い", [](auto& s) { s.actions[0].segment = 3; }},
            {"action が hold の外", [](auto& s) { s.actions[1].start = 7; }},
            {"action の尺 0", [](auto& s) { s.actions[0].duration = 0; }},
            {"action が重なる",
             [](auto& s) { s.actions[1] = {0, 1, 3, 2, math::EquationRenderOperation::Pulse}; }},
            {"action が状態の順でない", [](auto& s) { std::swap(s.actions[0], s.actions[1]); }},
            {"syntax", [](auto& s) { s.states[1].still.syntax = "typst"; }},
            {"compiler の版が空", [](auto& s) { s.compilerVersion.clear(); }},
    };
    for (const auto& [name, mutate] : cases) {
        auto spec = base();
        mutate(spec);
        check(!math::validateEquationSequenceRenderSpec(spec, error), "拒否: " + name);
    }
    auto touching = base();
    touching.actions = {{0, 0, 0, 2, math::EquationRenderOperation::Outline},
                        {0, 2, 2, 8, math::EquationRenderOperation::Pulse}};
    check(math::validateEquationSequenceRenderSpec(touching, error),
          "接する (重ならない) 区間は有効: " + error);
}

void testKey() {
    const math::MathToolchainFingerprint toolchain{"manim-mathtex", "manim=1\n"};
    const std::string tmpl = "manim-equation-sequence/1";
    const auto key = math::equationSequenceRenderKey(base(), toolchain, tmpl);
    check(key.size() == 64, "key は 16 進 64 桁");
    check(key == math::equationSequenceRenderKey(base(), toolchain, tmpl), "同じ入力は同じ key");
    // 静止・Write・P2 変形とは別の名前空間 (同じ式でも一致しない)。
    const auto still = base().states[0].still;
    check(key != math::mathRenderKey(still, toolchain), "静止の key と別");
    check(key != math::mathSequenceKey({still, math::MathAnimationKind::Write, 6}, toolchain, tmpl),
          "Write の key と別");
    check(key != math::mathTransformKey({still, base().states[1].still, 6}, toolchain, tmpl),
          "変形の key と別");
    check(std::string(math::kEquationSequenceKeyVersion) == "mvm-equation-sequence/1",
          "名前空間の版");

    const std::vector<
        std::pair<std::string, std::function<void(math::EquationSequenceRenderSpec&)>>>
        changes = {
            {"source",
             [](auto& s) {
                 s.states[0].still.source = "a+c";
                 s.states[0].segments[2].text = "c";
             }},
            {"font", [](auto& s) { s.states[0].still.fontSize = 73; }},
            {"色", [](auto& s) { s.states[1].foregroundArgb = 0xFF00FF00u; }},
            {"hold", [](auto& s) { s.states[0].holdFrames = 11; }},
            {"segment の種類",
             [](auto& s) { s.states[0].segments[0].kind = math::EquationRenderSegmentKind::Auto; }},
            {"分け方 (同じ連結)",
             [](auto& s) {
                 s.states[0].segments = {{math::EquationRenderSegmentKind::Auto, "a+"},
                                         {math::EquationRenderSegmentKind::Semantic, "b"}};
                 s.transitions[0].matching.pairs = {{0, 2}, {1, 0}};
                 s.transitions[0].matching.unmatchedTarget = {1};
                 s.actions[0].segment = 1;
             }},
            {"segmenter の版", [](auto& s) { s.states[0].segmenterVersion = "x/2"; }},
            {"変形の尺", [](auto& s) { s.transitions[0].frames = 7; }},
            {"対",
             [](auto& s) {
                 s.transitions[0].matching.pairs = {{1, 1}};
                 s.transitions[0].matching.unmatchedSource = {0, 2};
                 s.transitions[0].matching.unmatchedTarget = {0, 2};
             }},
            {"matcher の版", [](auto& s) { s.transitions[0].matcherVersion = "m/2"; }},
            {"operation",
             [](auto& s) { s.actions[0].operation = math::EquationRenderOperation::Pulse; }},
            {"action の尺", [](auto& s) { s.actions[0].duration = 4; }},
            {"action の start", [](auto& s) { s.actions[0].start = 1; }},
            {"action の対象", [](auto& s) { s.actions[0].segment = 2; }},
            {"compiler の版", [](auto& s) { s.compilerVersion = "equation-neutral/2"; }},
    };
    std::set<std::string> keys{key};
    for (const auto& [name, mutate] : changes) {
        auto spec = base();
        mutate(spec);
        std::string error;
        check(math::validateEquationSequenceRenderSpec(spec, error),
              "変更後も有効: " + name + " " + error);
        const auto changed = math::equationSequenceRenderKey(spec, toolchain, tmpl);
        check(changed != key, "key に入る: " + name);
        keys.insert(changed);
    }
    check(keys.size() == changes.size() + 1, "変更ごとに別の key");
    check(math::equationSequenceRenderKey(base(), {"manim-mathtex", "manim=2\n"}, tmpl) != key,
          "toolchain が key に入る");
    check(math::equationSequenceRenderKey(base(), toolchain, "manim-equation-sequence/2") != key,
          "template が key に入る");
    check(math::equationSequenceRenderKey(base(), {"other", "manim=1\n"}, tmpl) != key,
          "backend が key に入る");
}

// 層の合成 (P3-3.1)。期待値は out = over + round(under * (255 - over) / 255) の手計算。
void testComposition() {
    struct Case {
        int under, over, expected;
    };

    // 128 + round(128*127/255 = 63.75) = 192、200 + round(64*55/255 = 13.80) = 214、
    // 10 + round(100*245/255 = 96.08) = 106、0 + 37 = 37、254 + round(1/255) = 254。
    const Case cases[] = {{0, 0, 0},      {255, 0, 255},  {0, 77, 77}, {128, 128, 192},
                          {64, 200, 214}, {100, 10, 106}, {37, 0, 37}, {255, 255, 255},
                          {1, 254, 254},  {255, 1, 255}};
    for (const auto& c : cases)
        check(math::equationCoverageOver(static_cast<std::uint8_t>(c.under),
                                         static_cast<std::uint8_t>(c.over)) == c.expected,
              "合成の手計算 under=" + std::to_string(c.under) + " over=" + std::to_string(c.over));
    // alpha だけの source-over は順序によらない (整数の丸めでも)。全 65536 組で確かめる。
    bool commutative = true;
    bool bounded = true;
    for (int a = 0; a < 256; ++a)
        for (int b = 0; b < 256; ++b) {
            const auto ab = math::equationCoverageOver(static_cast<std::uint8_t>(a),
                                                       static_cast<std::uint8_t>(b));
            commutative =
                commutative && ab == math::equationCoverageOver(static_cast<std::uint8_t>(b),
                                                                static_cast<std::uint8_t>(a));
            bounded = bounded && ab >= std::max(a, b);
        }
    check(commutative, "合成は順序によらない");
    check(bounded, "合成は両方の層以上 (被覆を失わない)");
    // 重ならない 2 層の合成は、そのまま両方の画素になる (pulse の静止の分解の典型)。
    math::MathCoverage out;
    check(math::composeEquationCoverage({3, 1, {200, 0, 0}}, {3, 1, {0, 64, 0}}, out) &&
              out.alpha == std::vector<std::uint8_t>{200, 64, 0},
          "重ならない層の合成");
    check(!math::composeEquationCoverage({3, 1, {0, 0, 0}}, {2, 1, {0, 0}}, out),
          "大きさの違う層は合成しない");
    check(!math::composeEquationCoverage({3, 1, {0, 0}}, {3, 1, {0, 0, 0}}, out),
          "byte 数の合わない層は合成しない");
}

// 色付きの層の合成 (P3-4)。期待値は手計算。
std::vector<int> layerPixel(int base, std::uint32_t baseArgb, int accent,
                            std::uint32_t accentArgb) {
    std::uint8_t out[4] = {1, 2, 3, 4};
    math::composeEquationLayerPixel(static_cast<std::uint8_t>(base), baseArgb,
                                    static_cast<std::uint8_t>(accent), accentArgb, out);
    return {out[0], out[1], out[2], out[3]};
}

void testLayerComposition() {
    // base だけ: 色はそのまま、alpha は被覆。
    check(layerPixel(255, 0xFF4080C0u, 0, 0xFFFFFF00u) == std::vector<int>{0x40, 0x80, 0xC0, 255},
          "base だけの画素は base の色");
    check(layerPixel(255, 0xFF4080C0u, 255, 0xFFFFFF00u) == std::vector<int>{255, 255, 0, 255},
          "不透明な accent は base を隠す");
    // 白い base (255) の上に黄色の accent (128): b = 128*255 = 32640。
    // 青 = 255 * 65025 * (65025 - 32640) / 65025^2 = 255 * 32385 / 65025 = 127 (割り切れる)。
    // alpha = 128 + round(255 * 127 / 255) = 255。
    check(layerPixel(255, 0xFFFFFFFFu, 128, 0xFFFFFF00u) == std::vector<int>{255, 255, 127, 255},
          "白の上の半分の黄色");
    // 青 (被覆 100) の上に赤 (被覆 50): a = 25500、b = 12750。
    // 赤 = 255 * 12750*65025 / (12750*65025 + 25500*52275) = 97.78 -> 98、
    // 青 = 255 * 25500*52275 / 同 = 157.22 -> 157、alpha = 50 + (100*205 + 127) / 255 = 130。
    check(layerPixel(100, 0xFF0000FFu, 50, 0xFFFF0000u) == std::vector<int>{98, 0, 157, 130},
          "半透明の 2 層の手計算");
    // 色の alpha を被覆に掛ける: round(255 * 128 / 255) = 128。
    check(layerPixel(255, 0x80FFFFFFu, 0, 0) == std::vector<int>{255, 255, 255, 128},
          "色の alpha を掛ける");
    check(layerPixel(0, 0xFFFFFFFFu, 0, 0xFFFFFF00u) == std::vector<int>{0, 0, 0, 0},
          "何も無い画素は透明の 0");

    // 不透明な色どうしの alpha は composeEquationCoverage (P3-3.1) と全 65536 組で同じ。
    bool sameAlpha = true;
    for (int a = 0; a < 256; ++a)
        for (int b = 0; b < 256; ++b)
            sameAlpha = sameAlpha && layerPixel(a, 0xFF336699u, b, 0xFFFFFF00u)[3] ==
                                         math::equationCoverageOver(static_cast<std::uint8_t>(a),
                                                                    static_cast<std::uint8_t>(b));
    check(sameAlpha, "不透明な 2 層の alpha は層の被覆の合成規則と同じ");
    // accent の無い画素は単層の composeMathPatch (静止・変形と同じ式) と byte 単位で同じ。
    bool sameAsSingle = true;
    for (const std::uint32_t color : {0xFFFFFFFFu, 0xFF40C0FFu, 0x80FF2010u, 0x01ABCDEFu})
        for (int c = 0; c < 256; ++c) {
            std::uint8_t single[4] = {};
            const auto coverage = static_cast<std::uint8_t>(c);
            math::composeMathPatch(&coverage, 1, 1, {color, 0}, single);
            const auto pair = layerPixel(c, color, 0, 0xFFFFFF00u);
            sameAsSingle = sameAsSingle &&
                           pair == std::vector<int>{single[0], single[1], single[2], single[3]};
        }
    check(sameAsSingle, "accent の無い画素は単層の静止の合成と同じ");

    // 矩形への書き込み: 2x1 の層を幅 4 の画像の (1, 0) に置く。外は触らない。
    std::vector<std::uint8_t> image(4 * 4, 9);
    const std::uint8_t base[] = {255, 0};
    const std::uint8_t accent[] = {0, 255};
    math::composeEquationLayersAt(base, 0xFF102030u, accent, 0xFFFFFF00u, 2, 1, image.data(), 4, 1,
                                  0);
    check(std::vector<std::uint8_t>(image.begin(), image.begin() + 4) ==
                  std::vector<std::uint8_t>{9, 9, 9, 9} &&
              std::vector<std::uint8_t>(image.begin() + 4, image.begin() + 8) ==
                  std::vector<std::uint8_t>{0x10, 0x20, 0x30, 255} &&
              std::vector<std::uint8_t>(image.begin() + 8, image.begin() + 12) ==
                  std::vector<std::uint8_t>{255, 255, 0, 255} &&
              std::vector<std::uint8_t>(image.begin() + 12, image.end()) ==
                  std::vector<std::uint8_t>{9, 9, 9, 9},
          "2 層を矩形の位置へ書き、外は変えない");
}

void testNames() {
    check(std::string(math::equationRenderOperationName(math::EquationRenderOperation::Outline)) ==
                  "outline" &&
              std::string(math::equationRenderOperationName(
                  math::EquationRenderOperation::Pulse)) == "pulse",
          "operation の名前");
    std::set<std::string> names;
    for (int f = 0; f <= static_cast<int>(math::EquationBackendFailure::PulseBaseMismatch); ++f)
        names.insert(
            math::equationBackendFailureName(static_cast<math::EquationBackendFailure>(f)));
    check(names.size() ==
                  static_cast<std::size_t>(
                      static_cast<int>(math::EquationBackendFailure::PulseBaseMismatch) + 1) &&
              !names.count("unknown"),
          "失敗理由の名前は全て別");
    math::EquationBackendValidation validation;
    check(!validation.ready() && validation.failure == math::EquationBackendFailure::NotValidated,
          "既定の検証は未実行で ready ではない");
}

} // namespace

int main() {
    testProgress();
    testValidation();
    testKey();
    testComposition();
    testLayerComposition();
    testNames();
    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}
