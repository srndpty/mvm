#include "app/preview/preview_engine_rhi_item.h"
#include "media/mlt/mvm_mlt_runtime.h"
#include "mvm_controller.h"
#include "project/project_json.h"

#include <cstdio>
#include <filesystem>

#include <QFileInfo>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QStringList>

namespace {

struct AppArguments {
    std::filesystem::path projectPath;
    std::filesystem::path manimExecutablePath;
};

void usage() {
    std::fprintf(stderr, "使い方: mvm --project <project.json> "
                         "--manim-executable <absolute-manim.exe>\n");
}

bool parseArguments(const QStringList& arguments, AppArguments& parsed) {
    for (int index = 1; index < arguments.size(); ++index) {
        const QString key = arguments[index];
        if ((key == QStringLiteral("--project") || key == QStringLiteral("--manim-executable")) &&
            index + 1 < arguments.size()) {
            const std::filesystem::path value(arguments[++index].toStdWString());
            if (key == QStringLiteral("--project"))
                parsed.projectPath = value;
            else
                parsed.manimExecutablePath = value;
        } else {
            return false;
        }
    }
    if (parsed.projectPath.empty() || parsed.manimExecutablePath.empty() ||
        !parsed.manimExecutablePath.is_absolute()) {
        return false;
    }

    std::error_code error;
    parsed.projectPath = std::filesystem::absolute(parsed.projectPath, error).lexically_normal();
    if (error)
        return false;
    parsed.manimExecutablePath = parsed.manimExecutablePath.lexically_normal();
    return std::filesystem::is_regular_file(parsed.manimExecutablePath, error) && !error;
}

} // namespace

int main(int argc, char** argv) {
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);

    QGuiApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("mvm"));

    AppArguments arguments;
    if (!parseArguments(application.arguments(), arguments)) {
        usage();
        return 2;
    }

    mvm::project::Project project;
    std::error_code existsError;
    if (std::filesystem::exists(arguments.projectPath, existsError)) {
        const auto loaded = mvm::project::loadProjectJson(arguments.projectPath);
        if (!loaded.success) {
            std::fprintf(stderr, "Projectを開けません: %s\n", loaded.error.c_str());
            return 3;
        }
        project = loaded.project;
    } else if (existsError) {
        std::fprintf(stderr, "Project pathを確認できません: %s\n", existsError.message().c_str());
        return 3;
    }

    // 書き出しは MLT の avformat consumer を使う。QML の render thread が動き出す
    // 前に初期化を終わらせる (AGENTS.md の QML 起動順の規約)。
    // 場所を推測させず、build 時に確定した module / data directory を明示する。
    if (mvm_mlt_runtime_init(MVM_MLT_MODULE_DIR, MVM_MLT_DATA_DIR) != 0) {
        std::fprintf(stderr, "MLTを初期化できません。書き出しが行えないため起動を中止します\n");
        return 5;
    }
    QObject::connect(&application, &QCoreApplication::aboutToQuit,
                     [] { mvm_mlt_runtime_shutdown(); });

    mvm::app::MvmController controller(arguments.projectPath, arguments.manimExecutablePath,
                                       std::move(project));
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("mvmController"), &controller);
    engine.load(QUrl(QStringLiteral("qrc:/mvm/app/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        std::fprintf(stderr, "mvm QMLを読み込めませんでした\n");
        return 4;
    }

    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    auto* previewHost =
        window ? window->findChild<QQuickItem*>(QStringLiteral("previewHost")) : nullptr;
    if (!window || !previewHost) {
        std::fprintf(stderr, "mvmのWindowまたはPreview hostが見つかりません\n");
        return 4;
    }
    auto* surface = new mvm::app::PreviewEngineRhiItem(previewHost);
    surface->setWidth(previewHost->width());
    surface->setHeight(previewHost->height());
    QObject::connect(previewHost, &QQuickItem::widthChanged, surface,
                     [surface, previewHost] { surface->setWidth(previewHost->width()); });
    QObject::connect(previewHost, &QQuickItem::heightChanged, surface,
                     [surface, previewHost] { surface->setHeight(previewHost->height()); });
    controller.attachPreview(surface);

    QObject::connect(window, &QQuickWindow::closing, &controller,
                     [&controller](QQuickCloseEvent*) { controller.shutdown(); });
    QObject::connect(&application, &QCoreApplication::aboutToQuit, &controller,
                     &mvm::app::MvmController::shutdown);
    return application.exec();
}
