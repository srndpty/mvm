#include "app/graph_render_compile.h"
#include "app/preview/preview_engine_rhi_item.h"
#include "app/preview/test_window_mode.h"
#include "media/mlt/mvm_mlt_runtime.h"
#include "media_import.h"
#include "mvm_controller.h"
#include "preview_engine/preview_engine_internal.h"
#include "project/graph_edit.h"
#include "project/project_json.h"
#include "test_window_isolation.h"

#include <fstream>
#include <iostream>

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QQuickWindow>
#include <QThread>
#include <QUrl>

namespace {
using namespace mvm;
int checks = 0, failures = 0;

bool check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "失敗: " << message << '\n';
    }
    return condition;
}

bool pump(const std::function<bool()>& predicate, int timeout = 10000) {
    QElapsedTimer time;
    time.start();
    while (!predicate() && time.elapsed() < timeout) {
        QGuiApplication::processEvents();
        QThread::msleep(2);
    }
    return predicate();
}

std::vector<std::uint8_t> whiteOracle(const graph::Raster& raster, int shift = 0) {
    std::vector<std::uint8_t> result(raster.rgba.size(), 255);
    for (int y = 0; y < raster.height; ++y)
        for (int x = 0; x < raster.width; ++x) {
            const int sourceX = x - shift;
            if (sourceX < 0)
                continue;
            const auto at = static_cast<std::size_t>(y * raster.width + x) * 4;
            const auto source = static_cast<std::size_t>(y * raster.width + sourceX) * 4;
            const int a = raster.rgba[source + 3];
            for (std::size_t c = 0; c < 3; ++c)
                result[at + c] = static_cast<std::uint8_t>(
                    (raster.rgba[source + c] * a + 255 * (255 - a) + 127) / 255);
        }
    return result;
}

