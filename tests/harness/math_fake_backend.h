#ifndef MVM_TESTS_MATH_FAKE_BACKEND_H
#define MVM_TESTS_MATH_FAKE_BACKEND_H

// 数式の偽の backend (MathRasterCache の preflight 関数として注入する)。
// 式の内容で振る舞いを変え、描画の回数を数える。
//   "BAD"   TeX の誤り (InvalidSource、message は "Undefined control sequence.")
//   "GONE"  描画中に toolchain が消えた (BackendUnavailable)
//   "SLOW"  cancel されるまで待つ (最長 30 秒)。Write の連番でも同じ
//   "WIDE" を含む式  64x8 の白い帯 (位置で frame を見分ける試験用)。Write の frame i は
//                    左から 64 * i / frames 列だけが不透明 (frame 0 は空)
//   それ以外  3x2 の白い glyph の PNG (math_test_png.h) を書いて Ok。Write も各 frame が同じ PNG

#include "math_raster_cache.h"
#include "math_test_png.h"

#include <atomic>
#include <chrono>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

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

struct FakeMathBackend {
    std::shared_ptr<std::atomic<int>> renders = std::make_shared<std::atomic<int>>(0);
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
                withSequence = withSequence, maximumSequenceFrames = maximumSequenceFrames](
                   const std::filesystem::path&, const std::atomic<bool>*) {
            math::MathPreflightResult result;
            result.status = math::MathPreflightStatus::Available;
            result.backend.fingerprint = {"fake", canonical};
            result.backend.render = [renders, started,
                                     slow](const math::MathStaticRenderRequest& request,
                                           const std::atomic<bool>* cancel) {
                ++*renders;
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
