// Equation Sequence の product preview (P3-4) の純粋な部分: output frame → 区間 (P3-1 の写像)、
// 代用の規則、出力 raster への配置、provenance の色での合成。Qt・cache を使わない。
// 期待値は手で決めた値 (実装の式を呼ばない)。

#include "app/equation_sequence_compile.h"
#include "app/equation_sequence_preview.h"
#include "project/equation_sequence.h"

#include <cstdio>
#include <memory>
#include <set>
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

void part(EquationState& s, std::string id, const std::string& text) {
    const auto at = s.equation.source.find(text);
    s.parts.push_back({{std::move(id)},
                       "label",
                       {s.revision, static_cast<std::int64_t>(at),
                        static_cast<std::int64_t>(at + text.size()), text, BindingStatus::Bound}});
}

EquationState state(const std::string& id, const std::string& source, std::int64_t hold,
                    const std::string& color) {
    EquationState s;
    s.id = {id};
    s.revision = "rev-" + id;
    s.equation.source = source;
    s.equation.color = color;
    s.holdFrames = hold;
    return s;
}

EquationAction action(const std::string& id, const std::string& owner, const std::string& target,
                      std::int64_t start, std::int64_t duration, EquationOperation operation) {
    EquationAction a;
    a.id = {id};
    a.state = {owner};
    a.target = {target};
    a.start = start;
    a.duration = duration;
    a.operation = operation;
    return a;
}

// source frame の区間 (手で数えた値):
//   S0 hold 10  [0,10)   outline [2,5)、pulse [6,9)
//   T0 4 枚     [10,14)
//   S1 hold 6   [14,20)  pulse N=1 [16,17)
//   T1 1 枚     [20,21)
//   S2 hold 3   [21,24)  L = 24
EquationSequenceClipData sequence() {
    auto s0 = state("s0", "x=b^2-4ac", 10, "#FF102030");
    part(s0, "d0", "b^2-4ac");
    auto s1 = state("s1", "x+b^2-4ac", 6, "#FF405060");
    part(s1, "d1", "b^2-4ac");
    auto s2 = state("s2", "y=b^2-4ac", 3, "#FF708090");
    EquationSequenceClipData d;
    d.states = {s0, s1, s2};
    EquationStepTransition t0;
    t0.id = {"t0"};
    t0.from = s0.id;
    t0.to = s1.id;
    t0.frames = 4;
    t0.correspondence = {{{"d0"}, {"d1"}}};
    EquationStepTransition t1;
    t1.id = {"t1"};
    t1.from = s1.id;
    t1.to = s2.id;
    t1.frames = 1;
    d.transitions = {t0, t1};
    d.actions = {action("pulse", "s0", "d0", 6, 3, EquationOperation::Pulse),
                 action("outline", "s0", "d0", 2, 3, EquationOperation::Outline),
                 action("pulse1", "s1", "d1", 2, 1, EquationOperation::Pulse)};
    return d;
}

TimelineClip clip(std::int64_t start = 100, std::int64_t in = 0, std::int64_t out = 24,
                  std::int64_t fps = 60) {
    TimelineClip c;
    c.id = "seq";
    c.kind = TimelineClipKind::EquationSequence;
    c.timelineStartFrame = start;
    c.sourceFpsNum = fps;
    c.sourceFpsDen = 1;
    c.sourceFrameCount = 24;
    c.sourceInFrame = in;
    c.sourceOutFrame = out;
    c.equationSequence = sequence();
    return c;
}

EquationSequenceSpec compiled() {
    const auto result = compileEquationSequence(sequence());
    check(result.value.has_value(), "試験の sequence を compile できる");
    return result.value.value_or(EquationSequenceSpec{});
}

std::optional<EquationFrameLookup> lookupAt(const TimelineClip& c, const EquationSequenceSpec* spec,
                                            std::int64_t output, std::int64_t fps = 60) {
    std::string error;
    const auto time = equationPreviewTimeAt(c, fps, 1, spec, output, error);
    if (!time)
        return std::nullopt;
    return time->lookup;
}

bool isLookup(const std::optional<EquationFrameLookup>& got, EquationFrameKind kind,
              std::size_t state, std::size_t index, std::int64_t frame, std::int64_t frames) {
    if (!got || got->kind != kind || got->state != state)
        return false;
    switch (kind) {
    case EquationFrameKind::Hold:
        return true;
    case EquationFrameKind::HoldAction:
        return got->action == index && got->frame == frame && got->frames == frames;
    case EquationFrameKind::Transition:
        return got->transition == index && got->frame == frame && got->frames == frames;
    }
    return false;
}

