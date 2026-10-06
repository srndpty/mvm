#include "app/preview/test_window_mode.h"
// Equation Sequence の product preview (P3-4) を MvmController 経由で検査する。
//
//   (引数なし)              偽の backend、GPU なし (CTest:
//   math_equation_sequence_preview_controller)
//   --native <fixture>      実 D3D11 の preview (engine の render thread の評価) と、製品と同じ
//                           layer を product compositor で合成した画素の readback
//                           (CTest: math_equation_sequence_native_preview、workstation)
//   --write-fixture <path>  schema 21 の fixture (tests/fixtures/equation-sequence/) を書く
//
// 期待値は手で決めた区間の表と、artifact の provenance と disk の .a8 から試験が独立に組んだ
// 合成 (製品の合成関数を呼ばない) で作る。対照 (配置を 1 画素ずらす・accent を省く・色を取り違える)
// で、比較が違いを検出できることも確かめる。

#include "app/equation_sequence_compile.h"
#include "app/equation_sequence_preview.h"
#include "app/equation_sequence_render.h"
#include "app/math_clip_render.h"
#include "app/preview/preview_engine_rhi_item.h"
#include "app/timeline_preview_mapping.h"
#include "math_fake_backend.h"
#include "media/gpu_preview/gpu_compositor.h"
#include "media/gpu_preview/still_image_frame.h"
#include "mvm_controller.h"
#include "project/clip_effects.h"
#include "project/equation_sequence.h"
#include "project/equation_sequence_edit.h"
#include "project/project_json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <d3d11.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>

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

using mvm::app::EquationPreviewShown;
using mvm::app::EquationPreviewShownKind;
using mvm::app::MathRasterCache;
using mvm::app::MvmController;
using mvm::test::FakeMathBackend;
namespace project = mvm::project;
namespace app = mvm::app;
namespace math = mvm::math;

constexpr int kW = 1920;
constexpr int kH = 1080;
const QString kClip = QStringLiteral("seq");

bool pump(const std::function<bool()>& done, int milliseconds = 10000) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < milliseconds) {
        QGuiApplication::processEvents();
        QThread::msleep(2);
    }
    return done();
}

void settle(int milliseconds) {
    pump([] { return false; }, milliseconds);
}

// ---- fixture (schema 21) ----
//
// source frame の区間 (60 fps の Project、clip は V1 の 0 から、素材 fps 60 なので output =
// source):
//   S0 "SIZE40x20+b" hold 30 [0,30)   outline [4,10) と pulse [16,22) (対象 b)
//   T0 10 枚 [30,40)
//   S1 "SIZE30x24+b" hold 30 [40,70)  pulse N=1 [50,51)
//   T1 1 枚  [70,71)
//   S2 "SIZE36x18+c" hold 20 [71,91)
// spec の action の番号 (状態・start の順): 0 = outline、1 = pulse、2 = pulse N=1。
constexpr std::int64_t kLength = 91;
constexpr std::uint32_t kColors[] = {0xFF4080C0u, 0xFFC04080u, 0xFF80C040u};

void part(project::EquationState& s, std::string id, const std::string& text) {
    const auto at = s.equation.source.find(text);
    s.parts.push_back(
        {{std::move(id)},
         "label",
         {s.revision, static_cast<std::int64_t>(at), static_cast<std::int64_t>(at + text.size()),
          text, project::BindingStatus::Bound}});
}

project::EquationState state(const std::string& id, const std::string& source, std::int64_t hold,
                             const std::string& color) {
    project::EquationState s;
    s.id = {id};
    s.revision = "rev-" + id;
    s.equation.source = source;
    s.equation.color = color;
    s.holdFrames = hold;
    return s;
}

project::EquationAction action(const std::string& id, const std::string& owner,
                               const std::string& target, std::int64_t start, std::int64_t duration,
                               project::EquationOperation operation) {
    return {{id},  {owner},  {target}, project::EquationTargetStatus::Present,
            start, duration, operation};
}

project::EquationSequenceClipData fixtureData() {
    auto s0 = state("s0", "SIZE40x20+b", 30, "#FF4080C0");
    part(s0, "b0", "b");
    auto s1 = state("s1", "SIZE30x24+b", 30, "#FFC04080");
    part(s1, "b1", "b");
    auto s2 = state("s2", "SIZE36x18+c", 20, "#FF80C040");
    project::EquationSequenceClipData d;
    d.states = {s0, s1, s2};
    d.transitions = {{{"t0"}, {"s0"}, {"s1"}, 10, {{{"b0"}, {"b1"}}}},
                     {{"t1"}, {"s1"}, {"s2"}, 1, {}}};
    d.actions = {action("outline", "s0", "b0", 4, 6, project::EquationOperation::Outline),
                 action("pulse", "s0", "b0", 16, 6, project::EquationOperation::Pulse),
                 action("pulse1", "s1", "b1", 10, 1, project::EquationOperation::Pulse)};
    return d;
}

project::Project projectWith(project::EquationSequenceClipData data) {
    auto initial = project::createDefaultProject();
    const auto added =
        project::addEquationSequence(initial, std::move(data), kClip.toStdString(), "P3-4 preview",
                                     {project::TrackKind::Video, 0}, 0);
    check(added.success, "fixture の sequence を置ける: " + added.error);
    return initial;
}

project::Project fixtureProject() {
    return projectWith(fixtureData());
}

// 実 Manim の受け入れ: fixture と同じ区間の表で、式だけを実の TeX にする。
project::EquationSequenceClipData realFixtureData() {
    auto data = fixtureData();
    const auto rebind = [](project::EquationState& s, const std::string& source,
                           const std::string& target) {
        s.equation.source = source;
        s.equation.fontSize = 72;
        s.parts.clear();
        if (!target.empty())
            part(s, s.id.value == "s0" ? "b0" : "b1", target);
    };
    rebind(data.states[0], "x=\\frac{-b\\pm\\sqrt{b^2-4ac}}{2a}", "b^2-4ac");
    rebind(data.states[1], "\\Delta=b^2-4ac", "b^2-4ac");
    rebind(data.states[2], "\\Delta>0", "");
    return data;
}

// 手で決めた見せるもの (Ready なら) と、その区間の代用の状態。
struct Expected {
    EquationPreviewShownKind kind = EquationPreviewShownKind::Static;
    std::size_t index = 0;
    std::int64_t frame = 0;
    std::size_t fallback = 0;
};

Expected expectedAt(std::int64_t f) {
    using K = EquationPreviewShownKind;
    if (f >= 4 && f < 10)
        return {K::Action, 0, f - 4, 0};
    if (f >= 16 && f < 22)
        return {K::Action, 1, f - 16, 0};
    if (f < 30)
        return {K::Static, 0, 0, 0};
    if (f < 40)
        return {K::Transition, 0, f - 30, 0};
    if (f == 50)
        return {K::Action, 2, 0, 1};
    if (f < 70)
        return {K::Static, 1, 0, 1};
    if (f == 70)
        return {K::Transition, 1, 0, 1};
    return {K::Static, 2, 0, 2};
}

EquationPreviewShown shownOf(const Expected& e) {
    return {e.kind, e.index, e.frame};
}

EquationPreviewShown fallbackOf(const Expected& e) {
    return {EquationPreviewShownKind::Static, e.fallback, 0};
}

std::string describe(const EquationPreviewShown& shown) {
    return std::string(app::equationPreviewShownKindName(shown.kind)) + "(" +
           std::to_string(shown.index) + "," + std::to_string(shown.frame) + ")";
}

// ---- controller ----

std::unique_ptr<MvmController> makeController(const std::filesystem::path& path,
                                              const project::Project& initial,
                                              const std::filesystem::path& manim = {}) {
    return std::make_unique<MvmController>(
        path, manim, initial, nullptr,
        [](const project::Project&, const app::TimelineExportRequest& request) {
            app::TimelineExportResult result;
            result.success = true;
            result.outputPath = request.outputPath;
            return result;
        },
        MvmController::ExportThreadFactory{},
        MvmController::FileRevealer{[](const std::filesystem::path&, QString&) { return true; }});
}

bool backendAvailable(MvmController& controller) {
    return pump([&] {
        return controller.mathRastersForTest().backendState() ==
               MathRasterCache::BackendState::Available;
    });
}

