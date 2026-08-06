/*
 * mvm Phase 1 / P1 - preview_spike
 *
 * **製品 UI ではない。** decode -> D3D11 -> Qt Quick の縦切りを
 * 動かして計測するためだけのアプリである。
 *
 * 使い方:
 *   preview_spike                                   対話モード
 *   preview_spike --media <path>                    起動時に開く
 *   preview_spike --measure --media <path> --json <out> ...   計測して終了
 *
 * 終了コード:
 *   0 正常 / 2 使い方の誤り / 3 素材を開けない / 5 device 初期化失敗 / 6 出力失敗
 */

#include "app/preview/preview_rhi_item.h"
#include "spike_controller.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QStringList>

#include <cstdio>

using mvm::app::MeasureConfig;
using mvm::app::PreviewRhiItem;
using mvm::app::SpikeController;

namespace {

void usage() {
    std::fprintf(stderr,
                 "使い方: preview_spike [オプション]\n"
                 "  --media <path>        起動時に開く素材\n"
                 "  --measure             計測モード (終わったら終了する)\n"
                 "  --json <path>         計測結果の出力先\n"
                 "  --label <text>        JSON へ入れる識別ラベル\n"
                 "  --warmup-ms <n>       既定 5000\n"
                 "  --measure-ms <n>      既定 60000\n"
                 "  --seeks <n>           既定 1000\n"
                 "  --seed <n>            既定 20260806\n"
                 "  --marker-frames a,b,c 既定 0,1,137,299,600,1799,3599\n"
                 "  --display-timeout-ms <n>  seek 表示待ちの上限 (既定 2000)\n"
                 "  --color-patch WxH     color patch を診断読み取りする領域\n"
                 "  --gpu-completion fence|event_query  完了追跡の backend を強制\n");
}

bool parseArgs(const QStringList& args, MeasureConfig& cfg, QString& mediaPath) {
    cfg.markerFrames = {0, 1, 137, 299, 600, 1799, 3599};

    for (int i = 1; i < args.size(); i++) {
        const QString a = args[i];
        auto next = [&](QString& out) {
            if (i + 1 >= args.size())
                return false;
            out = args[++i];
            return true;
        };
        QString v;
        if (a == "--media") {
            if (!next(v))
                return false;
            mediaPath = v;
            cfg.mediaPath = v;
        } else if (a == "--measure") {
            cfg.enabled = true;
        } else if (a == "--json") {
            if (!next(v))
                return false;
            cfg.jsonPath = v;
        } else if (a == "--label") {
            if (!next(v))
                return false;
            cfg.label = v;
        } else if (a == "--warmup-ms") {
            if (!next(v))
                return false;
            cfg.warmupMs = v.toInt();
        } else if (a == "--measure-ms") {
            if (!next(v))
                return false;
            cfg.measureMs = v.toInt();
        } else if (a == "--seeks") {
            if (!next(v))
                return false;
            cfg.seekCount = v.toInt();
        } else if (a == "--seed") {
            if (!next(v))
                return false;
            cfg.seed = v.toUInt();
        } else if (a == "--display-timeout-ms") {
            if (!next(v))
                return false;
            cfg.displayTimeoutMs = v.toInt();
        } else if (a == "--color-patch") {
            if (!next(v))
                return false;
            const QStringList parts = v.split('x', Qt::SkipEmptyParts);
            if (parts.size() != 2) {
                std::fprintf(stderr, "--color-patch は WxH の形式で指定してください\n");
                return false;
            }
            cfg.colorPatchWidth = parts[0].toInt();
            cfg.colorPatchHeight = parts[1].toInt();
        } else if (a == "--gpu-completion") {
            if (!next(v))
                return false;
            if (v == "event_query") {
                cfg.gpuCompletion = mvm::gpu::GpuCompletionBackend::EventQuery;
            } else if (v == "fence") {
                cfg.gpuCompletion = mvm::gpu::GpuCompletionBackend::Fence;
            } else {
                std::fprintf(stderr, "--gpu-completion は fence か event_query です\n");
                return false;
            }
        } else if (a == "--marker-frames") {
            if (!next(v))
                return false;
            cfg.markerFrames.clear();
            for (const QString& p : v.split(',', Qt::SkipEmptyParts))
                cfg.markerFrames.push_back(p.trimmed().toLongLong());
        } else if (a == "--help" || a == "-h") {
            return false;
        } else {
            std::fprintf(stderr, "不明なオプション: %s\n", a.toUtf8().constData());
            return false;
        }
    }

    if (cfg.enabled && cfg.mediaPath.isEmpty()) {
        std::fprintf(stderr, "--measure には --media が必要です\n");
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    // **graphics API を明示的に固定する。** 既定値に依存しない。
    // P1 が検証しているのは D3D11 の経路そのものであり、
    // 黙って別の backend で動いた結果を「動いた」と報告しないため。
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);

    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("mvm preview_spike"));

    MeasureConfig cfg;
    QString mediaPath;
    if (!parseArgs(app.arguments(), cfg, mediaPath)) {
        usage();
        return 2;
    }

    qmlRegisterType<PreviewRhiItem>("mvm.preview", 1, 0, "PreviewSurface");

    SpikeController controller;
    controller.setMeasureConfig(cfg);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("spike"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("initialMedia"), mediaPath);
    engine.rootContext()->setContextProperty(QStringLiteral("measureMode"), cfg.enabled);

    engine.load(QUrl(QStringLiteral("qrc:/mvm/preview_spike/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        std::fprintf(stderr, "QML を読み込めませんでした\n");
        return 5;
    }

    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    if (!window) {
        std::fprintf(stderr, "root object が Window ではありません\n");
        return 5;
    }
    auto* surface = window->findChild<PreviewRhiItem*>(QStringLiteral("previewSurface"));
    if (!surface) {
        std::fprintf(stderr, "PreviewSurface が見つかりません\n");
        return 5;
    }
    surface->setPreferredCompletionBackend(cfg.gpuCompletion);
    controller.attach(surface);

    if (cfg.enabled) {
        QObject::connect(&controller, &SpikeController::finished, &app, [&] {
            std::fprintf(stdout, "計測を終了しました (exit %d)\n", controller.exitCode());
            app.exit(controller.exitCode());
        });
    } else if (!mediaPath.isEmpty()) {
        controller.openMedia(mediaPath);
    }

    return app.exec();
}