void timeMapping() {
    const auto spec = compiled();
    const auto c = clip();
    using K = EquationFrameKind;
    // spec の action の番号は状態・start の順: 0 = outline (2)、1 = pulse (6)、2 = pulse N=1。
    check(isLookup(lookupAt(c, &spec, 100), K::Hold, 0, 0, 0, 0), "clip の先頭は H0");
    check(isLookup(lookupAt(c, &spec, 101), K::Hold, 0, 0, 0, 0), "action の前の hold");
    check(isLookup(lookupAt(c, &spec, 102), K::HoldAction, 0, 0, 0, 3), "outline の frame 0");
    check(isLookup(lookupAt(c, &spec, 103), K::HoldAction, 0, 0, 1, 3), "outline の中央");
    check(isLookup(lookupAt(c, &spec, 104), K::HoldAction, 0, 0, 2, 3), "outline の最後の frame");
    check(isLookup(lookupAt(c, &spec, 105), K::Hold, 0, 0, 0, 0), "outline の直後は静止");
    check(isLookup(lookupAt(c, &spec, 107), K::HoldAction, 0, 1, 1, 3), "pulse の中央");
    check(isLookup(lookupAt(c, &spec, 109), K::Hold, 0, 0, 0, 0), "hold の最後の frame");
    check(isLookup(lookupAt(c, &spec, 110), K::Transition, 0, 0, 0, 4), "変形の frame 0");
    check(isLookup(lookupAt(c, &spec, 113), K::Transition, 0, 0, 3, 4),
          "変形の最後に見せる frame は N-1 (進み具合 3/4)");
    check(isLookup(lookupAt(c, &spec, 114), K::Hold, 1, 0, 0, 0), "変形の直後は target の hold 0");
    check(isLookup(lookupAt(c, &spec, 116), K::HoldAction, 1, 2, 0, 1), "N=1 の action");
    check(isLookup(lookupAt(c, &spec, 117), K::Hold, 1, 0, 0, 0), "N=1 の action の直後は静止");
    check(isLookup(lookupAt(c, &spec, 120), K::Transition, 1, 1, 0, 1), "N=1 の変形");
    check(isLookup(lookupAt(c, &spec, 121), K::Hold, 2, 0, 0, 0), "N=1 の変形の直後は H2");
    check(!lookupAt(c, &spec, 99) && !lookupAt(c, &spec, 124), "clip の外は区間を持たない");

    // 左 trim は可視範囲だけを変える (区間を最初からやり直さない)。
    check(isLookup(lookupAt(clip(100, 1), &spec, 100), K::Hold, 0, 0, 0, 0),
          "hold の途中から始まる clip");
    check(isLookup(lookupAt(clip(100, 3), &spec, 100), K::HoldAction, 0, 0, 1, 3),
          "action の途中から始まる clip は action の frame 1 から");
    check(isLookup(lookupAt(clip(100, 12), &spec, 100), K::Transition, 0, 0, 2, 4),
          "変形の途中から始まる clip は変形の frame 2 から");

    // 逆向き・飛び飛びの seek でも各 frame の結果は同じ (履歴を持たない)。
    const std::vector<std::int64_t> order = {113, 102, 110, 100, 116, 103, 113, 121, 104, 102};
    bool same = true;
    for (const auto frame : order) {
        const auto again = lookupAt(c, &spec, frame);
        const auto fresh = lookupAt(clip(), &spec, frame);
        same = same && again && fresh && *again == *fresh;
    }
    check(same, "逆向き・飛び飛びの seek でも同じ区間と frame");

    // 24 → 60: 先頭の output の内部 sample は 0,0,0,1,1,2,2,2 (P3-1 の手計算)。
    auto slow = clip(0, 0, 24, 24);
    std::string error;
    std::vector<std::int64_t> samples;
    for (std::int64_t k = 0; k < 8; ++k) {
        const auto time = equationPreviewTimeAt(slow, 60, 1, &spec, k, error);
        samples.push_back(time ? time->sourceFrame : -1);
    }
    check(samples == std::vector<std::int64_t>{0, 0, 0, 1, 1, 2, 2, 2},
          "24 → 60 の標本は P3-1 の写像と同じ");

    // compile できない sequence も P3-1 の評価で状態と変形を決める (action は付けない)。
    check(isLookup(lookupAt(c, nullptr, 103), K::Hold, 0, 0, 0, 0),
          "spec の無い action の区間は状態の静止");
    check(isLookup(lookupAt(c, nullptr, 111), K::Transition, 0, 0, 1, 4),
          "spec の無い変形の区間も変形として決まる");
}

