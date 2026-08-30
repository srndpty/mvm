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
    initializePreviewEngine(QStringLiteral("Preview初期化に失敗しました: "));
    restoreFirstManimClip();

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

bool MvmController::initializePreviewEngine(const QString& failurePrefix) {
    preview::PreviewEngineConfig config;
    config.output.frameRate = {60, 1};
    const auto initialized = previewEngine_->initialize(config, dispatcher_);
    if (!initialized) {
        statusText_ = failurePrefix + previewErrorText(initialized.error());
        return false;
    }
    return true;
}

bool MvmController::resetPreviewEngine() {
    auto replacement = std::make_shared<preview::PreviewEngine>();
    const auto previous = previewEngine_;
    previewEngine_ = replacement;
    if (!initializePreviewEngine(QStringLiteral("Preview再初期化に失敗しました: "))) {
        previewEngine_ = previous;
        Q_EMIT stateChanged();
        return false;
    }

    currentSource_.reset();
    staleSource_.reset();
    previewReady_ = false;
    if (previewSurface_)
        previewSurface_->setEngine(previewEngine_);
    Q_EMIT stateChanged();
    return true;
}

void MvmController::syncFirstManimAsset() {
    if (project_.manimAssets.empty()) {
        manimScriptPath_.clear();
        manimSceneName_.clear();
        manimStateText_.clear();
        return;
    }
    const project::ManimAsset& asset = project_.manimAssets.front();
    manimScriptPath_ = fromPath(asset.scriptPath);
    manimSceneName_ = QString::fromStdString(asset.sceneName);
    manimStateText_ = QString::fromLatin1(project::manimGenerationStateName(asset.generationState));
}

void MvmController::restoreFirstManimClip() {
    const ManimClipRestoreResult restored = mvm::app::restoreFirstManimClip(project_, projectPath_);
    syncFirstManimAsset();
    if (!restored.hasAsset)
        return;

    const project::ManimAsset& asset = project_.manimAssets.front();
    if (restored.generatedVideoAvailable) {
        const QString clipName = QString::fromStdString(asset.sceneName) + QStringLiteral(" — ") +
                                 QFileInfo(fromPath(asset.generatedVideoPath)).fileName();
        queueVideoClipInstall(asset.generatedVideoPath, clipName);
        statusText_ = restored.success
                          ? QStringLiteral("保存済みManim clipを復元しています")
                          : QString::fromStdString(restored.error);
    } else {
        statusText_ = restored.success
                          ? QStringLiteral("生成済みvideoがありません。Regenerateしてください")
                          : QString::fromStdString(restored.error);
    }
}

void MvmController::queueVideoClipInstall(const std::filesystem::path& videoPath,
                                          QString clipName) {
    pendingVideoPath_ = videoPath;
    pendingClipName_ = std::move(clipName);
}

void MvmController::pollPreviewState() {
    if (!previewEngine_)
        return;
    const auto status = previewEngine_->status();
    const bool ready = status.state == preview::PreviewEngineState::ReadyPaused ||
                       status.state == preview::PreviewEngineState::Playing;
    if (previewReady_ != ready) {
        previewReady_ = ready;
        if (ready && !busy_ && currentClipPath_.isEmpty() && !hasManimAsset())
            statusText_ = QStringLiteral("Add Manim Clipを選択してください");
        Q_EMIT stateChanged();
    }
    if (status.state == preview::PreviewEngineState::ReadyPaused && pendingVideoPath_) {
        const std::filesystem::path videoPath = std::move(*pendingVideoPath_);
        const QString clipName = std::move(pendingClipName_);
        pendingVideoPath_.reset();
        pendingClipName_.clear();
        installVideoClip(videoPath, clipName);
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
    statusText_ = QStringLiteral("Manim clipを再生しています");
    Q_EMIT stateChanged();
    return true;
}

bool MvmController::generateManimClip(const QUrl& scriptUrl, const QString& sceneName) {
    if (busy_)
        return false;
    const QString localScript =
        scriptUrl.isLocalFile() ? scriptUrl.toLocalFile() : scriptUrl.toString();
    return generateAndInstallManimClip(std::filesystem::path(localScript.toStdWString()), sceneName,
                                       true);
}

bool MvmController::regenerateManimClip() {
    if (project_.manimAssets.empty()) {
        setStatus(QStringLiteral("再生成するManim assetがありません"));
        return false;
    }
    const project::ManimAsset& asset = project_.manimAssets.front();
    return generateAndInstallManimClip(asset.scriptPath, QString::fromStdString(asset.sceneName),
                                       false);
}

bool MvmController::generateAndInstallManimClip(const std::filesystem::path& scriptPath,
                                                const QString& sceneName,
                                                bool requirePreviewReady) {
    if (busy_)
        return false;
    const QString localScript = fromPath(scriptPath);
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
    if (requirePreviewReady && !previewReady_) {
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
    request.scriptPath = scriptPath;
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

    syncFirstManimAsset();
    const QString clipName = trimmedScene + QStringLiteral(" — ") +
                             QFileInfo(fromPath(generated.outputVideoPath)).fileName();
    const preview::PreviewEngineState previewState = previewEngine_->status().state;
    if (previewState == preview::PreviewEngineState::ReadyPaused ||
        previewState == preview::PreviewEngineState::Playing) {
        return installVideoClip(generated.outputVideoPath, clipName);
    }

    queueVideoClipInstall(generated.outputVideoPath, clipName);
    if (previewState == preview::PreviewEngineState::ShuttingDown ||
        previewState == preview::PreviewEngineState::Shutdown ||
        previewState == preview::PreviewEngineState::Error) {
        if (!resetPreviewEngine()) {
            pendingVideoPath_.reset();
            pendingClipName_.clear();
            return false;
        }
    }
    statusText_ = QStringLiteral("生成済みclipのPreviewを準備しています");
    Q_EMIT stateChanged();
    return true;
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
