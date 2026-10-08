#include "app/graph_render_compile.h"
#include "graph_preview_cache.h"
#include "project/graph_edit.h"

#include <fstream>
#include <future>
#include <iostream>
#include <thread>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QThread>

namespace {
using Cache = mvm::app::GraphPreviewCache;
using namespace mvm;
int failures = 0;

void check(bool value, const char* message) {
    if (!value) {
        ++failures;
        std::cerr << "失敗: " << message << '\n';
    }
}

bool wait(const std::function<bool()>& predicate) {
    QElapsedTimer clock;
    clock.start();
    while (!predicate() && clock.elapsed() < 10000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return predicate();
}

graph::GraphRenderSpec spec(int n, const char* expression = "x^2") {
    auto data = project::defaultGraph({"owner"});
    data.axes = {false, false, "", ""};
    data.functions[0].expression = expression;
    data.intro = n ? project::GraphIntro{project::GraphIntroKind::Draw, n} : project::GraphIntro{};
    return std::get<graph::GraphRenderSpec>(app::compileGraphRender(data, 100, 64, 36));
}

graph::RenderResult fake(const graph::RenderRequest& request, const std::atomic<bool>*) {
    for (std::int64_t i = -1; i < request.spec.drawFrames; ++i) {
        graph::Raster raster{request.spec.width, request.spec.height,
                             std::vector<std::uint8_t>(64 * 36 * 4, 0)};
        raster.rgba[0] = request.spec.curves[0].ast == spec(0).curves[0].ast ? 200 : 100;
        raster.rgba[1] = static_cast<std::uint8_t>(i + 1);
        raster.rgba[3] = 128;
        if (!graph::writeRgba(request.job /
                                  (i < 0 ? "static.png" : "frame-" + std::to_string(i) + ".png"),
                              raster))
            return graph::Error{graph::Failure::RendererFailure, 0,
                                "試験 PNG の保存に失敗しました"};
    }
    return std::monostate{};
}

void residency(const std::filesystem::path& root) {
    std::atomic<int> calls{0};
    std::atomic<bool> available{true};
    Cache cache([&](const auto&, const auto*) -> std::variant<manim::GraphBackend, graph::Error> {
        if (!available)
            return graph::Error{graph::Failure::BackendUnavailable, 0, "試験で停止"};
        return manim::GraphBackend{"test-identity", [&](const auto& request, const auto* cancel) {
                                       ++calls;
                                       return fake(request, cancel);
                                   }};
    });
    cache.setAuthority(root, true);
    cache.request(spec(0));
    check(wait([&] { return cache.status(spec(0), 0).identityEstablished; }), "worker preflight");
    for (int n : {1, 3, 10}) {
        const auto s = spec(n);
        cache.request(s);
        cache.request(s);
        if (!wait([&] { return cache.status(s, 0).artifact == Cache::ArtifactState::Validated; })) {
            check(false, "完全な Draw の authority");
            std::cerr << cache.status(s, 0).message.toStdString() << '\n';
            return;
        }
        const int before = calls;
        for (int i : {n - 1, 0, n / 2, n, n + 1, 99}) {
            cache.requestFrame(s, i);
            if (!wait([&] { return cache.status(s, i).residency == Cache::Residency::Resident; })) {
                check(false, "非単調 seek の exact frame");
                return;
            }
            auto frames = cache.frames(s);
            auto frame = frames.at(i < n ? i : -1);
            check(frame->rgba[0] == 200 && frame->rgba[1] == (i < n ? i + 1 : 0) &&
                      frame->rgba[3] == 128,
                  "frame 番号・straight alpha は独立期待値と一致");
        }
        check(calls == before, "RAM からの再読込に renderer は不要");
        const auto count = cache.renderCount();
        for (int i = 0; i < 100; ++i)
            cache.status(s, i);
        check(cache.renderCount() == count && calls == before, "状態照会は描画しない");
    }
    const auto s = spec(3);
    cache.requestFrame(s, 1);
    check(wait([&] { return cache.status(s, 1).residency == Cache::Residency::Resident; }),
          "寿命試験の frame");
    auto held = cache.frames(s).at(1);
    const auto bytes = cache.residentBytes();
    cache.retainOnly({});
    check(held->rgba[1] == 2 && cache.residentBytes() == held->rgba.size() &&
              bytes >= held->rgba.size(),
          "退避後も GPU snapshot の共有所有と byte 計上を維持");
    held.reset();
    check(cache.residentBytes() == 0, "最後の所有者で byte を解放");
    cache.setBudgetForTest(1);
    cache.request(s);
    check(wait([&] { return cache.status(s, 0).artifact == Cache::ArtifactState::Validated; }),
          "disk 完全性は RAM 上限と独立");
    cache.requestFrame(s, 0);
    check(cache.status(s, 0).residency == Cache::Residency::OverBudget &&
              cache.status(s, 0).artifact == Cache::ArtifactState::Validated &&
              cache.residentBytes() == 0,
          "OverBudget は artifact 破損ではない");
    cache.setBudgetForTest(64 * 36 * 4);
    cache.requestFrame(s, 2);
    check(wait([&] { return cache.status(s, 2).residency == Cache::Residency::Resident; }),
          "一枚だけの予算");
    cache.requestFrame(s, 2);
    check(cache.status(s, 2).frameAvailable && cache.residentBytes() == 64 * 36 * 4 &&
              !cache.status(s, 3).frameAvailable,
          "先読みの予算不足は現在の exact frame を退避させない");
    available = false;
    cache.refreshBackend();
    check(wait([&] { return !cache.status(s, 2).backendAvailable; }), "backend の一時停止");
    check(cache.status(s, 2).identityEstablished && cache.frames(s).at(2)->rgba[1] == 3,
          "session 内の確定 identity と現在画素は保持");
    const auto b = spec(3, "sin(x)");
    cache.request(b);
    check(wait([&] { return cache.status(b, 0).job == Cache::Job::Failed; }) &&
              cache.frames(b).empty(),
          "backend 不在で missing key は透明");
    Cache unidentified(
        [](const auto&, const auto*) -> std::variant<manim::GraphBackend, graph::Error> {
            return graph::Error{graph::Failure::BackendUnavailable, 0, "identity 不明"};
        });
    unidentified.setAuthority(root, false);
    unidentified.request(s);
    check(unidentified.frames(s).empty() && !unidentified.status(s, 0).identityEstablished,
          "新 session は正体不明の旧 artifact を受理しない");
    cache.shutdown();
}

void shutdownLifetime(const std::filesystem::path& root) {
    std::promise<void> entered;
    auto started = entered.get_future();
    std::atomic<bool> cancelled{false};
    Cache cache([&](const auto&, const auto*) -> std::variant<manim::GraphBackend, graph::Error> {
        return manim::GraphBackend{
            "shutdown-identity", [&](const auto&, const auto* cancel) {
                entered.set_value();
                while (!cancel->load())
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                cancelled = true;
                return graph::RenderResult(
                    graph::Error{graph::Failure::Cancelled, 0, "試験の取消"});
            }};
    });
    cache.setAuthority(root, true);
    cache.request(spec(3));
    check(wait([&] {
              return started.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
          }),
          "shutdown 前に renderer が開始");
    cache.shutdown();
    check(cancelled && cache.frames(spec(3)).empty(), "shutdown は取消の観測と worker join を完了");
    QCoreApplication::processEvents();
    check(cache.frames(spec(3)).empty(), "join 後の完了 callback は resident を復活させない");
}

void mappedPixels(const std::filesystem::path& root) {
    Cache cache([](const auto&, const auto*) -> std::variant<manim::GraphBackend, graph::Error> {
        return manim::GraphBackend{"mapping-identity", fake};
    });
    cache.setAuthority(root, true);
    auto project = project::createDefaultProject();
    project.timelineFpsNum = 30;
    check(project::addGraph(project, "mapped", {"curve"}, "対応", {}, 0).success,
          "画素 mapping 用 Graph");
    auto& clip = project.timelineClips.back();
    clip.sourceFpsNum = 60;
    clip.sourceFrameCount = 100;
    clip.sourceInFrame = 2;
    clip.sourceOutFrame = 12;
    clip.graph.axes = {false, false, "", ""};
    clip.graph.intro = {project::GraphIntroKind::Draw, 10};
    const auto s =
        std::get<graph::GraphRenderSpec>(app::compileGraphRender(clip.graph, 100, 64, 36));
    cache.request(s);
    check(wait([&] { return cache.status(s, 0).artifact == Cache::ArtifactState::Validated; }),
          "mapping 用の完全 Draw");
    const auto before = project;
    check(project::splitTimelineClips(
              project, {"mapped"}, 3, [] { return "right"; }, project::LinkMode::Single)
              .success,
          "Draw の途中で分割");
    for (const auto& candidate : project.timelineClips) {
        for (int local = 0; local < (candidate.id == "mapped" ? 3 : 2); ++local) {
            std::string error;
            const auto mapping = project::evaluateGraphClip(candidate, {30, 1}, local, error);
            const auto expectedSource = 2 + 2 * (candidate.timelineStartFrame + local);
            if (!mapping) {
                check(false, "分割片の source frame は有効");
                continue;
            }
            check(mapping->sourceFrame == expectedSource, "trim と split は素材原点の位相を維持");
            cache.requestFrame(s, mapping->sourceFrame);
            check(wait([&] { return cache.status(s, mapping->sourceFrame).frameAvailable; }),
                  "mapping の exact pixel が resident");
            const auto index = expectedSource < 10 ? expectedSource : -1;
            const auto frames = cache.frames(s);
            if (frames.contains(index))
                check(frames.at(index)->rgba[1] == (expectedSource < 10 ? expectedSource + 1 : 0),
                      "異なる fps と分割後も Draw が再開しない独立画素期待値");
            else
                check(false, "期待 source frame の画素が存在");
        }
    }
    project = before;
    check(cache.keyFor(s) == cache.keyFor(std::get<graph::GraphRenderSpec>(app::compileGraphRender(
                                 project.timelineClips.back().graph, 100, 64, 36))),
          "Undo は renderer identity を維持");
    cache.shutdown();
}

void stale(const std::filesystem::path& root) {
    std::promise<void> entered, release;
    auto barrier = release.get_future().share();
    std::atomic<bool> first{true};
    Cache cache([&](const auto&, const auto*) -> std::variant<manim::GraphBackend, graph::Error> {
        return manim::GraphBackend{"test-stale", [&](const auto& request, const auto* cancel) {
                                       if (first.exchange(false)) {
                                           entered.set_value();
                                           barrier.wait();
                                       }
                                       return fake(request, cancel);
                                   }};
    });
    cache.setAuthority(root, true);
    const auto a = spec(3), b = spec(3, "sin(x)");
    cache.request(a);
    check(wait([&] { return cache.status(a, 0).identityEstablished; }), "stale 試験の preflight");
    cache.request(a);
    auto started = entered.get_future();
    check(wait([&] {
              return started.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
          }),
          "renderer の同期バリア");
    cache.retainOnly({cache.keyFor(b)});
    cache.request(b);
    check(cache.frames(b).empty(), "B pending は A を代用しない");
    check(wait([&] { return cache.status(b, 0).artifact == Cache::ArtifactState::Validated; }),
          "同期バリア中の A より先に B が完了");
    release.set_value();
    check(wait([&] { return cache.status(b, 0).artifact == Cache::ArtifactState::Validated; }),
          "旧要求解放後に B が Ready");
    cache.requestFrame(b, 1);
    check(wait([&] { return cache.status(b, 1).residency == Cache::Residency::Resident; }),
          "B の画素を decode");
    check(cache.frames(b).at(1)->rgba[0] == 100 && cache.frames(a).empty(),
          "旧キー・取消結果は現在へ昇格しない");
    cache.resetSession();
    check(cache.frames(b).empty(), "新 Project session へ画素を持ち込まない");
    cache.shutdown();
}

void corrupt(const std::filesystem::path& root) {
    Cache cache([](const auto&, const auto*) -> std::variant<manim::GraphBackend, graph::Error> {
        return manim::GraphBackend{"test-corrupt", fake};
    });
    const auto s = spec(3);
    cache.setAuthority(root, true);
    cache.request(s);
    if (!wait([&] { return cache.status(s, 0).artifact == Cache::ArtifactState::Validated; })) {
        check(false, "破損試験の完全 artifact");
        return;
    }
    const auto key = cache.keyFor(s);
    auto ownership = s;
    ownership.curves[0].geometry.functionId = "remapped-owner";
    check(cache.keyFor(ownership) == key, "所有 ID の remap は renderer identity を変えない");
    cache.retainOnly({key});
    cache.requestFrame(ownership, 1);
    check(wait([&] { return cache.status(ownership, 1).residency == Cache::Residency::Resident; }),
          "同じ artifact を残る consumer が使える");
    std::filesystem::remove(root / key.toStdString() / "frame-2.png");
    cache.requestFrame(s, 2);
    check(wait([&] { return cache.status(s, 2).artifact == Cache::ArtifactState::Corrupt; }) &&
              cache.frames(s).empty(),
          "欠けた exact frame は古い resident と一緒に authority を失う");
    cache.setAuthority(root, false);
    cache.request(s);
    check(wait([&] { return cache.status(s, 0).job == Cache::Job::Failed; }) &&
              cache.status(s, 0).artifact == Cache::ArtifactState::Corrupt,
          "不完全 Draw を読み取り専用で Ready にしない");
    const auto missing = spec(10);
    cache.request(missing);
    check(wait([&] { return cache.status(missing, 0).job == Cache::Job::Failed; }),
          "読み取り専用で missing artifact を公開しない");
    check(!std::filesystem::exists(root / cache.keyFor(missing).toStdString()),
          "非所有者は cache を作らない");
    cache.shutdown();
}

void pixelMismatch(const std::filesystem::path& root) {
    Cache cache([](const auto&, const auto*) -> std::variant<manim::GraphBackend, graph::Error> {
        return manim::GraphBackend{"test-pixel", fake};
    });
    const auto s = spec(1);
    cache.setAuthority(root, true);
    cache.request(s);
    if (!wait([&] { return cache.status(s, 0).artifact == Cache::ArtifactState::Validated; })) {
        check(false, "画素改変試験の完全 artifact");
        return;
    }
    const auto path = root / cache.keyFor(s).toStdString() / "frame-0.png";
    auto raster = graph::readRgba(path, 64, 36);
    if (!std::holds_alternative<graph::Raster>(raster)) {
        check(false, "改変前の frame を読めない");
        return;
    }
    auto image = std::get<graph::Raster>(raster);
    image.rgba[0] ^= 0xFF;
    check(graph::writeRgba(path, image), "検証済み raster の一画素を反転");
    const auto renders = cache.renderCount();
    cache.requestFrame(s, 0);
    check(wait([&] { return cache.status(s, 0).artifact == Cache::ArtifactState::Corrupt; }) &&
              cache.frames(s).empty() && cache.renderCount() == renders,
          "hash 不一致は Ready に昇格せず再描画もしない");
    cache.shutdown();
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir temp(QString::fromLocal8Bit(argc > 1 ? argv[1] : "build") +
                       QStringLiteral("/graph-preview-XXXXXX"));
    temp.setAutoRemove(false);
    if (!temp.isValid())
        return 2;
    const std::filesystem::path root(temp.path().toStdWString());
    std::cout << "証拠: " << temp.path().toStdString() << '\n';
    residency(root / "residency");
    stale(root / "stale");
    corrupt(root / "corrupt");
    pixelMismatch(root / "pixel");
    shutdownLifetime(root / "shutdown");
    mappedPixels(root / "mapping");
    return failures ? 1 : 0;
}