struct Availability final : EquationPreviewAvailability {
    std::vector<bool> statics = {true, true, true};
    std::set<std::pair<std::size_t, std::int64_t>> transitions;
    std::set<std::pair<std::size_t, std::int64_t>> actions;

    bool staticReady(std::size_t s) const override { return s < statics.size() && statics[s]; }

    bool transitionFrameReady(std::size_t t, std::int64_t f) const override {
        return transitions.contains({t, f});
    }

    bool actionFrameReady(std::size_t a, std::int64_t f) const override {
        return actions.contains({a, f});
    }
};

void fallbackRules() {
    const auto spec = compiled();
    const auto c = clip();
    using S = EquationPreviewShownKind;
    const auto shown = [&](const Availability& available, std::int64_t frame) {
        const auto lookup = lookupAt(c, &spec, frame);
        return lookup ? resolveEquationPreview(*lookup, available) : EquationPreviewShown{};
    };
    Availability none;
    none.statics = {false, false, false};
    check(shown(none, 100).kind == S::None, "静止が無ければ何も見せない (前の式で代用しない)");
    Availability statics;
    check((shown(statics, 100) == EquationPreviewShown{S::Static, 0, 0}), "hold は静止");
    check((shown(statics, 103) == EquationPreviewShown{S::Static, 0, 0}),
          "action が揃わない間は状態の静止");
    // 変形が無い間は区間の全 frame で前の状態の静止、target の hold 0 で後の状態へ。
    bool source = true;
    for (std::int64_t f = 110; f < 114; ++f)
        source = source && shown(statics, f) == EquationPreviewShown{S::Static, 0, 0};
    check(source, "変形を省いた代用は区間の全 frame で前の状態の静止 (途中で切り替えない)");
    check((shown(statics, 114) == EquationPreviewShown{S::Static, 1, 0}),
          "後の状態の静止へは target の hold の frame 0 で切り替わる");
    check((shown(statics, 120) == EquationPreviewShown{S::Static, 1, 0}) &&
              (shown(statics, 121) == EquationPreviewShown{S::Static, 2, 0}),
          "N=1 の変形の代用も同じ");
    Availability ready = statics;
    ready.transitions = {{0, 0}, {0, 3}};
    ready.actions = {{0, 1}};
    check((shown(ready, 110) == EquationPreviewShown{S::Transition, 0, 0}) &&
              (shown(ready, 113) == EquationPreviewShown{S::Transition, 0, 3}) &&
              (shown(ready, 111) == EquationPreviewShown{S::Static, 0, 0}),
          "揃った変形の frame だけを見せ、揃わない frame は前の状態の静止");
    check((shown(ready, 103) == EquationPreviewShown{S::Action, 0, 1}) &&
              (shown(ready, 102) == EquationPreviewShown{S::Static, 0, 0}),
          "揃った action の frame だけを見せる");
    Availability noTarget = ready;
    noTarget.statics = {true, false, true};
    check(shown(noTarget, 114).kind == S::None && shown(noTarget, 110).kind == S::Transition,
          "後の状態の静止が無ければ hold 0 は何も見せない");
}

// ---- 配置と画素 ----

constexpr int kOutputWidth = 64;
constexpr int kOutputHeight = 48;

EquationPreviewCoverage fullCoverage(int width, int height, std::uint8_t value = 255) {
    return std::make_shared<const std::vector<std::uint8_t>>(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height), value);
}

// (x, y) の 1 画素だけが value の被覆。
EquationPreviewCoverage dotCoverage(int width, int height, int x, int y, std::uint8_t value = 255) {
    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
    pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
           static_cast<std::size_t>(x)] = value;
    return std::make_shared<const std::vector<std::uint8_t>>(std::move(pixels));
}

// provenance の色は Project の色と別の値にする (色を計算し直すと一致しない)。
constexpr std::uint32_t kTransitionColor = 0xFF0A0B00u;
constexpr std::uint32_t kBaseColor = 0xFF123456u;
constexpr std::uint32_t kAccentColor = 0xFFFE0000u;