// frame の合成の Equation Sequence の layer (透明な下地と animation)。
std::optional<mvm::preview::PreviewCompositionLayer> sequenceLayer(const MvmController& controller,
                                                                   qint64 frame) {
    QString error;
    const auto composition = controller.subtitleCompositionForTest(frame, error);
    if (!composition)
        return std::nullopt;
    for (const auto& layer : composition->layers)
        if (layer.stillImage && layer.stillAnimation && layer.stillImage->width == kW &&
            std::all_of(layer.stillImage->rgba.begin(), layer.stillImage->rgba.end(),
                        [](std::uint8_t v) { return v == 0; }))
            return layer;
    return std::nullopt;
}

// 合成を組んで (要求・読み込みを進めて)、今の model が見せるもの。
EquationPreviewShown shownAt(const MvmController& controller, qint64 frame) {
    sequenceLayer(controller, frame);
    return controller.equationSequencePreviewStatus(kClip, frame).shown;
}

bool waitShown(const MvmController& controller, qint64 frame, const EquationPreviewShown& wanted,
               int milliseconds = 10000) {
    const bool done = pump([&] { return shownAt(controller, frame) == wanted; }, milliseconds);
    if (!done)
        std::fprintf(stderr, "  frame %lld: 見せたもの %s (期待 %s)\n",
                     static_cast<long long>(frame), describe(shownAt(controller, frame)).c_str(),
                     describe(wanted).c_str());
    return done;
}

// 今の Project の sequence の描画要求 (cache の key・artifact を引くため)。
std::optional<math::EquationSequenceRenderSpec> renderSpecOf(const MvmController& controller) {
    for (const auto& clip : controller.projectForTest().timelineClips) {
        if (clip.kind != project::TimelineClipKind::EquationSequence ||
            clip.id != kClip.toStdString())
            continue;
        const auto compiled = app::compileEquationSequence(clip.equationSequence);
        if (!compiled.value)
            return std::nullopt;
        std::string error;
        return app::equationSequenceRenderSpecFor(*compiled.value, error);
    }
    return std::nullopt;
}

bool waitDiskReady(MvmController& controller, int milliseconds = 20000) {
    return pump(
        [&] {
            sequenceLayer(controller, 0);
            return controller.equationSequencePreviewStatus(kClip, 0).disk ==
                   MathRasterCache::State::Ready;
        },
        milliseconds);
}

// 製品の layer の CPU の画素 (engine と同じく、静止画に state の patch を書く)。
std::vector<std::uint8_t> presentedPixels(const mvm::preview::PreviewCompositionLayer& layer,
                                          std::int64_t frame) {
    auto rgba = layer.stillImage->rgba;
    const auto state = layer.stillAnimation->stateAt(frame);
    if (state < 0)
        return rgba;
    const auto rect = layer.stillAnimation->patchRect();
    std::vector<std::uint8_t> patch(
        static_cast<std::size_t>(rect.width) * static_cast<std::size_t>(rect.height) * 4U, 0xCD);
    layer.stillAnimation->fillPatch(state, patch.data());
    const std::size_t row = static_cast<std::size_t>(rect.width) * 4U;
    for (int y = 0; y < rect.height; ++y)
        std::memcpy(
            rgba.data() +
                (static_cast<std::size_t>(rect.y + y) * kW + static_cast<std::size_t>(rect.x)) * 4U,
            patch.data() + static_cast<std::size_t>(y) * row, row);
    return rgba;
}

std::size_t differingPixels(const std::vector<std::uint8_t>& a,
                            const std::vector<std::uint8_t>& b) {
    if (a.size() != b.size())
        return static_cast<std::size_t>(-1);
    std::size_t count = 0;
    for (std::size_t at = 0; at < a.size(); at += 4)
        count += std::memcmp(a.data() + at, b.data() + at, 4) != 0;
    return count;
}

// ---- 独立の参照の合成 (製品の関数を呼ばない) ----

std::vector<std::uint8_t> readA8(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

struct ReferenceStatic {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> coverage;
};

enum class Variant { Exact, Shifted, OmitAccent, WrongColor };

struct Reference {
    app::EquationSequenceArtifact artifact;
    std::vector<ReferenceStatic> statics;
    std::vector<std::uint32_t> stateColors;

    static int staticLeft(int width) { return (kW - width) / 2; }

    static int staticTop(int height) { return (kH - height) / 2; }

    static void put(std::vector<std::uint8_t>& image, int x, int y, std::uint32_t argb, int alpha) {
        const auto at = (static_cast<std::size_t>(y) * kW + static_cast<std::size_t>(x)) * 4U;
        if (alpha <= 0) {
            image[at] = image[at + 1] = image[at + 2] = image[at + 3] = 0;
            return;
        }
        image[at] = static_cast<std::uint8_t>((argb >> 16) & 0xFF);
        image[at + 1] = static_cast<std::uint8_t>((argb >> 8) & 0xFF);
        image[at + 2] = static_cast<std::uint8_t>(argb & 0xFF);
        image[at + 3] = static_cast<std::uint8_t>(alpha);
    }

    // 1 層: alpha = round(被覆 * 色の alpha / 255)、色はそのまま。
    static void single(std::vector<std::uint8_t>& image, const std::vector<std::uint8_t>& coverage,
                       int width, int height, int left, int top, std::uint32_t argb) {
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const int c = coverage[static_cast<std::size_t>(y * width + x)];
                const int alpha =
                    static_cast<int>(std::floor(c * static_cast<double>(argb >> 24) / 255.0 + 0.5));
                put(image, left + x, top + y, argb, c == 0 ? 0 : alpha);
            }
    }

    // 2 層 (base の上に accent)。docs の規則を試験の側で書き直したもの。
    static void two(std::vector<std::uint8_t>& image, const std::vector<std::uint8_t>& base,
                    const std::vector<std::uint8_t>& accent, int width, int height, int left,
                    int top, std::uint32_t baseArgb, std::uint32_t accentArgb) {
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const auto at = static_cast<std::size_t>(y * width + x);
                const double a = base[at] * static_cast<double>(baseArgb >> 24) / 65025.0;
                const double b = accent[at] * static_cast<double>(accentArgb >> 24) / 65025.0;
                const double weightA = b;
                const double weightB = a * (1.0 - b);
                const auto px =
                    (static_cast<std::size_t>(top + y) * kW + static_cast<std::size_t>(left + x)) *
                    4U;
                if (weightA + weightB <= 0) {
                    image[px] = image[px + 1] = image[px + 2] = image[px + 3] = 0;
                    continue;
                }
                for (int ch = 0; ch < 3; ++ch) {
                    const int shift = 16 - 8 * ch;
                    const double value = (((accentArgb >> shift) & 0xFF) * weightA +
                                          ((baseArgb >> shift) & 0xFF) * weightB) /
                                         (weightA + weightB);
                    image[px + static_cast<std::size_t>(ch)] =
                        static_cast<std::uint8_t>(std::floor(value + 0.5));
                }
                const int la = static_cast<int>(std::floor(a * 255.0 + 0.5));
                const int lb = static_cast<int>(std::floor(b * 255.0 + 0.5));
                image[px + 3] = static_cast<std::uint8_t>(lb + (la * (255 - lb) + 127) / 255);
            }
    }

    std::vector<std::uint8_t> staticImage(std::size_t s) const {
        std::vector<std::uint8_t> image(static_cast<std::size_t>(kW) * kH * 4U, 0);
        const auto& still = statics[s];
        single(image, still.coverage, still.width, still.height, staticLeft(still.width),
               staticTop(still.height), stateColors[s]);
        return image;
    }

    // 変形の frame i の左上: 端点の差は各軸 1 画素以内で、進み具合 i/N が 1/2 以上なら target。
    static int originAt(int from, int to, std::int64_t i, std::int64_t frames) {
        return from != to && 2 * i >= frames ? to : from;
    }

    std::vector<std::uint8_t> image(const Expected& e, Variant variant = Variant::Exact) const {
        std::vector<std::uint8_t> out(static_cast<std::size_t>(kW) * kH * 4U, 0);
        const int shift = variant == Variant::Shifted ? 1 : 0;
        switch (e.kind) {
        case EquationPreviewShownKind::None:
            return out;
        case EquationPreviewShownKind::Static:
            return staticImage(e.index);
        case EquationPreviewShownKind::Transition: {
            const auto& item = artifact.transitions[e.index];
            const auto& from = statics[e.index];
            const auto& to = statics[e.index + 1];
            const int sx = staticLeft(from.width) - item.sourceX;
            const int sy = staticTop(from.height) - item.sourceY;
            const int tx = staticLeft(to.width) - item.targetX;
            const int ty = staticTop(to.height) - item.targetY;
            const auto frames = static_cast<std::int64_t>(item.frames.size());
            const auto& frame = item.frames[static_cast<std::size_t>(e.frame)];
            // 色の取り違えの対照: 後の状態の色 (frame ごとの provenance の色を使わない)。
            const std::uint32_t color =
                variant == Variant::WrongColor ? stateColors[e.index + 1] : frame.colorArgb;
            single(out, readA8(frame.path), item.width, item.height,
                   originAt(sx, tx, e.frame, frames) + shift, originAt(sy, ty, e.frame, frames),
                   color);
            return out;
        }
        case EquationPreviewShownKind::Action: {
            const auto& item = artifact.actions[e.index];
            const auto& still = statics[e.fallback];
            const int left = staticLeft(still.width) - item.staticX + shift;
            const int top = staticTop(still.height) - item.staticY;
            const auto base = readA8(item.base.path);
            auto accent = readA8(item.accent[static_cast<std::size_t>(e.frame)].path);
            if (variant == Variant::OmitAccent)
                std::fill(accent.begin(), accent.end(), std::uint8_t{0});
            // 色の取り違えの対照: accent に状態の色 (provenance を使わず Project から作った色)。
            const std::uint32_t accentColor =
                variant == Variant::WrongColor
                    ? stateColors[e.fallback]
                    : item.accent[static_cast<std::size_t>(e.frame)].colorArgb;
            two(out, base, accent, item.width, item.height, left, top, item.base.colorArgb,
                accentColor);
            return out;
        }
        }
        return out;
    }
};

