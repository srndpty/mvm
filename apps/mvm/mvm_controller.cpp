#include "mvm_controller.h"

#include "app/manim_clip_workflow.h"
#include "app/preview/preview_engine_rhi_item.h"
#include "app/timeline_export.h"
#include "project/project_json.h"
#include "project/timeline_edit.h"

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

// timeline 上の Manim clip の位置。M4 は Manim clip を 1 本しか扱わない。
int indexOfManimClip(const std::vector<project::TimelineClip>& clips) {
    for (std::size_t index = 0; index < clips.size(); ++index) {
        if (clips[index].kind == project::TimelineClipKind::Manim)
            return static_cast<int>(index);
    }
    return -1;
}

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

bool MvmController::hasManimTimelineClip() const {
    return indexOfManimClip(project_.timelineClips) >= 0;
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

    if (restored.generatedVideoAvailable) {
        const int manimIndex = indexOfManimClip(project_.timelineClips);
        if (manimIndex < 0) {
            statusText_ = restored.success
                              ? QStringLiteral("生成済みManim assetをtimelineへ追加できます")
                              : QString::fromStdString(restored.error);
            return;
        }
        if (!syncManimTimelineClip(false))
            return;
        const project::ManimAsset& asset = project_.manimAssets.front();
        const QString clipName = QString::fromStdString(asset.sceneName) + QStringLiteral(" — ") +
                                 QFileInfo(fromPath(asset.generatedVideoPath)).fileName();
        queueVideoClipInstall(asset.generatedVideoPath, clipName, manimIndex);
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
                                          QString clipName, int clipIndex) {
    pendingVideoPath_ = videoPath;
    pendingClipName_ = std::move(clipName);
    pendingClipIndex_ = clipIndex;
}

QStringList MvmController::clipNames() const {
    QStringList names;
    names.reserve(static_cast<qsizetype>(project_.timelineClips.size()));
    for (const auto& clip : project_.timelineClips)
        names.append(QString::fromStdString(clip.name));
    return names;
}

bool MvmController::saveProject(project::Project candidate, const QString& failurePrefix) {
    const project::ProjectIoResult saved =
        project::saveProjectJsonTransaction(project_, std::move(candidate), projectPath_);
    if (!saved.success) {
        setStatus(failurePrefix + QString::fromStdString(saved.error));
        return false;
    }
    return true;
}

bool MvmController::syncManimTimelineClip(bool addIfMissing) {
    if (project_.manimAssets.empty())
        return true;
    const project::ManimAsset& asset = project_.manimAssets.front();
    if (asset.generatedVideoPath.empty())
        return true;

    project::Project candidate = project_;
    const int index = indexOfManimClip(candidate.timelineClips);
    project::TimelineClip clip{project::TimelineClipKind::Manim, asset.generatedVideoPath,
                               asset.sceneName};
    if (index < 0) {
        if (!addIfMissing)
            return true;
        candidate.timelineClips.push_back(std::move(clip));
    } else {
        const project::TimelineClip& existing =
            candidate.timelineClips[static_cast<std::size_t>(index)];
        if (existing.mediaPath == clip.mediaPath && existing.name == clip.name)
            return true; // 変化が無ければ project を書き直さない
        candidate.timelineClips[static_cast<std::size_t>(index)] = std::move(clip);
    }
    return saveProject(std::move(candidate), QStringLiteral("Projectを保存できません: "));
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
        const int clipIndex = pendingClipIndex_;
        pendingVideoPath_.reset();
        pendingClipName_.clear();
        pendingClipIndex_ = -1;
        installVideoClip(videoPath, clipName, clipIndex);
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
                                     const QString& clipName, int clipIndex) {
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
    currentClipIndex_ = clipIndex;
    statusText_ = clipName + QStringLiteral(" を再生しています");
    Q_EMIT stateChanged();
    return true;
}

bool MvmController::generateManimClip(const QUrl& scriptUrl, const QString& sceneName) {
    if (busy_)
        return false;
    if (hasManimAsset()) {
        setStatus(QStringLiteral("Manim assetはすでに存在します"));
        return false;
    }
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
    if (!hasManimTimelineClip()) {
        setStatus(QStringLiteral("先にManim assetをtimelineへ追加してください"));
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

    const bool addTimelinePlacement = project_.manimAssets.empty();

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
    if (!syncManimTimelineClip(addTimelinePlacement))
        return false;
    const int manimIndex = indexOfManimClip(project_.timelineClips);
    if (manimIndex < 0) {
        setStatus(QStringLiteral("Manim assetを生成しましたがtimelineには配置されていません"));
        return true;
    }
    const QString clipName = trimmedScene + QStringLiteral(" — ") +
                             QFileInfo(fromPath(generated.outputVideoPath)).fileName();
    const preview::PreviewEngineState previewState = previewEngine_->status().state;
    if (previewState == preview::PreviewEngineState::ReadyPaused ||
        previewState == preview::PreviewEngineState::Playing) {
        return installVideoClip(generated.outputVideoPath, clipName, manimIndex);
    }

    queueVideoClipInstall(generated.outputVideoPath, clipName, manimIndex);
    if (previewState == preview::PreviewEngineState::ShuttingDown ||
        previewState == preview::PreviewEngineState::Shutdown ||
        previewState == preview::PreviewEngineState::Error) {
        if (!resetPreviewEngine()) {
            pendingVideoPath_.reset();
            pendingClipName_.clear();
            pendingClipIndex_ = -1;
            return false;
        }
    }
    statusText_ = QStringLiteral("生成済みclipのPreviewを準備しています");
    Q_EMIT stateChanged();
    return true;
}

bool MvmController::addManimToTimeline() {
    if (busy_)
        return false;
    if (project_.manimAssets.empty()) {
        setStatus(QStringLiteral("timelineへ追加するManim assetがありません"));
        return false;
    }
    if (hasManimTimelineClip()) {
        setStatus(QStringLiteral("Manim assetはすでにtimelineへ配置されています"));
        return false;
    }

    const project::ManimAsset& asset = project_.manimAssets.front();
    if (!std::filesystem::is_regular_file(asset.generatedVideoPath)) {
        setStatus(QStringLiteral("生成済みManim videoがありません: ") +
                  fromPath(asset.generatedVideoPath));
        return false;
    }

    project::Project candidate = project_;
    const project::TimelineEditResult placed =
        project::appendManimTimelineClip(candidate, candidate.manimAssets.front());
    if (!placed.success) {
        setStatus(QString::fromStdString(placed.error));
        return false;
    }
    if (!saveProject(std::move(candidate), QStringLiteral("Projectを保存できません: ")))
        return false;

    Q_EMIT stateChanged();
    return selectClip(placed.selectedIndex);
}

bool MvmController::addVideoClip(const QUrl& fileUrl) {
    if (busy_)
        return false;
    if (!fileUrl.isLocalFile()) {
        setStatus(QStringLiteral("ローカルファイルを選択してください"));
        return false;
    }
    const QString localFile = fileUrl.toLocalFile();
    const QFileInfo info(localFile);
    if (!info.exists() || !info.isFile()) {
        setStatus(QStringLiteral("存在する動画ファイルを選択してください"));
        return false;
    }

    const std::filesystem::path mediaPath(localFile.toStdWString());
    project::Project candidate = project_;
    const project::TimelineClip clip{project::TimelineClipKind::Video, mediaPath,
                                     info.fileName().toStdString()};
    candidate.timelineClips.push_back(clip);

    if (!saveProject(std::move(candidate), QStringLiteral("Projectを保存できません: ")))
        return false;

    const int index = static_cast<int>(project_.timelineClips.size()) - 1;
    Q_EMIT stateChanged();
    return selectClip(index);
}

bool MvmController::selectClip(int index) {
    if (index < 0 || index >= static_cast<int>(project_.timelineClips.size())) {
        setStatus(QStringLiteral("選択したclipがありません"));
        return false;
    }
    const project::TimelineClip& clip = project_.timelineClips[static_cast<std::size_t>(index)];
    const QString clipName = QString::fromStdString(clip.name);
    if (!std::filesystem::is_regular_file(clip.mediaPath)) {
        setStatus(clipName + QStringLiteral(" のファイルがありません: ") +
                  fromPath(clip.mediaPath));
        return false;
    }

    const preview::PreviewEngineState previewState = previewEngine_->status().state;
    if (previewState == preview::PreviewEngineState::ReadyPaused ||
        previewState == preview::PreviewEngineState::Playing) {
        return installVideoClip(clip.mediaPath, clipName, index);
    }
    queueVideoClipInstall(clip.mediaPath, clipName, index);
    setStatus(QStringLiteral("Previewの準備を待っています"));
    return true;
}

bool MvmController::moveCurrentClip(int offset) {
    if (busy_)
        return false;
    project::Project candidate = project_;
    const project::TimelineEditResult moved =
        project::moveTimelineClip(candidate, currentClipIndex_, offset);
    if (!moved.success) {
        setStatus(QString::fromStdString(moved.error));
        return false;
    }
    if (!saveProject(std::move(candidate), QStringLiteral("Projectを保存できません: ")))
        return false;

    currentClipIndex_ = moved.selectedIndex;
    statusText_ = QStringLiteral("clipを移動しました");
    Q_EMIT stateChanged();
    return true;
}

bool MvmController::moveCurrentClipLeft() {
    return moveCurrentClip(-1);
}

bool MvmController::moveCurrentClipRight() {
    return moveCurrentClip(1);
}

bool MvmController::deleteCurrentClip() {
    if (busy_)
        return false;

    project::Project candidate = project_;
    const project::TimelineEditResult deleted =
        project::deleteTimelineClip(candidate, currentClipIndex_);
    if (!deleted.success) {
        setStatus(QString::fromStdString(deleted.error));
        return false;
    }

    const preview::PreviewEngineState previewState = previewEngine_->status().state;
    const bool wasPlaying = previewState == preview::PreviewEngineState::Playing;
    if (wasPlaying) {
        const auto paused = previewEngine_->pause();
        if (!paused) {
            setStatus(QStringLiteral("削除前にPreviewを停止できません: ") +
                      previewErrorText(paused.error()));
            return false;
        }
    }

    if (!saveProject(std::move(candidate), QStringLiteral("Projectを保存できません: "))) {
        if (wasPlaying)
            resumeCurrentClip();
        return false;
    }

    pendingVideoPath_.reset();
    pendingClipName_.clear();
    pendingClipIndex_ = -1;
    const bool previewReset = resetPreviewEngine();
    currentSource_.reset();
    staleSource_.reset();
    currentClipName_.clear();
    currentClipPath_.clear();
    currentClipIndex_ = -1;
    Q_EMIT stateChanged();
    if (!previewReset)
        return false;

    if (deleted.selectedIndex >= 0)
        return selectClip(deleted.selectedIndex);

    setStatus(QStringLiteral("timelineから最後のclipを削除しました"));
    return true;
}

bool MvmController::exportTimeline(const QUrl& outputUrl) {
    if (busy_)
        return false;
    if (project_.timelineClips.empty()) {
        setStatus(QStringLiteral("書き出すclipがありません"));
        return false;
    }
    if (!outputUrl.isLocalFile()) {
        setStatus(QStringLiteral("ローカルの書き出し先を指定してください"));
        return false;
    }

    TimelineExportRequest request;
    request.outputPath = std::filesystem::path(outputUrl.toLocalFile().toStdWString());

    // Manim 生成と同じく GUI thread で同期実行する (M4 は非同期 job を作らない)。
    busy_ = true;
    statusText_ = QStringLiteral("書き出しています…");
    Q_EMIT stateChanged();
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    const TimelineExportResult exported = mvm::app::exportTimeline(project_, request);

    busy_ = false;
    if (!exported.success) {
        setStatus(QStringLiteral("書き出しに失敗しました: ") +
                  QString::fromStdString(exported.error));
        return false;
    }
    setStatus(QStringLiteral("書き出しました: ") + fromPath(exported.outputPath) +
              QStringLiteral(" (") + QString::number(exported.frameCount) +
              QStringLiteral(" frame / ") + QString::number(exported.durationSec, 'f', 2) +
              QStringLiteral(" 秒)"));
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