// S0 6x4、S1 7x5、S2 5x3 の静止。変形 0 は 9x7 (source (1,1)、target (1,1))、変形 1 は 8x6
// (source (1,1)、target (1,1))。action は 10x8 で静止は (2,2)。
EquationPreviewInputs inputsFor(const TimelineClip& c, bool layers) {
    EquationPreviewInputs in;
    in.clip = c;
    in.timelineFpsNum = 60;
    in.timelineFpsDen = 1;
    in.outputWidth = kOutputWidth;
    in.outputHeight = kOutputHeight;
    in.spec = compiled();
    in.statics = {EquationPreviewStatic{6, 4, fullCoverage(6, 4), 0xFF102030u},
                  EquationPreviewStatic{7, 5, fullCoverage(7, 5), 0xFF405060u},
                  EquationPreviewStatic{5, 3, fullCoverage(5, 3), 0xFF708090u}};
    in.artifactReady = true;
    EquationPreviewTransitionArtifact t0{9, 7, 1, 1, 1, 1, {}};
    for (std::uint32_t i = 0; i < 4; ++i)
        t0.colors.push_back(kTransitionColor + i);
    in.transitions = {t0, EquationPreviewTransitionArtifact{8, 6, 1, 1, 1, 1, {0xFF00FF00u}}};
    for (std::size_t a = 0; a < 3; ++a) {
        EquationPreviewActionArtifact item{10, 8, 2, 2, kBaseColor, {}};
        const std::int64_t frames = in.spec->actions[a].duration;
        for (std::int64_t i = 0; i < frames; ++i)
            item.accentColors.push_back(kAccentColor + static_cast<std::uint32_t>(i));
        in.actions.push_back(item);
    }
    if (!layers)
        return in;
    // 変形の frame i は (i, 0) の 1 画素。frame を取り違えると位置が変わる。
    for (std::int64_t i = 0; i < 4; ++i)
        in.transitionFrames[{0, i}] = dotCoverage(9, 7, static_cast<int>(i), 0);
    in.transitionFrames[{1, 0}] = dotCoverage(8, 6, 0, 0);
    for (std::size_t a = 0; a < 3; ++a) {
        in.actionBases[a] = fullCoverage(10, 8, 0); // 各 action の base は下で個別に作る
        for (std::int64_t i = 0; i < in.spec->actions[a].duration; ++i)
            in.actionAccents[{a, i}] = dotCoverage(10, 8, static_cast<int>(i), 0);
    }
    // base は静止の位置 (2,2) から 6x4 の不透明 (outline の base = 状態全体と同じ形)。
    std::vector<std::uint8_t> base(80, 0);
    for (int y = 2; y < 6; ++y)
        for (int x = 2; x < 8; ++x)
            base[static_cast<std::size_t>(y * 10 + x)] = 255;
    // accent の frame 0 の画素 (0,0) と、base の上に重なる半分の accent (3,3) を作る。
    in.actionBases[0] = std::make_shared<const std::vector<std::uint8_t>>(base);
    in.actionBases[1] = in.actionBases[0];
    std::vector<std::uint8_t> overlap(80, 0);
    overlap[3 * 10 + 3] = 128;
    overlap[1] = 255;
    in.actionAccents[{1, 1}] = std::make_shared<const std::vector<std::uint8_t>>(overlap);
    // 変形 1・action 2 は状態 1 の上 (7x5 を (2,2) に置くと 10x8 に収まる)。
    std::vector<std::uint8_t> base1(80, 0);
    for (int y = 2; y < 7; ++y)
        for (int x = 2; x < 9; ++x)
            base1[static_cast<std::size_t>(y * 10 + x)] = 255;
    in.actionBases[2] = std::make_shared<const std::vector<std::uint8_t>>(base1);
    return in;
}

