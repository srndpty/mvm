#include "app/preview/preview_engine_rhi_item.h"
#include "alt_repeat_filter.h"
#include "focus_release_filter.h"
#include "timeline_wheel_filter.h"
#include "media/mlt/mvm_mlt_runtime.h"
#include "mvm_controller.h"
#include "project/project_json.h"
#include "util/mvm_win_utf8.h"
#include "waveform_cache.h"
#include "waveform_view.h"

#include <cstdio>
#include <filesystem>

#include <QByteArray>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QWheelEvent>
#include <qqml.h>

namespace {

struct AppArguments {
    std::filesystem::path projectPath;
    std::filesystem::path manimExecutablePath;
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
    // stderr のメッセージは UTF-8 で書く。コンソールが CP932 のままだと文字化けする。
    mvm_enable_utf8_console();
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);

    QGuiApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("mvm"));
    application.setOrganizationName(QStringLiteral("mvm"));
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
    // dev.ps1 は .mvm を固定で渡すため、どのファイルを開こうとしたかを成否に関係なく出す。
    const QByteArray projectPathText =
        QString::fromStdWString(arguments.projectPath.wstring()).toUtf8();
    std::error_code existsError;
    if (std::filesystem::exists(arguments.projectPath, existsError)) {
        std::fprintf(stderr, "Projectを開きます: %s\n", projectPathText.constData());
        const auto loaded = mvm::project::loadProjectJson(arguments.projectPath);
        if (!loaded.success) {
            std::fprintf(stderr, "Projectを開けません: %s: %s\n", projectPathText.constData(),
                         loaded.error.c_str());
            return 3;
        }
        project = loaded.project;
    } else if (!existsError) {
        std::fprintf(stderr, "Projectが無いため既定構成で始めます: %s\n",
                     projectPathText.constData());
    } else {
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
    mvm::app::MvmController controller(arguments.projectPath, arguments.manimExecutablePath,
                                       std::move(project));
    if (!controller.holdsProjectLock()) {
        std::fprintf(stderr, "%s\n", controller.statusText().toUtf8().constData());
        mvm_mlt_runtime_shutdown();
        return 6;
    }
    // engine より先に破棄されないよう、engine より前に宣言する。
    mvm::app::WaveformCache waveformCache;
    QQmlApplicationEngine engine;
    engine.setInitialProperties(
        {{QStringLiteral("mvmController"), QVariant::fromValue(&controller)},
         {QStringLiteral("waveformCache"), QVariant::fromValue(&waveformCache)}});
    // 外部ツールで素材を差し替えて戻ってきたとき、古い波形・画像を出し続けない。
    QObject::connect(&application, &QGuiApplication::applicationStateChanged, &waveformCache,
                     [&waveformCache, &controller](Qt::ApplicationState state) {
                         if (state == Qt::ApplicationActive) {
                             waveformCache.revalidateAll();
                             controller.revalidateMedia();
                         }
                     });
    engine.load(QUrl(QStringLiteral("qrc:/mvm/app/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        std::fprintf(stderr, "mvm QMLを読み込めませんでした\n");
        return 4;
    }

    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    auto* timelinePanel =
        window ? window->findChild<QQuickItem*>(QStringLiteral("timelinePanel")) : nullptr;
    auto* surface =
        window
            ? window->findChild<mvm::app::PreviewEngineRhiItem*>(QStringLiteral("previewSurface"))
            : nullptr;
    if (!window || !surface || !timelinePanel) {
        std::fprintf(stderr, "mvmのWindow、Preview、またはtimeline panelが見つかりません\n");
        return 4;
    }
    TimelineWheelEventFilter timelineWheelFilter(window, timelinePanel);
    window->installEventFilter(&timelineWheelFilter);
    window->installEventFilter(new mvm::app::FocusReleaseFilter(window));
    window->installEventFilter(new mvm::app::AltRepeatFilter(window));
    controller.attachPreview(surface);

    QObject::connect(&application, &QCoreApplication::aboutToQuit, &controller,
                     &mvm::app::MvmController::shutdown);
    // export workerをjoinするcontroller shutdownより後にMLT runtimeを閉じる。
    QObject::connect(&application, &QCoreApplication::aboutToQuit,
                     [] { mvm_mlt_runtime_shutdown(); });
    return application.exec();
}
