#ifndef MVM_TESTS_MATH_FAKE_BACKEND_H
#define MVM_TESTS_MATH_FAKE_BACKEND_H

// 数式の偽の backend (MathRasterCache の preflight 関数として注入する)。
// 式の内容で振る舞いを変え、描画の回数を数える。
//   "BAD"   TeX の誤り (InvalidSource、message は "Undefined control sequence.")
//   "GONE"  描画中に toolchain が消えた (BackendUnavailable)
//   "SLOW"  cancel されるまで待つ (最長 30 秒)
//   それ以外  3x2 の白い glyph の PNG (math_test_png.h) を書いて Ok

#include "math_raster_cache.h"
#include "math_test_png.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

namespace mvm::test {

struct FakeMathBackend {
    std::shared_ptr<std::atomic<int>> renders = std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<bool>> slowStarted = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> slowSawCancel = std::make_shared<std::atomic<bool>>(false);
    std::string canonical = "backend=fake\nversion=1\n";

    app::MathRasterCache::PreflightFunction preflight() const {
        return [renders = renders, started = slowStarted, slow = slowSawCancel,
                canonical = canonical](const std::filesystem::path&, const std::atomic<bool>*) {
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
                std::ofstream(png, std::ios::binary) << mathTestPngBytes();
                rendered.status = math::MathRenderStatus::Ok;
                rendered.png = png;
                rendered.width = kMathTestPngWidth;
                rendered.height = kMathTestPngHeight;
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
