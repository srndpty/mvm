#include "mvm_controller.h"

#include "app/manim_clip_workflow.h"
#include "app/preview/preview_engine_rhi_item.h"

#include <utility>

#include <QCoreApplication>
#include <QEventLoop>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>

namespace mvm::app {
namespace {

QString fromPath(const std::filesystem::path& path) {
    return QString::fromStdWString(path.wstring());
}

QString previewErrorText(const preview::PreviewError& error) {
    return QString::fromStdString(error.detail);
}

class QtEventDispatcher final : public preview::PreviewEventDispatcher {
public:
    explicit QtEventDispatcher(QObject* context) : context_(context) {}

    bool post(std::function<void()> callback) override {
        if (!context_)
            return false;
        return QMetaObject::invokeMethod(
            context_, [callback = std::move(callback)] { callback(); }, Qt::QueuedConnection);
    }

private:
    QPointer<QObject> context_;
};

} // namespace

MvmController::MvmController(std::filesystem::path projectPath,
                             std::filesystem::path manimExecutablePath, project::Project project,
                             QObject* parent)
    : QObject(parent), projectPath_(std::move(projectPath)),
      manimExecutablePath_(std::move(manimExecutablePath)), project_(std::move(project)),
      previewEngine_(std::make_shared<preview::PreviewEngine>()),
      dispatcher_(std::make_shared<QtEventDispatcher>(this)) {
    preview::PreviewEngineConfig config;
    config.output.frameRate = {60, 1};
    const auto initialized = previewEngine_->initialize(config, dispatcher_);
    if (!initialized) {
        statusText_ =
            QStringLiteral("Preview初期化に失敗しました: ") + previewErrorText(initialized.error());
    }

    stateTimer_.setInterval(100);
    connect(&stateTimer_, &QTimer::timeout, this, &MvmController::pollPreviewState);
    stateTimer_.start();
}

MvmController::~MvmController() {
    shutdown();
}

void MvmController::attachPreview(PreviewEngineRhiItem* surface) {
    previewSurface_ = surface;
    if (previewSurface_)
        previewSurface_->setEngine(previewEngine_);
}

QString MvmController::projectPath() const {
    return fromPath(projectPath_);
}

void MvmController::setStatus(QString status) {
    statusText_ = std::move(status);
    Q_EMIT stateChanged();
}

void MvmController::pollPreviewState() {
    if (!previewEngine_)
        return;
    const auto status = previewEngine_->status();
    const bool ready = status.state == preview::PreviewEngineState::ReadyPaused ||
                       status.state == preview::PreviewEngineState::Playing;
    if (previewReady_ != ready) {
        previewReady_ = ready;
        if (ready && !busy_ && currentClipPath_.isEmpty())
            statusText_ = QStringLiteral("Add Manim Clipを選択してください");
        Q_EMIT stateChanged();
    }
    if (status.state == preview::PreviewEngineState::Error && status.lastError) {
        const QString message =
            QStringLiteral("Preview error: ") + previewErrorText(*status.lastError);
        if (statusText_ != message)
            setStatus(message);
    }
}

void MvmController::resumeCurrentClip() {
    if (!currentSource_ || !previewEngine_)
        return;
    if (previewEngine_->status().state == preview::PreviewEngineState::ReadyPaused)
        previewEngine_->play();
}

bool MvmController::installVideoClip(const std::filesystem::path& videoPath,
                                     const QString& clipName) {
    const auto state = previewEngine_->status().state;
    if (state == preview::PreviewEngineState::Playing) {
        const auto paused = previewEngine_->pause();
        if (!paused) {
            setStatus(QStringLiteral("現在のclipを停止できません: ") +
                      previewErrorText(paused.error()));
            return false;
        }
    } else if (state != preview::PreviewEngineState::ReadyPaused) {
        setStatus(QStringLiteral("Previewがclip追加可能な状態ではありません"));
        return false;
    }

    if (staleSource_) {
        const auto removed = previewEngine_->removeSource(*staleSource_);
        if (!removed) {
            setStatus(QStringLiteral("以前のclip sourceを解放できません: ") +
                      previewErrorText(removed.error()));
            resumeCurrentClip();
            return false;
        }
        staleSource_.reset();
    }

    preview::PreviewSourceDescriptor descriptor;
    descriptor.mediaPath = videoPath;
    descriptor.videoEnabled = true;
    descriptor.audioEnabled = false;
    const auto added = previewEngine_->addSource(descriptor);
    if (!added) {
        setStatus(QStringLiteral("生成videoをPreviewで開けません: ") +
                  previewErrorText(added.error()));
        resumeCurrentClip();
        return false;
    }

    auto composition = std::make_shared<preview::CompositionSnapshot>();
    composition->layers.push_back(
        {added.value(), {0.0F, 0.0F, 1.0F, 1.0F}, {0.0F, 0.0F, 1.0F, 1.0F}, 1.0F});
    const auto submitted = previewEngine_->submitComposition(composition);
    if (!submitted) {
        previewEngine_->removeSource(added.value());
        setStatus(QStringLiteral("生成videoをcompositionへ追加できません: ") +
                  previewErrorText(submitted.error()));
        resumeCurrentClip();
        return false;
    }

    const auto played = previewEngine_->play();
    if (!played) {
        setStatus(QStringLiteral("生成videoを再生できません: ") + previewErrorText(played.error()));
        return false;
    }

    staleSource_ = currentSource_;
    currentSource_ = added.value();
    currentClipName_ = clipName;
    currentClipPath_ = fromPath(videoPath);
    statusText_ = QStringLiteral("Manim clipを生成して再生しています");
    Q_EMIT stateChanged();
    return true;
}

bool MvmController::generateManimClip(const QUrl& scriptUrl, const QString& sceneName) {
    if (busy_)
        return false;
    const QString localScript =
        scriptUrl.isLocalFile() ? scriptUrl.toLocalFile() : scriptUrl.toString();
    const QFileInfo scriptInfo(localScript);
    const QString trimmedScene = sceneName.trimmed();
    if (!scriptInfo.exists() || !scriptInfo.isFile() ||
        scriptInfo.suffix().compare(QStringLiteral("py"), Qt::CaseInsensitive) != 0) {
        setStatus(QStringLiteral("存在する.py scriptを選択してください"));
        return false;
    }
    if (trimmedScene.isEmpty()) {
        setStatus(QStringLiteral("Scene class名を入力してください"));
        return false;
    }
    if (!previewReady_) {
        setStatus(QStringLiteral("Previewの準備が完了していません"));
        return false;
    }

    busy_ = true;
    statusText_ = QStringLiteral("Manimを生成しています…");
    Q_EMIT stateChanged();
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    ManimClipGenerationRequest request;
    request.manimExecutablePath = manimExecutablePath_;
    request.projectPath = projectPath_;
    request.scriptPath = std::filesystem::path(localScript.toStdWString());
    request.sceneName = trimmedScene.toStdString();
    request.width = 640;
    request.height = 360;
    request.fps = 60;
    const ManimClipGenerationResult generated = mvm::app::generateManimClip(project_, request);

    busy_ = false;
    if (!generated.success) {
        QString detail = QString::fromStdString(generated.error);
        if (!generated.stderrText.empty())
            detail += QStringLiteral("\n") + QString::fromStdString(generated.stderrText);
        setStatus(detail);
        return false;
    }

    const QString clipName = trimmedScene + QStringLiteral(" — ") +
                             QFileInfo(fromPath(generated.outputVideoPath)).fileName();
    return installVideoClip(generated.outputVideoPath, clipName);
}

void MvmController::shutdown() {
    if (shutdownStarted_)
        return;
    shutdownStarted_ = true;
    stateTimer_.stop();
    if (previewEngine_)
        previewEngine_->requestShutdown();
    if (previewSurface_)
        previewSurface_->setEngine({});
}

} // namespace mvm::app
