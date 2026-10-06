#ifndef MVM_TESTS_MATH_FAKE_BACKEND_H
#define MVM_TESTS_MATH_FAKE_BACKEND_H

// 数式の偽の backend (MathRasterCache の preflight 関数として注入する)。
// 式の内容で振る舞いを変え、描画の回数を数える。
//   "BAD"   TeX の誤り (InvalidSource、message は "Undefined control sequence.")
//   "GONE"  描画中に toolchain が消えた (BackendUnavailable)
//   "SLOW"  cancel されるまで待つ (最長 30 秒)。Write の連番でも同じ
//   "WIDE" を含む式  64x8 の白い帯 (位置で frame を見分ける試験用)。Write の frame i は
//                    左から 64 * i / frames 列だけが不透明 (frame 0 は空)
//   "GATE" を含む式  staticGate が true の間は静止の描画を終えない (cancel で止まる)
//   "SIZE<w>x<h>" を含む式  w x h の全面が不透明 (alpha 255) な白の静止 (偶奇を変える試験用)。
//                           Write の frame i は左から w * i / frames 列だけが不透明
//   それ以外  3x2 の白い glyph の PNG (math_test_png.h) を書いて Ok。Write も各 frame が同じ PNG
//
// 変形 (renderTransform): canvas は大きい方の端点の静止 + 各辺 kFakeTransformPadding。端点は
// mathTransformPlacement の位置に置く。frame 0 は source の静止の mask だけ、frame 1 以降は
// target の静止の mask と、canvas の (2, 3) の 1 画素 (alpha 200)。artifact は両端の静止の矩形と
// その 1 画素の和。受け取った両端の mask を記録する。
//   transformCancellableGate が true の間は、cancel されるか gate が下りるまで待つ。cancel なら
//   Cancelled を返し、transformSawCancel を立てる (取消を見る renderer を真似る)
//   transformGate が true の間は終えない (cancel を見ずに待つ。取り消された後に結果を返す
//   renderer を真似る)
//   target が "BADT" を含む  InvalidSource
//   target が "GONET" を含む BackendUnavailable (変形の描画中に toolchain が消えた)
//   source が "LIE" を含む   artifact の矩形を (2, 3) の画素を含めずに報告する
//   source が "SHIFT" を含む frame 0 の source を右へ 1 画素ずらして描く

#include "math_raster_cache.h"
#include "math_test_png.h"
#include "media/math/equation_sequence_render.h"
#include "media/math/math_transform.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QImage>
#include <QString>

namespace mvm::test {

inline constexpr int kWideMathWidth = 64;
inline constexpr int kWideMathHeight = 8;

// "WIDE" の Write の frame index / frames で不透明にする列の数。試験の期待値もこれを使わずに
// 独立に計算すること (ここは偽の backend の振る舞いの定義)。
inline int wideRevealColumns(std::int64_t index, std::int64_t frames) {
    return static_cast<int>(kWideMathWidth * index / frames);
}

inline bool writeWidePng(const std::filesystem::path& path, int opaqueColumns) {
    QImage image(kWideMathWidth, kWideMathHeight, QImage::Format_RGBA8888);
    image.fill(Qt::transparent);
    for (int y = 0; y < kWideMathHeight; ++y)
        for (int x = 0; x < opaqueColumns; ++x)
            image.setPixelColor(x, y, QColor(255, 255, 255, 255));
    return image.save(QString::fromStdWString(path.wstring()), "PNG");
}

// 式の "SIZE<w>x<h>" (例 "SIZE7x2") の大きさ。無ければ false。
inline bool fakeMathSize(const std::string& source, int& width, int& height) {
    const auto at = source.find("SIZE");
    if (at == std::string::npos)
        return false;
    int w = 0;
    int h = 0;
    if (std::sscanf(source.c_str() + at, "SIZE%dx%d", &w, &h) != 2 || w <= 0 || h <= 0)
        return false;
    width = w;
    height = h;
    return true;
}

inline constexpr int kFakeTransformPadding = 6;
inline constexpr int kFakeTransformExtraX = 2;
inline constexpr int kFakeTransformExtraY = 3;
inline constexpr std::uint8_t kFakeTransformExtraAlpha = 200;

// 1 画素 1 byte の画像の (x, y) の位置。
inline std::size_t coverageIndex(int width, int x, int y) {
    return static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
           static_cast<std::size_t>(x);
}

inline bool writeCoveragePng(const std::filesystem::path& path,
                             const math::MathCoverage& coverage) {
    QImage image(coverage.width, coverage.height, QImage::Format_RGBA8888);
    image.fill(Qt::transparent);
    for (int y = 0; y < coverage.height; ++y)
        for (int x = 0; x < coverage.width; ++x)
            image.setPixelColor(
                x, y, QColor(255, 255, 255, coverage.alpha[coverageIndex(coverage.width, x, y)]));
    return image.save(QString::fromStdWString(path.wstring()), "PNG");
}

inline void stampCoverage(math::MathCoverage& canvas, const math::MathCoverage& mask, int left,
                          int top) {
    for (int y = 0; y < mask.height; ++y)
        for (int x = 0; x < mask.width; ++x)
            canvas.alpha[coverageIndex(canvas.width, left + x, top + y)] =
                mask.alpha[coverageIndex(mask.width, x, y)];
}

// 偽の変形が受け取った両端の静止の mask と、描き終えた順の記録 (events)。
// events は "static:<式>" (静止を描き終えた)・"transform:<target の式>" (変形を描き終えた)・
// "transform-cancelled:<target の式>" (取消を見て止めた)。
struct FakeTransformLog {
    std::mutex mutex;
    std::vector<std::pair<math::MathCoverage, math::MathCoverage>> received;
    std::vector<std::string> events;