std::vector<std::uint8_t> render(const EquationPreviewModel& model, std::int64_t outputFrame) {
    const auto rect = model.patchRect();
    std::vector<std::uint8_t> patch(
        static_cast<std::size_t>(rect.width) * static_cast<std::size_t>(rect.height) * 4U, 0xCD);
    model.fill(model.stateCode(model.shownAt(outputFrame)), patch.data());
    // 出力全体の座標へ戻す。
    std::vector<std::uint8_t> image(
        static_cast<std::size_t>(kOutputWidth) * static_cast<std::size_t>(kOutputHeight) * 4U, 0);
    for (int y = 0; y < rect.height; ++y)
        for (int x = 0; x < rect.width; ++x)
            for (int ch = 0; ch < 4; ++ch)
                image[(static_cast<std::size_t>(rect.y + y) * kOutputWidth +
                       static_cast<std::size_t>(rect.x + x)) *
                          4U +
                      static_cast<std::size_t>(ch)] =
                    patch[(static_cast<std::size_t>(y) * static_cast<std::size_t>(rect.width) +
                           static_cast<std::size_t>(x)) *
                              4U +
                          static_cast<std::size_t>(ch)];
    return image;
}

std::vector<int> pixel(const std::vector<std::uint8_t>& image, int x, int y) {
    const auto at = (static_cast<std::size_t>(y) * kOutputWidth + static_cast<std::size_t>(x)) * 4U;
    return {image[at], image[at + 1], image[at + 2], image[at + 3]};
}

std::vector<int> rgba(std::uint32_t argb, int alpha) {
    return {static_cast<int>((argb >> 16) & 0xFFU), static_cast<int>((argb >> 8) & 0xFFU),
            static_cast<int>(argb & 0xFFU), alpha};
}

int opaqueCount(const std::vector<std::uint8_t>& image) {
    int count = 0;
    for (std::size_t at = 3; at < image.size(); at += 4)
        count += image[at] != 0;
    return count;
}

void placementAndPixels() {
    const EquationPreviewModel model(inputsFor(clip(), true));
    check(model.diagnostics().empty(), "試験の入力は全て置ける");
    // 静止は中央 (余りは左上): S0 (64-6)/2 = 29, (48-4)/2 = 22。S1 (28, 21)。
    check((model.staticRect(0) == EquationPreviewRect{29, 22, 6, 4}) &&
              (model.staticRect(1) == EquationPreviewRect{28, 21, 7, 5}),
          "状態の静止は通常の Math と同じ配置");
    // 変形の artifact の左上 = 静止の配置 - artifact の中の静止の位置。
    const auto t0 = model.transitionPlacement(0);
    check(t0 && t0->sourceLeft == 28 && t0->sourceTop == 21 && t0->targetLeft == 27 &&
              t0->targetTop == 20,
          "変形の配置は artifact の source/target の位置から決める");
    check((model.actionRect(0) == EquationPreviewRect{27, 20, 10, 8}),
          "action の base と accent は同じ原点 (静止の配置 - artifact の中の静止の位置)");

    // H0: 静止の色 (Project の通常の Math の色)。
    const auto h0 = render(model, 100);
    check(pixel(h0, 29, 22) == rgba(0xFF102030u, 255) &&
              pixel(h0, 34, 25) == rgba(0xFF102030u, 255) && pixel(h0, 28, 22)[3] == 0 &&
              opaqueCount(h0) == 24,
          "H0 は状態 0 の静止 (6x4) だけ");
    // 変形 frame 0: 原点 (28,21) の (0,0) の画素。色は provenance の frame 0 の色。
    const auto f0 = render(model, 110);
    check(pixel(f0, 28, 21) == rgba(kTransitionColor, 255) && opaqueCount(f0) == 1,
          "変形 frame 0 は frame 0 の被覆を source の位置に、provenance の色で");
    // 中央 frame 2 (進み具合 2/4 = 1/2 は target 側へ丸める): 原点 (27,20) の (2,0)。
    const auto f2 = render(model, 112);
    check(pixel(f2, 29, 20) == rgba(kTransitionColor + 2, 255) && opaqueCount(f2) == 1,
          "変形 frame 2 は frame 2 の被覆 (frame を取り違えない) と frame 2 の色");
    const auto f3 = render(model, 113);
    check(pixel(f3, 30, 20) == rgba(kTransitionColor + 3, 255) && opaqueCount(f3) == 1,
          "最後に見せる frame は N-1 = 3");
    const auto h1 = render(model, 114);
    check(pixel(h1, 28, 21) == rgba(0xFF405060u, 255) && opaqueCount(h1) == 35,
          "変形の直後は状態 1 の静止");

    // outline frame 1 (output 103): base (静止の形、base の色) と accent (1,0) の 1 画素。
    const auto o1 = render(model, 103);
    check(pixel(o1, 29, 22) == rgba(kBaseColor, 255) &&
              pixel(o1, 28, 20) == rgba(kAccentColor + 1, 255) && opaqueCount(o1) == 25,
          "outline は base の上に accent (provenance の 2 色、同じ原点)");
    // pulse frame 1 (output 107): accent の (3,3) は base の上の半分 (128)。
    // 白でない 2 色: 赤 = round((254*128*255*65025 + 0x12*65025*(65025-32640)) / (65025*65025))
    //   = round(254*32640/65025 + 18*32385/65025) = round(127.50 + 8.96) = 136
    //   緑 = round(0 + 0x34*32385/65025) = round(25.90) = 26、青 = round(0x56*32385/65025) = 43
    const auto p1 = render(model, 107);
    check(pixel(p1, 30, 23) == std::vector<int>{136, 26, 43, 255},
          "pulse の重なった画素は base の上に accent の source-over");
    check(pixel(p1, 28, 20) == rgba(kAccentColor + 1, 255), "pulse の accent の frame 1 の色");
    // N=1 の action と変形。action 2 は状態 1 (28,21) の上: 原点 (26,19) の (0,0)。
    check(pixel(render(model, 116), 26, 19) == rgba(kAccentColor, 255),
          "N=1 の action の唯一の frame");
    const auto t1 = render(model, 120);
    check(opaqueCount(t1) == 1 && pixel(t1, 28 - 1, 21 - 1) == rgba(0xFF00FF00u, 255),
          "N=1 の変形の唯一の frame (source の位置)");

    // 番号と見せるものの往復。
    bool roundTrip = true;
    for (std::int64_t f = 100; f < 124; ++f) {
        const auto shown = model.shownAt(f);
        roundTrip = roundTrip && model.shownForCode(model.stateCode(shown)) == shown;
    }
    check(roundTrip, "見せるものの番号は往復する");
    check(model.stateCode(EquationPreviewShown{}) == -1, "何も見せない番号は -1");
}