std::optional<Reference> referenceFor(MvmController& controller) {
    const auto spec = renderSpecOf(controller);
    auto& cache = controller.mathRastersForTest();
    if (!spec)
        return std::nullopt;
    const auto artifact = cache.equationSequenceArtifactOf(*spec);
    if (!artifact)
        return std::nullopt;
    Reference reference;
    reference.artifact = *artifact;
    for (std::size_t s = 0; s < spec->states.size(); ++s) {
        const auto entry = cache.request(spec->states[s].still);
        if (entry.state != MathRasterCache::State::Ready || !entry.mask)
            return std::nullopt;
        ReferenceStatic still{entry.mask->width, entry.mask->height, {}};
        for (std::size_t at = 3; at < entry.mask->rgba.size(); at += 4)
            still.coverage.push_back(entry.mask->rgba[at]);
        reference.statics.push_back(std::move(still));
        reference.stateColors.push_back(spec->states[s].foregroundArgb);
    }
    return reference;
}

// 受け入れの frame (手で決めた区間の表のどこか)。
struct AcceptanceFrame {
    const char* name;
    std::int64_t frame;
};

const AcceptanceFrame kAcceptance[] = {
    {"H0 静止", 0},
    {"outline 先頭", 4},
    {"outline 中央", 7},
    {"outline 最後", 9},
    {"outline の後の静止", 10},
    {"pulse 先頭", 16},
    {"pulse 中央", 19},
    {"pulse 最後", 21},
    {"pulse の後の静止", 22},
    {"T0 frame 0", 30},
    {"T0 中央", 35},
    {"T0 最後に見せる frame", 39},
    {"H1 frame 0", 40},
    {"N=1 pulse", 50},
    {"T1 (N=1)", 70},
    {"H2 frame 0", 71},
};

// 全区間の層を memory に読んで見せる (今の frame を順に要求する)。
bool loadAllIntervals(MvmController& controller) {
    bool all = true;
    for (const auto& item : kAcceptance)
        all = waitShown(controller, item.frame, shownOf(expectedAt(item.frame))) && all;
    return all;
}

// ---- 偽の backend の集中試験 ----

struct Fixture {
    QTemporaryDir temp;
    FakeMathBackend backend;
    std::unique_ptr<MvmController> controller;
};

std::unique_ptr<Fixture> openFixture(const std::string& name, project::Project initial) {
    auto fixture = std::make_unique<Fixture>();
    check(fixture->temp.isValid(), name + ": 作業 directory");
    const auto path = std::filesystem::path(fixture->temp.filePath("p34.mvm").toStdWString());
    check(project::saveProjectJson(initial, path).success, name + ": Project の保存");
    fixture->controller = makeController(path, initial);
    fixture->controller->setMathPreflightForTest(fixture->backend.preflight());
    check(backendAvailable(*fixture->controller), name + ": 偽の backend が使える");
    return fixture;
}

void testPresentationAndPixels() {
    auto f = openFixture("提示", fixtureProject());
    auto& controller = *f->controller;
    check(waitDiskReady(controller), "提示: disk の artifact が Ready");
    check(loadAllIntervals(controller), "提示: 全区間を P3-1 の区間の表どおりに見せる");
    const auto reference = referenceFor(controller);
    check(reference.has_value(), "提示: 参照の artifact と静止");
    if (!reference)
        return;
    // 区間の全 frame で、見せるものは手の表どおり (最後の frame・直後・N=1 を含む)。
    bool table = true;
    for (std::int64_t frame = 0; frame < kLength; ++frame) {
        const auto layer = sequenceLayer(controller, frame);
        const auto shown = controller.equationSequencePreviewStatus(kClip, frame).shown;
        const auto expected = expectedAt(frame);
        // 先読みで読めていない frame は代用 (状態の静止) だけを許す。
        if (!layer || (shown != shownOf(expected) && shown != fallbackOf(expected))) {
            table = false;
            std::fprintf(stderr, "  frame %lld: %s\n", static_cast<long long>(frame),
                         describe(shown).c_str());
        }
    }
    check(table, "提示: 全 frame で区間の表の frame か、その区間の代用だけを見せる");
    // 受け入れの frame の画素は、provenance と disk の .a8 から独立に組んだ参照と全画素一致。
    for (const auto& item : kAcceptance) {
        const auto expected = expectedAt(item.frame);
        waitShown(controller, item.frame, shownOf(expected));
        const auto layer = sequenceLayer(controller, item.frame);
        if (!layer) {
            check(false, std::string("提示: layer がある: ") + item.name);
            continue;
        }
        const auto product = presentedPixels(*layer, item.frame);
        const auto exact = reference->image(expected);
        const auto different = differingPixels(product, exact);
        check(different == 0, std::string("提示: 参照と全画素一致: ") + item.name + " (違う画素 " +
                                  std::to_string(different) + ")");
        if (expected.kind != EquationPreviewShownKind::Static) {
            check(differingPixels(product, reference->image(expected, Variant::Shifted)) > 0,
                  std::string("対照: 1 画素ずらすと一致しない: ") + item.name);
            check(differingPixels(product, reference->image(expected, Variant::WrongColor)) > 0,
                  std::string("対照: 色を取り違えると一致しない: ") + item.name);
        }
        if (expected.kind == EquationPreviewShownKind::Action)
            check(differingPixels(product, reference->image(expected, Variant::OmitAccent)) > 0,
                  std::string("対照: accent を省くと一致しない: ") + item.name);
        check(std::abs(layer->opacity - 1.0F) < 1e-6F && !layer->effectsEnabled,
              std::string("提示: 既定の ClipEffects では layer を変えない: ") + item.name);
    }
    // 逆向き・飛び飛びの seek でも同じ (履歴を持たない)。
    bool reverse = true;
    for (const std::int64_t frame : {39, 4, 50, 19, 30, 9, 70, 0, 35, 21}) {
        const auto layer = sequenceLayer(controller, frame);
        reverse = reverse && layer &&
                  presentedPixels(*layer, frame) == reference->image(expectedAt(frame));
    }
    check(reverse, "提示: 逆向き・飛び飛びに組み直しても同じ画素");
    // 同じ animation を別の frame で評価しても、その frame の内容 (履歴でなく output frame)。
    // frame 19 で組んだ animation は今の束と先読み (pulse の残りと次の T0) の層を持つので T0 の
    // 中央はその frame、持たない outline の frame は状態の静止の代用。
    const auto layer = sequenceLayer(controller, 19);
    const auto at7 = layer ? presentedPixels(*layer, 7) : std::vector<std::uint8_t>{};
    check(layer && presentedPixels(*layer, 35) == reference->image(expectedAt(35)) &&
              (at7 == reference->image(expectedAt(7)) ||
               at7 == reference->staticImage(expectedAt(7).fallback)),
          "提示: 1 つの animation は全区間を output frame で決める (区間の frame かその代用)");
    controller.shutdown();
}