    void record(std::string event) {
        std::lock_guard lock(mutex);
        events.push_back(std::move(event));
    }
};

inline math::MathTransformRenderResult
fakeRenderTransform(const math::MathTransformRenderRequest& request,
                    const math::MathCoverageLoader& loader, const std::atomic<bool>* cancel,
                    const std::shared_ptr<std::atomic<bool>>& gate,
                    const std::shared_ptr<std::atomic<bool>>& held) {
    math::MathTransformRenderResult result;
    if (gate->load()) {
        held->store(true);
        for (int i = 0; i < 3000 && gate->load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    (void)cancel;
    if (!loader) {
        result.message = "loader がありません";
        return result;
    }
    const auto& source = request.spec.source.source;
    if (request.spec.target.source.find("BADT") != std::string::npos) {
        result.status = math::MathRenderStatus::InvalidSource;
        result.message = "fake transform error";
        return result;
    }
    if (request.spec.target.source.find("GONET") != std::string::npos) {
        result.status = math::MathRenderStatus::BackendUnavailable;
        result.message = "fake transform backend gone";
        return result;
    }
    const auto& from = request.sourceStatic;
    const auto& to = request.targetStatic;
    const int canvasWidth = std::max(from.width, to.width) + 2 * kFakeTransformPadding;
    const int canvasHeight = std::max(from.height, to.height) + 2 * kFakeTransformPadding;
    math::MathTransformPlacement placement;
    if (!math::mathTransformPlacement(from.width, from.height, to.width, to.height, canvasWidth,
                                      canvasHeight, placement)) {
        result.message = "配置できません";
        return result;
    }
    const bool shift = source.find("SHIFT") != std::string::npos;
    for (std::int64_t index = 0; index < request.spec.frames; ++index) {
        math::MathCoverage canvas{
            canvasWidth, canvasHeight,
            std::vector<std::uint8_t>(coverageIndex(canvasWidth, 0, canvasHeight), 0)};
        if (index == 0) {
            stampCoverage(canvas, from, placement.source.left + (shift ? 1 : 0),
                          placement.source.top);
        } else {
            stampCoverage(canvas, to, placement.target.left, placement.target.top);
            canvas.alpha[coverageIndex(canvasWidth, kFakeTransformExtraX, kFakeTransformExtraY)] =
                kFakeTransformExtraAlpha;
        }
        wchar_t name[32] = {};
        std::swprintf(name, std::size(name), L"t%04lld.png", static_cast<long long>(index));
        const auto png = request.jobDirectory / name;
        if (!writeCoveragePng(png, canvas)) {
            result.message = "PNG を書けません";
            return result;
        }
        result.frames.push_back(png);
    }
    result.status = math::MathRenderStatus::Ok;
    result.canvasWidth = canvasWidth;
    result.canvasHeight = canvasHeight;
    result.placement = placement;
    result.artifact =
        math::mathRectUnion({placement.source.left, placement.source.top, from.width, from.height},
                            {placement.target.left, placement.target.top, to.width, to.height});
    if (request.spec.frames > 1 && source.find("LIE") == std::string::npos)
        result.artifact = math::mathRectUnion(result.artifact,
                                              {kFakeTransformExtraX, kFakeTransformExtraY, 1, 1});
    return result;
}

// 偽の Equation Sequence (P3-3)。canvas は大きい方の静止 + 各辺 kFakeTransformPadding、端点は
// 中立な配置の規則の位置。変形の frame 0 は前の状態の静止、frame 1 以降は後の状態の静止と
// (2, 3) の 1 画素。action の base は状態の静止、accent の frame i は (2 + i % 3, 3) の 1 画素。
// 構造検証は各 segment が 1 個の glyph を排他的に所有する結果を返す。最初の状態の式の印:
//   "EQFAIL"   構造検証の失敗 (EmptyActionTarget) を返す (Manim の終了コード 0 でも失敗)
//   "EQSHORT"  変形 0 の frame を 1 枚少なく返す (status は Ok)
//   "EQBADPNG" 変形 0 の frame 1 を読めない内容にする (status は Ok)
//   gate が true の間は終えない (cancel を見ずに待つ。取り消された後に結果を返す renderer)
//   cancellableGate が true の間は cancel か gate が下りるまで待つ (取消を見たら Cancelled)
struct FakeEquationLog {
    std::mutex mutex;
    std::vector<std::string> events; // "equation:<最初の状態の式>" / "equation-cancelled:..."

    void record(std::string event) {
        std::lock_guard lock(mutex);
        events.push_back(std::move(event));
    }
};

inline math::EquationSequenceRenderResult fakeRenderEquationSequence(
    const math::EquationSequenceRenderRequest& request, const math::MathCoverageLoader& loader,
    const std::atomic<bool>* cancel, const std::shared_ptr<std::atomic<bool>>& gate,
    const std::shared_ptr<std::atomic<bool>>& cancellableGate,
    const std::shared_ptr<std::atomic<bool>>& held, const std::shared_ptr<FakeEquationLog>& log) {
    math::EquationSequenceRenderResult result;
    const auto& spec = request.spec;
    const std::string first = spec.states.empty() ? std::string() : spec.states[0].still.source;
    if (gate->load()) {
        held->store(true);
        for (int i = 0; i < 3000 && gate->load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (cancellableGate->load()) {
        held->store(true);
        for (int i = 0; i < 3000 && cancellableGate->load() && !cancel->load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (cancel->load()) {
            log->record("equation-cancelled:" + first);
            result.status = math::MathRenderStatus::Cancelled;
            return result;
        }
    }
    if (!loader || request.stateStatics.size() != spec.states.size()) {
        result.message = "要求が不正です";
        return result;
    }
    if (first.find("EQFAIL") != std::string::npos) {
        result.validation.failure = math::EquationBackendFailure::EmptyActionTarget;
        result.validation.detail = "fake: 空の対象";
        result.message = "fake: 空の対象";
        return result;
    }
    for (std::size_t s = 0; s < spec.states.size(); ++s)
        for (std::size_t k = 0; k < spec.states[s].segments.size(); ++k)
            result.validation.segments.push_back(
                {s, k, "MathTexPart", 1, 1, 1, true, {static_cast<std::int64_t>(k)}});
    result.validation.failure = math::EquationBackendFailure::None;
    const auto blank = [](int width, int height) {
        return math::MathCoverage{width, height,
                                  std::vector<std::uint8_t>(coverageIndex(width, 0, height), 0)};
    };
    for (std::size_t t = 0; t < spec.transitions.size(); ++t) {
        const auto& item = spec.transitions[t];
        const auto& from = request.stateStatics[item.fromState];
        const auto& to = request.stateStatics[item.toState];
        math::EquationTransitionRaster raster;
        auto& interval = raster.interval;
        interval.canvasWidth = std::max(from.width, to.width) + 2 * kFakeTransformPadding;
        interval.canvasHeight = std::max(from.height, to.height) + 2 * kFakeTransformPadding;
        math::mathTransformPlacement(from.width, from.height, to.width, to.height,
                                     interval.canvasWidth, interval.canvasHeight, raster.placement);
        const auto& p = raster.placement;
        interval.artifact =
            math::mathRectUnion({p.source.left, p.source.top, from.width, from.height},
                                {p.target.left, p.target.top, to.width, to.height});
        std::int64_t frames = item.frames;
        if (t == 0 && first.find("EQSHORT") != std::string::npos)
            --frames;
        for (std::int64_t i = 0; i < frames; ++i) {
            auto canvas = blank(interval.canvasWidth, interval.canvasHeight);
            if (i == 0) {
                stampCoverage(canvas, from, p.source.left, p.source.top);
            } else {
                stampCoverage(canvas, to, p.target.left, p.target.top);
                canvas.alpha[coverageIndex(canvas.width, kFakeTransformExtraX,
                                           kFakeTransformExtraY)] = kFakeTransformExtraAlpha;
                interval.artifact = math::mathRectUnion(
                    interval.artifact, {kFakeTransformExtraX, kFakeTransformExtraY, 1, 1});
            }
            const auto png = request.jobDirectory /
                             (L"t" + std::to_wstring(t) + L"-" + std::to_wstring(i) + L".png");
            if (t == 0 && i == 1 && first.find("EQBADPNG") != std::string::npos)
                std::ofstream(png, std::ios::binary) << "not a png";
            else
                writeCoveragePng(png, canvas);
            interval.frames.push_back(png);
        }
        result.transitions.push_back(std::move(raster));
    }
    for (std::size_t a = 0; a < spec.actions.size(); ++a) {
        const auto& item = spec.actions[a];
        const auto& still = request.stateStatics[item.state];
        math::EquationActionRaster raster;
        auto& interval = raster.interval;
        interval.canvasWidth = still.width + 2 * kFakeTransformPadding;
        interval.canvasHeight = still.height + 2 * kFakeTransformPadding;
        math::mathEndpointPlacement(still.width, still.height, interval.canvasWidth,
                                    interval.canvasHeight, raster.placement);
        interval.artifact = {raster.placement.left, raster.placement.top, still.width,
                             still.height};
        auto base = blank(interval.canvasWidth, interval.canvasHeight);
        stampCoverage(base, still, raster.placement.left, raster.placement.top);
        raster.base = request.jobDirectory / (L"a" + std::to_wstring(a) + L"-base.png");
        writeCoveragePng(raster.base, base);
        for (std::int64_t i = 0; i < item.duration; ++i) {
            auto canvas = blank(interval.canvasWidth, interval.canvasHeight);
            const int x = 2 + static_cast<int>(i % 3);
            canvas.alpha[coverageIndex(canvas.width, x, 3)] = 255;
            interval.artifact = math::mathRectUnion(interval.artifact, {x, 3, 1, 1});
            const auto png = request.jobDirectory /
                             (L"a" + std::to_wstring(a) + L"-" + std::to_wstring(i) + L".png");
            writeCoveragePng(png, canvas);
            interval.frames.push_back(png);
        }
        result.actions.push_back(std::move(raster));
    }
    log->record("equation:" + first);
    result.status = math::MathRenderStatus::Ok;
    return result;
}

struct FakeMathBackend {
    std::shared_ptr<std::atomic<int>> renders = std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<int>> transformRenders = std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<bool>> staticGate = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> transformGate = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> transformHeld = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> transformCancellableGate =
        std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> transformSawCancel =
        std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<FakeTransformLog> transformLog = std::make_shared<FakeTransformLog>();
    std::shared_ptr<std::atomic<int>> equationRenders = std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<bool>> equationGate = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> equationCancellableGate =
        std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> equationHeld = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<FakeEquationLog> equationLog = std::make_shared<FakeEquationLog>();
    std::string equationTemplate = "fake-equation-sequence/1";
    bool withEquationSequence = true;
    std::string transformTemplate = "fake-transform/1";
    bool withTransform = true;
    std::int64_t maximumTransformFrames = 9998;
    std::shared_ptr<std::atomic<int>> sequenceRenders = std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<bool>> slowStarted = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> slowSawCancel = std::make_shared<std::atomic<bool>>(false);
    std::string canonical = "backend=fake\nversion=1\n";
    std::string sequenceTemplate = "fake-write/1";
    // false なら連番を描けない backend を真似る (renderSequence を渡さない)。
    bool withSequence = true;
    // 連番の枚数の上限 (backend の能力)。超える要求は cache が描かずに未対応として失敗させる。
    std::int64_t maximumSequenceFrames = 9999;

    app::MathRasterCache::PreflightFunction preflight() const {
        return [renders = renders, sequenceRenders = sequenceRenders, started = slowStarted,
                slow = slowSawCancel, canonical = canonical, sequenceTemplate = sequenceTemplate,
                withSequence = withSequence, maximumSequenceFrames = maximumSequenceFrames,
                transformRenders = transformRenders, staticGate = staticGate,
                transformGate = transformGate, transformHeld = transformHeld,
                cancellableGate = transformCancellableGate, sawCancel = transformSawCancel,
                transformLog = transformLog, transformTemplate = transformTemplate,
                withTransform = withTransform, maximumTransformFrames = maximumTransformFrames,
                equationRenders = equationRenders, equationGate = equationGate,
                equationCancellableGate = equationCancellableGate, equationHeld = equationHeld,
                equationLog = equationLog, equationTemplate = equationTemplate,
                withEquationSequence = withEquationSequence](const std::filesystem::path&,
                                                             const std::atomic<bool>*) {
            math::MathPreflightResult result;
            result.status = math::MathPreflightStatus::Available;
            result.backend.fingerprint = {"fake", canonical};
            if (withEquationSequence) {
                result.backend.equationSequenceTemplate = equationTemplate;
                result.backend.maximumEquationSequenceFrames = 9998;
                result.backend.renderEquationSequence =
                    [equationRenders, equationGate, equationCancellableGate, equationHeld,
                     equationLog](const math::EquationSequenceRenderRequest& request,
                                  const math::MathCoverageLoader& loader,
                                  const std::atomic<bool>* cancel) {
                        ++*equationRenders;
                        return fakeRenderEquationSequence(request, loader, cancel, equationGate,
                                                          equationCancellableGate, equationHeld,
                                                          equationLog);
                    };
            }
            if (withTransform) {
                result.backend.transformTemplate = transformTemplate;
                result.backend.maximumTransformFrames = maximumTransformFrames;
                result.backend.renderTransform =
                    [transformRenders, transformGate, transformHeld, cancellableGate, sawCancel,
                     transformLog](const math::MathTransformRenderRequest& request,
                                   const math::MathCoverageLoader& loader,
                                   const std::atomic<bool>* cancel) {
                        ++*transformRenders;
                        {
                            std::lock_guard lock(transformLog->mutex);
                            transformLog->received.emplace_back(request.sourceStatic,
                                                                request.targetStatic);
                        }
                        const auto& target = request.spec.target.source;
                        if (cancellableGate->load()) {
                            transformHeld->store(true);
                            for (int i = 0; i < 3000 && cancellableGate->load() && !cancel->load();
                                 ++i)
                                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                            if (cancel->load()) {
                                sawCancel->store(true);
                                transformLog->record("transform-cancelled:" + target);
                                math::MathTransformRenderResult cancelled;
                                cancelled.status = math::MathRenderStatus::Cancelled;
                                return cancelled;
                            }
                        }
                        auto result = fakeRenderTransform(request, loader, cancel, transformGate,
                                                          transformHeld);
                        if (result.status == math::MathRenderStatus::Ok)
                            transformLog->record("transform:" + target);
                        return result;
                    };
            }
            result.backend.render = [renders, started, slow, staticGate,
                                     transformLog](const math::MathStaticRenderRequest& request,
                                                   const std::atomic<bool>* cancel) {
                ++*renders;
                if (request.spec.source.find("GATE") != std::string::npos) {
                    while (staticGate->load() && !cancel->load())
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    if (cancel->load()) {
                        math::MathStaticRenderResult cancelled;
                        cancelled.status = math::MathRenderStatus::Cancelled;
                        return cancelled;
                    }
                }
                math::MathStaticRenderResult rendered;
                if (request.spec.source == "BAD") {
                    rendered.status = math::MathRenderStatus::InvalidSource;
                    rendered.message = "Undefined control sequence.";
                    rendered.log = "fake log";
                    return rendered;
                }
                if (request.spec.source == "GONE") {
                    rendered.status = math::MathRenderStatus::BackendUnavailable;
                    rendered.message = "latex が消えました";
                    return rendered;
                }
                if (request.spec.source == "SLOW") {
                    started->store(true);
                    for (int i = 0; i < 3000 && !cancel->load(); ++i)
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    slow->store(cancel->load());
                    rendered.status = math::MathRenderStatus::Cancelled;
                    return rendered;
                }
                const auto png = request.jobDirectory / L"out.png";
                int sizedWidth = 0;
                int sizedHeight = 0;
                if (fakeMathSize(request.spec.source, sizedWidth, sizedHeight)) {
                    writeCoveragePng(
                        png, {sizedWidth, sizedHeight,
                              std::vector<std::uint8_t>(static_cast<std::size_t>(sizedWidth) *
                                                            static_cast<std::size_t>(sizedHeight),
                                                        255)});
                    rendered.width = sizedWidth;
                    rendered.height = sizedHeight;
                } else if (request.spec.source.find("WIDE") != std::string::npos) {
                    writeWidePng(png, kWideMathWidth);
                    rendered.width = kWideMathWidth;
                    rendered.height = kWideMathHeight;
                } else {
                    std::ofstream(png, std::ios::binary) << mathTestPngBytes();
                    rendered.width = kMathTestPngWidth;
                    rendered.height = kMathTestPngHeight;
                }
                rendered.status = math::MathRenderStatus::Ok;
                rendered.png = png;
                transformLog->record("static:" + request.spec.source);
                return rendered;
            };
            if (!withSequence)
                return result;
            result.backend.sequenceTemplate = sequenceTemplate;
            result.backend.maximumSequenceFrames = maximumSequenceFrames;
            result.backend.renderSequence = [sequenceRenders, started,
                                             slow](const math::MathSequenceRenderRequest& request,
                                                   const std::atomic<bool>* cancel) {
                ++*sequenceRenders;
                math::MathSequenceRenderResult rendered;
                const auto& source = request.spec.still.source;
                if (source == "BAD") {
                    rendered.status = math::MathRenderStatus::InvalidSource;
                    rendered.message = "Undefined control sequence.";
                    return rendered;
                }
                if (source == "GONE") {
                    rendered.status = math::MathRenderStatus::BackendUnavailable;
                    rendered.message = "latex が消えました";
                    return rendered;
                }
                if (source == "SLOW" || source == "SLOW_WRITE") {
                    started->store(true);
                    for (int i = 0; i < 3000 && !cancel->load(); ++i)
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    slow->store(cancel->load());
                    rendered.status = math::MathRenderStatus::Cancelled;
                    return rendered;
                }
                const bool wide = source.find("WIDE") != std::string::npos;
                int sizedWidth = 0;
                int sizedHeight = 0;
                const bool sized = fakeMathSize(source, sizedWidth, sizedHeight);
                for (std::int64_t index = 0; index < request.spec.frames; ++index) {
                    wchar_t name[32] = {};
                    std::swprintf(name, std::size(name), L"f%04lld.png",
                                  static_cast<long long>(index));
                    const auto png = request.jobDirectory / name;
                    if (sized) {
                        // frame i は左から w * i / frames 列だけが不透明 (WIDE と同じ規則)。
                        math::MathCoverage coverage{
                            sizedWidth, sizedHeight,
                            std::vector<std::uint8_t>(static_cast<std::size_t>(sizedWidth) *
                                                          static_cast<std::size_t>(sizedHeight),
                                                      0)};
                        const auto columns =
                            static_cast<int>(sizedWidth * index / request.spec.frames);
                        for (int y = 0; y < sizedHeight; ++y)
                            for (int x = 0; x < columns; ++x)
                                coverage.alpha[coverageIndex(sizedWidth, x, y)] = 255;
                        writeCoveragePng(png, coverage);
                    } else if (wide) {
                        writeWidePng(png, wideRevealColumns(index, request.spec.frames));
                    } else {
                        std::ofstream(png, std::ios::binary) << mathTestPngBytes();
                    }
                    rendered.frames.push_back(png);
                }
                rendered.width = sized ? sizedWidth : wide ? kWideMathWidth : kMathTestPngWidth;
                rendered.height = sized ? sizedHeight : wide ? kWideMathHeight : kMathTestPngHeight;
                rendered.status = math::MathRenderStatus::Ok;
                return rendered;
            };
            return result;
        };
    }

    static app::MathRasterCache::PreflightFunction unavailable(std::string message) {
        return [message](const std::filesystem::path&, const std::atomic<bool>*) {
            math::MathPreflightResult result;
            result.status = math::MathPreflightStatus::Unavailable;
            result.message = message;
            return result;
        };
    }
};

} // namespace mvm::test

#endif