void atomicAndStale() {
    const auto c = clip();
    using S = EquationPreviewShownKind;
    // base だけ・accent だけでは action を見せない。
    auto baseOnly = inputsFor(c, true);
    baseOnly.actionAccents.erase({0, 1});
    check((EquationPreviewModel(baseOnly).shownAt(103) == EquationPreviewShown{S::Static, 0, 0}),
          "accent が無い action は静止 (半分の action を見せない)");
    auto accentOnly = inputsFor(c, true);
    accentOnly.actionBases.erase(0);
    check((EquationPreviewModel(accentOnly).shownAt(103) == EquationPreviewShown{S::Static, 0, 0}),
          "base が無い action は静止");
    // 中央の frame だけが読めた: 直接 seek した中央を見せ、先頭は静止 (先頭からやり直さない)。
    auto middleOnly = inputsFor(c, false);
    middleOnly.actionBases = inputsFor(c, true).actionBases;
    middleOnly.actionAccents[{1, 1}] = inputsFor(c, true).actionAccents.at({1, 1});
    const EquationPreviewModel middle(middleOnly);
    check((middle.shownAt(107) == EquationPreviewShown{S::Action, 1, 1}) &&
              (middle.shownAt(106) == EquationPreviewShown{S::Static, 0, 0}),
          "読めた中央の frame へそのまま入り、frame 0 からやり直さない");
    // 読む前と後: 同じ output frame で、静止 → 同じ区間の同じ frame。
    const EquationPreviewModel before(inputsFor(c, false));
    const EquationPreviewModel after(inputsFor(c, true));
    check((before.shownAt(112) == EquationPreviewShown{S::Static, 0, 0}) &&
              (after.shownAt(112) == EquationPreviewShown{S::Transition, 0, 2}),
          "変形が読めたら同じ output frame の frame 2 へ入る");

    // artifact が今の key で Ready でなければ、渡された層を使わない (古い key の層を出さない)。
    auto stale = inputsFor(c, true);
    stale.artifactReady = false;
    const EquationPreviewModel staleModel(stale);
    check((staleModel.shownAt(103) == EquationPreviewShown{S::Static, 0, 0}) &&
              (staleModel.shownAt(112) == EquationPreviewShown{S::Static, 0, 0}),
          "artifact が Ready でなければ層があっても静止");
    // 区間の数が spec と違う artifact も使わない。
    auto mismatched = inputsFor(c, true);
    mismatched.actions.pop_back();
    const EquationPreviewModel mismatchedModel(mismatched);
    check(!mismatchedModel.diagnostics().empty() &&
              (mismatchedModel.shownAt(103) == EquationPreviewShown{S::Static, 0, 0}),
          "spec と区間の数が違う artifact は使わない");
    // compile できない (spec が無い) ときは静止だけ。
    auto failed = inputsFor(c, true);
    failed.spec.reset();
    const EquationPreviewModel failedModel(failed);
    check((failedModel.shownAt(103) == EquationPreviewShown{S::Static, 0, 0}) &&
              (failedModel.shownAt(112) == EquationPreviewShown{S::Static, 0, 0}) &&
              (failedModel.shownAt(114) == EquationPreviewShown{S::Static, 1, 0}),
          "compile の失敗中は今の状態の静止だけ (前の spec の animation を出さない)");
    // 静止が無い状態の区間は何も見せない (別の式で代用しない)。
    auto missing = inputsFor(c, true);
    missing.statics[1].reset();
    const EquationPreviewModel missingModel(missing);
    check(missingModel.shownAt(114).kind == S::None && !missingModel.transitionPlacement(0),
          "後の状態の静止が無ければ変形の配置も決めない");
    // 大きさの合わない層は使わない。
    auto wrongSize = inputsFor(c, true);
    wrongSize.transitionFrames[{0, 2}] = fullCoverage(3, 3);
    check((EquationPreviewModel(wrongSize).shownAt(112) == EquationPreviewShown{S::Static, 0, 0}),
          "大きさの合わない変形の frame は使わない");
    // ClipEffects は model の画素に入らない (layer が 1 回だけ掛ける)。
    auto moved = c;
    moved.effects.positionXPercent = 25;
    moved.effects.scaleXPercent = 150;
    moved.effects.rotationDegrees = 30;
    moved.effects.opacityPercent = 40;
    const EquationPreviewModel movedModel(inputsFor(moved, true));
    check(movedModel.patchRect() == after.patchRect() &&
              render(movedModel, 107) == render(after, 107),
          "ClipEffects は sequence の内側の合成に入らない");
}