void testDelayedDirectSeek() {
    auto f = openFixture("遅延", fixtureProject());
    auto& controller = *f->controller;
    auto& cache = controller.mathRastersForTest();
    check(waitDiskReady(controller), "遅延: disk の artifact が Ready");
    for (const auto& [name, frame, heldLayers] : {std::tuple{"pulse の中央", std::int64_t{19}, 2},
                                                  std::tuple{"T0 の中央", std::int64_t{35}, 1}}) {
        const auto expected = expectedAt(frame);
        // memory の層を捨て、読み込みを留める (disk は Ready のまま)。
        cache.setResidentMemoryBudget(MathRasterCache::kDefaultResidentMemoryBudget);
        cache.holdResidentLoadsForTest(true);
        const auto playhead = controller.playheadFrame();
        check(shownAt(controller, frame) == fallbackOf(expected),
              std::string("遅延: ") + name + " へ直接行くと、層が届くまでは状態の静止");
        check(pump([&] { return cache.heldResidentLoadCountForTest() == heldLayers; }),
              std::string("遅延: ") + name + " の層を読み終えても届けない (" +
                  std::to_string(cache.heldResidentLoadCountForTest()) + " 件)");
        const auto status = controller.equationSequencePreviewStatus(kClip, frame);
        check(status.disk == MathRasterCache::State::Ready &&
                  status.residency == MathRasterCache::Residency::Loading,
              std::string("遅延: ") + name + " は disk Ready・memory Loading (別の状態)");
        cache.holdResidentLoadsForTest(false);
        check(waitShown(controller, frame, shownOf(expected)),
              std::string("遅延: 届いた後は同じ output frame の ") + name +
                  " の frame へ入る (区間の先頭からやり直さない)");
        check(controller.playheadFrame() == playhead,
              std::string("遅延: 再生位置は変えない: ") + name);
    }
    controller.shutdown();
}

void testAtomicBundleAndBudget() {
    auto f = openFixture("予算", fixtureProject());
    auto& controller = *f->controller;
    auto& cache = controller.mathRastersForTest();
    check(waitDiskReady(controller), "予算: disk の artifact が Ready");
    const auto reference = referenceFor(controller);
    if (!reference) {
        check(false, "予算: artifact");
        return;
    }
    const auto& pulse = reference->artifact.actions[1];
    const auto layerBytes =
        static_cast<std::size_t>(pulse.width) * static_cast<std::size_t>(pulse.height);
    // 1 層だけ入る上限: 2 層の束は入らないので、片方だけ読まずに静止で見せる。
    cache.setResidentMemoryBudget(layerBytes);
    check(shownAt(controller, 19) == fallbackOf(expectedAt(19)),
          "予算: 束の 2 層が入らなければ action を見せない (静止)");
    // 前の上限の予約が返ると OverBudget の印を消して要求し直させるので、要求しながら確かめる。
    check(pump([&] {
              const bool shownStatic = shownAt(controller, 19) == fallbackOf(expectedAt(19));
              const auto over = controller.equationSequencePreviewStatus(kClip, 19);
              return shownStatic && over.residency == MathRasterCache::Residency::OverBudget &&
                     over.disk == MathRasterCache::State::Ready && !over.residencyMessage.isEmpty();
          }),
          "予算: memory は OverBudget、disk は Ready のまま");
    check(cache.residentEquationLayerCount() == 0 && cache.residentBytes() == 0,
          "予算: 1 層だけを読んで上限を使わない");
    // 変形の 1 枚も入らない上限: 区間の全 frame で前の状態の静止、後の hold 0 で後の状態。
    const auto& t0 = reference->artifact.transitions[0];
    cache.setResidentMemoryBudget(
        static_cast<std::size_t>(t0.width) * static_cast<std::size_t>(t0.height) - 1);
    bool source = true;
    for (std::int64_t frame = 30; frame < 40; ++frame)
        source = source && shownAt(controller, frame) == fallbackOf(expectedAt(frame));
    check(source, "予算: 変形の frame が入らなければ区間の全 frame で前の状態の静止");
    check(shownAt(controller, 40) == EquationPreviewShown{EquationPreviewShownKind::Static, 1, 0},
          "予算: 後の状態の静止へは target の hold 0 で切り替わる");
    // 上限を戻すと、同じ output frame へそのまま入る (やり直さない)。
    cache.setResidentMemoryBudget(2 * layerBytes);
    check(waitShown(controller, 19, shownOf(expectedAt(19))),
          "予算: 2 層が入る上限に戻すと pulse の frame 3 へ入る");
    check(cache.residentBytes() == 2 * layerBytes,
          "予算: memory の量は 2 層の実 byte (" + std::to_string(cache.residentBytes()) + ")");
    controller.shutdown();
}

// Write の mask と Equation Sequence の層は同じ上限で数える。
void testSharedBudget() {
    auto initial = fixtureProject();
    project::TimelineClip write;
    write.id = "write";
    write.name = "WIDE";
    write.kind = project::TimelineClipKind::Math;
    write.track = {project::TrackKind::Video, 1};
    write.sourceFpsNum = 60;
    write.sourceFpsDen = 1;
    write.sourceFrameCount = write.sourceOutFrame = 60;
    write.math.source = "WIDE w";
    write.mathAnimation = {project::MathIntroKind::Write, 30};
    initial.timelineClips.push_back(write);
    check(project::validateTimeline(initial).success, "共有: Write と sequence の Project");
    auto f = openFixture("共有", initial);
    auto& controller = *f->controller;
    auto& cache = controller.mathRastersForTest();
    check(waitDiskReady(controller), "共有: disk の artifact が Ready");
    const auto reference = referenceFor(controller);
    if (!reference) {
        check(false, "共有: artifact");
        return;
    }
    const auto& pulse = reference->artifact.actions[1];
    const std::size_t bundle =
        2U * static_cast<std::size_t>(pulse.width) * static_cast<std::size_t>(pulse.height);
    const std::size_t writeBytes = 64U * 8U * 30U; // WIDE の Write: 64x8 x 30 枚
    const auto bothResident = [&] {
        QString error;
        const auto composition = controller.subtitleCompositionForTest(19, error);
        int animated = 0;
        if (composition)
            for (const auto& layer : composition->layers)
                animated += layer.stillAnimation ? 1 : 0;
        return controller.equationSequencePreviewStatus(kClip, 19).shown ==
                   shownOf(expectedAt(19)) &&
               animated == 2 &&
               controller.mathClipData(QStringLiteral("write")).value("writePreview").toString() ==
                   QStringLiteral("ready");
    };
    cache.setResidentMemoryBudget(writeBytes + bundle);
    check(pump(bothResident), "共有: 合計が入る上限なら Write と pulse の両方を見せる");
    check(cache.residentBytes() == writeBytes + bundle,
          "共有: 同じ上限に Write と sequence の実 byte を合わせて数える (" +
              std::to_string(cache.residentBytes()) + ")");
    cache.setResidentMemoryBudget(writeBytes + bundle - 1);
    settle(200);
    check(!pump(bothResident, 1500) && cache.residentBytes() <= writeBytes + bundle - 1,
          "共有: 1 byte 足りなければ両方は置けない (別の上限を持たない)");
    controller.shutdown();
}

