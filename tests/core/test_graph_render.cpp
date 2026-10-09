#include "app/graph_render_compile.h"
#include "media/graph_manim/graph_manim_backend.h"

#include <algorithm>
#include <array>
#include <barrier>
#include <cmath>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <thread>

namespace {
int failures = 0;

void check(bool value, const char* text) {
    if (!value) {
        std::cerr << "失敗: " << text << '\n';
        ++failures;
    }
}

mvm::graph::GraphRenderSpec spec(mvm::project::GraphClipData data, int width = 854,
                                 int height = 480) {
    auto result = mvm::app::compileGraphRender(data, 10000, width, height);
    if (const auto* error = std::get_if<mvm::graph::Error>(&result)) {
        std::cerr << error->message << '\n';
        ++failures;
        return {};
    }
    return std::get<mvm::graph::GraphRenderSpec>(result);
}

mvm::project::GraphClipData line(const char* color = "#FFFF0000", double width = 64) {
    auto data = mvm::project::defaultGraph({"owner"});
    data.axes = {false, false, "", ""};
    data.functions[0].expression = "0";
    data.functions[0].color = color;
    data.functions[0].strokeWidth = width;
    return data;
}

void pure() {
    using namespace mvm::graph;
    auto data = line();
    auto s = spec(data, 1920, 1080);
    check(s.plot == Rectangle{240, 135, 1440, 810}, "基準 plot");
    check(mapPoint(s, {-5, -5}) == Point{240, 945} && mapPoint(s, {5, 5}) == Point{1680, 135},
          "端の座標");
    check(mapPoint(s, {0, 0}) == Point{960, 540}, "zero の座標");
    check(strokeScale(1280, 720) == 2.0 / 3 && strokeScale(1920, 540) == .5, "非等方線幅");
    data.axes.showGrid = true;
    data.viewport = {1, 5, 2, 20};
    s = spec(data);
    check(s.background.size() == 22, "軸非表示でも11本ずつ grid");
    data.axes.showAxes = true;
    check(spec(data).background.size() == 22, "範囲外 zero に偽軸を作らない");
    data.viewport = {-20, -1, -10, -2};
    check(spec(data).background.size() == 22, "負範囲の偽軸なし");
    data = line();
    data.viewport = {-1e150, 1e150, -1e150, 1e150};
    s = spec(data);
    check(mapPoint(s, {0, 0}) == Point{427, 240}, "極端な有限 span");
    data = line();
    s = spec(data);
    auto copy = s;
    copy.curves[0].geometry.functionId = "other-owner";
    check(staticKey(s, "tool") == staticKey(copy, "tool"), "所有 ID は key に入らない");
    copy.drawFrames = 3;
    check(staticKey(s, "tool") == staticKey(copy, "tool"), "N は static を無効化しない");
    copy.drawFrames = 10000;
    check(staticKey(s, "tool") == staticKey(copy, "tool"),
          "Draw budget 超過でも静止 identity を維持");
    check(requestJson(copy, "tool").empty(), "過大 Draw の protocol を確保しない");
    check(drawKey(staticKey(s, "tool"), 1) != drawKey(staticKey(s, "tool"), 3),
          "N は Draw を無効化する");
    check(staticKey(s, "tool") != staticKey(s, "other-tool"), "toolchain の変更");
    for (auto mutate : {0, 1, 2, 3, 4}) {
        copy = s;
        if (mutate == 0)
            copy.curves[0].argb ^= 1;
        if (mutate == 1)
            copy.labels[0].text = "x";
        if (mutate == 2)
            copy.curves[0].ast += "x";
        if (mutate == 3) {
            copy.curves[0].referenceWidth = 3;
            copy.curves[0].widthPixels = 3 * strokeScale(copy.width, copy.height);
        }
        if (mutate == 4)
            copy.curves[0].geometry.segments[0][1].y += .1;
        check(staticKey(s, "tool") != staticKey(copy, "tool"), "描画情報の変更で key 更新");
    }
    for (int n : {1, 3, 10}) {
        copy = s;
        copy.drawFrames = n;
        check(framePaths(copy, 0).empty(), "Draw 0 は曲線なし");
        check(framePaths(copy, n) == framePaths(copy, -1), "Draw N は静止端点");
        for (int frame : {n - 1, 0, n / 2, n - 1})
            check(framePaths(copy, frame) == framePaths(copy, frame), "任意順の pure geometry");
    }
    copy = s;
    copy.labels[0].band.top = 100;
    check(std::holds_alternative<Error>(validateSpec(copy)), "label の plot 侵入拒否");
    auto invalid = mvm::app::compileGraphRender(data, 10000, 8193, 1080);
    check(std::holds_alternative<Error>(invalid), "canvas budget の負例");
    data.functions[0].expression = "sqrt(-1)";
    check(std::holds_alternative<Error>(mvm::app::compileGraphRender(data, 10000, 854, 480)),
          "有限点なしの負例");
    // 独立な浮動小数オラクル。整数実装の式・helper は共有しない。
    for (int ua : {0, 1, 127, 128, 255})
        for (int ca : {0, 1, 127, 128, 255})
            for (int coverage : {0, 1, 127, 128, 255}) {
                std::uint8_t under[4] = {31, 129, 219, static_cast<std::uint8_t>(ua)}, out[4];
                sourceOver(under, static_cast<std::uint8_t>(coverage),
                           (static_cast<std::uint32_t>(ca) << 24) | 0xEE7722, out);
                const long double a = std::round(coverage * ca / 255.0L) / 255;
                const long double b = ua / 255.0L, alpha = a + b * (1 - a);
                const int color[] = {238, 119, 34};
                check(out[3] == std::round(alpha * 255), "alpha oracle");
                for (int i = 0; i < 3; ++i)
                    check(out[i] ==
                              (alpha ? std::round((color[i] * a + under[i] * b * (1 - a)) / alpha)
                                     : 0),
                          "straight RGB oracle");
            }
}

void artifacts(const std::filesystem::path& root, bool responsivenessOnly = false) {
    using namespace mvm::graph;
    auto data = line();
    data.intro = {mvm::project::GraphIntroKind::Draw, 3};
    const auto s = spec(data, 64, 36);
    PublicationAuthority authority;
    const auto generation = authority.supersede();
    std::atomic<bool> cancel{false};
    auto failedAs = [](const ArtifactResult& result, Failure reason) {
        const auto* error = std::get_if<Error>(&result);
        return error && error->failure == reason;
    };
    GraphRenderer fake = [](const RenderRequest& request,
                            const std::atomic<bool>*) -> RenderResult {
        for (int i = -1; i < request.spec.drawFrames; ++i) {
            Raster raster{request.spec.width, request.spec.height,
                          std::vector<std::uint8_t>(static_cast<std::size_t>(request.spec.width) *
                                                    static_cast<std::size_t>(request.spec.height) *
                                                    4)};
            if (i != 0) {
                raster.rgba[0] = 255;
                raster.rgba[1] = i < 0 ? 0 : static_cast<std::uint8_t>(i);
                raster.rgba[3] = 255;
            }
            if (!writeRgba(request.job /
                               (i < 0 ? "static.png" : "frame-" + std::to_string(i) + ".png"),
                           raster))
                return Error{Failure::RendererFailure, 0, "試験 PNG を書けません"};
        }
        return std::monostate{};
    };
    RenderRequest request{s, "test-authority", root / "job"};
    auto excessive = request;
    excessive.spec = spec(line(), 1920, 1080);
    excessive.spec.drawFrames = 10000;
    excessive.job = root / "budget-job";
    bool excessiveInvoked = false;
    const auto excessiveResult =
        authority.generate(excessive, root / "budget-cache", generation,
                           [&](const RenderRequest&, const std::atomic<bool>*) -> RenderResult {
                               excessiveInvoked = true;
                               return std::monostate{};
                           });
    check(failedAs(excessiveResult, Failure::ResourceLimit) && !excessiveInvoked &&
              !std::filesystem::exists(excessive.job),
          "Draw の全体 budget は起動前に拒否");
    auto rendered = authority.generate(request, root / "cache", generation, fake, &cancel);
    check(std::holds_alternative<Artifact>(rendered), "検証後の公開");
    if (!std::holds_alternative<Artifact>(rendered))
        return;
    const auto artifact = std::get<Artifact>(rendered);
    bool invoked = false;
    request.job = root / "reuse-job";
    auto reused =
        authority.generate(request, root / "cache", generation,
                           [&](const RenderRequest&, const std::atomic<bool>*) -> RenderResult {
                               invoked = true;
                               return Error{Failure::RendererFailure, 0, "呼ばれないはずです"};
                           });
    check(std::holds_alternative<Artifact>(reused) && !invoked, "検証済み cache を再利用");
    auto differentN = request;
    differentN.spec.drawFrames = 1;
    differentN.job = root / "different-n-job";
    int staticRenders = 0;
    const auto differentResult =
        authority.generate(differentN, root / "cache", generation,
                           [&](const RenderRequest& r, const std::atomic<bool>* flag) {
                               ++staticRenders;
                               return fake(r, flag);
                           });
    check(std::holds_alternative<Artifact>(differentResult) && staticRenders == 1 &&
              staticKey(differentN.spec, request.toolchain) == artifact.staticIdentity &&
              std::filesystem::exists(differentN.job / "static.png"),
          "異なる Draw N は静止 identity を保つが static を再描画する契約 B");
    const auto completePending = root / ".pending-complete";
    std::filesystem::copy(artifact.directory, completePending,
                          std::filesystem::copy_options::recursive);
    check(std::holds_alternative<Error>(validateArtifact(completePending, s, request.toolchain)),
          "全 PNG と正常 manifest があっても pending は非 authority");
    check(std::holds_alternative<Artifact>(
              validateArtifact(artifact.directory, s, request.toolchain)),
          "独立再読込");
    check(std::holds_alternative<Error>(validateArtifact(artifact.directory, s, "wrong-tool")),
          "toolchain 偽装拒否");
    auto changed = s;
    changed.curves[0].argb ^= 1;
    check(std::holds_alternative<Error>(
              validateArtifact(artifact.directory, changed, request.toolchain)),
          "key と segment authority の不一致");
    for (const auto& name : {"manifest.txt", "static.png", "frame-1.png"}) {
        const auto path = artifact.directory / name,
                   backup = artifact.directory / (std::string(name) + ".saved");
        std::filesystem::rename(path, backup);
        check(std::holds_alternative<Error>(
                  validateArtifact(artifact.directory, s, request.toolchain)),
              "欠損を Ready にしない");
        std::filesystem::rename(backup, path);
    }
    auto manifestPath = artifact.directory / "manifest.txt";
    std::ifstream in(manifestPath, std::ios::binary);
    const std::string original{std::istreambuf_iterator<char>(in), {}};
    in.close();
    for (std::size_t offset : {std::size_t(0), original.size() / 2, original.size() - 1}) {
        auto bytes = original;
        bytes[offset] ^= 1;
        {
            std::ofstream out(manifestPath, std::ios::binary);
            out << bytes;
        }
        check(std::holds_alternative<Error>(
                  validateArtifact(artifact.directory, s, request.toolchain)),
              "manifest 改竄を拒否");
    }
    {
        std::ofstream out(manifestPath, std::ios::binary);
        out << original;
    }
    for (const auto& token : {std::string(staticVersion), artifact.staticIdentity,
                              std::string("test-authority"), std::string("endpoint=static.png")}) {
        auto bytes = original;
        const auto location = bytes.find(token);
        check(location != std::string::npos, "manifest corruption の対象が存在する");
        if (location == std::string::npos)
            continue;
        bytes[location] ^= 1;
        {
            std::ofstream out(manifestPath, std::ios::binary);
            out << bytes;
        }
        check(std::holds_alternative<Error>(
                  validateArtifact(artifact.directory, s, request.toolchain)),
              "namespace/key/toolchain/endpoint の改竄拒否");
    }
    {
        std::ofstream out(manifestPath, std::ios::binary);
        out << original;
    }
    std::filesystem::rename(artifact.directory / "frame-1.png", artifact.directory / "swap.png");
    std::filesystem::rename(artifact.directory / "frame-2.png", artifact.directory / "frame-1.png");
    std::filesystem::rename(artifact.directory / "swap.png", artifact.directory / "frame-2.png");
    check(std::holds_alternative<Error>(validateArtifact(artifact.directory, s, request.toolchain)),
          "frame 入れ替え拒否");
    std::filesystem::rename(artifact.directory / "frame-1.png", artifact.directory / "swap.png");
    std::filesystem::rename(artifact.directory / "frame-2.png", artifact.directory / "frame-1.png");
    std::filesystem::rename(artifact.directory / "swap.png", artifact.directory / "frame-2.png");
    Raster wrong{64, 36, std::vector<std::uint8_t>(64 * 36 * 4)};
    wrong.rgba[0] = 17;
    wrong.rgba[3] = 255;
    check(writeRgba(artifact.directory / "frame-1.png", wrong), "改竄 PNG の作成");
    check(std::holds_alternative<Error>(validateArtifact(artifact.directory, s, request.toolchain)),
          "decoded hash の不一致");
    Raster small{32, 18, std::vector<std::uint8_t>(32 * 18 * 4)};
    check(writeRgba(artifact.directory / "frame-1.png", small), "不正寸法 PNG");
    check(std::holds_alternative<Error>(validateArtifact(artifact.directory, s, request.toolchain)),
          "寸法不一致拒否");
    {
        std::ofstream out(artifact.directory / "frame-1.png", std::ios::binary);
        out << "PNG";
    }
    check(std::holds_alternative<Error>(validateArtifact(artifact.directory, s, request.toolchain)),
          "切断 PNG 拒否");
    wrong.rgba[4] = 255;
    check(writeRgba(artifact.directory / "frame-1.png", wrong), "不正透明 RGB PNG");
    check(std::holds_alternative<Error>(readRgba(artifact.directory / "frame-1.png", 64, 36)),
          "hash 以外でも不正透明 RGB を拒否");
    check(std::holds_alternative<Error>(validateArtifact(artifact.directory, s, request.toolchain)),
          "透明 RGB canonicalization の拒否");
    request.job = root / "cancel-job";
    cancel = true;
    check(failedAs(authority.generate(request, root / "cancel-cache", generation, fake, &cancel),
                   Failure::Cancelled),
          "起動前取消");
    cancel = false;
    const auto newer = authority.supersede();
    check(failedAs(authority.generate(request, root / "stale-cache", generation, fake),
                   Failure::Superseded),
          "旧世代拒否");
    request.job = root / "mid-job";
    auto stale = authority.generate(request, root / "mid-cache", newer,
                                    [&](const RenderRequest& r, const std::atomic<bool>* flag) {
                                        auto result = fake(r, flag);
                                        authority.supersede();
                                        return result;
                                    });
    check(failedAs(stale, Failure::Superseded) && !std::filesystem::exists(root / "mid-cache"),
          "stale 完了を公開しない");
    request.job = root / "cancel-mid-job";
    auto cancelled = authority.generate(
        request, root / "cancel-mid-cache", authority.supersede(),
        [&](const RenderRequest& r, const std::atomic<bool>* flag) {
            auto result = fake(r, flag);
            cancel = true;
            return result;
        },
        &cancel);
    check(failedAs(cancelled, Failure::Cancelled), "検証前取消");
    cancel = false;
    request.job = root / "partial-job";
    auto partial =
        authority.generate(request, root / "partial-cache", authority.supersede(),
                           [](const RenderRequest&, const std::atomic<bool>*) -> RenderResult {
                               return std::monostate{};
                           });
    check(std::holds_alternative<Error>(partial) &&
              !std::filesystem::exists(request.job / "manifest.txt"),
          "部分出力に provenance を公開しない");
    request.job = root / "blank-wrong-job";
    auto blankWrong = authority.generate(
        request, root / "blank-wrong-cache", authority.supersede(),
        [](const RenderRequest& r, const std::atomic<bool>*) -> RenderResult {
            Raster blank{r.spec.width, r.spec.height,
                         std::vector<std::uint8_t>(static_cast<std::size_t>(r.spec.width) *
                                                   static_cast<std::size_t>(r.spec.height) * 4)};
            for (std::int64_t i = -1; i < r.spec.drawFrames; ++i)
                if (!writeRgba(r.job /
                                   (i < 0 ? "static.png" : "frame-" + std::to_string(i) + ".png"),
                               blank))
                    return Error{Failure::RendererFailure, 0, "空白試験の PNG を書けません"};
            return std::monostate{};
        });
    check(std::holds_alternative<Error>(blankWrong),
          "可視曲線を全透明へ縮退させた renderer の負例");
    std::barrier rendezvous(2);
    const auto concurrentGeneration = authority.supersede();
    GraphRenderer concurrent = [&](const RenderRequest& r, const std::atomic<bool>* flag) {
        auto result = fake(r, flag);
        if (r.job.filename() == "concurrent-b") {
            auto raster = std::get<Raster>(readRgba(r.job / "static.png", s.width, s.height));
            raster.rgba[0] = 128;
            writeRgba(r.job / "static.png", raster);
        }
        return result;
    };
    PublicationObserver concurrentStaging = [&](PublicationStage stage) {
        if (stage == PublicationStage::Validate && !responsivenessOnly)
            rendezvous.arrive_and_wait();
    };
    auto first = request;
    first.job = root / "concurrent-a";
    auto second = request;
    second.job = root / "concurrent-b";
    auto a = std::async(std::launch::async, [&] {
        return authority.generate(first, root / "concurrent-cache", concurrentGeneration,
                                  concurrent, nullptr, concurrentStaging);
    });
    auto b = std::async(std::launch::async, [&] {
        return authority.generate(second, root / "concurrent-cache", concurrentGeneration,
                                  concurrent, nullptr, concurrentStaging);
    });
    const auto resultA = a.get(), resultB = b.get();
    check(std::holds_alternative<Artifact>(resultA) && std::holds_alternative<Artifact>(resultB),
          "同じ key の staging 重複同時要求");
    if (std::holds_alternative<Artifact>(resultA) && std::holds_alternative<Artifact>(resultB))
        check(std::get<Artifact>(resultA).pixelHashes == std::get<Artifact>(resultB).pixelHashes,
              "異なる描画結果の同じ key は勝者を置換せず同じ artifact を返す");
    // I/O 境界を明示的に止め、無効化の完了を待ってから解放する。
    // timeout は旧 mutex の deadlock を安全に解放するためだけに使う。
    for (const auto stage : {PublicationStage::Copy, PublicationStage::Validate}) {
        for (int action = 0; action < 3; ++action) {
            PublicationAuthority stagedAuthority;
            const auto oldGeneration = stagedAuthority.supersede();
            const auto name =
                std::to_string(static_cast<int>(stage)) + "-" + std::to_string(action);
            const auto stagedCache = root / ("staging-cache-" + name);
            auto stagedRequest = request;
            stagedRequest.job = root / ("staging-job-" + name);
            std::promise<void> enteredStage, releaseStage;
            auto enteredFuture = enteredStage.get_future();
            auto releaseFuture = releaseStage.get_future().share();
            bool observed = false;
            std::atomic<bool> stagedCancel{false};
            auto oldJob = std::async(std::launch::async, [&] {
                return stagedAuthority.generate(stagedRequest, stagedCache, oldGeneration, fake,
                                                &stagedCancel, [&](PublicationStage current) {
                                                    if (current == stage && !observed) {
                                                        observed = true;
                                                        enteredStage.set_value();
                                                        releaseFuture.wait();
                                                    }
                                                });
            });
            enteredFuture.wait();
            std::uint64_t nextGeneration = 0;
            auto invalidate = std::async(std::launch::async, [&] {
                if (action == 0)
                    nextGeneration = stagedAuthority.supersede();
                else if (action == 1)
                    stagedAuthority.shutdown();
                else
                    stagedCancel.store(true);
            });
            const bool responsive =
                invalidate.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
            check(responsive, "staging を解放せずに authority 無効化が完了する");
            const auto target =
                stagedCache / drawKey(staticKey(s, request.toolchain), s.drawFrames);
            check(!std::filesystem::exists(target), "staging 中は Ready が存在しない");
            for (const auto& entry : std::filesystem::directory_iterator(stagedCache))
                check(std::holds_alternative<Error>(
                          validateArtifact(entry.path(), s, request.toolchain)),
                      "pending は公開 authority ではない");
            // supersede が完了した場合、新世代は古い staging を待たずに公開できる。
            if (responsive && action == 0) {
                auto newRequest = request;
                newRequest.job = root / ("new-staging-job-" + name);
                check(std::holds_alternative<Artifact>(
                          stagedAuthority.generate(newRequest, stagedCache, nextGeneration, fake)),
                      "古い staging が停止中でも新世代を公開できる");
            }
            releaseStage.set_value();
            invalidate.get();
            const auto oldResult = oldJob.get();
            check(failedAs(oldResult, action == 0 ? Failure::Superseded : Failure::Cancelled),
                  "staging 中の取消・旧世代は typed failure");
            if (responsive && action == 0)
                check(std::holds_alternative<Artifact>(
                          validateArtifact(target, s, request.toolchain)),
                      "旧世代の失敗でも新しい正常 cache は保存する");
            else
                check(!std::filesystem::exists(target), "取消後の staging は公開されない");
            // shutdown 後も job の join を済ませてから authority を破棄する。
            stagedAuthority.shutdown();
        }
    }
    auto next = request;
    next.job = root / "failed-new-job";
    next.spec.curves[0].argb ^= 1;
    auto failed =
        authority.generate(next, root / "concurrent-cache", concurrentGeneration,
                           [](const RenderRequest&, const std::atomic<bool>*) -> RenderResult {
                               return Error{Failure::RendererFailure, 0, "意図した renderer 失敗"};
                           });
    check(std::holds_alternative<Error>(failed), "新しい key の描画失敗");
    check(std::holds_alternative<Artifact>(validateArtifact(
              root / "concurrent-cache" / drawKey(staticKey(s, request.toolchain), s.drawFrames), s,
              request.toolchain)),
          "新規失敗でも前の正常 artifact は保存");
    std::atomic<bool> entered{false};
    request.job = root / "cancel-during-job";
    auto during = std::async(std::launch::async, [&] {
        return authority.generate(
            request, root / "cancel-during-cache", concurrentGeneration,
            [&](const RenderRequest&, const std::atomic<bool>* flag) -> RenderResult {
                entered.store(true);
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                while (!flag->load() && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if (!flag->load())
                    return Error{Failure::RendererFailure, 0, "取消が伝わりません"};
                return Error{Failure::Cancelled, 0, "描画中の取消"};
            },
            &cancel);
    });
    while (!entered.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    cancel.store(true);
    check(failedAs(during.get(), Failure::Cancelled), "描画中も取消を backend に伝える");
    cancel.store(false);
    authority.shutdown();
    request.job = root / "shutdown-job";
    check(failedAs(authority.generate(request, root / "shutdown-cache", concurrentGeneration, fake),
                   Failure::Cancelled),
          "shutdown 後は公開しない");

    PublicationAuthority retryAuthority;
    const auto retryGeneration = retryAuthority.supersede();
    RenderRequest retryRequest{s, "test-authority", root / "share-job"};
    const auto retryCache = root / "share-cache";
    std::unique_ptr<std::ifstream> held;
    std::atomic<bool> locked{false};
    std::thread releaser;
    const auto retryResult = retryAuthority.generate(
        retryRequest, retryCache, retryGeneration, fake, nullptr, [&](PublicationStage stage) {
            if (stage != PublicationStage::Validate || locked.load())
                return;
            for (const auto& entry : std::filesystem::directory_iterator(retryCache)) {
                if (!entry.path().filename().string().starts_with(".pending-"))
                    continue;
                held =
                    std::make_unique<std::ifstream>(entry.path() / "static.png", std::ios::binary);
                if (!held->is_open())
                    return;
                locked.store(true);
                releaser = std::thread([&held] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                    held.reset();
                });
                return;
            }
        });
    if (releaser.joinable())
        releaser.join();
    check(locked.load() && std::holds_alternative<Artifact>(retryResult),
          "共有ロック中の directory rename は待って公開する");
    if (const auto* error = std::get_if<Error>(&retryResult))
        std::cerr << error->message << '\n';
    if (const auto* published = std::get_if<Artifact>(&retryResult)) {
        const auto manifestBytes = [](const auto& directory) {
            std::ifstream stream(directory / "manifest.txt", std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(stream), {});
        };
        const auto publishedManifest = manifestBytes(published->directory);
        const auto publishedHashes = published->pixelHashes;
        for (int attempt = 0; attempt < 3; ++attempt) {
            retryRequest.job = root / ("repeat-share-job-" + std::to_string(attempt));
            const auto repeated =
                retryAuthority.generate(retryRequest, retryCache, retryGeneration,
                                        [](const auto&, const auto*) -> RenderResult {
                                            return Error{Failure::RendererFailure, 0,
                                                         "公開済みなら renderer は呼ばれません"};
                                        });
            const auto* existing = std::get_if<Artifact>(&repeated);
            check(existing && !publishedManifest.empty() &&
                      manifestBytes(existing->directory) == publishedManifest &&
                      existing->pixelHashes == publishedHashes,
                  "共有ロック後の繰り返し公開も既存の有効 artifact を置換しない");
        }
    }
    // 実際の lock error の後、mutex の外にある retry 境界で操作を同期する。
    for (int mode = 0; mode < 4; ++mode) {
        PublicationAuthority retrying;
        const auto current = retrying.supersede();
        const auto prefix = "retry-control-" + std::to_string(mode);
        const auto cachePath = root / (prefix + "-cache");
        RenderRequest first{s, "test-authority", root / (prefix + "-first")};
        std::atomic<bool> stop{false}, observed{false};
        std::promise<void> entered, releaseRetry;
        auto enteredFuture = entered.get_future();
        auto barrier = releaseRetry.get_future().share();
        std::unique_ptr<std::ifstream> lockFile;
        auto task = std::async(std::launch::async, [&] {
            return retrying.generate(
                first, cachePath, current, fake, &stop, [&](PublicationStage stage) {
                    if (stage == PublicationStage::Validate)
                        for (const auto& entry : std::filesystem::directory_iterator(cachePath))
                            if (entry.path().filename().string().starts_with(".pending-")) {
                                lockFile = std::make_unique<std::ifstream>(
                                    entry.path() / "static.png", std::ios::binary);
                                break;
                            }
                    if (stage == PublicationStage::RenameRetry && !observed.exchange(true)) {
                        entered.set_value();
                        barrier.wait();
                    }
                });
        });
        const bool retryEntered =
            enteredFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
        check(retryEntered, "共有 lock が実際の rename retry を起こす");
        std::optional<Artifact> winner;
        if (retryEntered) {
            auto operation = std::async(std::launch::async, [&] {
                if (mode == 0)
                    stop.store(true);
                else if (mode == 1)
                    retrying.supersede();
                else if (mode == 2)
                    retrying.shutdown();
                else {
                    RenderRequest competing{s, "test-authority", root / (prefix + "-winner")};
                    const auto result = retrying.generate(
                        competing, cachePath, current,
                        [&](const auto& candidate, const auto* flag) -> RenderResult {
                            auto result = fake(candidate, flag);
                            if (std::holds_alternative<Error>(result))
                                return result;
                            auto pixels = readRgba(candidate.job / "static.png", 64, 36);
                            auto* raster = std::get_if<Raster>(&pixels);
                            if (!raster)
                                return std::get<Error>(pixels);
                            raster->rgba[0] ^= 255;
                            if (!writeRgba(candidate.job / "static.png", *raster))
                                return Error{Failure::RendererFailure, 0,
                                             "競合する試験画像を保存できません"};
                            return std::monostate{};
                        });
                    if (const auto* artifactValue = std::get_if<Artifact>(&result))
                        winner = *artifactValue;
                }
            });
            const bool responsive =
                operation.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
            check(responsive, "retry を止めたまま cancellation・supersession・別の公開が完了");
            if (!responsive) {
                releaseRetry.set_value();
                operation.wait();
            } else {
                operation.get();
                releaseRetry.set_value();
            }
        } else {
            stop.store(true);
            releaseRetry.set_value();
        }
        const auto completed = task.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
        check(completed, "共有 lock を解放しなくても retry は変更された authority に応答する");
        lockFile.reset();
        const auto result = task.get();
        if (mode < 3)
            check(failedAs(result, mode == 1 ? Failure::Superseded : Failure::Cancelled),
                  "rename retry 中の取消は型付き失敗として返る");
        else {
            const auto* retained = std::get_if<Artifact>(&result);
            check(winner && retained && retained->pixelHashes == winner->pixelHashes,
                  "rename を繰り返しても先に公開された異なる有効画素を置換しない");
        }
    }
}

void real(const std::filesystem::path& root, const std::filesystem::path& python,
          const std::filesystem::path& script, const std::string& mode = {}) {
    using namespace mvm::graph;
    auto preflight = mvm::graph_manim::preflightGraph(python, script, root / "preflight");
    if (auto* error = std::get_if<Error>(&preflight)) {
        check(false, error->message.c_str());
        return;
    }
    const auto backend = std::get<mvm::graph::GraphBackend>(preflight);
    std::ofstream report(root / "measurements.tsv");
    report << "条件\t幅\t高さ\t指定線幅\t画素線幅\n";
    int index = 0;
    auto render = [&](const mvm::project::GraphClipData& data, int width = 854,
                      int height = 480) -> ArtifactResult {
        auto s = spec(data, width, height);
        PublicationAuthority authority;
        RenderRequest request{s, backend.toolchain, root / ("job-" + std::to_string(index++))};
        auto result =
            authority.generate(request, root / "cache", authority.supersede(), backend.render);
        if (auto* error = std::get_if<Error>(&result))
            check(false, error->message.c_str());
        if (std::holds_alternative<Artifact>(result)) {
            const auto raster = std::get<Raster>(
                readRgba(std::get<Artifact>(result).directory / "static.png", width, height));
            for (const auto& label : s.labels) {
                if (label.text.empty())
                    continue;
                std::size_t visible = 0;
                for (int y = 0; y < height; ++y)
                    for (int x = 0; x < width; ++x)
                        if (x + .5 >= label.band.left &&
                            x + .5 < label.band.left + label.band.width &&
                            y + .5 >= label.band.top && y + .5 < label.band.top + label.band.height)
                            visible += raster.rgba[(static_cast<std::size_t>(y) *
                                                        static_cast<std::size_t>(width) +
                                                    static_cast<std::size_t>(x)) *
                                                       4 +
                                                   3] != 0;
                check(visible > 0, "ラベルの実 raster に指定帯の可視画素がある");
            }
        }
        return result;
    };
    if (!mode.empty()) {
        auto data = line("#80FF0000");
        int width = 854, height = 480;
        if (mode == "geometry")
            data.functions[0].expression = "1/x";
        if (mode == "stroke") {
            data = line("#FFFF0000", 3);
            width = 1280;
            height = 720;
        }
        if (mode == "grid") {
            data = line("#00FF0000");
            data.axes.showGrid = true;
        }
        if (mode == "labels") {
            data.axes.xLabel = "x";
            data.axes.yLabel = "y";
            data.functions[0].label = "y=\\sin(x)";
        }
        if (mode == "draw")
            data.intro = {mvm::project::GraphIntroKind::Draw, 3};
        if (mode == "alpha" || mode == "order") {
            auto f = data.functions[0];
            f.id.value = "blue";
            f.color = "#800000FF";
            data.functions.push_back(f);
            if (mode == "order") {
                f.id.value = "green";
                f.color = "#8000FF00";
                data.functions.push_back(f);
            }
        }
        const auto result = render(data, width, height);
        if (!std::holds_alternative<Artifact>(result))
            return;
        const auto artifact = std::get<Artifact>(result);
        const auto pixels =
            std::get<Raster>(readRgba(artifact.directory / "static.png", width, height));
        const auto center =
            (static_cast<std::size_t>(height / 2) * static_cast<std::size_t>(width) +
             static_cast<std::size_t>(width / 2)) *
            4;
        if (mode == "alpha" || mode == "order") {
            const std::array<int, 4> expected = mode == "alpha"
                                                    ? std::array<int, 4>{85, 0, 170, 192}
                                                    : std::array<int, 4>{36, 146, 73, 224};
            for (std::size_t i = 0; i < 4; ++i)
                check(pixels.rgba[center + i] == expected[i], "変異検査の多色 oracle");
        }
        if (mode == "stroke") {
            long double footprint = 0;
            for (int y = 0; y < height; ++y)
                footprint +=
                    pixels.rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                 static_cast<std::size_t>(width / 2)) *
                                    4 +
                                3] /
                    255.0L;
            check(std::abs(footprint - 2) < .07, "変異検査の解像度別線幅");
        }
        if (mode == "grid")
            check(std::any_of(pixels.rgba.begin(), pixels.rgba.end(), [](auto v) { return v > 0; }),
                  "軸なしでも grid が可視");
        if (mode == "draw") {
            const auto first =
                std::get<Raster>(readRgba(artifact.directory / "frame-0.png", width, height));
            check(std::all_of(first.rgba.begin(), first.rgba.end(), [](auto v) { return v == 0; }),
                  "frame0 の進捗は0");
            const auto middle =
                std::get<Raster>(readRgba(artifact.directory / "frame-1.png", width, height));
            check(middle.rgba[(240 * 854 + 200) * 4 + 3] == 128 &&
                      middle.rgba[(240 * 854 + 350) * 4 + 3] == 0,
                  "frame1 の進捗は1/3");
        }
        return;
    }
    for (const auto& size : {std::pair{1920, 1080}, std::pair{1280, 720}, std::pair{854, 480}}) {
        for (double stroke : {.1, 3.0, 64.0}) {
            auto result = render(line("#FFFF0000", stroke), size.first, size.second);
            if (!std::holds_alternative<Artifact>(result))
                continue;
            const auto raster = std::get<Raster>(readRgba(
                std::get<Artifact>(result).directory / "static.png", size.first, size.second));
            long double footprint = 0;
            for (int y = 0; y < size.second; ++y)
                footprint +=
                    raster
                        .rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(size.first) +
                               static_cast<std::size_t>(size.first / 2)) *
                                  4 +
                              3] /
                    255.0L;
            const auto expected = stroke * strokeScale(size.first, size.second);
            report << "stroke\t" << size.first << '\t' << size.second << '\t' << expected << '\t'
                   << footprint << '\n';
            // この水平 fixture の Cairo 被覆量子化だけを許す。一般の画素同値には適用しない。
            check(std::abs(footprint - expected) <= 1.0L / 15 + 1.0L / 255,
                  "被覆積分で pixel 線幅を確認");
        }
    }
    for (const auto* expression : {"x^2", "sin(x)", "1/x", "sqrt(x)", "log(x)", "tan(x)", "100"}) {
        auto data = line("#FFFF6655", 3);
        data.functions[0].expression = expression;
        auto result = render(data);
        if (!std::holds_alternative<Artifact>(result))
            continue;
        if (std::string(expression) == "1/x" || std::string(expression) == "tan(x)") {
            auto raster = std::get<Raster>(
                readRgba(std::get<Artifact>(result).directory / "static.png", 854, 480));
            const int column = std::string(expression) == "1/x" ? 427 : 528;
            check(raster.rgba[(240 * 854 + static_cast<std::size_t>(column)) * 4 + 3] == 0,
                  "不連続に false bridge がない");
        }
    }
    // 同じ水平線の内部は coverage=255。独立な確定値で重なりを照合する。
    for (int count = 1; count <= 3; ++count) {
        auto data = line("#80FF0000");
        if (count >= 2) {
            auto f = data.functions[0];
            f.id.value = "blue";
            f.color = "#800000FF";
            data.functions.push_back(f);
        }
        if (count >= 3) {
            auto f = data.functions[0];
            f.id.value = "green";
            f.color = "#8000FF00";
            data.functions.push_back(f);
        }
        auto result = render(data);
        if (!std::holds_alternative<Artifact>(result))
            continue;
        auto raster = std::get<Raster>(
            readRgba(std::get<Artifact>(result).directory / "static.png", 854, 480));
        const std::array<std::array<int, 4>, 3> expected = {
            {{255, 0, 0, 128}, {85, 0, 170, 192}, {36, 146, 73, 224}}};
        const auto offset = (240 * 854 + 427) * 4;
        for (std::size_t c = 0; c < 4; ++c)
            check(raster.rgba[offset + c] == expected[static_cast<std::size_t>(count - 1)][c],
                  "多色 interior oracle");
    }
    for (bool axis : {false, true}) {
        auto data = line("#80FF0000");
        data.axes.showAxes = axis;
        data.axes.showGrid = !axis;
        auto result = render(data, 1920, 1080);
        if (!std::holds_alternative<Artifact>(result))
            continue;
        const auto raster = std::get<Raster>(
            readRgba(std::get<Artifact>(result).directory / "static.png", 1920, 1080));
        std::array<long double, 4> expected{};
        const auto paths = framePaths(spec(data, 1920, 1080), -1);
        for (std::size_t p = 0; p < paths.size(); ++p) {
            const auto mask =
                std::get<Raster>(readRgba(root / ("job-" + std::to_string(index - 1)) /
                                              ("coverage-" + std::to_string(p) + ".png"),
                                          1920, 1080));
            const auto coverage = mask.rgba[(540 * 1920 + 960) * 4 + 3];
            const long double a = std::round(coverage * (paths[p].argb >> 24) / 255.0L) / 255;
            const long double b = expected[3] / 255, opacity = a + b * (1 - a);
            for (std::size_t c = 0; c < 3; ++c) {
                const auto color = (paths[p].argb >> (16 - 8 * c)) & 255;
                expected[c] =
                    opacity ? std::round((color * a + expected[c] * b * (1 - a)) / opacity) : 0;
            }
            expected[3] = std::round(opacity * 255);
        }
        for (std::size_t c = 0; c < 4; ++c)
            check(raster.rgba[(540 * 1920 + 960) * 4 + c] == expected[c],
                  "半透明曲線と axis/grid の独立 oracle");
    }
    {
        auto result = render(line("#80FF0000", 3));
        if (std::holds_alternative<Artifact>(result)) {
            const auto raster = std::get<Raster>(
                readRgba(std::get<Artifact>(result).directory / "static.png", 854, 480));
            std::size_t partial = 0;
            for (std::size_t p = 0; p < raster.rgba.size(); p += 4)
                if (raster.rgba[p + 3] > 0 && raster.rgba[p + 3] < 128) {
                    ++partial;
                    check(raster.rgba[p] == 255 && raster.rgba[p + 1] == 0 &&
                              raster.rgba[p + 2] == 0,
                          "AA 境界も straight RGB");
                }
            check(partial > 0, "AA の検査が空振りでない");
        }
    }
    auto data = line("#00FF0000");
    auto transparent = render(data);
    if (std::holds_alternative<Artifact>(transparent)) {
        auto raster = std::get<Raster>(
            readRgba(std::get<Artifact>(transparent).directory / "static.png", 854, 480));
        check(std::all_of(raster.rgba.begin(), raster.rgba.end(), [](auto v) { return v == 0; }),
              "alpha-zero の全透明を受理");
    }
    for (int n : {1, 3, 10}) {
        data = line();
        data.intro = {mvm::project::GraphIntroKind::Draw, n};
        auto result = render(data);
        if (!std::holds_alternative<Artifact>(result))
            continue;
        auto artifact = std::get<Artifact>(result);
        const auto blank = std::get<Raster>(readRgba(artifact.directory / "frame-0.png", 854, 480));
        check(std::all_of(blank.rgba.begin(), blank.rgba.end(), [](auto v) { return v == 0; }),
              "Draw frame0 は正当な全透明");
        for (int f : {n - 1, 0, n / 2, n - 1}) {
            auto raster = std::get<Raster>(
                readRgba(artifact.directory / ("frame-" + std::to_string(f) + ".png"), 854, 480));
            check(digest({reinterpret_cast<const char*>(raster.rgba.data()), raster.rgba.size()}) ==
                      artifact.pixelHashes[static_cast<std::size_t>(f + 1)],
                  "任意順で保存画素が一致");
        }
        if (n == 10) {
            std::atomic<bool> cancel{false};
            std::promise<void> entered;
            auto signal = entered.get_future();
            const auto expectedSpec = spec(data);
            auto validating = std::async(std::launch::async, [&] {
                entered.set_value();
                return validateArtifact(artifact.directory, expectedSpec, backend.toolchain,
                                        &cancel);
            });
            signal.wait();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const bool pending =
                validating.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready;
            cancel.store(true);
            const auto cancelled = validating.get();
            const auto* error = std::get_if<Error>(&cancelled);
            check(pending && error && error->failure == Failure::Cancelled,
                  "読み取り検証の途中の取消");
        }
    }
    data = line();
    data.axes = {true, true, "x", "y"};
    data.functions[0].label = "y=\\frac{1}{x}";
    auto labels = render(data);
    check(std::holds_alternative<Artifact>(labels), "実 TeX label");
    data.functions[0].label =
        "y=x^2\\quad\\text{very long function label repeated many times to check layout}";
    data.axes.xLabel = "x\\quad\\text{long horizontal axis label repeated for fitting}";
    data.axes.yLabel = "y\\quad\\text{long vertical axis label repeated for fitting}";
    for (int i = 1; i < 3; ++i) {
        auto f = data.functions[0];
        f.id.value = "label-" + std::to_string(i);
        f.expression = i == 1 ? "sin(x)" : "1/x";
        f.color = i == 1 ? "#8055CCFF" : "#80FFFF00";
        f.label = i == 1 ? "y=\\sin(x)" : "y=\\frac{1}{x}";
        data.functions.push_back(f);
    }
    check(std::holds_alternative<Artifact>(render(data)), "三つの label と長い label の実配置");
    data = line();
    data.axes = {false, false, "x", "y"};
    data.functions[0].label = "\\notACommand{";
    PublicationAuthority authority;
    RenderRequest request{spec(data), backend.toolchain, root / "malformed-label"};
    auto malformed =
        authority.generate(request, root / "cache", authority.supersede(), backend.render);
    check(std::holds_alternative<Error>(malformed) &&
              std::get<Error>(malformed).failure == Failure::LabelFailure,
          "TeX failure は typed");
    for (const char* expression : {"abs(x)", "1/x"}) {
        data = line();
        data.functions[0].expression = expression;
        const auto result = render(data);
        if (!std::holds_alternative<Artifact>(result))
            continue;
        const auto raster = std::get<Raster>(
            readRgba(std::get<Artifact>(result).directory / "static.png", 854, 480));
        for (int y = 0; y < 480; ++y)
            for (int x = 0; x < 854; ++x)
                if (x + .5 < 106.75 || x + .5 >= 747.25 || y + .5 < 60 || y + .5 >= 420)
                    check(raster.rgba[(static_cast<std::size_t>(y) * 854 +
                                       static_cast<std::size_t>(x)) *
                                          4 +
                                      3] == 0,
                          "太い線・鋭い角も plot の外へ描かない");
    }
    data = line("#FFFF0000", 3);
    data.axes = {true, true, "x", "y"};
    data.intro = {mvm::project::GraphIntroKind::Draw, 3};
    const auto drawBackground = render(data);
    data.functions[0].color = "#00000000";
    data.intro = {};
    const auto staticBackground = render(data);
    if (std::holds_alternative<Artifact>(drawBackground) &&
        std::holds_alternative<Artifact>(staticBackground)) {
        const auto first = std::get<Raster>(
            readRgba(std::get<Artifact>(drawBackground).directory / "frame-0.png", 854, 480));
        const auto base = std::get<Raster>(
            readRgba(std::get<Artifact>(staticBackground).directory / "static.png", 854, 480));
        check(first.rgba == base.rgba &&
                  std::any_of(first.rgba.begin(), first.rgba.end(), [](auto v) { return v != 0; }),
              "Draw frame0 でも軸・grid・ラベルは静止");
    }
    for (const std::string phase : {"startup", "static", "draw"}) {
        data = line();
        data.intro = {mvm::project::GraphIntroKind::Draw, 10};
        RenderRequest cancelRequest{spec(data), backend.toolchain, root / ("cancel-" + phase)};
        PublicationAuthority cancelAuthority;
        const auto generation = cancelAuthority.supersede();
        std::atomic<bool> cancel{false};
        auto task = std::async(std::launch::async, [&] {
            return cancelAuthority.generate(cancelRequest, root / ("cancel-cache-" + phase),
                                            generation, backend.render, &cancel);
        });
        bool observed = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (std::chrono::steady_clock::now() < deadline) {
            std::ifstream input(cancelRequest.job / "phase.txt");
            std::string actual;
            input >> actual;
            if (actual == phase) {
                observed = true;
                break;
            }
            if (task.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        cancel.store(true);
        const auto result = task.get();
        const auto* error = std::get_if<Error>(&result);
        check(observed && error && error->failure == Failure::Cancelled &&
                  !std::filesystem::exists(root / ("cancel-cache-" + phase)),
              "実子 process の各段階の取消は Ready にしない");
    }
}
} // namespace

int main(int argc, char** argv) {
    pure();
    if (argc >= 2) {
        const std::filesystem::path root = std::filesystem::absolute(argv[1]);
        if (std::filesystem::exists(root)) {
            std::cerr << "新しい証拠 directory が必要です\n";
            return 2;
        }
        std::filesystem::create_directories(root);
        artifacts(root, argc == 3 && std::string(argv[2]) == "responsiveness");
        if (argc >= 4)
            real(root, argv[2], argv[3], argc == 5 ? argv[4] : "");
    }
    std::cout << "Graph 描画検証: 失敗 " << failures << " 件\n";
    return failures ? 1 : 0;
}