void layerRequests() {
    const auto spec = compiled();
    const auto c = clip();
    using R = EquationPreviewLayerRole;
    using Bundle = std::vector<EquationPreviewLayerId>;
    const auto timeAt = [&](std::int64_t output) {
        std::string error;
        return equationPreviewTimeAt(c, 60, 1, &spec, output, error)
            .value_or(EquationPreviewTime{});
    };
    check(equationPreviewCurrentLayers(timeAt(100).lookup).empty(), "hold は層を要求しない");
    check(equationPreviewCurrentLayers(timeAt(103).lookup) ==
              Bundle{{R::ActionBase, 0, 0}, {R::ActionAccent, 0, 1}},
          "action は base と今の accent の 2 枚を 1 つの束で要求する");
    check(equationPreviewCurrentLayers(timeAt(112).lookup) == Bundle{{R::TransitionFrame, 0, 2}},
          "変形は今の frame の 1 枚");
    // 先読み: outline frame 1 からは outline の残り (frame 2) と次の pulse の 3 枚。
    const auto fromOutline = equationPreviewUpcomingLayers(spec, timeAt(103), 100);
    check((fromOutline == std::vector<Bundle>{{{R::ActionBase, 0, 0}, {R::ActionAccent, 0, 2}},
                                              {{R::ActionBase, 1, 0}, {R::ActionAccent, 1, 0}},
                                              {{R::ActionBase, 1, 0}, {R::ActionAccent, 1, 1}},
                                              {{R::ActionBase, 1, 0}, {R::ActionAccent, 1, 2}}}),
          "今の区間の残りと次の区間を先読みする");
    const auto fromHold = equationPreviewUpcomingLayers(spec, timeAt(109), 100);
    check(fromHold.size() == 4 && fromHold.front() == Bundle{{R::TransitionFrame, 0, 0}} &&
              fromHold.back() == Bundle{{R::TransitionFrame, 0, 3}},
          "hold の中は次の区間 (変形) を先読みする");
    check(equationPreviewUpcomingLayers(spec, timeAt(103), 2).size() == 2, "先読みは上限まで");
    check(equationPreviewUpcomingLayers(spec, timeAt(122), 100).empty(),
          "最後の hold の後には先読みが無い");
}

} // namespace

int main() {
    timeMapping();
    layerRequests();
    fallbackRules();
    placementAndPixels();
    atomicAndStale();
    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}