void testStaleAndCorrupt() {
    auto f = openFixture("古い", fixtureProject());
    auto& controller = *f->controller;
    auto& cache = controller.mathRastersForTest();
    check(waitDiskReady(controller), "古い: disk の artifact が Ready");
    check(waitShown(controller, 35, shownOf(expectedAt(35))), "古い: 変形の中央を見せる");
    const auto oldKey = controller.equationSequencePreviewStatus(kClip, 35).sequenceKey;
    const auto edit = [&](std::int64_t hold) {
        return controller.editEquationSequenceData(
            kClip.toStdString(),
            [hold](project::EquationSequenceClipData& data, std::string& error) {
                return project::changeEquationHold(data, {"s2"}, hold, 1080, error);
            });
    };

    // 1. 編集で key が変わった。新しい描画を止めている間、古い key の変形を見せない。
    f->backend.equationGate->store(true);
    check(edit(25), "古い: 状態 2 の hold を変える (sequence の key が変わる)");
    const auto pending = controller.equationSequencePreviewStatus(kClip, 35);
    check(pending.sequenceKey != oldKey && pending.disk == MathRasterCache::State::Pending,
          "古い: 新しい key は Pending");
    check(shownAt(controller, 35) == fallbackOf(expectedAt(35)),
          "古い: 新しい artifact が揃うまで前の key の変形を使わず、前の状態の静止");
    check(cache.residentEquationLayerCount() == 0, "古い: 前の key の層は memory から外す");
    f->backend.equationGate->store(false);
    check(waitShown(controller, 35, shownOf(expectedAt(35)), 20000),
          "古い: 新しい artifact が揃えば同じ frame の変形");

    // 2. 前の key の読み込みが編集の後に終わっても、今の frame に入れない。
    const auto keyBefore = controller.equationSequencePreviewStatus(kClip, 35).sequenceKey;
    cache.setResidentMemoryBudget(MathRasterCache::kDefaultResidentMemoryBudget);
    cache.holdResidentLoadsForTest(true);
    shownAt(controller, 35);
    check(pump([&] { return cache.heldResidentLoadCountForTest() == 1; }),
          "古い: 前の key の層を読み終えて留める");
    f->backend.equationGate->store(true);
    check(edit(30), "古い: もう一度編集する (key が戻るのではなく別の値)");
    cache.holdResidentLoadsForTest(false);
    settle(100);
    const auto afterLate = controller.equationSequencePreviewStatus(kClip, 35);
    check(afterLate.sequenceKey != keyBefore && cache.residentEquationLayerCount() == 0 &&
              shownAt(controller, 35) == fallbackOf(expectedAt(35)),
          "古い: 編集の後に終わった前の key の層は使わない (静止の代用)");
    f->backend.equationGate->store(false);
    check(waitShown(controller, 35, shownOf(expectedAt(35)), 20000), "古い: 今の key の変形");

    // 3. disk の層を壊す・provenance を消す: memory へ読むときに見つけ、静止で見せる。
    struct Corruption {
        const char* name;
        std::int64_t frame;
        std::function<std::filesystem::path(const app::EquationSequenceArtifact&)> target;
        bool remove;
    };

    const std::vector<Corruption> corruptions = {
        {"変形の frame", 35, [](const auto& a) { return a.transitions[0].frames[5].path; }, false},
        {"action の base", 19, [](const auto& a) { return a.actions[1].base.path; }, false},
        {"action の accent", 19, [](const auto& a) { return a.actions[1].accent[3].path; }, false},
        {"provenance", 35,
         [&](const auto&) {
             return cache.cacheDirectory() / L"equation-sequence" /
                    (controller.equationSequencePreviewStatus(kClip, 35).sequenceKey.toStdString() +
                     ".txt");
         },
         true},
    };
    for (const auto& corruption : corruptions) {
        const auto spec = renderSpecOf(controller);
        const auto artifact = spec ? cache.equationSequenceArtifactOf(*spec) : std::nullopt;
        if (!artifact) {
            check(false, std::string("壊れ: artifact がある: ") + corruption.name);
            continue;
        }
        const auto path = corruption.target(*artifact);
        std::error_code error;
        if (corruption.remove) {
            std::filesystem::remove(path, error);
        } else {
            auto bytes = readA8(path);
            for (auto& value : bytes)
                value = static_cast<std::uint8_t>(value ^ 0x5A);
            std::ofstream(path, std::ios::binary | std::ios::trunc)
                .write(reinterpret_cast<const char*>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()));
        }
        // memory の層を捨てて、disk から読み直させる。
        cache.setResidentMemoryBudget(MathRasterCache::kDefaultResidentMemoryBudget);
        const auto expected = expectedAt(corruption.frame);
        check(pump([&] {
                  shownAt(controller, corruption.frame);
                  const auto status =
                      controller.equationSequencePreviewStatus(kClip, corruption.frame);
                  return status.disk == MathRasterCache::State::Failed &&
                         status.diskFailure == math::EquationBackendFailure::CorruptFrame;
              }),
              std::string("壊れ: ") + corruption.name + " を読むと disk を Failed (CorruptFrame)");
        check(shownAt(controller, corruption.frame) == fallbackOf(expected),
              std::string("壊れ: ") + corruption.name +
                  " の後は静止だけ (壊れた animation を見せない)");
        // 再試行で描き直すと戻る。
        controller.retryMathRendering();
        check(backendAvailable(controller) &&
                  waitShown(controller, corruption.frame, shownOf(expected), 20000),
              std::string("壊れ: 再試行で描き直して戻る: ") + corruption.name);
    }
    controller.shutdown();
}

void testCompileFailureBackendAndEffects() {
    {
        auto f = openFixture("compile", fixtureProject());
        auto& controller = *f->controller;
        check(waitDiskReady(controller) && waitShown(controller, 19, shownOf(expectedAt(19))),
              "compile: pulse を見せる");
        check(controller.editEquationSequenceData(
                  kClip.toStdString(),
                  [](project::EquationSequenceClipData& data, std::string& error) {
                      return project::replaceEquationSource(data, {"s0"}, "SIZE44x20+b", "rev-2",
                                                            1080, error);
                  }),
              "compile: 状態 0 の式を置き換える (binding は Invalid)");
        const auto before = controller.projectForTest();
        const auto status = controller.equationSequencePreviewStatus(kClip, 19);
        check(status.compile == app::EquationCompileFailure::InvalidBinding,
              "compile: 失敗の理由 (InvalidBinding) を保つ");
        check(waitShown(controller, 19,
                        EquationPreviewShown{EquationPreviewShownKind::Static, 0, 0}) &&
                  waitShown(controller, 35,
                            EquationPreviewShown{EquationPreviewShownKind::Static, 0, 0}),
              "compile: 今の状態の静止だけ (前の spec の action・変形を出さない)");
        const auto layer = sequenceLayer(controller, 19);
        bool newStatic = false;
        if (layer) {
            const auto pixels = presentedPixels(*layer, 19);
            // 新しい状態 0 の静止は 44x20 (中央 938, 530)。
            const auto at = (static_cast<std::size_t>(530) * kW + 938U) * 4U;
            const auto outside = (static_cast<std::size_t>(530) * kW + 937U) * 4U;
            newStatic = pixels[at + 3] == 255 && pixels[outside + 3] == 0;
        }
        check(newStatic, "compile: 静止は今の式 (44x20) の描画");
        check(controller.projectForTest() == before, "compile: preview は Project を直さない");
        controller.shutdown();
    }
    {
        auto f = openFixture("backend", fixtureProject());
        auto& controller = *f->controller;
        controller.setMathPreflightForTest(FakeMathBackend::unavailable("fake: Manim が無い"));
        check(pump([&] {
                  return controller.mathRastersForTest().backendState() ==
                         MathRasterCache::BackendState::Unavailable;
              }),
              "backend: 使えない");
        const auto status = controller.equationSequencePreviewStatus(kClip, 19);
        check(status.backend == MathRasterCache::BackendState::Unavailable &&
                  status.disk == MathRasterCache::State::Unavailable &&
                  status.compile == app::EquationCompileFailure::None,
              "backend: compile の失敗と区別して Unavailable");
        check(!sequenceLayer(controller, 19) &&
                  shownAt(controller, 19).kind == EquationPreviewShownKind::None,
              "backend: 静止も描けないので何も見せない (前の画素で代用しない)");
        controller.shutdown();
    }
    {
        auto initial = fixtureProject();
        auto& clip = initial.timelineClips.front();
        clip.effects.positionXPercent = 10;
        clip.effects.scaleXPercent = 150;
        clip.effects.scaleYPercent = 150;
        clip.effects.rotationDegrees = 15;
        clip.effects.opacityPercent = 50;
        auto f = openFixture("effects", initial);
        auto& controller = *f->controller;
        check(waitDiskReady(controller) && waitShown(controller, 19, shownOf(expectedAt(19))),
              "effects: pulse を見せる");
        const auto layer = sequenceLayer(controller, 19);
        // 期待値: layer へ 1 回だけ掛けた ClipEffects (数式・画像と同じ写像)。
        mvm::preview::PreviewCompositionLayer once;
        app::applyPreviewLayerEffects(once, project::evaluateClipEffects(clip.effects, 19), 0.5, 0,
                                      kLength);
        check(layer && layer->destination == once.destination &&
                  layer->sourceRect == once.sourceRect && std::abs(layer->opacity - 0.5F) < 1e-6F &&
                  std::abs(layer->rotationDegrees - 15.0F) < 1e-4F && layer->effectsEnabled,
              "effects: 位置・拡大・回転・不透明度を layer に 1 回だけ掛ける");
        const auto reference = referenceFor(controller);
        check(layer && reference && presentedPixels(*layer, 19) == reference->image(expectedAt(19)),
              "effects: sequence の内側の画素には effect を掛けない");
        controller.shutdown();
    }
}

