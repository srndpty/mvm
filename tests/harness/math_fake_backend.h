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
//   それ以外  3x2 の白い glyph の PNG (math_test_png.h) を書いて Ok。Write も各 frame が同じ PNG
//
// 変形 (renderTransform): canvas は大きい方の端点の静止 + 各辺 kFakeTransformPadding。端点は
// mathTransformPlacement の位置に置く。frame 0 は source の静止の mask だけ、frame 1 以降は
// target の静止の mask と、canvas の (2, 3) の 1 画素 (alpha 200)。artifact は両端の静止の矩形と
// その 1 画素の和。受け取った両端の mask を記録する。
//   transformGate が true の間は終えない (cancel を見ずに待つ。取り消された後に結果を返す
//   renderer を真似る)
//   target が "BADT" を含む  InvalidSource
//   source が "LIE" を含む   artifact の矩形を (2, 3) の画素を含めずに報告する
//   source が "SHIFT" を含む frame 0 の source を右へ 1 画素ずらして描く

#include "math_raster_cache.h"
#include "math_test_png.h"
#include "media/math/math_transform.h"

#include <algorithm>
#include <atomic>
#include <chrono>
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

// 偽の変形が受け取った両端の静止の mask。
struct FakeTransformLog {
    std::mutex mutex;
    std::vector<std::pair<math::MathCoverage, math::MathCoverage>> received;
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

struct FakeMathBackend {
    std::shared_ptr<std::atomic<int>> renders = std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<int>> transformRenders = std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<bool>> staticGate = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> transformGate = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> transformHeld = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<FakeTransformLog> transformLog = std::make_shared<FakeTransformLog>();
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
                transformLog = transformLog, transformTemplate = transformTemplate,
                withTransform = withTransform, maximumTransformFrames = maximumTransformFrames](
                   const std::filesystem::path&, const std::atomic<bool>*) {
            math::MathPreflightResult result;
            result.status = math::MathPreflightStatus::Available;
            result.backend.fingerprint = {"fake", canonical};
            if (withTransform) {
                result.backend.transformTemplate = transformTemplate;
                result.backend.maximumTransformFrames = maximumTransformFrames;
                result.backend.renderTransform =
                    [transformRenders, transformGate, transformHeld, transformLog](
                        const math::MathTransformRenderRequest& request,
                        const math::MathCoverageLoader& loader, const std::atomic<bool>* cancel) {
                        ++*transformRenders;
                        {
                            std::lock_guard lock(transformLog->mutex);
                            transformLog->received.emplace_back(request.sourceStatic,
                                                                request.targetStatic);
                        }
                        return fakeRenderTransform(request, loader, cancel, transformGate,
                                                   transformHeld);
                    };
            }
            result.backend.render = [renders, started, slow,
                                     staticGate](const math::MathStaticRenderRequest& request,
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
                if (request.spec.source.find("WIDE") != std::string::npos) {
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
                for (std::int64_t index = 0; index < request.spec.frames; ++index) {
                    wchar_t name[32] = {};
                    std::swprintf(name, std::size(name), L"f%04lld.png",
                                  static_cast<long long>(index));
                    const auto png = request.jobDirectory / name;
                    if (wide)
                        writeWidePng(png, wideRevealColumns(index, request.spec.frames));
                    else
                        std::ofstream(png, std::ios::binary) << mathTestPngBytes();
                    rendered.frames.push_back(png);
                }
                rendered.width = wide ? kWideMathWidth : kMathTestPngWidth;
                rendered.height = wide ? kWideMathHeight : kMathTestPngHeight;
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