std::vector<std::uint8_t> pixels(const QImage& input) {
    const auto image = input.convertToFormat(QImage::Format_RGBA8888);
    std::vector<std::uint8_t> result;
    for (int y = 0; y < image.height(); ++y)
        result.insert(result.end(), image.constScanLine(y),
                      image.constScanLine(y) + image.width() * 4);
    return result;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "使い方: mvm_test_graph_native_preview <manim.exe> <白い動画> <新規証拠 "
                     "directory>\n";
        return 2;
    }
    app::prepareTestFixedWindowEnvironment();
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    QGuiApplication application(argc, argv);
    const auto args = application.arguments();
    if (!check(mvm_mlt_runtime_init(MVM_MLT_MODULE_DIR, MVM_MLT_DATA_DIR) == 0,
               "MLT runtime の検証済み初期化"))
        return 1;
    std::error_code pathError;
    const std::filesystem::path manim =
        std::filesystem::absolute(std::filesystem::path(args[1].toStdWString()), pathError)
            .lexically_normal();
    const std::filesystem::path video =
        std::filesystem::absolute(std::filesystem::path(args[2].toStdWString()), pathError)
            .lexically_normal();
    const std::filesystem::path root =
        std::filesystem::absolute(std::filesystem::path(args[3].toStdWString()), pathError)
            .lexically_normal();
    if (pathError || manim.empty() || video.empty() || root.empty())
        return 2;
    std::error_code ec;
    if (!std::filesystem::create_directories(root, ec) || ec)
        return 2;
    auto initial = project::createDefaultProject();
    initial.timelineFpsNum = 30;
    initial.outputWidth = 320;
    initial.outputHeight = 180;
    auto imported = app::probeMediaFile(video);
    if (!check(imported.success, "通常動画の probe"))
        return 1;
    imported.item.id = "video-item";
    imported.item.name = "白い通常動画";
    initial.mediaItems.push_back(imported.item);
    project::TimelineClip base;
    base.id = "video";
    base.name = "通常動画";
    base.mediaItemId = "video-item";
    base.mediaPath = video;
    base.sourceFpsNum = imported.item.fpsNum;
    base.sourceFpsDen = imported.item.fpsDen;
    base.sourceFrameCount = imported.item.frameCount;
    base.sourceOutFrame = 10;
    initial.timelineClips.push_back(base);
    const auto added =
        project::addGraph(initial, "graph", {"f0"}, "Graph", {project::TrackKind::Video, 1}, 0);
    if (!check(added.success, "動画の上に Graph を置く")) {
        std::cerr << added.error << '\n';
        return 1;
    }
    auto& clip = initial.timelineClips.back();
    clip.sourceFrameCount = clip.sourceOutFrame = 10;
    clip.graph.intro = {project::GraphIntroKind::Draw, 3};
    clip.graph.functions[0].label = "x^2";
    clip.graph.functions[0].color = "#80FF6655";
    auto second = clip.graph.functions[0];
    second.id = {"f1"};
    second.expression = second.label = "sin(x)";
    second.color = "#C044CCFF";
    clip.graph.functions.push_back(second);
    second.id = {"f2"};
    second.expression = second.label = "1/x";
    second.color = "#FF66FF88";
    clip.graph.functions.push_back(second);
    const auto path = root / "graph.mvm";
    if (!check(project::saveProjectJson(initial, path).success, "schema22 を保存"))
        return 1;
    {
        app::MvmController closed(path, manim, initial);
        closed.shutdown();
    }
    const auto reopened = project::loadProjectJson(path);
    if (!check(reopened.success && reopened.project == initial, "閉じて schema22 を再読込")) {
        if (!reopened.success)
            std::cerr << reopened.error << '\n';
        return 1;
    }
    app::MvmController controller(path, manim, reopened.project);
    QQuickWindow window;
    window.setFlags(app::testBackgroundWindowFlags());
    app::applyTestFixedWindow(window);
    window.resize(320, 180);
    auto* surface = new app::PreviewEngineRhiItem(window.contentItem());
    surface->setWidth(320);
    surface->setHeight(180);
    controller.attachPreview(surface);
    window.show();
    if (!check(pump([&] { return controller.previewReady(); }), "実 D3D11 preview の初期化"))
        return 1;
    QString isolationReason;
    if (!test::backgroundWindowIsolated(window, isolationReason)) {
        std::cerr << "PROTOCOL_INVALID: " << isolationReason.toStdString() << '\n';
        return 3;
    }
    QElapsedTimer rendering;
    rendering.start();
    if (!check(pump(
                   [&] {
                       return controller.graphPreviewStatus("graph", 0).artifact ==
                              app::GraphPreviewCache::ArtifactState::Validated;
                   },
                   600000),
               "実 Manim の完全 artifact")) {
        std::cerr << controller.graphPreviewStatus("graph", 0).message.toStdString() << '\n';
        return 1;
    }
    const auto readyMs = rendering.elapsed();
    const auto compiled = app::compileGraphRender(clip.graph, 10, 320, 180);
    const auto spec = std::get<graph::GraphRenderSpec>(compiled);
    const auto key = controller.graphRastersForTest().keyFor(spec).toStdString();
    const auto artifact = root / "cache" / "graph" / "graph.mvm" / key;
    std::ofstream observations(root / "observations.tsv");
    observations
        << "source_frame\tdecode_and_submission_ms\tresident_bytes\tpeak_bytes\tmismatch_pixels\n";
    for (int frame : {2, 0, 1, 3, 4, 9}) {
        QElapsedTimer latency;
        latency.start();
        bool accepted = false;
        if (!check(pump([&] {
                       if (!accepted)
                           accepted = controller.seekTimelineFrame(frame);
                       return accepted;
                   }),
                   "任意順 seek")) {
            const auto telemetry = controller.previewEngineForTest()->telemetry();
            const auto diagnostic = preview::internal::PreviewRenderPort::runtimeDiagnostics(
                *controller.previewEngineForTest());
            std::cerr << controller.statusText().toStdString()
                      << " state=" << static_cast<int>(telemetry.status.state)
                      << " exposed=" << window.isExposed()
                      << " presented=" << telemetry.presentedFrameCount
                      << " queue=" << telemetry.currentSourceQueueDepth
                      << " seek=" << diagnostic.seekRequestCount
                      << " decoded=" << diagnostic.seekDecodeReadyCount
                      << " completed=" << diagnostic.seekCompletedCount << '\n';
            break;
        }
        if (!check(pump([&] {
                       return controller.graphPreviewStatus("graph", frame).residency ==
                                  app::GraphPreviewCache::Residency::Resident &&
                              controller.previewPresentedLatest();
                   }),
                   "exact frame を実 GPU に提示"))
            continue;
        const auto image = window.grabWindow();
        image.save(QString::fromStdWString(
            (root / ("frame-" + std::to_string(frame) + ".png")).wstring()));
        const auto rasterResult = graph::readRgba(
            artifact / (frame < 3 ? "frame-" + std::to_string(frame) + ".png" : "static.png"), 320,
            180);
        if (!check(std::holds_alternative<graph::Raster>(rasterResult), "独立 oracle の disk PNG"))
            continue;
        const auto& raster = std::get<graph::Raster>(rasterResult);
        const auto expected = whiteOracle(raster);
        const auto actual = pixels(image);
        std::size_t mismatch = 0;
        if (actual.size() == expected.size())
            for (std::size_t at = 0; at < actual.size(); at += 4)
                mismatch += !std::equal(actual.begin() + static_cast<std::ptrdiff_t>(at),
                                        actual.begin() + static_cast<std::ptrdiff_t>(at + 4),
                                        expected.begin() + static_cast<std::ptrdiff_t>(at));
        else
            mismatch = expected.size() / 4;
        check(mismatch == 0, "native 全画素は独立 straight source-over と完全一致");
        check(expected != whiteOracle(raster, true), "一画素移動の変異を oracle が検出");
        observations << frame << '\t' << latency.elapsed() << '\t'
                     << controller.graphRastersForTest().residentBytes() << '\t'
                     << controller.graphRastersForTest().peakBytes() << '\t' << mismatch << '\n';
    }
    const auto rendersBeforeEffect = controller.graphRastersForTest().renderCount();
    check(controller.setClipEffectValues(QStringLiteral("graph"),
                                         {{QStringLiteral("positionX"), 10.0}}, true),
          "Graph の外側 ClipEffects を確定");
    bool effectSeek = false;
    check(pump([&] {
              if (!effectSeek)
                  effectSeek = controller.seekTimelineFrame(9);
              return effectSeek && controller.previewPresentedLatest();
          }),
          "外側 ClipEffects を native に提示");
    const auto finalRaster = graph::readRgba(artifact / "static.png", 320, 180);
    if (auto* raster = std::get_if<graph::Raster>(&finalRaster)) {
        const auto actual = pixels(window.grabWindow());
        check(actual == whiteOracle(*raster, 32), "外側移動は32画素だけ、合成の全画素は完全一致");
        check(actual != whiteOracle(*raster, 64), "ClipEffects の二重適用を独立 oracle が拒否");
    } else
        check(false, "ClipEffects oracle の完全な artifact");
    check(controller.graphRastersForTest().keyFor(spec).toStdString() == key &&
              controller.graphRastersForTest().renderCount() == rendersBeforeEffect,
          "外側 ClipEffects は renderer identity を変えない");
    std::ofstream results(root / "results.json");
    results << "{\"checks\":" << checks << ",\"failures\":" << failures
            << ",\"artifact_ready_ms\":" << readyMs << "}\n";
    controller.shutdown();
    return failures ? 1 : 0;
}