void testVisibleSetAndStatus() {
    auto initial = fixtureProject();
    // 同じ sequence を別の時刻 (見えない位置) にもう 1 本置く。
    auto other = fixtureData();
    for (auto& s : other.states)
        s.equation.color = "#FFFFFFFF";
    check(project::addEquationSequence(initial, other, "far", "見えない",
                                       {project::TrackKind::Video, 0}, 500)
              .success,
          "可視: 2 本目を置く");
    auto f = openFixture("可視", initial);
    auto& controller = *f->controller;
    auto& cache = controller.mathRastersForTest();
    check(waitDiskReady(controller) && waitShown(controller, 19, shownOf(expectedAt(19))),
          "可視: 見えている sequence を見せる");
    settle(200);
    check(cache.equationSequenceRecordCount() == 1 && f->backend.equationRenders->load() == 1,
          "可視: 見えない sequence は描かない (Project にあるだけで要求しない)");
    // 小さい上限でも、見えない sequence は今の sequence の層を追い出さない (要求しないので)。
    const auto reference = referenceFor(controller);
    if (reference) {
        const auto& pulse = reference->artifact.actions[1];
        cache.setResidentMemoryBudget(2U * static_cast<std::size_t>(pulse.width) *
                                      static_cast<std::size_t>(pulse.height));
        check(waitShown(controller, 19, shownOf(expectedAt(19))) &&
                  cache.equationSequenceRecordCount() == 1,
              "可視: 見えない sequence と上限を取り合わない");
    }
    controller.shutdown();
}

// P3-4.1: 先読みで待っている層へ直接 seek すると、その層を残りの先読みより先に読む。
// worker (1 本) を止めて待ち行列を作り、取り出した順を見る。
void testPrefetchPromotion() {
    auto f = openFixture("昇格", fixtureProject());
    auto& controller = *f->controller;
    auto& cache = controller.mathRastersForTest();
    check(waitDiskReady(controller), "昇格: disk の artifact が Ready");
    const auto spec = renderSpecOf(controller);
    if (!spec) {
        check(false, "昇格: 描画要求");
        return;
    }
    cache.setResidentMemoryBudget(MathRasterCache::kDefaultResidentMemoryBudget);
    settle(100);
    cache.pauseEquationLayerLoadsForTest(true);
    const auto takenBefore = cache.equationLayerLoadOrderForTest().size();
    using Role = MathRasterCache::EquationLayerRole;
    // 先読みの待ち行列: T0 の 10 枚、pulse の 6 束 (base と accent)。
    for (std::int64_t i = 0; i < 10; ++i)
        cache.residentEquationLayers(*spec, {{Role::TransitionFrame, 0, i}}, false);
    for (std::int64_t i = 0; i < 6; ++i)
        cache.residentEquationLayers(*spec, {{Role::ActionBase, 1, 0}, {Role::ActionAccent, 1, i}},
                                     false);
    const QString key = cache.equationSequenceKeyFor(*spec);
    const QString target = MathRasterCache::equationLayerKey(key, {Role::TransitionFrame, 0, 7});
    check(cache.equationLayerLoadOrderForTest().size() == takenBefore,
          "昇格: worker を止めている間は何も取り出さない");
    // 待っている T0 の frame 7 (output 37) へ直接行く。今の frame の束になる。
    const auto playhead = controller.playheadFrame();
    check(shownAt(controller, 37) == fallbackOf(expectedAt(37)),
          "昇格: 読み込みの前は前の状態の静止");
    cache.pauseEquationLayerLoadsForTest(false);
    check(waitShown(controller, 37, shownOf(expectedAt(37))),
          "昇格: 届いた後は同じ output frame の T0 frame 7");
    check(controller.playheadFrame() == playhead, "昇格: 再生位置は変えない");
    const auto order = cache.equationLayerLoadOrderForTest();
    std::size_t at = order.size();
    for (std::size_t i = takenBefore; i < order.size(); ++i)
        if (order[i] == target) {
            at = i - takenBefore;
            break;
        }
    std::fprintf(stderr, "昇格: 取り出した %zu 件のうち対象は %zu 番目\n",
                 order.size() - takenBefore, at);
    check(at == 0, "昇格: 今の frame になった層は残りの先読みより先に読む");
    // 予約は 1 回だけ (移した仕事は予約をそのまま持つ): 全部読めた後の量は層の実 byte の和。
    // 今の frame が揃った後の通常の先読みで、次の区間 (N=1 の pulse の base と accent) も読む。
    const auto reference = referenceFor(controller);
    if (reference) {
        const auto& t0 = reference->artifact.transitions[0];
        const auto& pulse = reference->artifact.actions[1];
        const auto& single = reference->artifact.actions[2];
        const std::size_t expected =
            10U * static_cast<std::size_t>(t0.width) * static_cast<std::size_t>(t0.height) +
            7U * static_cast<std::size_t>(pulse.width) * static_cast<std::size_t>(pulse.height) +
            2U * static_cast<std::size_t>(single.width) * static_cast<std::size_t>(single.height);
        check(pump([&] { return cache.residentEquationLayerCount() >= 19; }) &&
                  cache.residentBytes() == expected,
              "昇格: 予約は層ごとに 1 回だけ (" + std::to_string(cache.residentBytes()) + " / " +
                  std::to_string(expected) + ")");
    }
    controller.shutdown();
}

// P3-4.1: 状態の問い合わせは読むだけ。再生位置に無い sequence を問い合わせても描画を始めない。
void testStatusIsReadOnly() {
    auto initial = fixtureProject();
    auto other = fixtureData();
    other.states[0].equation.source = "SIZE41x21+b";
    other.states[1].equation.source = "SIZE31x25+b";
    other.states[2].equation.source = "SIZE37x19+c";
    for (auto& s : other.states)
        for (auto& p : s.parts)
            p.binding.revision = s.revision;
    check(project::addEquationSequence(initial, other, "far", "再生位置の外",
                                       {project::TrackKind::Video, 0}, 500)
              .success,
          "問い合わせ: 再生位置の外に 2 本目を置く");
    auto f = openFixture("問い合わせ", initial);
    auto& controller = *f->controller;
    auto& cache = controller.mathRastersForTest();
    check(waitDiskReady(controller), "問い合わせ: 見えている sequence は Ready");
    settle(200);
    const auto records = cache.recordCount();
    const auto sequences = cache.equationSequenceRecordCount();
    const auto renders = f->backend.renders->load();
    const auto equationRenders = f->backend.equationRenders->load();
    const auto loads = cache.residentLoadCount();
    bool pending = true;
    for (qint64 frame = 500; frame < 500 + kLength; ++frame) {
        const auto status = controller.equationSequencePreviewStatus(QStringLiteral("far"), frame);
        pending = pending && status.found && status.disk == MathRasterCache::State::Pending &&
                  status.residency == MathRasterCache::Residency::NotReady &&
                  std::none_of(status.staticsReady.begin(), status.staticsReady.end(),
                               [](bool ready) { return ready; });
    }
    for (qint64 frame = 0; frame < kLength; ++frame)
        controller.equationSequencePreviewStatus(kClip, frame);
    settle(300);
    check(pending, "問い合わせ: 再生位置の外の sequence は Pending・静止も未描画のまま");
    check(cache.recordCount() == records && cache.equationSequenceRecordCount() == sequences,
          "問い合わせ: cache の record を作らない (静止 " + std::to_string(cache.recordCount()) +
              " / " + std::to_string(records) + "、sequence " +
              std::to_string(cache.equationSequenceRecordCount()) + " / " +
              std::to_string(sequences) + ")");
    check(f->backend.renders->load() == renders &&
              f->backend.equationRenders->load() == equationRenders &&
              cache.residentLoadCount() == loads,
          "問い合わせ: 描画・層の読み込みを始めない");
    controller.shutdown();
}

} // namespace

// ---- 実 D3D11 (--native) ----
namespace {

struct Observation {
    std::mutex mutex;
    std::vector<
        std::tuple<std::int64_t, std::optional<app::EquationPreviewTime>, EquationPreviewShown>>
        records;

    void clear() {
        std::lock_guard lock(mutex);
        records.clear();
    }

    auto snapshot() {
        std::lock_guard lock(mutex);
        return records;
    }
};

struct PreviewWindowHarness {
    QQuickWindow window;
    mvm::app::PreviewEngineRhiItem* surface = nullptr;

