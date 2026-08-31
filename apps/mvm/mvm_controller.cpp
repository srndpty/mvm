#include "mvm_controller.h"

#include "app/manim_clip_workflow.h"
#include "app/preview/preview_engine_rhi_item.h"
#include "app/timeline_export.h"
#include "media/mlt/mvm_mlt_probe.h"
#include "project/project_json.h"
#include "project/timeline_edit.h"
#include "timeline_clip_model.h"

#include <algorithm>
#include <numeric>
#include <utility>

#include <QCoreApplication>
#include <QEventLoop>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QUuid>

namespace mvm::app {
namespace {

QString fromPath(const std::filesystem::path& path) {
    return QString::fromStdWString(path.wstring());
}

QString previewErrorText(const preview::PreviewError& error) {
    return QString::fromStdString(error.detail);
}

struct ProbedMedia {
    bool success = false;
    std::int64_t fpsNum = 0;
    std::int64_t fpsDen = 1;
    std::int64_t frameCount = 0;
    QString error;
};

ProbedMedia probeMedia(const std::filesystem::path& path) {
    ProbedMedia result;
    const auto text = path.u8string();
    const std::string utf8(reinterpret_cast<const char*>(text.data()), text.size());
    MvmMltProbeResult probe{};
    if (mvm_mlt_probe_file(utf8.c_str(), &probe) != 0 || !probe.ok) {
        result.error = QStringLiteral("素材を解析できません: ") +
                       QString::fromUtf8(probe.error[0] ? probe.error : utf8.c_str());
        return result;
    }
    if (!probe.has_video || probe.is_unbounded_length || probe.frame_count <= 0 ||
        probe.fps_num <= 0 || probe.fps_den <= 0) {
        result.error = QStringLiteral("有限尺と有効なFPSを持つ動画ではありません");
        return result;
    }
    const auto divisor = std::gcd(probe.fps_num, probe.fps_den);
    result.fpsNum = probe.fps_num / divisor;
    result.fpsDen = probe.fps_den / divisor;
    result.frameCount = probe.frame_count;
    result.success = true;
    return result;
}

std::string newClipId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
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
      dispatcher_(std::make_shared<QtEventDispatcher>(this)),
      timelineModel_(std::make_unique<TimelineClipModel>()) {
    refreshTimelineModel();
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
        const auto& clip = project_.timelineClips[static_cast<std::size_t>(manimIndex)];
        if (project::sourceRateMatchesTimelineRate(project_, clip))
            queueVideoClipInstall(asset.generatedVideoPath, clipName, manimIndex,
                                  clip.sourceInFrame);
        statusText_ = restored.success ? QStringLiteral("保存済みManim clipを復元しました")
                                       : QString::fromStdString(restored.error);
    } else {
        statusText_ = restored.success
                          ? QStringLiteral("生成済みvideoがありません。Regenerateしてください")
                          : QString::fromStdString(restored.error);
    }
}

void MvmController::queueVideoClipInstall(const std::filesystem::path& videoPath,
                                          QString clipName, int clipIndex,
                                          std::int64_t sourceFrame) {
    pendingVideoPath_ = videoPath;
    pendingClipName_ = std::move(clipName);
    pendingClipIndex_ = clipIndex;
    pendingSourceFrame_ = sourceFrame;
}

QStringList MvmController::clipNames() const {
    QStringList names;
    names.reserve(static_cast<qsizetype>(project_.timelineClips.size()));
    for (const auto& clip : project_.timelineClips)
        names.append(QString::fromStdString(clip.name));
    return names;
}

QAbstractItemModel* MvmController::timelineModel() const {
    return timelineModel_.get();
}

QString MvmController::currentTimeText() const {
    const qint64 frames = std::max<qint64>(0, playheadFrame_);
    const qint64 totalSeconds = frames / 60;
    return QStringLiteral("%1:%2:%3:%4")
        .arg(totalSeconds / 3600, 2, 10, QLatin1Char('0'))
        .arg((totalSeconds / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(totalSeconds % 60, 2, 10, QLatin1Char('0'))
        .arg(frames % 60, 2, 10, QLatin1Char('0'));
}

void MvmController::refreshTimelineModel() {
    if (timelineModel_)
        timelineModel_->setProject(project_);
    const auto timeline = project::validateTimeline(project_);
    totalTimelineFrames_ = timeline.success ? timeline.totalFrames : 0;
    if (totalTimelineFrames_ == 0)
        playheadFrame_ = 0;
    else
        playheadFrame_ = std::clamp<std::int64_t>(playheadFrame_, 0, totalTimelineFrames_ - 1);
}

bool MvmController::saveProject(project::Project candidate, const QString& failurePrefix) {
    const project::ProjectIoResult saved =
        project::saveProjectJsonTransaction(project_, std::move(candidate), projectPath_);
    if (!saved.success) {
        setStatus(failurePrefix + QString::fromStdString(saved.error));
        return false;
    }
    refreshTimelineModel();
    return true;
}

bool MvmController::syncManimTimelineClip(bool addIfMissing) {
    if (project_.manimAssets.empty())
        return true;
    const project::ManimAsset& asset = project_.manimAssets.front();
    if (asset.generatedVideoPath.empty())
        return true;

    const ProbedMedia media = probeMedia(asset.generatedVideoPath);
    if (!media.success) {
        setStatus(QStringLiteral("Manim videoをtimelineへ反映できません: ") + media.error);
        return false;
    }

    project::Project candidate = project_;
    const int index = indexOfManimClip(candidate.timelineClips);
    if (index < 0) {
        if (!addIfMissing)
            return true;
        const auto placed = project::appendManimTimelineClip(
            candidate, asset, newClipId(), media.fpsNum, media.fpsDen, media.frameCount);
        if (!placed.success) {
            setStatus(QString::fromStdString(placed.error));
            return false;
        }
    } else {
        project::TimelineClip& existing = candidate.timelineClips[static_cast<std::size_t>(index)];
        if (existing.sourceFpsNum != media.fpsNum || existing.sourceFpsDen != media.fpsDen ||
            existing.sourceOutFrame > media.frameCount) {
            setStatus(QStringLiteral("再生成後のManim素材では既存trimを保持できません。"
                                     "timelineから削除して追加し直してください"));
            return false;
        }
        if (existing.mediaPath == asset.generatedVideoPath && existing.name == asset.sceneName &&
            existing.sourceFrameCount == media.frameCount)
            return true; // 変化が無ければ project を書き直さない
        existing.mediaPath = asset.generatedVideoPath;
        existing.name = asset.sceneName;
        existing.sourceFrameCount = media.frameCount;
        const auto valid = project::recomputeTimelineStarts(candidate);
        if (!valid.success) {
            setStatus(QString::fromStdString(valid.error));
            return false;
        }
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
        const std::int64_t sourceFrame = pendingSourceFrame_;
        pendingVideoPath_.reset();
        pendingClipName_.clear();
        pendingClipIndex_ = -1;
        pendingSourceFrame_ = 0;
        installVideoClip(videoPath, clipName, clipIndex, sourceFrame);
    }
    if (status.state == preview::PreviewEngineState::Error && status.lastError) {
        const QString message =
            QStringLiteral("Preview error: ") + previewErrorText(*status.lastError);
        if (statusText_ != message)
            setStatus(message);
    }
}

bool MvmController::installVideoClip(const std::filesystem::path& videoPath,
                                     const QString& clipName, int clipIndex,
                                     std::int64_t sourceFrame) {
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
        return false;
    }

    const auto sought = previewEngine_->seek({sourceFrame});
    if (!sought) {
        setStatus(QStringLiteral("Previewをseekできません: ") + previewErrorText(sought.error()));
        return false;
    }

    staleSource_ = currentSource_;
    currentSource_ = added.value();
    currentClipName_ = clipName;
    currentClipPath_ = fromPath(videoPath);
    currentClipIndex_ = clipIndex;
    statusText_ = clipName + QStringLiteral(" を表示しています");
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
        const auto& clip = project_.timelineClips[static_cast<std::size_t>(manimIndex)];
        return installVideoClip(generated.outputVideoPath, clipName, manimIndex,
                                clip.sourceInFrame);
    }

    const auto& clip = project_.timelineClips[static_cast<std::size_t>(manimIndex)];
    queueVideoClipInstall(generated.outputVideoPath, clipName, manimIndex, clip.sourceInFrame);
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
    const ProbedMedia media = probeMedia(asset.generatedVideoPath);
    if (!media.success) {
        setStatus(media.error);
        return false;
    }
    const project::TimelineEditResult placed = project::appendManimTimelineClip(
        candidate, candidate.manimAssets.front(), newClipId(), media.fpsNum, media.fpsDen,
        media.frameCount);
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
    const ProbedMedia media = probeMedia(mediaPath);
    if (!media.success) {
        setStatus(media.error);
        return false;
    }
    project::Project candidate = project_;
    const project::TimelineClip clip{project::TimelineClipKind::Video, mediaPath,
                                     info.fileName().toStdString(), newClipId(), media.fpsNum,
                                     media.fpsDen, media.frameCount, 0, media.frameCount, 0};
    candidate.timelineClips.push_back(clip);
    const auto valid = project::recomputeTimelineStarts(candidate);
    if (!valid.success) {
        setStatus(QString::fromStdString(valid.error));
        return false;
    }

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
    return seekTimelineFrame(clip.timelineStartFrame);
}

bool MvmController::seekTimelineFrame(qint64 frame) {
    if (project_.timelineClips.empty()) {
        setStatus(QStringLiteral("seekするclipがありません"));
        return false;
    }
    const qint64 clamped = std::clamp<qint64>(frame, 0, totalTimelineFrames_ - 1);
    playheadFrame_ = clamped;
    const int index = project::timelineClipIndexAtFrame(project_, clamped);
    Q_EMIT stateChanged();
    if (index < 0) {
        setStatus(QStringLiteral("playhead位置にclipがありません"));
        return false;
    }
    const project::TimelineClip& clip = project_.timelineClips[static_cast<std::size_t>(index)];
    const QString clipName = QString::fromStdString(clip.name);
    if (!std::filesystem::is_regular_file(clip.mediaPath)) {
        setStatus(clipName + QStringLiteral(" のファイルがありません: ") +
                  fromPath(clip.mediaPath));
        return false;
    }

    if (!project::sourceRateMatchesTimelineRate(project_, clip)) {
        pendingVideoPath_.reset();
        pendingClipName_.clear();
        pendingClipIndex_ = -1;
        pendingSourceFrame_ = 0;
        resetPreviewEngine();
        currentSource_.reset();
        staleSource_.reset();
        currentClipIndex_ = index;
        currentClipName_ = clipName;
        currentClipPath_.clear();
        setStatus(clipName + QStringLiteral(
                                 " は60fpsではないためPreview未対応です。編集・保存・Exportは可能です"));
        return true;
    }

    const std::int64_t sourceFrame =
        clip.sourceInFrame + (clamped - clip.timelineStartFrame);
    if (currentSource_ && currentClipIndex_ == index) {
        const auto state = previewEngine_->status().state;
        if (state != preview::PreviewEngineState::ReadyPaused) {
            setStatus(QStringLiteral("Previewがseek可能になるまで待ってください"));
            return false;
        }
        const auto sought = previewEngine_->seek({sourceFrame});
        if (!sought) {
            setStatus(QStringLiteral("Previewをseekできません: ") +
                      previewErrorText(sought.error()));
            return false;
        }
        currentClipName_ = clipName;
        currentClipPath_ = fromPath(clip.mediaPath);
        setStatus(clipName + QStringLiteral(" を表示しています"));
        return true;
    }

    const preview::PreviewEngineState previewState = previewEngine_->status().state;
    if (previewState == preview::PreviewEngineState::ReadyPaused) {
        return installVideoClip(clip.mediaPath, clipName, index, sourceFrame);
    }
    queueVideoClipInstall(clip.mediaPath, clipName, index, sourceFrame);
    setStatus(QStringLiteral("Previewの準備を待っています"));
    return true;
}

bool MvmController::reorderClip(const QString& clipId, int destinationIndex) {
    if (busy_)
        return false;
    project::Project candidate = project_;
    const auto moved = project::reorderTimelineClip(candidate, clipId.toStdString(),
                                                     destinationIndex);
    if (!moved.success) {
        setStatus(QString::fromStdString(moved.error));
        return false;
    }
    if (!saveProject(std::move(candidate), QStringLiteral("Projectを保存できません: ")))
        return false;
    resetPreviewEngine();
    currentSource_.reset();
    staleSource_.reset();
    currentClipName_.clear();
    currentClipPath_.clear();
    currentClipIndex_ = moved.selectedIndex;
    playheadFrame_ = project_.timelineClips[static_cast<std::size_t>(moved.selectedIndex)]
                         .timelineStartFrame;
    statusText_ = QStringLiteral("clipを移動しました");
    Q_EMIT stateChanged();
    return true;
}

bool MvmController::trimClip(const QString& clipId, const QString& edge,
                             qint64 projectFrameDelta) {
    if (busy_)
        return false;
    project::TrimEdge trimEdge;
    if (edge == QStringLiteral("left"))
        trimEdge = project::TrimEdge::Left;
    else if (edge == QStringLiteral("right"))
        trimEdge = project::TrimEdge::Right;
    else {
        setStatus(QStringLiteral("trim edgeが不正です"));
        return false;
    }
    project::Project candidate = project_;
    const auto trimmed = project::trimTimelineClip(candidate, clipId.toStdString(), trimEdge,
                                                    projectFrameDelta);
    if (!trimmed.success) {
        setStatus(QString::fromStdString(trimmed.error));
        return false;
    }
    if (!saveProject(std::move(candidate), QStringLiteral("Projectを保存できません: ")))
        return false;
    resetPreviewEngine();
    currentSource_.reset();
    staleSource_.reset();
    currentClipName_.clear();
    currentClipPath_.clear();
    currentClipIndex_ = trimmed.selectedIndex;
    playheadFrame_ = project_.timelineClips[static_cast<std::size_t>(trimmed.selectedIndex)]
                         .timelineStartFrame;
    statusText_ = QStringLiteral("clipをtrimしました");
    Q_EMIT stateChanged();
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

    if (!saveProject(std::move(candidate), QStringLiteral("Projectを保存できません: ")))
        return false;

    pendingVideoPath_.reset();
    pendingClipName_.clear();
    pendingClipIndex_ = -1;
    pendingSourceFrame_ = 0;
    const bool previewReset = resetPreviewEngine();
    currentSource_.reset();
    staleSource_.reset();
    currentClipName_.clear();
    currentClipPath_.clear();
    currentClipIndex_ = -1;
    Q_EMIT stateChanged();
    if (!previewReset) {
        const QString resetFailure = statusText_;
        setStatus(QStringLiteral("clipは削除しましたが、") + resetFailure);
        return true;
    }

    if (deleted.selectedIndex >= 0) {
        if (!selectClip(deleted.selectedIndex)) {
            const QString previewFailure = statusText_;
            setStatus(QStringLiteral("clipは削除しましたが、次のclipをPreviewできません: ") +
                      previewFailure);
        }
        return true;
    }

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
