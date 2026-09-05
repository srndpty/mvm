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
#include <qqml.h>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QStringList>
#include <QVariant>
#include <QWheelEvent>

namespace {

struct AppArguments {
    std::filesystem::path projectPath;
    std::filesystem::path manimExecutablePath;
};

class TimelineWheelEventFilter final : public QObject {
public:
    TimelineWheelEventFilter(QQuickWindow* window, QQuickItem* timelinePanel)
        : window_(window), timelinePanel_(timelinePanel) {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched != window_ || event->type() != QEvent::Wheel || !timelinePanel_)
            return QObject::eventFilter(watched, event);
        const auto* wheel = static_cast<QWheelEvent*>(event);
        const QPointF local = timelinePanel_->mapFromScene(wheel->position());
        if (!timelinePanel_->contains(local))
            return QObject::eventFilter(watched, event);

        const QPoint angleDelta = wheel->angleDelta();
        const QPoint pixelDelta = wheel->pixelDelta();
        // Windowsやmouse driverによってはAlt+縦wheelが横wheelへ変換される。
        // 縦成分だけを見るとdelta=0をzoom-outと誤認し、最小zoomから戻れなくなる。
        const int delta = angleDelta.y() != 0   ? angleDelta.y()
                          : angleDelta.x() != 0 ? angleDelta.x()
                          : pixelDelta.y() != 0 ? pixelDelta.y()
                                                : pixelDelta.x();
        const char* method = nullptr;
        QVariantList arguments;
        if (wheel->modifiers().testFlag(Qt::AltModifier)) {
            method = "handleNativeAltWheel";
            arguments = {delta, local.x()};
        } else if (wheel->modifiers().testFlag(Qt::ControlModifier)) {
            method = "handleNativeCtrlWheel";
            arguments = {delta};
        } else if (wheel->modifiers().testFlag(Qt::ShiftModifier)) {
            method = "handleNativeShiftWheel";
            arguments = {delta};
        } else {
            return QObject::eventFilter(watched, event);
        }

        if (delta == 0)
            return true;

        if (arguments.size() == 2) {
            QMetaObject::invokeMethod(
                timelinePanel_, method, Qt::DirectConnection, Q_ARG(QVariant, arguments[0]),
                Q_ARG(QVariant, arguments[1]));
        } else {
            QMetaObject::invokeMethod(timelinePanel_, method, Qt::DirectConnection,
                                      Q_ARG(QVariant, arguments[0]));
        }
        // modifier付きwheelはFlickableへ流さず、通常scrollへ化ける挙動を止める。
        return true;
    }

private:
    QQuickWindow* window_ = nullptr;
    QQuickItem* timelinePanel_ = nullptr;
};

void usage() {
    std::fprintf(stderr, "使い方: mvm --project <project.mvm> "
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
    // native (Windows) style は background / contentItem の差し替えを黙って無視する。
    // track の mute 状態などを色で出しているため、customization できる style を選ぶ。
    // QML の読み込みより前に確定させる (AGENTS.md の起動時 configuration 確定順序)。
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    AppArguments arguments;
    if (!parseArguments(application.arguments(), arguments)) {
        usage();
        return 2;
    }
    // Project が無ければ既定構成 (V1/V2 + A1) から始める。track 0 本の Project を
    // 作らせない。
    mvm::project::Project project = mvm::project::createDefaultProject();
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
    qmlRegisterType<mvm::app::PreviewEngineRhiItem>("mvm.preview", 1, 0, "PreviewSurface");
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("mvmController"), &controller);
    engine.load(QUrl(QStringLiteral("qrc:/mvm/app/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        std::fprintf(stderr, "mvm QMLを読み込めませんでした\n");
        return 4;
    }

    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    auto* timelinePanel =
        window ? window->findChild<QQuickItem*>(QStringLiteral("timelinePanel")) : nullptr;
    auto* surface = window ? window->findChild<mvm::app::PreviewEngineRhiItem*>(
                                 QStringLiteral("previewSurface"))
                           : nullptr;
    if (!window || !surface || !timelinePanel) {
        std::fprintf(stderr,
                     "mvmのWindow、Preview、またはtimeline panelが見つかりません\n");
        return 4;
    }
    TimelineWheelEventFilter timelineWheelFilter(window, timelinePanel);
    window->installEventFilter(&timelineWheelFilter);
    controller.attachPreview(surface);

    QObject::connect(window, &QQuickWindow::closing, &controller,
                     [&controller](QQuickCloseEvent*) { controller.shutdown(); });
    QObject::connect(&application, &QCoreApplication::aboutToQuit, &controller,
                     &mvm::app::MvmController::shutdown);
    return application.exec();
}