    void attach(MvmController& controller) {
        // 入力を送らない。前面とフォーカスを奪わず、OS のマウス入力も透過させる。
        window.setFlags(mvm::app::testBackgroundWindowFlags());
        window.resize(640, 360);
        surface = new mvm::app::PreviewEngineRhiItem(window.contentItem());
        surface->setWidth(640);
        surface->setHeight(360);
        controller.attachPreview(surface);
        window.show();
    }
};

bool seekWhenReady(MvmController& controller, qint64 frame) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (controller.seekTimelineFrame(frame))
            return true;
        settle(50);
    }
    std::fprintf(stderr, "seek できません: %s\n", qUtf8Printable(controller.statusText()));
    return false;
}

template<class T>
struct ComRelease {
    void operator()(T* pointer) const {
        if (pointer)
            pointer->Release();
    }
};

// 製品の layer (透明な下地 + animation + layer の幾何) を、engine と同じ手順 (更新可能な静止画の
// texture を作り、state の patch を書き込む) で product compositor に通し、出力全体を読む。
struct GpuReadback {
    std::unique_ptr<ID3D11Device, ComRelease<ID3D11Device>> device;
    std::unique_ptr<ID3D11DeviceContext, ComRelease<ID3D11DeviceContext>> context;
    mvm::gpu::SharedD3D11Device shared;
    mvm::gpu::ReadbackCounters readbacks;
    mvm::gpu::GpuCompositor compositor;
    bool ready = false;

    bool open() {
        ID3D11Device* rawDevice = nullptr;
        ID3D11DeviceContext* rawContext = nullptr;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL selected{};
        if (FAILED(D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                D3D11_SDK_VERSION, &rawDevice, &selected, &rawContext)))
            return false;
        device.reset(rawDevice);
        context.reset(rawContext);
        std::string error;
        ready = shared.adopt(device.get(), context.get(), error) &&
                compositor.initialize(shared, readbacks, kW, kH, error);
        if (!ready)
            std::fprintf(stderr, "GPU compositor: %s\n", error.c_str());
        return ready;
    }

    std::vector<unsigned char> compose(const mvm::preview::PreviewCompositionLayer& layer,
                                       std::int64_t frame) {
        std::string error;
        mvm::gpu::DecodedGpuFrame still;
        const auto& image = *layer.stillImage;
        if (!mvm::gpu::makeUpdatableStillImageFrame(shared, image.width, image.height,
                                                    image.rgba.data(), image.rgba.size(), {}, still,
                                                    error))
            return {};
        const auto state = layer.stillAnimation->stateAt(frame);
        if (state >= 0) {
            const auto rect = layer.stillAnimation->patchRect();
            std::vector<std::uint8_t> patch(static_cast<std::size_t>(rect.width) *
                                            static_cast<std::size_t>(rect.height) * 4U);
            layer.stillAnimation->fillPatch(state, patch.data());
            if (!mvm::gpu::updateStillImageRegion(shared, still, rect.x, rect.y, rect.width,
                                                  rect.height, patch.data(), patch.size(), error))
                return {};
        }
        mvm::gpu::CompositionLayerFrame composed;
        composed.frame = still;
        composed.destination = {layer.destination.x, layer.destination.y, layer.destination.width,
                                layer.destination.height};
        composed.sourceUv = {layer.sourceRect.x, layer.sourceRect.y, layer.sourceRect.width,
                             layer.sourceRect.height};
        composed.effectsEnabled = layer.effectsEnabled;
        composed.opacity = layer.opacity;
        composed.rotationDegrees = layer.rotationDegrees;
        mvm::gpu::ComposedFrame frameToCompose;
        frameToCompose.compositionEpoch = {static_cast<std::uint64_t>(frame + 1)};
        frameToCompose.layers.push_back(composed);
        D3D11_TEXTURE2D_DESC description{};
        description.Width = kW;
        description.Height = kH;
        description.MipLevels = description.ArraySize = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BindFlags = D3D11_BIND_RENDER_TARGET;
        ID3D11Texture2D* rawTexture = nullptr;
        ID3D11RenderTargetView* rawTarget = nullptr;
        if (SUCCEEDED(device->CreateTexture2D(&description, nullptr, &rawTexture)))
            device->CreateRenderTargetView(rawTexture, nullptr, &rawTarget);
        std::unique_ptr<ID3D11Texture2D, ComRelease<ID3D11Texture2D>> texture(rawTexture);
        std::unique_ptr<ID3D11RenderTargetView, ComRelease<ID3D11RenderTargetView>> target(
            rawTarget);
        if (!texture || !target)
            return {};
        {
            std::lock_guard<mvm::gpu::D3D11Lock> lock(shared.lock());
            const float black[] = {0, 0, 0, 1};
            context->ClearRenderTargetView(target.get(), black);
        }
        bool success =
            compositor.composeLayersToTarget(frameToCompose, {target.get(), kW, kH}, 1, error);
        std::vector<unsigned char> result;
        for (int y = 0; success && y < kH; y += 60) {
            std::vector<unsigned char> band;
            success = compositor.readExternalOutputProbe(texture.get(), 0, y, kW, 60, band, error);
            result.insert(result.end(), band.begin(), band.end());
        }
        compositor.retireLayerTexture(still.texture);
        if (!success)
            std::fprintf(stderr, "GPU 合成: %s\n", error.c_str());
        return success ? result : std::vector<unsigned char>{};
    }
};

// CPU の参照 (straight alpha) を黒の上へ source-over した値 round(色 * alpha / 255) と GPU の
// 画素の違い。alpha 0 / 255 と半透明の画素を分けて数える。
struct GpuAgreement {
    std::size_t exactMismatch = 0; // alpha 0 / 255 の画素で 1 でも違う画素
    std::size_t partialPixels = 0; // 0 < alpha < 255 の画素
    int partialMaxDifference = 0;  // その画素の channel の最大差
    std::size_t covered = 0;       // alpha > 0 の画素
};

GpuAgreement compareGpu(const std::vector<unsigned char>& gpu,
                        const std::vector<std::uint8_t>& reference) {
    GpuAgreement result;
    for (std::size_t at = 0; at + 3 < reference.size() && at + 3 < gpu.size(); at += 4) {
        const int alpha = reference[at + 3];
        result.covered += alpha > 0;
        int maxDifference = 0;
        for (std::size_t ch = 0; ch < 3; ++ch) {
            const int wanted = (reference[at + ch] * alpha + 127) / 255;
            maxDifference =
                std::max(maxDifference, std::abs(static_cast<int>(gpu[at + ch]) - wanted));
        }
        if (alpha == 0 || alpha == 255) {
            result.exactMismatch += maxDifference != 0;
        } else {
            ++result.partialPixels;
            result.partialMaxDifference = std::max(result.partialMaxDifference, maxDifference);
        }
    }
    return result;
}

// 実 D3D11 の受け入れ。manim が空なら偽の backend、あれば実 Manim (CTest に入れない)。
int nativePreview(const project::Project& initial, const std::filesystem::path& manim) {
    QTemporaryDir temp;
    check(temp.isValid(), "native: 作業 directory");
    const bool real = !manim.empty();
    const auto path = std::filesystem::path(temp.filePath("native p34.mvm").toStdWString());
    check(project::saveProjectJson(initial, path).success, "native: 作業用の保存");
    FakeMathBackend backend;
    auto controller = makeController(path, initial, manim);
    PreviewWindowHarness harness;
    harness.attach(*controller);
    check(pump([&] { return controller->previewReady(); }), "native: preview の初期化");
    if (!real)
        controller->setMathPreflightForTest(backend.preflight());
    check(pump(
              [&] {
                  return controller->mathRastersForTest().backendState() ==
                         MathRasterCache::BackendState::Available;
              },
              real ? 180000 : 10000),
          "native: backend が使える: " +
              controller->mathRastersForTest().backendMessage().toStdString());
    auto& cache = controller->mathRastersForTest();
    if (real)
        std::fprintf(stderr, "native: toolchain\n%s\n", qUtf8Printable(cache.toolchainText()));
    QElapsedTimer renderTimer;
    renderTimer.start();
    check(waitDiskReady(*controller, real ? 900000 : 20000),
          "native: disk の artifact が Ready: " +
              controller->equationSequencePreviewStatus(kClip, 0).diskMessage.toStdString());
    std::fprintf(stderr, "native: sequence の描画 %lld ms\n",
                 static_cast<long long>(renderTimer.elapsed()));
    auto observation = std::make_shared<Observation>();
    controller->setEquationPreviewObserverForTest(
        [observation](const std::string& clipId, std::int64_t frame,
                      const std::optional<app::EquationPreviewTime>& time,
                      const EquationPreviewShown& shown) {
            if (clipId != kClip.toStdString())
                return;
            std::lock_guard lock(observation->mutex);
            observation->records.emplace_back(frame, time, shown);
        });
    const auto evaluated = [&](std::int64_t frame, const EquationPreviewShown& wanted) {
        for (const auto& [at, time, shown] : observation->snapshot())
            if (at == frame && shown == wanted)
                return true;
        return false;
    };

    // 1. 一時停止中に区間の途中へ直接 seek。層は disk Ready だが memory に無く、読み込みを留める。
    for (const auto& [name, frame] :
         {std::pair{"pulse の中央", std::int64_t{19}}, std::pair{"T0 の中央", std::int64_t{35}}}) {
        const auto expected = expectedAt(frame);
        cache.setResidentMemoryBudget(MathRasterCache::kDefaultResidentMemoryBudget);
        cache.holdResidentLoadsForTest(true);
        observation->clear();
        check(seekWhenReady(*controller, frame) && pump([&] {
                  return controller->previewPresentedLatest() &&
                         evaluated(frame, fallbackOf(expected)) &&
                         cache.heldResidentLoadCountForTest() > 0;
              }),
              std::string("native 1: ") + name + " へ seek すると、層が届くまで静止を提示");
        const auto rebuilds = controller->playbackRebuildCount();
        cache.holdResidentLoadsForTest(false);
        check(pump([&] { return evaluated(frame, shownOf(expected)); }),
              std::string("native 1: 届いた後、同じ frame の ") + name + " の frame を提示");
        check(controller->playheadFrame() == frame && !controller->playing() &&
                  controller->playbackRebuildCount() == rebuilds,
              std::string("native 1: seek・再生をやり直さない: ") + name);
    }

    // 2. 再生中に層が届く: 届く前は静止、届いた後は timeline の frame の区間の frame。
    cache.setResidentMemoryBudget(MathRasterCache::kDefaultResidentMemoryBudget);
    cache.holdResidentLoadsForTest(true);
    check(seekWhenReady(*controller, 0) &&
              pump([&] { return controller->previewPresentedLatest(); }),
          "native 2: 先頭へ戻る");
    observation->clear();
    check(controller->playTimeline(), "native 2: 再生する");
    check(pump([&] { return controller->playheadFrame() >= 18; }),
          "native 2: pulse の途中まで進む");
    const auto released = controller->playheadFrame();
    cache.holdResidentLoadsForTest(false);
    check(pump([&] { return controller->playheadFrame() >= 60; }), "native 2: 一時停止せずに進む");
    const bool stillPlaying = controller->playing();
    controller->pauseTimeline();
    const auto records = observation->snapshot();
    std::size_t animated = 0;
    bool authoritative = true;
    bool pulseRestart = false;
    std::int64_t firstPulse = -1;
    for (const auto& [frame, time, shown] : records) {
        if (frame < 0 || frame >= kLength)
            continue;
        const auto expected = expectedAt(frame);
        const bool ok = shown == shownOf(expected) || shown == fallbackOf(expected);
        authoritative = authoritative && ok && time && time->sourceFrame == frame;
        if (shown.kind != EquationPreviewShownKind::Static)
            ++animated;
        if (shown.kind == EquationPreviewShownKind::Action && shown.index == 1) {
            if (firstPulse < 0)
                firstPulse = frame;
            pulseRestart = pulseRestart || shown.frame != frame - 16;
        }
    }
    std::fprintf(stderr,
                 "native 2: 評価 %zu 件 (animation %zu 件)、届けた時の再生位置 %lld、"
                 "pulse を最初に見せた frame %lld\n",
                 records.size(), animated, static_cast<long long>(released),
                 static_cast<long long>(firstPulse));
    check(stillPlaying, "native 2: 層が届いても再生を止めない");
    check(authoritative, "native 2: 全評価で区間の frame か、その区間の代用 (output frame が正)");
    check(animated > 0 && !pulseRestart,
          "native 2: 再生中に届いた層は timeline の frame の区間の frame で見せ、やり直さない");
    check(firstPulse < 0 || firstPulse > 16,
          "native 2: 途中から見せ始めた pulse に frame 0 を出さない");

    // 3. 受け入れの frame を、engine と同じ手順で product compositor に通して読む。
    check(loadAllIntervals(*controller), "native 3: 全区間の層を memory に読む");
    const auto reference = referenceFor(*controller);
    GpuReadback gpu;
    check(gpu.open(), "native 3: 実 D3D11 の product compositor");
    if (reference && gpu.ready) {
        for (const auto& item : kAcceptance) {
            const auto expected = expectedAt(item.frame);
            waitShown(*controller, item.frame, shownOf(expected));
            const auto layer = sequenceLayer(*controller, item.frame);
            if (!layer) {
                check(false, std::string("native 3: layer: ") + item.name);
                continue;
            }
            const auto composed = gpu.compose(*layer, item.frame);
            const auto exact = reference->image(expected);
            const auto agreement = compareGpu(composed, exact);
            std::fprintf(
                stderr,
                "native 3: %-24s 被覆 %zu、alpha 0/255 の不一致 %zu、半透明 %zu (最大差 %d)\n",
                item.name, agreement.covered, agreement.exactMismatch, agreement.partialPixels,
                agreement.partialMaxDifference);
            // 許容差を置かない (半透明の画素も CPU の参照を黒へ重ねた値と完全一致)。
            check(!composed.empty() && agreement.covered > 0 && agreement.exactMismatch == 0 &&
                      agreement.partialMaxDifference == 0,
                  std::string("native 3: GPU の合成は CPU の参照の source-over と一致: ") +
                      item.name);
            if (expected.kind != EquationPreviewShownKind::Static) {
                check(compareGpu(composed, reference->image(expected, Variant::Shifted))
                              .exactMismatch > 0,
                      std::string("native 3 対照: 配置を 1 画素ずらすと検出: ") + item.name);
                check(compareGpu(composed, reference->image(expected, Variant::WrongColor))
                              .exactMismatch > 0,
                      std::string("native 3 対照: 色を取り違えると検出: ") + item.name);
            }
            if (expected.kind == EquationPreviewShownKind::Action)
                check(compareGpu(composed, reference->image(expected, Variant::OmitAccent))
                              .exactMismatch > 0,
                      std::string("native 3 対照: accent を省くと検出: ") + item.name);
        }
    }
    std::string error;
    if (gpu.ready)
        gpu.compositor.shutdown(5000, error);
    controller->shutdown();
    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--write-fixture") {
        const auto saved =
            project::saveProjectJson(fixtureProject(), std::filesystem::path(argv[2]));
        std::fprintf(stderr, "%s\n", saved.success ? "fixture を書きました" : saved.error.c_str());
        return saved.success && failures == 0 ? 0 : 1;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--native") {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
        QQuickStyle::setStyle(QStringLiteral("Basic"));
        QGuiApplication app(argc, argv);
        // schema 21 の fixture を読み、試験の中の定義と同じ data であること (取り違え防止)。
        const auto loaded = project::loadProjectJson(
            std::filesystem::path(QGuiApplication::arguments()[2].toStdWString()));
        check(loaded.success && loaded.project.schemaVersion == 21,
              "native: schema 21 の fixture を読める: " + loaded.error);
        if (!loaded.success)
            return 1;
        const auto clip =
            std::find_if(loaded.project.timelineClips.begin(), loaded.project.timelineClips.end(),
                         [](const auto& c) { return c.id == kClip.toStdString(); });
        check(clip != loaded.project.timelineClips.end() &&
                  clip->kind == project::TimelineClipKind::EquationSequence &&
                  clip->equationSequence == fixtureData() && clip->sourceFrameCount == kLength,
              "native: fixture は試験の区間の表と同じ sequence");
        return nativePreview(loaded.project, {});
    }
    // 実 Manim の製品 preview の受け入れ (CTest に登録しない。画面を表示するが入力は送らない)。
    if (argc == 3 && std::string_view(argv[1]) == "--real-manim") {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
        QQuickStyle::setStyle(QStringLiteral("Basic"));
        QGuiApplication app(argc, argv);
        return nativePreview(projectWith(realFixtureData()),
                             std::filesystem::path(QGuiApplication::arguments()[2].toStdWString()));
    }
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QGuiApplication app(argc, argv);
    testPresentationAndPixels();
    testDelayedDirectSeek();
    testAtomicBundleAndBudget();
    testSharedBudget();
    testStaleAndCorrupt();
    testCompileFailureBackendAndEffects();
    testVisibleSetAndStatus();
    testPrefetchPromotion();
    testStatusIsReadOnly();
    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}
